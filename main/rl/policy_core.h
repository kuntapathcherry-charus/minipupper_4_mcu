/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * The policy's arithmetic, with nothing ESP-specific in it, so the same file
 * is compiled on a PC and checked against omniRL's numpy runner
 * (mp3_walk_policy/contract.py and runner.py). Keep the two in step: every
 * function names the Python it mirrors.
 *
 * The .mpxp file (tools/policy/export_mpxp.py writes it, little-endian):
 *   mpxp_header_t (PC side: HEADER_SIZE), then for each layer W[out][in] and
 *   b[out] as float32, then n_fixtures observations (dims[0] floats each: 45, or
 *   45 x history) and n_fixtures actions;
 *   a CLIMB policy (MPXP_FLAG_CLIMB) then its reference block:
 *   uint32 n_ref, uint32 0, float32 res_units[12], float32 ref_units[n_ref][12];
 *   a policy with mini_pupper_4_recovery's decoder (MPXP_FLAG_DECODER) then its DEC1 block (mpxp_dec_t).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPXP_MAGIC        0x5058504Du   /* "MPXP" */
#define MPXP_VERSION      1
#define MPXP_HEADER_SIZE  640
#define MPXP_MAX_LAYERS   8
#define MPXP_MAX_CFG      8
#define MPXP_ACT_ELU      1
#define MPXP_FLAG_OMNI    1u
#define MPXP_FLAG_GETUP   2u   /* a get-up policy (omniRL getup.py): runs after a fall, not a walk */
#define MPXP_FLAG_GETUP_PHASE 4u   /* ... that tracks a motion (getup_track.py): its command inputs carry the
                                     phase phi, sin 2 pi phi, cos 2 pi phi; cmd_max[0] is the motion's length (s),
                                     cmd_max[1] the phase at which it is upright */
#define MPXP_FLAG_CLIMB   8u   /* a residual climb (omniRL climb_track.py --residual-deg): the phase in its command inputs
                                  as a tracked get-up's (cmd_max[0] = the motion's length, s; cmd_max[2] = the
                                  correction's range, deg, for show); its action is a CORRECTION to the trajopt
                                  reference the file carries: pol_climb_action() */
#define MPXP_FLAG_HEADING 16u  /* a terrain / stairs walk (RLmp4 train_terrain.py --heading loop): trained with its yaw-rate
                                  command from a heading loop (heading.pi_step: PI on the integrated heading, kp 2, ki 0.5,
                                  the integrator within +-0.5, clipped at cmd_max[2]); the runner closes the same loop
                                  (pol_heading_*) and holds the heading the robot had at the start or after a turn */
#define MPXP_FLAG_DECODER 32u  /* mini_pupper_4_recovery's contract (its docs/POLICY_CONTRACT.md, contract.py ActionDecoder):
                                  the file carries a DEC1 block (mpxp_dec_t, after the fixtures and any climb block) with
                                  the target filter, the stance offset, the joint bounds (knee coupling, hip envelope) and
                                  the runner's rules (deploy/runner.py: fall, hand-back, phase retry, guards).
                                  pol_decode_net() is its decoder; the runner uses the file's calibration */
#define MPXP_DEC_MAGIC    0x31434544u   /* "DEC1" */
#define MPXP_DEC_VERSION  1
#define POL_ENV_MAX       128           /* hip-envelope points per leg */

#define POL_OBS  45
#define POL_HIST_MAX 16   /* the longest observation history a file may ask for */
#define POL_ACT  12
#define POL_N    12   /* servos, id 1..12 at index 0..11 */

typedef struct {
    uint32_t magic, version, total_size, crc32;
    char     name[48];
    char     robot[16];
    uint32_t n_layers;
    uint32_t dims[MPXP_MAX_LAYERS + 1];
    uint32_t activation, flags, n_fixtures, history;   /* history: 0 or 1 = the observation alone, k > 1 = the
                                                          last k observations stacked per term (pol_hist_stack) */
    float    ctrl_dt, state_dt, step_limit_deg, tilt_limit_deg;   /* degrees: the C side
                                                                    converts as np.radians */
    float    tilt_tau_s, temp_stop_c, temp_start_c, cap_ma;
    float    cmd_max[3];
    float    imu_R[9];             /* sensor -> body, row-major */
    float    signs[POL_N];
    float    scale[POL_N];
    float    band_deg[POL_N];
    float    offsets_deg[POL_N];   /* the contract's calibration */
    int32_t  stand_dd[POL_N];
    uint32_t n_cfg;
    struct { uint32_t param_id; float value; } cfg[MPXP_MAX_CFG];
} mpxp_header_t;

/* The DEC1 block (MPXP_FLAG_DECODER), little-endian, every field 4 bytes; then per leg (FR, FL, RR, RL) with
 * n_env[leg] > 0 its hip envelope: float abd[n], lo[n], hi[n] (model rad, abd increasing). Angles in rad unless
 * named _deg; "model" = the contract's stand_policy_rad + rel. export_mpxp.py DEC_FIELDS writes it. */
typedef struct {
    uint32_t magic, version, words;   /* words: the whole block, envelopes included, in 4-byte words */
    uint32_t kind;                    /* 0 walk, 1 get-up */
    uint32_t has_bounds;
    uint32_t n_env[4];
    uint32_t max_getups;              /* falls a run gets up from; the next one goes limp */
    float    filter_alpha;            /* the target filter: rel = rel + alpha (prev_rel - rel); 0 = none */
    float    stand_off_deg[POL_N];    /* the stance offset: the policy's stand minus the calibrated stand (rel, deg) */
    float    stand_model[POL_N];      /* the calibrated stand in model coordinates (contract stand_policy_rad) */
    float    lo[POL_N], hi[POL_N];    /* target bounds, model coordinates */
    float    kc[4][2];                /* knee tendon = kc0 * thigh + kc1 * calf */
    float    calf_lo[4], calf_hi[4];  /* the calf joint's range */
    float    target_end_deg;          /* every target at least this far from a track end (0 .. 3100 dd) */
    float    fall_tilt_deg, fall_hold_s, fall_fast_tilt_deg, fall_fast_gyro;   /* estimators.FallDetector */
    float    cf_tau_s, cf_gate_lo_g, cf_gate_hi_g;                            /* estimators.GravityFilter */
    float    handback_tilt_deg, handback_gyro, handback_rms_deg, handback_steps;
    float    getup_timeout_s, phase_s, phase_restart_s, phase_restart_tilt_deg;
    float    cur_ma, cur_s;           /* the current guard while this policy drives; cur_ma 0 = none */
    float    temp_stop_c, temp_guard_c, temp_guard_s, temp_start_c;
    float    heading_wz_max;          /* the heading loop's clip while no turn is commanded (walk); 0 = no loop */
    float    track_margin_deg;        /* the hips' and knees' bounds are this far inside their pots' track ends, for the
                                         file's calibration (limits.py TRACK_MARGIN_DEG): pol_bounds_calibrate() moves them
                                         with another calibration; 0 = the bounds are fixed to the file's */
    float    reserved[6];
} mpxp_dec_t;

/* A parsed policy: pointers into the file's bytes (which must stay alive).
 * A layer with qbits[l] = 8 or 16 runs from Wq[l] (int8_t or int16_t,
 * row-major) and sq[l] (one float scale per output row) instead of W[l]:
 * pol_quantize_layer(). qbits[l] = 0: the file's float weights. */
typedef struct {
    const mpxp_header_t *h;
    const float *W[MPXP_MAX_LAYERS];
    const float *b[MPXP_MAX_LAYERS];
    const void  *Wq[MPXP_MAX_LAYERS];
    const float *sq[MPXP_MAX_LAYERS];
    uint8_t      qbits[MPXP_MAX_LAYERS];
    const float *fix_obs, *fix_act;
    int max_width;
    /* MPXP_FLAG_CLIMB: the reference (n_ref rows of 12, action units) and the correction's range (action units) */
    uint32_t     n_ref;
    const float *ref_units;
    const float *res_units;
    /* MPXP_FLAG_DECODER: the block, and each leg's hip envelope (NULL: none) */
    const mpxp_dec_t *dec;
    const float *env_abd[4], *env_lo[4], *env_hi[4];
    double       blo[POL_N], bhi[POL_N];   /* the bounds in use (model): the file's, or pol_bounds_calibrate()'s */
} pol_net_t;

/** Check and index a file. Returns NULL, or a reason it was refused. */
const char *pol_parse(const void *data, size_t len, pol_net_t *out);

uint32_t pol_crc32(const uint8_t *p, size_t n);

/** The observations the network takes at once: the file's history, at least 1. */
int pol_history(const mpxp_header_t *h);

/** The network (export.py NumpyPolicy.__call__). `obs` has dims[0] floats (45 x pol_history()).
 *  `work` needs 2 * max_width floats. */
void pol_forward(const pol_net_t *net, const float *obs, float act[POL_ACT], float *work);

/** Largest |forward(fixture_obs) - fixture_act| over the file's fixtures. */
float pol_selftest(const pol_net_t *net, float *work);

/** The same check in joint degrees (|error| x the action scale): the largest,
 *  and the mean in *mean_deg (may be NULL). */
float pol_selftest_deg(const pol_net_t *net, float *work, float *mean_deg);

/* ── 8- and 16-bit weights ───────────────────────────────────────────────
 * Each output row j of a layer: w[j][i] ~= s[j] * q[j][i], q an integer in
 * [-M, M] (M = 127 or 32767), s[j] = max_i |w[j][i]| / M (a row of zeros:
 * s 0). The activations stay float: y[j] = s[j] * sum_i q[j][i] * x[i] + b[j].
 * Half (16-bit) or a quarter (8-bit) of the bytes to read per step, at a
 * rounding of the weights pol_selftest_deg() measures against training. */

/** Bytes of one quantized layer's block: q (in*o integers, padded to 4), s[o], b[o]. */
size_t pol_quant_layer_size(int in, int o, int bits);

/** Fill `blk` (pol_quant_layer_size bytes, 4-byte aligned) from float
 *  weights W[o][in] and biases b[o]. bits: 8 or 16. */
void pol_quantize_layer(const float *W, const float *b, int in, int o, int bits, void *blk);

/** Point layer l of `net` at a quantized block. */
void pol_use_quant_layer(pol_net_t *net, uint32_t l, int bits, const void *blk);

/** A CLIMB policy's action at phase step `k` (steps since the climb began, the step whose observation had phase
 *  k * ctrl_dt / cmd_max[0]): the reference's next servo command plus the network's correction,
 *  clip(ref_units[min(k + 1, n_ref - 1)], +-1) + clip(raw, +-1) * res_units (climb_track.py
 *  ResidualServoAction.process_actions, teacher_action). Goes to pol_decode() as the raw action. */
void pol_climb_action(const pol_net_t *net, uint32_t k, const float raw[POL_ACT], float out[POL_ACT]);

/* ── the contract (contract.py Contract) ─────────────────────────────── */

/* A measured AT32 angle (dd) as a policy position: rel_from_dd(). */
double pol_rel_from_dd(double dd, double sign, double offset_deg);

/* The dd the firmware path would send for a policy target: quantise_rel()'s
 * dd (float32 arithmetic and C truncation, exactly as contract.py). */
int pol_dd_from_rel(double rel, double sign, double offset_deg);

/* ── ActionDecoder ───────────────────────────────────────────────────── */
typedef struct {
    double prev_rel[POL_ACT];
    double prev_action[POL_ACT];
} pol_decoder_t;

void pol_decoder_reset(pol_decoder_t *d);

/** raw action -> dd targets; updates prev_rel / prev_action (ActionDecoder.__call__). */
void pol_decode(pol_decoder_t *d, const mpxp_header_t *h, const double offsets_deg[POL_N],
                const float raw[POL_ACT], int dd_out[POL_ACT]);

/* ── mini_pupper_4_recovery's decoder (MPXP_FLAG_DECODER) ─────────────────
 * prev_rel and prev_action are the policy's (physical rel minus the stance offset), as contract.py's. A file without
 * the block: these fall back to pol_decode() and pol_decoder_reset(). */

/** recovery ActionDecoder.__call__: clip, scale, the target filter, the band, the step limit, + the stance offset,
 *  the bounds, the firmware's ticks; prev_rel = the quantised target minus the offset. */
void pol_decode_net(pol_decoder_t *d, const pol_net_t *net, const double offsets_deg[POL_N],
                    const float raw[POL_ACT], int dd_out[POL_ACT]);

/** recovery ActionDecoder.reset(rel0): rel0 NULL = the stand, else a measured pose (physical rel, rad): minus the
 *  offset, clipped to the band, into the bounds. */
void pol_decoder_seed(pol_decoder_t *d, const pol_net_t *net, const double rel0[POL_N]);

/** recovery Bounds.clamp_rel: physical rel (rad, about the calibrated stand) into the bounds in use, in place. */
void pol_bounds_clamp(const pol_net_t *net, double rel[POL_N]);

/** The bounds for a calibration (servo offsets, deg) other than the file's (recovery limits.decoder_bounds for a
 *  recalibrated robot): every servo's track limits (track_margin_deg inside its pot's ends) recomputed with these
 *  offsets, the physical ones (the abductions' measured range, the knee coupling, the hip envelope) kept as they are
 *  in the model's coordinates. The file's own offsets give the file's bounds back unchanged. */
void pol_bounds_calibrate(pol_net_t *net, const double offsets_deg[POL_N]);

/** The policy's stand as dd (recovery ActionDecoder.stand_dd): the stance offset, through the bounds, quantised. */
void pol_stand_dd(const pol_net_t *net, const double offsets_deg[POL_N], int dd_out[POL_N]);

/** The stance offset subtracted from a measured rel to give the position observation (0 without the block). */
double pol_obs_offset(const pol_net_t *net, int i);

/** recovery checks.decoder_selftest, deterministic: from the stand, the track ends, +-pi and random poses, saturated
 *  and random actions; every target inside the bounds and its dd at least target_end_deg from a track end.
 *  Returns NULL or why not. */
const char *pol_decoder_selftest(const pol_net_t *net, const double offsets_deg[POL_N]);

/* ── recovery estimators: the complementary-filter tilt and the fall detector ── */
typedef struct {
    double g[3];          /* gravity direction, body frame */
    bool   primed;
    double tau_s, gate_lo, gate_hi;
} pol_cf_t;

void   pol_cf_init(pol_cf_t *f, double tau_s, double gate_lo_g, double gate_hi_g);
/** GravityFilter.update: body-frame gyro (rad/s, bias removed) and accelerometer (g); returns the tilt (deg, NAN
 *  until primed). */
double pol_cf_update(pol_cf_t *f, const double gyro_body[3], const double acc_body_g[3], double dt);
double pol_cf_tilt(const pol_cf_t *f);

typedef struct {
    double since;         /* s; < 0: not above the tilt */
    bool   fallen;
    int    trigger;       /* 1: tilt held, 2: tilt with a fast turn */
} pol_fall_t;

void pol_fall_reset(pol_fall_t *f);
/** FallDetector.update at time t (s): latches. */
bool pol_fall_update(pol_fall_t *f, const mpxp_dec_t *d, double tilt_deg, double gyro_norm, double t);

/* ── VelocityFilter ──────────────────────────────────────────────────── */
typedef struct {
    double hist[6][POL_N];
    bool   primed;
} pol_velfilt_t;

void pol_vel_reset(pol_velfilt_t *f);
void pol_vel_push(pol_velfilt_t *f, const double x[POL_N]);
void pol_vel_get(const pol_velfilt_t *f, double v[POL_N]);

/* ── the IMU (ObservationBuilder.gyro_body / gravity_body, TiltFilter) ── */
void pol_gyro_body(const mpxp_header_t *h, const double gyro_rad_s_sensor[3],
                   const double bias_sensor[3], double out[3]);
/* false if |acc| is 0 */
bool pol_gravity_body(const mpxp_header_t *h, const double acc_sensor[3], double out[3]);

/** The body's tilt from upright (runner.tilt_deg); NAN for a zero vector. */
double pol_tilt_deg(const double acc_sensor[3]);

typedef struct {
    double acc[3];
    bool   primed;
    double k;
} pol_tilt_t;

void   pol_tilt_init(pol_tilt_t *t, double tau_s, double dt);
double pol_tilt_push(pol_tilt_t *t, const double acc_sensor[3]);

/** assemble_observation(): command is (vx, vy, wz); a forward-only policy gets (vx, 0, 0). */
void pol_observe(const double gyro_body[3], const double gravity_body[3], const double cmd[3],
                 const double pos_rel[POL_N], const double vel[POL_N], const double prev_action[POL_ACT],
                 float obs[POL_OBS]);

/* ── the observation history (contract.py HistoryStacker) ─────────────────
 * Each term's frames oldest to newest, the terms (ang_vel 3, gravity 3, command 3, pos 12, vel 12,
 * prev_action 12) concatenated in that order; the first observation after a reset fills every slot. */
typedef struct {
    float frames[POL_HIST_MAX][POL_OBS];   /* frames[0] the oldest */
    int   length;
    bool  primed;
} pol_hist_t;

void pol_hist_reset(pol_hist_t *s, int length);
/** Push this step's observation and write the stacked input (length x 45 floats) to `out`. */
void pol_hist_stack(pol_hist_t *s, const float obs[POL_OBS], float *out);

/* ── the heading loop (runner.HeadingLoop, heading.pi_step) ───────────── */
#define POL_HEADING_KP    2.0
#define POL_HEADING_KI    0.5
#define POL_HEADING_I_MAX 0.5
#define POL_HEADING_TAU_G 1.0   /* s: gravity low-passed for "up" */
typedef struct {
    double psi;        /* rad: the heading since the reference, + = turned left */
    double integ;      /* rad s */
    double g_lp[3];
    bool   primed;
    double err;        /* the last error the loop used */
} pol_heading_t;

void pol_heading_reset(pol_heading_t *hd);
/** Every STATE frame (dt s): the body yaw rate about the low-passed "up". */
void pol_heading_push(pol_heading_t *hd, const double gyro_body[3], const double gravity_body[3], double dt);
/** Each policy step (dt s): the yaw-rate command back to the reference heading, within +-wz_max. */
double pol_heading_wz(pol_heading_t *hd, double dt, double wz_max);

/** mp3link cli.smoothstep */
double pol_smoothstep(double x);

#ifdef __cplusplus
}
#endif
