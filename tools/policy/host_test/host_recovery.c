/* Host test of policy_core.c against mini_pupper_4_recovery (compare_recovery.py loads it with ctypes):
 * its decoder (bounds, knee coupling, hip envelope, target filter, stance offset), tilt filter, fall detector,
 * and the observation -> network -> targets path policy_run.cc runs for a recovery walk or get-up. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "policy_core.h"
static pol_net_t NET; static pol_decoder_t DEC; static double OFF[POL_N]; static float *WORK; static void *BUF;
static pol_cf_t CF; static pol_fall_t FALL;
const char *t_load(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return "open";
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    BUF = aligned_alloc(16, (n + 15) / 16 * 16); if (fread(BUF, 1, n, f) != (size_t)n) return "read"; fclose(f);
    const char *e = pol_parse(BUF, n, &NET); if (e) return e;
    for (int i = 0; i < POL_N; ++i) OFF[i] = NET.h->offsets_deg[i];
    WORK = malloc(sizeof(float) * 2 * NET.max_width);
    pol_decoder_reset(&DEC);
    if (NET.dec) pol_cf_init(&CF, NET.dec->cf_tau_s, NET.dec->cf_gate_lo_g, NET.dec->cf_gate_hi_g);
    pol_fall_reset(&FALL);
    return NULL;
}
float t_selftest(void) { return pol_selftest(&NET, WORK); }
const char *t_dec_selftest(void) { return pol_decoder_selftest(&NET, OFF); }
void t_seed(const double *rel0) { pol_decoder_seed(&DEC, &NET, rel0); }
void t_decode(const float *raw, int *dd, double *prev_action, double *prev_rel) {
    pol_decode_net(&DEC, &NET, OFF, raw, dd);
    for (int i = 0; i < POL_ACT; ++i) { prev_action[i] = DEC.prev_action[i]; prev_rel[i] = DEC.prev_rel[i]; }
}
void t_prev(double *pa) { for (int i = 0; i < POL_ACT; ++i) pa[i] = DEC.prev_action[i]; }
void t_stand(int *dd) { pol_stand_dd(&NET, OFF, dd); }
void t_bounds(double *v) { pol_bounds_clamp(&NET, v); }
void t_forward(const float *obs, float *act) { pol_forward(&NET, obs, act, WORK); }
double t_rel_from_dd(double dd, int i) { return pol_rel_from_dd(dd, NET.h->signs[i], OFF[i]); }
double t_obs_off(int i) { return pol_obs_offset(&NET, i); }
double t_cf(const double *w, const double *acc, double dt) { return pol_cf_update(&CF, w, acc, dt); }
int t_fall(double tilt, double gn, double t) { return pol_fall_update(&FALL, NET.dec, tilt, gn, t) ? FALL.trigger : 0; }
/* policy_run.cc's per-tick and per-step path for one network (as RUN / GETUP use it) */
static pol_velfilt_t VF; static pol_hist_t HIST; static float STK[POL_OBS * POL_HIST_MAX];
void t_run_reset(const double *dd, int seed_from_pose) {
    double rel[POL_N];
    for (int i = 0; i < POL_N; ++i) rel[i] = pol_rel_from_dd(dd[i], NET.h->signs[i], OFF[i]);
    pol_hist_reset(&HIST, pol_history(NET.h));
    pol_decoder_seed(&DEC, &NET, seed_from_pose ? rel : NULL);
    pol_vel_reset(&VF); pol_vel_push(&VF, rel);
}
void t_run_tick(const double *dd) {
    double rel[POL_N];
    for (int i = 0; i < POL_N; ++i) rel[i] = pol_rel_from_dd(dd[i], NET.h->signs[i], OFF[i]);
    pol_vel_push(&VF, rel);
}
void t_run_step(const double *dd, const double *acc_g, const double *gyr_rad, const double *bias, const double *cmd,
                float *obs_out, float *raw, int *dd_out) {
    double gb[3], grav[3], rel[POL_N], vel[POL_N];
    pol_gyro_body(NET.h, gyr_rad, bias, gb);
    pol_gravity_body(NET.h, acc_g, grav);
    for (int i = 0; i < POL_N; ++i) rel[i] = pol_rel_from_dd(dd[i], NET.h->signs[i], OFF[i]) - pol_obs_offset(&NET, i);
    pol_vel_get(&VF, vel);
    float o1[POL_OBS];
    pol_observe(gb, grav, cmd, rel, vel, DEC.prev_action, o1);
    if (pol_history(NET.h) > 1) pol_hist_stack(&HIST, o1, STK); else memcpy(STK, o1, sizeof(o1));
    memcpy(obs_out, o1, sizeof(o1));
    pol_forward(&NET, STK, raw, WORK);
    pol_decode_net(&DEC, &NET, OFF, raw, dd_out);
}
float t_quant(int bits, float *mean) {
    for (uint32_t l = 0; l < NET.h->n_layers; ++l) {
        int in = NET.h->dims[l], o = NET.h->dims[l + 1];
        void *blk = aligned_alloc(16, (pol_quant_layer_size(in, o, bits) + 15) / 16 * 16);
        pol_quantize_layer(NET.W[l], NET.b[l], in, o, bits, blk);
        pol_use_quant_layer(&NET, l, bits, blk);
    }
    return pol_selftest_deg(&NET, WORK, mean);
}
/* the robot's own calibration: the bounds and the decoder on these offsets */
const char *t_calibrate(const double *off) {
    for (int i = 0; i < POL_N; ++i) OFF[i] = off[i];
    pol_bounds_calibrate(&NET, OFF);
    return pol_decoder_selftest(&NET, OFF);
}
void t_bounds_now(double *lo, double *hi) { for (int i = 0; i < POL_N; ++i) { lo[i] = NET.blo[i]; hi[i] = NET.bhi[i]; } }
