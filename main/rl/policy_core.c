/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See policy_core.h. Plain C, no ESP headers: tools/policy/host_test builds
 * this file on a PC and compares it with omniRL's numpy.
 */
#include "policy_core.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

_Static_assert(offsetof(mpxp_header_t, name) == 16, "header layout");
_Static_assert(offsetof(mpxp_header_t, n_layers) == 80, "header layout");
_Static_assert(offsetof(mpxp_header_t, activation) == 120, "header layout");
_Static_assert(offsetof(mpxp_header_t, ctrl_dt) == 136, "header layout");
_Static_assert(offsetof(mpxp_header_t, cmd_max) == 168, "header layout");
_Static_assert(offsetof(mpxp_header_t, imu_R) == 180, "header layout");
_Static_assert(offsetof(mpxp_header_t, signs) == 216, "header layout");
_Static_assert(offsetof(mpxp_header_t, stand_dd) == 408, "header layout");
_Static_assert(offsetof(mpxp_header_t, cfg) == 460, "header layout");
_Static_assert(sizeof(mpxp_header_t) <= MPXP_HEADER_SIZE, "header layout");
_Static_assert(sizeof(mpxp_dec_t) == 420, "decoder block layout (export_mpxp.py DEC_WORDS 105)");
_Static_assert(offsetof(mpxp_dec_t, filter_alpha) == 40, "decoder block layout");
_Static_assert(offsetof(mpxp_dec_t, target_end_deg) == 300, "decoder block layout");
_Static_assert(offsetof(mpxp_dec_t, heading_wz_max) == 388, "decoder block layout");
_Static_assert(offsetof(mpxp_dec_t, track_margin_deg) == 392, "decoder block layout");

#define DEG2RAD (M_PI / 180.0)   /* np.radians */
#define RAD2DEG (180.0 / M_PI)   /* np.degrees */
#define DD0 (3100.0 - 511.0 * 3100.0 / 1024.0)

/* ── the file ────────────────────────────────────────────────────────── */

uint32_t pol_crc32(const uint8_t *p, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;   /* zlib.crc32 */
    for (size_t i = 0; i < n; ++i) {
        c ^= p[i];
        for (int k = 0; k < 8; ++k) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static bool finite_pos(float v)
{
    return isfinite(v) && v > 0.0f;
}

/* The DEC1 block at `p` (`avail` bytes left in the file): checked and indexed; *floats advanced past it. */
static const char *parse_dec(const float *p, size_t avail, pol_net_t *out, size_t *floats)
{
    if (avail < sizeof(mpxp_dec_t)) return "no decoder block in the file";
    const mpxp_dec_t *d = (const mpxp_dec_t *)p;
    if (d->magic != MPXP_DEC_MAGIC || d->version != MPXP_DEC_VERSION) return "unsupported decoder block";
    if (d->kind > 1) return "decoder block: unknown policy kind";
    size_t words = sizeof(mpxp_dec_t) / 4;
    const float *e = (const float *)(d + 1);
    for (int leg = 0; leg < 4; ++leg) {
        const uint32_t n = d->n_env[leg];
        if (n == 0) continue;
        if (n < 2 || n > POL_ENV_MAX || !d->has_bounds) return "decoder block: bad hip envelope";
        if ((words + 3u * n) * 4u > avail) return "decoder block: truncated hip envelope";
        out->env_abd[leg] = e;
        out->env_lo[leg] = e + n;
        out->env_hi[leg] = e + 2 * n;
        for (uint32_t k = 0; k < n; ++k) {
            if (!isfinite(e[k]) || !isfinite(e[n + k]) || !isfinite(e[2 * n + k]) || e[n + k] > e[2 * n + k] ||
                (k > 0 && !(e[k] > e[k - 1])))
                return "decoder block: bad hip envelope";
        }
        e += 3 * n;
        words += 3u * n;
    }
    if (d->words != words) return "decoder block: size does not match";
    if (!(d->filter_alpha >= 0.0f && d->filter_alpha < 1.0f)) return "decoder block: target filter not in [0, 1)";
    for (int i = 0; i < POL_N; ++i) {
        if (!isfinite(d->stand_off_deg[i]) || fabsf(d->stand_off_deg[i]) > 30.0f) return "decoder block: bad stance offset";
        if (d->has_bounds && (!isfinite(d->stand_model[i]) || !isfinite(d->lo[i]) || !isfinite(d->hi[i]) ||
                              !(d->lo[i] < d->hi[i])))
            return "decoder block: bad bounds";
    }
    if (d->has_bounds) {
        for (int leg = 0; leg < 4; ++leg) {
            if (!isfinite(d->kc[leg][0]) || !isfinite(d->kc[leg][1]) || d->kc[leg][1] == 0.0f ||
                !isfinite(d->calf_lo[leg]) || !isfinite(d->calf_hi[leg]) || !(d->calf_lo[leg] < d->calf_hi[leg]))
                return "decoder block: bad knee coupling";
        }
    }
    if (!finite_pos(d->fall_tilt_deg) || !finite_pos(d->fall_hold_s) || !finite_pos(d->fall_fast_tilt_deg) ||
        !finite_pos(d->fall_fast_gyro) || !finite_pos(d->cf_tau_s) || !finite_pos(d->cf_gate_hi_g) ||
        !(d->cf_gate_lo_g >= 0.0f && d->cf_gate_lo_g < d->cf_gate_hi_g))
        return "decoder block: bad fall rules";
    if (!finite_pos(d->handback_tilt_deg) || !finite_pos(d->handback_gyro) || !finite_pos(d->handback_rms_deg) ||
        !(d->handback_steps >= 1.0f && d->handback_steps <= 1000.0f))
        return "decoder block: bad hand-back rule";
    if (!finite_pos(d->getup_timeout_s) || !(d->phase_s >= 0.0f) || !(d->phase_restart_s >= 0.0f) ||
        !isfinite(d->phase_restart_tilt_deg))
        return "decoder block: bad get-up timing";
    if (d->kind == 1 && !(d->phase_s > 0.0f)) return "decoder block: a get-up without its phase clock";
    if (!(d->cur_ma >= 0.0f && d->cur_ma <= 32767.0f) || !(d->cur_s >= 0.0f) || (d->cur_ma > 0.0f && !(d->cur_s > 0.0f)))
        return "decoder block: bad current guard";
    if (!finite_pos(d->temp_stop_c) || !finite_pos(d->temp_guard_c) || !finite_pos(d->temp_guard_s) ||
        !finite_pos(d->temp_start_c) || !(d->target_end_deg >= 0.0f && d->target_end_deg < 90.0f) ||
        !(d->heading_wz_max >= 0.0f && d->heading_wz_max < 5.0f) || d->max_getups > 100)
        return "decoder block: bad limits";
    if (!(d->track_margin_deg >= 0.0f && d->track_margin_deg < 60.0f)) return "decoder block: bad track margin";
    out->dec = d;
    for (int i = 0; i < POL_N; ++i) {
        out->blo[i] = (double)d->lo[i];
        out->bhi[i] = (double)d->hi[i];
    }
    *floats += words;
    return NULL;
}

const char *pol_parse(const void *data, size_t len, pol_net_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!data || len < MPXP_HEADER_SIZE) return "too short for a policy file";
    if (((uintptr_t)data & 3u) != 0) return "not 4-byte aligned";
    const mpxp_header_t *h = (const mpxp_header_t *)data;
    if (h->magic != MPXP_MAGIC) return "not a policy file (no MPXP magic)";
    if (h->version != MPXP_VERSION) return "unsupported policy file version";
    if (h->total_size != len) return "size does not match the header (truncated?)";
    if (pol_crc32((const uint8_t *)data + 16, len - 16) != h->crc32) return "checksum mismatch";
    if (h->n_layers < 1 || h->n_layers > MPXP_MAX_LAYERS) return "bad layer count";
    if (h->history > POL_HIST_MAX) return "observation history too long";
    if (h->dims[0] != (uint32_t)(POL_OBS * pol_history(h)) || h->dims[h->n_layers] != POL_ACT)
        return "network is not 45 (x its history) -> 12";
    if (h->activation != MPXP_ACT_ELU) return "unsupported activation";
    if (h->n_cfg > MPXP_MAX_CFG) return "bad servo configuration";
    if (!(h->ctrl_dt > 0.0f) || !(h->state_dt > 0.0f)) return "bad control period";

    const float *p = (const float *)((const uint8_t *)data + MPXP_HEADER_SIZE);
    size_t floats = 0;
    int width = 0;
    for (uint32_t l = 0; l < h->n_layers; ++l) {
        const uint32_t in = h->dims[l], o = h->dims[l + 1];
        if (in == 0 || o == 0 || in > 4096 || o > 4096) return "bad layer size";
        out->W[l] = p + floats;
        floats += (size_t)in * o;
        out->b[l] = p + floats;
        floats += o;
        if ((int)o > width) width = (int)o;
        if ((int)in > width) width = (int)in;
    }
    out->fix_obs = p + floats;
    floats += (size_t)h->n_fixtures * h->dims[0];
    out->fix_act = p + floats;
    floats += (size_t)h->n_fixtures * POL_ACT;
    if (h->flags & MPXP_FLAG_CLIMB) {
        /* the reference block: n_ref, 0, res_units[12], ref_units[n_ref][12] */
        if (MPXP_HEADER_SIZE + (floats + 2 + POL_ACT) * sizeof(float) > len) return "no climb reference in the file";
        const uint32_t *u = (const uint32_t *)(p + floats);
        const uint32_t n_ref = u[0];
        if (n_ref < 2 || n_ref > 20000 || u[1] != 0) return "bad climb reference";
        out->n_ref = n_ref;
        out->res_units = p + floats + 2;
        out->ref_units = p + floats + 2 + POL_ACT;
        floats += 2 + POL_ACT + (size_t)n_ref * POL_ACT;
        if (!(h->cmd_max[0] > 0.0f)) return "a climb policy needs its length";
    }
    if (h->flags & MPXP_FLAG_DECODER) {
        const char *e = parse_dec(p + floats, len - MPXP_HEADER_SIZE - floats * sizeof(float), out, &floats);
        if (e) return e;
    }
    if (MPXP_HEADER_SIZE + floats * sizeof(float) != len) return "layer sizes do not add up to the file";
    out->h = h;
    out->max_width = width;
    return NULL;
}

static float clampf1(float v)
{
    return v < -1.0f ? -1.0f : (v > 1.0f ? 1.0f : v);   /* torch .clamp(-1, 1) */
}

void pol_climb_action(const pol_net_t *net, uint32_t k, const float raw[POL_ACT], float out[POL_ACT])
{
    if (!net->ref_units || net->n_ref < 1) {
        for (int i = 0; i < POL_ACT; ++i) out[i] = raw[i];
        return;
    }
    uint32_t nxt = k + 1;
    if (nxt > net->n_ref - 1) nxt = net->n_ref - 1;
    const float *r = net->ref_units + (size_t)nxt * POL_ACT;
    for (int i = 0; i < POL_ACT; ++i) out[i] = clampf1(r[i]) + clampf1(raw[i]) * net->res_units[i];
}

/* ── the network ─────────────────────────────────────────────────────── */

/* One integer-weight layer (the float loop's four sums, the row's scale once
 * at the end). */
#define DENSE_Q(NAME, T)                                                                   \
static void NAME(const T *Q, const float *sc, const float *b, int in, int o, int elu,       \
                 const float *x, float *y)                                                 \
{                                                                                          \
    for (int j = 0; j < o; ++j) {                                                          \
        const T *q = Q + (size_t)j * in;                                                   \
        float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;                                  \
        int i = 0;                                                                         \
        for (; i + 4 <= in; i += 4) {                                                      \
            a0 += (float)q[i] * x[i];                                                      \
            a1 += (float)q[i + 1] * x[i + 1];                                              \
            a2 += (float)q[i + 2] * x[i + 2];                                              \
            a3 += (float)q[i + 3] * x[i + 3];                                              \
        }                                                                                  \
        for (; i < in; ++i) a0 += (float)q[i] * x[i];                                      \
        float s = ((a0 + a1) + (a2 + a3)) * sc[j] + b[j];                                  \
        if (elu) s = s > 0.0f ? s : expm1f(s);                                             \
        y[j] = s;                                                                          \
    }                                                                                      \
}
DENSE_Q(dense_q8, int8_t)
DENSE_Q(dense_q16, int16_t)

int pol_history(const mpxp_header_t *h)
{
    return h->history > 1 ? (int)h->history : 1;
}

void pol_forward(const pol_net_t *net, const float *obs, float act[POL_ACT], float *work)
{
    const mpxp_header_t *h = net->h;
    float *x = work, *y = work + net->max_width;
    memcpy(x, obs, sizeof(float) * h->dims[0]);
    for (uint32_t l = 0; l < h->n_layers; ++l) {
        const int in = (int)h->dims[l], o = (int)h->dims[l + 1];
        const float *W = net->W[l], *b = net->b[l];
        if (net->qbits[l] == 8) {
            dense_q8((const int8_t *)net->Wq[l], net->sq[l], b, in, o, l + 1 < h->n_layers, x, y);
            float *t = x; x = y; y = t;
            continue;
        }
        if (net->qbits[l] == 16) {
            dense_q16((const int16_t *)net->Wq[l], net->sq[l], b, in, o, l + 1 < h->n_layers, x, y);
            float *t = x; x = y; y = t;
            continue;
        }
        for (int j = 0; j < o; ++j) {
            const float *w = W + (size_t)j * in;
            /* Four independent sums: the S3's FPU pipelines them, where one
             * running sum waits for each add (minipupper_rl's dense()). */
            float a0 = 0.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;
            int i = 0;
            for (; i + 4 <= in; i += 4) {
                a0 += w[i] * x[i];
                a1 += w[i + 1] * x[i + 1];
                a2 += w[i + 2] * x[i + 2];
                a3 += w[i + 3] * x[i + 3];
            }
            for (; i < in; ++i) a0 += w[i] * x[i];
            float s = (a0 + a1) + (a2 + a3);
            s += b[j];
            if (l + 1 < h->n_layers) s = s > 0.0f ? s : expm1f(s);   /* ELU */
            y[j] = s;
        }
        float *t = x; x = y; y = t;
    }
    memcpy(act, x, sizeof(float) * POL_ACT);
}

static size_t quant_q_bytes(int in, int o, int bits)
{
    return (((size_t)in * (size_t)o * (size_t)(bits / 8)) + 3u) & ~(size_t)3u;
}

size_t pol_quant_layer_size(int in, int o, int bits)
{
    return quant_q_bytes(in, o, bits) + 2u * sizeof(float) * (size_t)o;
}

void pol_quantize_layer(const float *W, const float *b, int in, int o, int bits, void *blk)
{
    const long M = bits == 16 ? 32767 : 127;
    float *s = (float *)((uint8_t *)blk + quant_q_bytes(in, o, bits));
    float *bb = s + o;
    memset(blk, 0, quant_q_bytes(in, o, bits));
    for (int j = 0; j < o; ++j) {
        const float *w = W + (size_t)j * in;
        float m = 0.0f;
        for (int i = 0; i < in; ++i) {
            const float a = fabsf(w[i]);
            if (a > m) m = a;
        }
        const float sc = m > 0.0f ? m / (float)M : 0.0f;
        for (int i = 0; i < in; ++i) {
            long v = sc > 0.0f ? lrintf(w[i] / sc) : 0;
            if (v > M) v = M;
            if (v < -M) v = -M;
            if (bits == 16) ((int16_t *)blk)[(size_t)j * in + i] = (int16_t)v;
            else            ((int8_t *)blk)[(size_t)j * in + i] = (int8_t)v;
        }
        s[j] = sc;
        bb[j] = b[j];
    }
}

void pol_use_quant_layer(pol_net_t *net, uint32_t l, int bits, const void *blk)
{
    const int in = (int)net->h->dims[l], o = (int)net->h->dims[l + 1];
    net->Wq[l] = blk;
    net->sq[l] = (const float *)((const uint8_t *)blk + quant_q_bytes(in, o, bits));
    net->b[l] = net->sq[l] + o;
    net->qbits[l] = (uint8_t)bits;
}

float pol_selftest_deg(const pol_net_t *net, float *work, float *mean_deg)
{
    float worst = 0.0f, act[POL_ACT];
    double sum = 0.0;
    uint32_t n = 0;
    for (uint32_t k = 0; k < net->h->n_fixtures; ++k) {
        pol_forward(net, net->fix_obs + (size_t)k * net->h->dims[0], act, work);
        for (int j = 0; j < POL_ACT; ++j) {
            const float e = fabsf(act[j] - net->fix_act[(size_t)k * POL_ACT + j]) * net->h->scale[j] *
                            (float)RAD2DEG;
            if (!(e <= worst)) worst = e;   /* a NaN wins */
            sum += e;
            ++n;
        }
    }
    if (mean_deg) *mean_deg = n ? (float)(sum / n) : 0.0f;
    return worst;
}

float pol_selftest(const pol_net_t *net, float *work)
{
    float worst = 0.0f, act[POL_ACT];
    for (uint32_t k = 0; k < net->h->n_fixtures; ++k) {
        pol_forward(net, net->fix_obs + (size_t)k * net->h->dims[0], act, work);
        for (int j = 0; j < POL_ACT; ++j) {
            const float e = fabsf(act[j] - net->fix_act[(size_t)k * POL_ACT + j]);
            if (!(e <= worst)) worst = e;   /* a NaN wins */
        }
    }
    return worst;
}

/* ── the contract ────────────────────────────────────────────────────── */

double pol_rel_from_dd(double dd, double sign, double offset_deg)
{
    const double servo_deg = (DD0 - dd) / 10.0 - offset_deg;   /* servo_deg_from_dd */
    return sign * (servo_deg * DEG2RAD);                        /* rel_from_servo_deg */
}

int pol_dd_from_rel(double rel, double sign, double offset_deg)
{
    const double servo_deg = rel * RAD2DEG * sign;              /* servo_deg_from_rel */
    /* ticks_from_servo_deg: float32 throughout, as numpy does it */
    volatile float ang = (float)servo_deg + (float)offset_deg;
    volatile float q = ang * 10240.0f;
    q = q / 3100.0f;
    long t = 511 + (long)truncf(q);
    if (t < 0) t = 0;
    if (t > 1023) t = 1023;
    /* dd_from_ticks */
    volatile float tf = (float)t;
    volatile float r = tf * 3100.0f;
    r = r / 1024.0f;
    r = 3100.0f - r;
    return (int)truncf(r);
}

void pol_decoder_reset(pol_decoder_t *d)
{
    memset(d, 0, sizeof(*d));
}

static double clampd(double v, double lo, double hi)
{
    return v < lo ? lo : (v > hi ? hi : v);   /* np.clip */
}

void pol_decode(pol_decoder_t *d, const mpxp_header_t *h, const double offsets_deg[POL_N],
                const float raw[POL_ACT], int dd_out[POL_ACT])
{
    const double limit = (double)h->step_limit_deg * DEG2RAD;
    for (int i = 0; i < POL_ACT; ++i) {
        const double scale = (double)h->scale[i];
        const double band = (double)h->band_deg[i] * DEG2RAD;
        const double a = clampd((double)raw[i], -1.0, 1.0);
        double rel = clampd(scale * a, -band, band);
        rel = clampd(rel, d->prev_rel[i] - limit, d->prev_rel[i] + limit);
        const int dd = pol_dd_from_rel(rel, (double)h->signs[i], offsets_deg[i]);
        rel = pol_rel_from_dd((double)dd, (double)h->signs[i], offsets_deg[i]);
        dd_out[i] = dd;
        d->prev_rel[i] = rel;
        d->prev_action[i] = rel / scale;
    }
}

/* ── mini_pupper_4_recovery's decoder ────────────────────────────────── */

static double npclip(double v, double lo, double hi)
{
    return fmin(fmax(v, lo), hi);   /* np.clip: minimum(maximum(v, lo), hi) */
}

/* np.interp(x, xp, fp): constant beyond the ends */
static double npinterp(double x, const float *xp, const float *fp, uint32_t n)
{
    if (!(x >= (double)xp[0])) return x < (double)xp[0] ? (double)fp[0] : x;   /* NaN stays NaN */
    if (x >= (double)xp[n - 1]) return (double)fp[n - 1];
    uint32_t lo = 0, hi = n - 1;                                             /* xp[lo] <= x < xp[hi] */
    while (hi - lo > 1) {
        const uint32_t m = (lo + hi) / 2;
        if ((double)xp[m] <= x) lo = m;
        else hi = m;
    }
    const double slope = ((double)fp[hi] - (double)fp[lo]) / ((double)xp[hi] - (double)xp[lo]);
    return slope * (x - (double)xp[lo]) + (double)fp[lo];
}

double pol_obs_offset(const pol_net_t *net, int i)
{
    return net->dec ? (double)net->dec->stand_off_deg[i] * DEG2RAD : 0.0;   /* stand_offset_rel: sign * np.radians */
}

void pol_bounds_clamp(const pol_net_t *net, double v[POL_N])
{
    const mpxp_dec_t *d = net->dec;
    if (!d || !d->has_bounds) return;
    double st[POL_N], lo[POL_N], hi[POL_N];
    for (int i = 0; i < POL_N; ++i) {
        st[i] = (double)d->stand_model[i];
        lo[i] = net->blo[i] - st[i];
        hi[i] = net->bhi[i] - st[i];
        v[i] = npclip(v[i], lo[i], hi[i]);
    }
    for (int leg = 0; leg < 4; ++leg) {
        const uint32_t n = d->n_env[leg];
        if (n == 0) continue;
        const int a = 3 * leg, t = 3 * leg + 1;
        const double abd = st[a] + v[a];
        const double elo = npinterp(abd, net->env_abd[leg], net->env_lo[leg], n) - st[t];
        const double ehi = npinterp(abd, net->env_abd[leg], net->env_hi[leg], n) - st[t];
        v[t] = npclip(npclip(v[t], elo, ehi), lo[t], hi[t]);
    }
    for (int leg = 0; leg < 4; ++leg) {
        const int t = 3 * leg + 1, k = 3 * leg + 2;
        const double thigh = st[t] + v[t];
        const double a = (double)d->kc[leg][0] * thigh + (double)d->kc[leg][1] * (double)d->calf_lo[leg] - st[k];
        const double b = (double)d->kc[leg][0] * thigh + (double)d->kc[leg][1] * (double)d->calf_hi[leg] - st[k];
        v[k] = npclip(npclip(v[k], fmin(a, b), fmax(a, b)), lo[k], hi[k]);
    }
}

/* limits.track_rel + the stand: servo i's target range margin_deg inside its pot's track ends, model coordinates */
static void track_model(const pol_net_t *net, int i, double off_deg, double *lo, double *hi)
{
    const double m = 10.0 * (double)net->dec->track_margin_deg, s = (double)net->h->signs[i];
    const double a = pol_rel_from_dd(0.0 + m, s, off_deg), b = pol_rel_from_dd(3100.0 - m, s, off_deg);
    *lo = (double)net->dec->stand_model[i] + fmin(a, b);
    *hi = (double)net->dec->stand_model[i] + fmax(a, b);
}

void pol_bounds_calibrate(pol_net_t *net, const double offsets_deg[POL_N])
{
    const mpxp_dec_t *d = net->dec;
    if (!d) return;
    bool same = true;
    for (int i = 0; i < POL_N; ++i) same = same && fabs(offsets_deg[i] - (double)net->h->offsets_deg[i]) < 1e-9;
    for (int i = 0; i < POL_N; ++i) {
        net->blo[i] = (double)d->lo[i];
        net->bhi[i] = (double)d->hi[i];
        if (same || !d->has_bounds || !(d->track_margin_deg > 0.0f)) continue;
        double flo, fhi, rlo, rhi;
        track_model(net, i, (double)net->h->offsets_deg[i], &flo, &fhi);   /* the file's calibration */
        track_model(net, i, offsets_deg[i], &rlo, &rhi);                     /* this one */
        /* a bound at the file's track limit is a track limit: it moves; any other is physical: it stays, within the
         * new track limits (limits.decoder_bounds: the abductions' range intersected with the track's) */
        net->blo[i] = fabs(net->blo[i] - flo) < 1e-5 ? rlo : fmax(net->blo[i], rlo);
        net->bhi[i] = fabs(net->bhi[i] - fhi) < 1e-5 ? rhi : fmin(net->bhi[i], rhi);
        if (!(net->blo[i] < net->bhi[i])) {   /* cannot happen on a sane calibration: keep the file's */
            net->blo[i] = (double)d->lo[i];
            net->bhi[i] = (double)d->hi[i];
        }
    }
}

void pol_decode_net(pol_decoder_t *d, const pol_net_t *net, const double offsets_deg[POL_N],
                    const float raw[POL_ACT], int dd_out[POL_ACT])
{
    const mpxp_dec_t *dc = net->dec;
    const mpxp_header_t *h = net->h;
    if (!dc) {
        pol_decode(d, h, offsets_deg, raw, dd_out);
        return;
    }
    const double limit = (double)h->step_limit_deg * DEG2RAD;
    const double alpha = (double)dc->filter_alpha;
    double phys[POL_ACT];
    for (int i = 0; i < POL_ACT; ++i) {
        const double scale = (double)h->scale[i];
        const double band = (double)h->band_deg[i] * DEG2RAD;
        double rel = scale * clampd((double)raw[i], -1.0, 1.0);
        if (alpha != 0.0) rel = rel + alpha * (d->prev_rel[i] - rel);   /* the target filter */
        rel = clampd(rel, -band, band);
        rel = clampd(rel, d->prev_rel[i] - limit, d->prev_rel[i] + limit);
        phys[i] = rel + pol_obs_offset(net, i);                         /* + the stance offset */
    }
    pol_bounds_clamp(net, phys);
    for (int i = 0; i < POL_ACT; ++i) {
        const int dd = pol_dd_from_rel(phys[i], (double)h->signs[i], offsets_deg[i]);
        const double rel = pol_rel_from_dd((double)dd, (double)h->signs[i], offsets_deg[i]) - pol_obs_offset(net, i);
        dd_out[i] = dd;
        d->prev_rel[i] = rel;
        d->prev_action[i] = rel / (double)h->scale[i];
    }
}

void pol_decoder_seed(pol_decoder_t *d, const pol_net_t *net, const double rel0[POL_N])
{
    pol_decoder_reset(d);
    if (!net->dec) return;
    const mpxp_header_t *h = net->h;
    double phys[POL_N];
    for (int i = 0; i < POL_N; ++i) {
        const double band = (double)h->band_deg[i] * DEG2RAD;
        d->prev_rel[i] = rel0 ? clampd(rel0[i] - pol_obs_offset(net, i), -band, band) : 0.0;
        phys[i] = d->prev_rel[i] + pol_obs_offset(net, i);
    }
    if (net->dec->has_bounds) {
        pol_bounds_clamp(net, phys);
        for (int i = 0; i < POL_N; ++i) d->prev_rel[i] = phys[i] - pol_obs_offset(net, i);
    }
    for (int i = 0; i < POL_N; ++i) d->prev_action[i] = d->prev_rel[i] / (double)h->scale[i];
}

void pol_stand_dd(const pol_net_t *net, const double offsets_deg[POL_N], int dd_out[POL_N])
{
    const mpxp_header_t *h = net->h;
    bool plain = !net->dec || !net->dec->has_bounds;
    for (int i = 0; i < POL_N && plain; ++i) plain = pol_obs_offset(net, i) == 0.0;
    double phys[POL_N];
    for (int i = 0; i < POL_N; ++i) phys[i] = pol_obs_offset(net, i);
    pol_bounds_clamp(net, phys);
    for (int i = 0; i < POL_N; ++i)
        dd_out[i] = plain ? pol_dd_from_rel(0.0, (double)h->signs[i], offsets_deg[i])
                          : pol_dd_from_rel(phys[i], (double)h->signs[i], offsets_deg[i]);
}

/* xorshift32: the self-test's random poses and actions, the same on every build */
static double sel_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return (double)x / 4294967296.0;
}

const char *pol_decoder_selftest(const pol_net_t *net, const double offsets_deg[POL_N])
{
    if (!net->dec) return NULL;
    const mpxp_header_t *h = net->h;
    const double m = 10.0 * (double)net->dec->target_end_deg;
    double ends[2][POL_N];
    for (int i = 0; i < POL_N; ++i) {
        ends[0][i] = pol_rel_from_dd(0.0, (double)h->signs[i], offsets_deg[i]);
        ends[1][i] = pol_rel_from_dd(3100.0, (double)h->signs[i], offsets_deg[i]);
    }
    uint32_t seed = 0x2545F491u;
    pol_decoder_t d;
    double rel0[POL_N];
    float a[POL_ACT];
    int dd[POL_ACT];
    for (int s = 0; s < 9; ++s) {
        for (int i = 0; i < POL_N; ++i) {
            const double lo = fmin(ends[0][i], ends[1][i]), hi = fmax(ends[0][i], ends[1][i]);
            switch (s) {
            case 1: rel0[i] = M_PI; break;
            case 2: rel0[i] = -M_PI; break;
            case 3: rel0[i] = ends[0][i]; break;
            case 4: rel0[i] = ends[1][i]; break;
            case 5: case 6: rel0[i] = -M_PI + 2.0 * M_PI * sel_rand(&seed); break;
            default: rel0[i] = lo + (hi - lo) * sel_rand(&seed); break;
            }
        }
        pol_decoder_seed(&d, net, s == 0 ? NULL : rel0);
        for (int k = 0; k < 60; ++k) {
            for (int i = 0; i < POL_ACT; ++i) {
                const int r = k % 4;
                a[i] = r == 0 ? 1.0f : r == 1 ? -1.0f : r == 2 ? (sel_rand(&seed) < 0.5 ? -1.0f : 1.0f)
                                                         : (float)(-1.0 + 2.0 * sel_rand(&seed));
            }
            pol_decode_net(&d, net, offsets_deg, a, dd);
            for (int i = 0; i < POL_ACT; ++i)
                if (dd[i] < m || dd[i] > 3100.0 - m) return "a target too near a servo's track end";
        }
    }
    return NULL;
}

/* ── recovery estimators ─────────────────────────────────────────────── */

void pol_cf_init(pol_cf_t *f, double tau_s, double gate_lo_g, double gate_hi_g)
{
    memset(f, 0, sizeof(*f));
    f->tau_s = tau_s;
    f->gate_lo = gate_lo_g;
    f->gate_hi = gate_hi_g;
}

double pol_cf_tilt(const pol_cf_t *f)
{
    if (!f->primed) return NAN;
    return acos(clampd(-f->g[2], -1.0, 1.0)) * RAD2DEG;
}

double pol_cf_update(pol_cf_t *f, const double w[3], const double acc[3], double dt)
{
    const double dropout = 0.001;   /* g: runner ACC_DROPOUT_MG */
    const double n = sqrt(acc[0] * acc[0] + acc[1] * acc[1] + acc[2] * acc[2]);
    if (!f->primed) {
        if (n <= dropout || fabs(n - 1.0) >= f->gate_hi) return NAN;
        for (int i = 0; i < 3; ++i) f->g[i] = -acc[i] / n;
        f->primed = true;
        return pol_cf_tilt(f);
    }
    double g[3] = { f->g[0], f->g[1], f->g[2] };
    const double wn = sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    const double th = wn * dt;
    if (th > 0.0) {   /* g turned by -th about w / |w| (Rodrigues) */
        const double k[3] = { w[0] / wn, w[1] / wn, w[2] / wn };
        const double c = cos(th), s = sin(th);
        const double kxg[3] = { k[1] * g[2] - k[2] * g[1], k[2] * g[0] - k[0] * g[2], k[0] * g[1] - k[1] * g[0] };
        const double kg = k[0] * g[0] + k[1] * g[1] + k[2] * g[2];
        for (int i = 0; i < 3; ++i) g[i] = g[i] * c - kxg[i] * s + k[i] * kg * (1.0 - c);
    }
    if (n > dropout) {
        const double dev = fabs(n - 1.0);
        const double wt = fmin(1.0, fmax(0.0, (f->gate_hi - dev) / (f->gate_hi - f->gate_lo)));
        const double gain = fmin(1.0, dt / f->tau_s) * wt;
        for (int i = 0; i < 3; ++i) g[i] = g[i] + gain * (-acc[i] / n - g[i]);
    }
    double gn = sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (gn < 1e-12) gn = 1e-12;
    for (int i = 0; i < 3; ++i) f->g[i] = g[i] / gn;
    return pol_cf_tilt(f);
}

void pol_fall_reset(pol_fall_t *f)
{
    f->since = -1.0;
    f->fallen = false;
    f->trigger = 0;
}

bool pol_fall_update(pol_fall_t *f, const mpxp_dec_t *d, double tilt, double gyro_norm, double t)
{
    if (f->fallen) return true;
    if (tilt > (double)d->fall_tilt_deg) {   /* NaN: not above */
        if (f->since < 0.0) f->since = t;
        if (t - f->since >= (double)d->fall_hold_s - 1e-9) {
            f->fallen = true;
            f->trigger = 1;
        }
    } else {
        f->since = -1.0;
    }
    if (!f->fallen && tilt > (double)d->fall_fast_tilt_deg && gyro_norm > (double)d->fall_fast_gyro) {
        f->fallen = true;
        f->trigger = 2;
    }
    return f->fallen;
}

/* ── VelocityFilter ──────────────────────────────────────────────────── */

void pol_vel_reset(pol_velfilt_t *f)
{
    memset(f, 0, sizeof(*f));
}

void pol_vel_push(pol_velfilt_t *f, const double x[POL_N])
{
    if (!f->primed) {
        for (int k = 0; k < 6; ++k) memcpy(f->hist[k], x, sizeof(double) * POL_N);
        f->primed = true;
        return;
    }
    memmove(f->hist[0], f->hist[1], sizeof(double) * POL_N * 5);
    memcpy(f->hist[5], x, sizeof(double) * POL_N);
}

static double med3(double a, double b, double c)
{
    if (a > b) { double t = a; a = b; b = t; }
    if (b > c) { b = c; }
    return a > b ? a : b;
}

void pol_vel_get(const pol_velfilt_t *f, double v[POL_N])
{
    static const double c[4] = { -1.5, -0.5, 0.5, 1.5 };
    const double den = 5.0 * 0.005;   /* 5.0 * STATE_DT */
    if (!f->primed) {
        memset(v, 0, sizeof(double) * POL_N);
        return;
    }
    for (int j = 0; j < POL_N; ++j) {
        double s = 0.0;
        for (int k = 0; k < 4; ++k) {
            s += (c[k] / den) * med3(f->hist[k][j], f->hist[k + 1][j], f->hist[k + 2][j]);
        }
        v[j] = s;
    }
}

/* ── the IMU ─────────────────────────────────────────────────────────── */

static void rot(const mpxp_header_t *h, const double in[3], double out[3])
{
    for (int r = 0; r < 3; ++r) {
        out[r] = (double)h->imu_R[3 * r + 0] * in[0] + (double)h->imu_R[3 * r + 1] * in[1] +
                 (double)h->imu_R[3 * r + 2] * in[2];
    }
}

void pol_gyro_body(const mpxp_header_t *h, const double gyro_rad_s_sensor[3],
                   const double bias_sensor[3], double out[3])
{
    const double g[3] = { gyro_rad_s_sensor[0] - bias_sensor[0], gyro_rad_s_sensor[1] - bias_sensor[1],
                          gyro_rad_s_sensor[2] - bias_sensor[2] };
    rot(h, g, out);
}

bool pol_gravity_body(const mpxp_header_t *h, const double acc_sensor[3], double out[3])
{
    double a[3];
    rot(h, acc_sensor, a);
    const double n = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(n > 0.0)) {
        out[0] = out[1] = out[2] = 0.0;
        return false;
    }
    for (int i = 0; i < 3; ++i) out[i] = -a[i] / n;
    return true;
}

double pol_tilt_deg(const double a[3])
{
    const double n = sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(n > 0.0)) return NAN;
    return acos(clampd(-a[2] / n, -1.0, 1.0)) * RAD2DEG;
}

void pol_tilt_init(pol_tilt_t *t, double tau_s, double dt)
{
    memset(t, 0, sizeof(*t));
    t->k = dt / tau_s;
}

double pol_tilt_push(pol_tilt_t *t, const double a[3])
{
    if (!t->primed) {
        memcpy(t->acc, a, sizeof(t->acc));
        t->primed = true;
    } else {
        for (int i = 0; i < 3; ++i) t->acc[i] = t->acc[i] + t->k * (a[i] - t->acc[i]);
    }
    return pol_tilt_deg(t->acc);
}

void pol_observe(const double gyro_body[3], const double gravity_body[3], const double cmd[3],
                 const double pos_rel[POL_N], const double vel[POL_N], const double prev_action[POL_ACT],
                 float obs[POL_OBS])
{
    for (int i = 0; i < 3; ++i) {
        obs[i] = (float)gyro_body[i];
        obs[3 + i] = (float)gravity_body[i];
        obs[6 + i] = (float)cmd[i];
    }
    for (int i = 0; i < POL_N; ++i) {
        obs[9 + i] = (float)pos_rel[i];
        obs[21 + i] = (float)vel[i];
        obs[33 + i] = (float)prev_action[i];
    }
}

/* ── the observation history ─────────────────────────────────────────── */

void pol_hist_reset(pol_hist_t *s, int length)
{
    memset(s, 0, sizeof(*s));
    s->length = length < 1 ? 1 : (length > POL_HIST_MAX ? POL_HIST_MAX : length);
}

void pol_hist_stack(pol_hist_t *s, const float obs[POL_OBS], float *out)
{
    static const int term_start[7] = { 0, 3, 6, 9, 21, 33, 45 };   /* contract.py OBS_TERMS */
    const int L = s->length;
    if (!s->primed) {
        for (int k = 0; k < L; ++k) memcpy(s->frames[k], obs, sizeof(float) * POL_OBS);
        s->primed = true;
    } else {
        memmove(s->frames[0], s->frames[1], sizeof(float) * POL_OBS * (size_t)(L - 1));
        memcpy(s->frames[L - 1], obs, sizeof(float) * POL_OBS);
    }
    int o = 0;
    for (int t = 0; t < 6; ++t) {
        const int a = term_start[t], n = term_start[t + 1] - a;
        for (int k = 0; k < L; ++k) {
            memcpy(out + o, s->frames[k] + a, sizeof(float) * (size_t)n);
            o += n;
        }
    }
}

/* ── the heading loop ────────────────────────────────────────────────── */

void pol_heading_reset(pol_heading_t *hd)
{
    memset(hd, 0, sizeof(*hd));
}

void pol_heading_push(pol_heading_t *hd, const double gyro_body[3], const double gravity_body[3], double dt)
{
    if (!hd->primed) {
        memcpy(hd->g_lp, gravity_body, sizeof(hd->g_lp));
        hd->primed = true;
    } else {
        const double alpha = dt / POL_HEADING_TAU_G;
        for (int i = 0; i < 3; ++i) hd->g_lp[i] += alpha * (gravity_body[i] - hd->g_lp[i]);
    }
    double n = sqrt(hd->g_lp[0] * hd->g_lp[0] + hd->g_lp[1] * hd->g_lp[1] + hd->g_lp[2] * hd->g_lp[2]);
    if (n < 1e-9) n = 1e-9;
    double rate = 0.0;
    for (int i = 0; i < 3; ++i) rate += gyro_body[i] * (-hd->g_lp[i] / n);
    hd->psi += rate * dt;
}

double pol_heading_wz(pol_heading_t *hd, double dt, double wz_max)
{
    /* runner.HeadingLoop.error(): back to the reference heading, wrapped to [-pi, pi) */
    const double e = -(fmod(fmod(hd->psi + M_PI, 2.0 * M_PI) + 2.0 * M_PI, 2.0 * M_PI) - M_PI);
    hd->err = e;
    /* heading.pi_step, antiwindup "cond" */
    double nxt = clampd(hd->integ + e * dt, -POL_HEADING_I_MAX, POL_HEADING_I_MAX);
    const double u0 = POL_HEADING_KP * e + POL_HEADING_KI * hd->integ;
    if (fabs(u0) >= wz_max && e * u0 > 0.0) nxt = hd->integ;
    hd->integ = nxt;
    return clampd(POL_HEADING_KP * e + POL_HEADING_KI * nxt, -wz_max, wz_max);
}

double pol_smoothstep(double x)
{
    x = clampd(x, 0.0, 1.0);
    return x * x * (3.0 - 2.0 * x);
}
