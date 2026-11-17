"""The robot's recovery arithmetic (main/rl/policy_core.c, through host_recovery.so) against mini_pupper_4_recovery's
numpy: contract.py ActionDecoder and Bounds, deploy/estimators.py GravityFilter, FallDetector and ObservationBuilder,
policy_file.NumpyPolicy.
    python3 compare_recovery.py <mini_pupper_4_recovery> ./host_recovery.so walker rec_walker.mpxp
Expected: "0 of ... targets differ", stand dd equal, decoder self-test ok, fall detector disagreements 0, and the
closed-loop path "dd differ 0/..."."""

import ctypes as C, json, math, sys
from pathlib import Path
import numpy as np
repo, so, pname, mpxp = sys.argv[1:5]
sys.path.insert(0, str(Path(repo) / "src"))
from mp4_recovery.contract import (ActionDecoder, Contract, policy_band_deg, policy_bounds, policy_filter_alpha,
                                   policy_stand_offset_deg)
from mp4_recovery.deploy.estimators import FallDetector, GravityFilter
lib = C.CDLL(so)
lib.t_load.restype = C.c_char_p; lib.t_dec_selftest.restype = C.c_char_p
lib.t_selftest.restype = C.c_float; lib.t_cf.restype = C.c_double; lib.t_obs_off.restype = C.c_double
lib.t_cf.argtypes = [C.POINTER(C.c_double), C.POINTER(C.c_double), C.c_double]
lib.t_fall.argtypes = [C.c_double, C.c_double, C.c_double]
lib.t_obs_off.argtypes = [C.c_int]
err = lib.t_load(mpxp.encode())
assert err is None, err
D12 = C.c_double * 12; F12 = C.c_float * 12; I12 = C.c_int * 12
c = Contract.load()
z = np.load(Path(repo) / "policies" / pname / "policy.npz")
meta = json.loads(str(z["meta"]))
scale = z["scale"].astype(float)
def dec_py():
    return ActionDecoder(c, scale, band_deg=policy_band_deg(meta), bounds=policy_bounds(meta),
                         stand_offset_abd_deg=policy_stand_offset_deg(meta), filter_alpha=policy_filter_alpha(meta))
print(f"{pname}: fixtures max err {lib.t_selftest():.2e}; decoder self-test: {lib.t_dec_selftest() or 'ok'}")
d = dec_py()
sd = I12(); lib.t_stand(sd)
print("stand dd C ", list(sd)); print("stand dd py", d.stand_dd().tolist(), "equal:", list(sd) == d.stand_dd().tolist())
print("obs offset C", [round(lib.t_obs_off(i), 9) for i in range(12)], "py", np.round(d.obs_offset, 9).tolist())
rng = np.random.default_rng(7)
ends = np.stack([c.rel_from_dd(np.zeros(12)), c.rel_from_dd(np.full(12, 3100.0))])
n_dd = n_tot = 0; worst_pa = 0.0
for trial in range(60):
    d = dec_py()
    kind = trial % 4
    rel0 = None if kind == 0 else rng.uniform(ends.min(0), ends.max(0)) if kind == 1 else rng.uniform(-math.pi, math.pi, 12) \
        if kind == 2 else c.rel_from_dd(rng.uniform(1000, 2200, 12))
    d.reset(rel0)
    lib.t_seed(None if rel0 is None else D12(*rel0))
    pa = D12(); lib.t_prev(pa)
    worst_pa = max(worst_pa, float(np.abs(np.array(pa) - d.prev_action).max()))
    for k in range(200):
        m = k % 5
        raw = (np.ones(12) if m == 0 else -np.ones(12) if m == 1 else rng.choice([-1., 1.], 12) if m == 2
               else rng.normal(0, 1.2, 12)).astype(np.float32)
        r = d(raw)
        dd = I12(); pa = D12(); pr = D12()
        lib.t_decode(F12(*raw), dd, pa, pr)
        n_dd += int(np.sum(np.array(dd) != r["dd"])); n_tot += 12
        worst_pa = max(worst_pa, float(np.abs(np.array(pa) - r["action"]).max()))
        if np.any(np.array(dd) != r["dd"]):   # resync so one flip does not cascade
            d.prev_rel = np.array(pr); d.prev_action = d.prev_rel / d.scale
print(f"decoder: {n_dd} of {n_tot} targets differ in dd; prev_action max diff {worst_pa:.2e}")
b = policy_bounds(meta)
w = 0.0
for _ in range(20000):
    v = rng.uniform(-3.5, 3.5, 12)
    vc = D12(*v); lib.t_bounds(vc)
    w = max(w, float(np.abs(np.array(vc) - b.clamp_rel(v, c.stand_rel_rad)).max()))
print(f"bounds clamp: max diff {w:.2e} rad over 20000 random poses")
# the tilt filter and the fall detector
gf, fd = GravityFilter(), FallDetector()
tw, fall_diff = 0.0, 0
th = np.zeros(3); g_true = np.array([0.0, 0.0, -1.0])
for t in range(6000):
    wv = rng.normal(0, 0.3, 3) + (np.array([2.5, 0, 0]) if 1500 < t < 1600 or 3000 < t < 3040 else 0)
    acc = -g_true * 1000 + rng.normal(0, 150, 3)
    if t % 97 == 0: acc = acc * 2.5
    if t % 501 == 0: acc = np.zeros(3)
    dt = 0.005 * (1 + (t % 233 == 0))
    tp = gf.update(wv, acc, dt)
    tc = lib.t_cf(C.cast((C.c_double * 3)(*wv), C.POINTER(C.c_double)), C.cast((C.c_double * 3)(*(acc / 1000)), C.POINTER(C.c_double)), dt)
    if not (math.isnan(tp) and math.isnan(tc)):
        tw = max(tw, abs(tp - tc))
    fp = fd.update(tp, float(np.linalg.norm(wv)), t * 0.005)
    fc = lib.t_fall(tc, float(np.linalg.norm(wv)), t * 0.005)
    fall_diff += int(bool(fp) != bool(fc))
    # rotate the true gravity with the gyro so the tilt moves
    ang = np.linalg.norm(wv) * dt
    if ang > 0:
        k = wv / np.linalg.norm(wv); g_true = g_true * math.cos(ang) - np.cross(k, g_true) * math.sin(ang) + k * k.dot(g_true) * (1 - math.cos(ang))
print(f"tilt filter: max diff {tw:.2e} deg (final {tp:.1f} deg); fall detector disagreements {fall_diff}, fell {fd.fallen} ({fd.trigger})")

# the closed-loop path
from mp4_recovery.contract import HistoryStacker
from mp4_recovery.deploy.estimators import ObservationBuilder, getup_phase
from mp4_recovery.policy_file import NumpyPolicy
D = lambda n: C.c_double * n
pol = NumpyPolicy(Path(repo) / "policies" / pname / "policy.npz")
hist = int(pol.meta.get("history", 0) or 0)
getup = pol.meta.get("kind") == "getup"
class St: pass
R = np.array([[0.0, 1.0, 0.0], [1.0, 0.0, 0.0], [0.0, 0.0, -1.0]])
rng = np.random.default_rng(3)
w = dict(obs=0.0, raw=0.0); ndd = ntot = 0
for trial in range(6):
    bias = rng.normal(0, 0.01, 3)
    ob = ObservationBuilder(c, R, bias)
    dec = ActionDecoder(c, pol.scale, band_deg=policy_band_deg(pol.meta), bounds=policy_bounds(pol.meta), stand_offset_abd_deg=policy_stand_offset_deg(pol.meta), filter_alpha=policy_filter_alpha(pol.meta))
    stack = HistoryStacker(hist) if hist else None
    dd = np.asarray(c.stand_dd, float) + rng.normal(0, 30, 12)
    seed = trial % 2 == 1
    def frame(t):
        s = St(); s.tick = t; s.pos_dd = np.clip(np.rint(dd), 200, 2900); s.acc_mg = np.array([20., -30., -1000.]) + rng.normal(0, 200, 3)
        s.gyr_ddps = rng.normal(0, 300, 3); return s
    s = frame(0); ob.push(s)
    dec.reset(c.rel_from_dd(s.pos_dd) if seed else None)
    lib.t_run_reset(D(12)(*s.pos_dd), int(seed))
    for k in range(150):
        for j in range(4):
            dd = dd + rng.normal(0, 5, 12)
            s = frame(1 + 4 * k + j); ob.push(s)
            if j < 3: lib.t_run_tick(D(12)(*s.pos_dd))
        cmd = getup_phase(k * 0.02, 4.0) if getup else tuple(rng.uniform(-0.12, 0.12, 3))
        obs = ob.observe(cmd, dec.prev_action, dec.obs_offset)
        x = stack(obs) if stack else obs
        raw = pol(x); d = dec(raw)
        lib.t_run_tick(D(12)(*s.pos_dd))
        o = (C.c_float * 45)(); r = (C.c_float * 12)(); ddo = (C.c_int * 12)()
        lib.t_run_step(D(12)(*s.pos_dd), D(3)(*(s.acc_mg / 1000)), D(3)(*(s.gyr_ddps * 0.1 * math.pi / 180)), D(3)(*bias), D(3)(*cmd), o, r, ddo)
        w["obs"] = max(w["obs"], float(np.abs(np.array(o) - obs).max())); w["raw"] = max(w["raw"], float(np.abs(np.array(r) - raw).max()))
        ndd += int(np.sum(np.array(ddo) != d["dd"])); ntot += 12
print(f"{pname}: obs max diff {w['obs']:.2e}, raw {w['raw']:.2e}, dd differ {ndd}/{ntot}")

# the robot's own calibration (pol_bounds_calibrate) against limits.decoder_bounds' arithmetic on a recalibrated contract
import copy
from mp4_recovery.contract import Bounds, TRACK_DD
lib.t_calibrate.restype = C.c_char_p
info = (meta.get("bounds") or {}).get("info") or {}
margin = float(info.get("track_margin_deg", 20.0))
nd = nt = 0; fails = 0
for trial in range(20):
    c2 = copy.deepcopy(c)
    c2.offsets_deg = c.offsets_deg + (rng.uniform(-10, 10, 12) if trial else 0.0)
    why = lib.t_calibrate(D(12)(*c2.offsets_deg))
    if why: fails += 1
    b0 = policy_bounds(meta)
    m = 10.0 * margin
    a_ = c2.rel_from_dd(np.full(12, TRACK_DD[0] + m)); b_ = c2.rel_from_dd(np.full(12, TRACK_DD[1] - m))
    lo = c.stand_rel_rad + np.minimum(a_, b_); hi = c.stand_rel_rad + np.maximum(a_, b_)
    for i in (0, 3, 6, 9):                       # the abductions' physical range (the stored one), within the track
        lo[i] = max(lo[i], b0.lo[i]); hi[i] = min(hi[i], b0.hi[i])
    if trial == 0:
        lo, hi = b0.lo, b0.hi
    b2 = Bounds(lo=lo, hi=hi, kc=b0.kc, calf_lo=b0.calf_lo, calf_hi=b0.calf_hi, hip_envelope=b0.hip_envelope, mode=b0.mode)
    clo, chi = D(12)(), D(12)(); lib.t_bounds_now(clo, chi)
    wb = max(np.abs(np.array(clo) - lo).max(), np.abs(np.array(chi) - hi).max())
    d = ActionDecoder(c2, scale, band_deg=policy_band_deg(meta), bounds=b2, stand_offset_abd_deg=policy_stand_offset_deg(meta),
                      filter_alpha=policy_filter_alpha(meta))
    d.reset(); lib.t_seed(None)
    for k in range(300):
        raw = (rng.choice([-1., 1.], 12) if k % 2 else rng.normal(0, 1.2, 12)).astype(np.float32)
        r = d(raw); dd = (C.c_int * 12)(); pa = D(12)(); pr = D(12)()
        lib.t_decode((C.c_float * 12)(*raw), dd, pa, pr)
        nd += int(np.sum(np.array(dd) != r["dd"])); nt += 12
        if np.any(np.array(dd) != r["dd"]):
            d.prev_rel = np.array(pr); d.prev_action = d.prev_rel / d.scale
print(f"own calibration (offsets +-10 deg, 20 robots): bounds max diff {wb:.1e} rad, {nd} of {nt} targets differ, "
      f"decoder self-test failed on {fails}")
