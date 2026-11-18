#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Pack an omniRL-style walk policy into one .mpxp file the robot runs itself.

    python tools/policy/export_mpxp.py <omniRL>/policies/bam_t5ub06_stops_v1 \\
        --contract <omniRL>/mp3_walk_policy/data/sku2_contract.json \\
        --mount <omniRL>/mp3_walk_policy/data/imu_mount.json --robot sku2 \\
        --out bam_t5ub06_stops_v1.mpxp

The file holds everything the Pi runner (omniRL mp3_walk_policy/runner.py)
needs besides the robot: the network (Linear layers, ELU between them), the
contract (servo signs, calibration offsets, action scale, the band about the
stand, the per-step limit), the IMU mounting, the servo configuration the
policy was identified with (`mp3link config`), the safety limits, and a few
fixtures (observations with the actions training produced for them) so the
robot can check its own forward pass. numpy only.

Format (little-endian, every field 4 bytes): see HEADER below and
main/rl/policy_core.h, which must agree.
"""
import argparse
import json
import struct
import zlib
from pathlib import Path

import numpy as np

MAGIC = b"MPXP"
VERSION = 1
HEADER_SIZE = 640
MAX_LAYERS = 8
MAX_CFG = 8
ACT_ELU = 1
FLAG_OMNI = 1
FLAG_GETUP = 2      # omniRL getup.py: the firmware runs it after a fall (policy getup, never stays down)
FLAG_GETUP_PHASE = 4   # omniRL getup_track.py: it tracks a motion, and the firmware feeds the phase in the command
                       # inputs; cmd_max carries the motion's length (s) and the phase at which it is upright
FLAG_CLIMB = 8      # omniRL climb_track.py with --residual-deg (the climb7 residual RL): the phase in the command
                    # inputs as a tracked get-up's (cmd_max[0] = the motion's length, s), and the action is a CORRECTION
                    # added to a trajopt reference the file carries (the CLIMB block after the fixtures):
                    #   uint32 n_ref, uint32 0, float32 res_units[12], float32 ref_units[n_ref][12]
                    # action at phase step k = clip(ref_units[min(k+1, n_ref-1)]) + clip(raw, +-1) * res_units
                    # (climb_track.py ResidualServoAction / teacher_action), then the walk's decoder as ever
CLIMB_TILT_LIMIT_DEG = 55.0   # the climb pitches ~30 deg; training ends an episode at "up" 0.4 off the reference's
FLAG_HEADING = 16   # RLmp4 train_terrain.py --heading loop --heading-law pi (the terrain and stairs walks): trained with the
                    # yaw-rate command from a heading loop; the firmware closes it (kp 2, ki 0.5, |integ| <= 0.5, clipped at
                    # cmd_max[2] = the run's heading_wz) and holds the heading. The header's `history` (the 4th word after the
                    # dims, formerly reserved): the observations stacked per term (contract.py HistoryStacker), 0 = none
TERRAIN_TILT_LIMIT_DEG = 55.0   # stairs pitch the body; training's tilt limit there is 1.0 rad (57 deg)
HISTORY_MAX = 16
FLAG_DECODER = 32   # mini_pupper_4_recovery's walker and get-up (its docs/POLICY_CONTRACT.md, contract.py ActionDecoder,
                    # deploy/runner.py): the DEC1 block after the fixtures (and any climb block) carries the target filter,
                    # the stance offset, the joint bounds (knee coupling, hip envelope) and the Pi runner's rules; the
                    # firmware decodes with them (policy_core.h mpxp_dec_t) and runs on the file's calibration
DEC_MAGIC = 0x31434544      # "DEC1"
DEC_VERSION = 1
DEC_WORDS = 105             # sizeof(mpxp_dec_t) / 4, without the envelopes
LEGS = ("FR", "FL", "RR", "RL")
ABD_OUT_SIGN = (1.0, -1.0, -1.0, 1.0)    # recovery contract.py: outward is + rel on FR and RL, - on FL and RR
# recovery deploy/runner.py and estimators.py (its defaults: --getup-max 10, --getup-timeout 20, --getup-phase-restart 1),
# and its firmware's guards (sdkconfig.defaults: walking 1500 mA for 0.5 s, none in get-up mode; 60 degC for 1 s)
REC_RULES = dict(target_end_deg=15.0, fall=(60.0, 0.3, 45.0, 2.0), cf=(1.0, 0.15, 0.5), handback=(12.0, 0.8, 10.0, 25.0),
                 getup_timeout_s=20.0, phase_restart_s=1.0, phase_restart_tilt_deg=45.0, max_getups=10,
                 temp=(55.0, 60.0, 1.0, 45.0), walk_cur=(1500.0, 0.5), heading_wz_max=0.4)
REC_CMD_MAX = (0.12, 0.12, 0.4)   # the walker's command (m/s, m/s, rad/s); wz: the heading loop's clip

# omniRL's runner and contract (mp3_walk_policy/contract.py, runner.py)
STEP_LIMIT_DEG = 20.0
ABDUCTION = (0, 3, 6, 9)
BAND_DEG = [20.0 if i in ABDUCTION else 60.0 for i in range(12)]
CTRL_DT = 0.020
STATE_DT = 0.005
TILT_LIMIT_DEG = 35.0
TILT_TAU_S = 0.1
TEMP_STOP_C = 55.0
TEMP_START_C = 45.0
CAP_MA = 2000.0
# Training's command ranges (docs/USER_GUIDE.md: 0.05-0.12 m/s in any
# direction, |wz| <= 0.6 rad/s); the robot clamps to these.
CMD_MAX = (0.12, 0.12, 0.6)
# `mp3link config` (the configuration the servos were identified with).
# Driver-board parameter ids: main/robot/driver_board.h.
SERVO_CFG = [(11, 1.0),      # control_mode
             (5, 80.0),      # kp_position
             (6, 400.0),     # kd_position
             (10, 0.0),      # ki_position
             (7, 0.0),       # kp_current
             (8, 0.001),     # kff_current
             (9, 0.99)]      # max_pwm_duty_cycle
IMU_MOUNTS = {
    "flip_x": [[1, 0, 0], [0, -1, 0], [0, 0, -1]],
    "flip_y": [[-1, 0, 0], [0, 1, 0], [0, 0, -1]],
    "swap_xy": [[0, 1, 0], [1, 0, 0], [0, 0, -1]],
    "swap_xy_neg": [[0, -1, 0], [-1, 0, 0], [0, 0, -1]],
}


def header(name, robot, dims, flags, n_fix, c, mount, band=BAND_DEG, tilt_limit=TILT_LIMIT_DEG, cmd_max=CMD_MAX,
           history=0):
    f = []
    f.append(struct.pack("<4sIII", MAGIC, VERSION, 0, 0))            # size, crc filled in later
    f.append(struct.pack("<48s16s", name.encode()[:47], robot.encode()[:15]))
    f.append(struct.pack("<I", len(dims) - 1))
    f.append(struct.pack("<9I", *(list(dims) + [0] * (MAX_LAYERS + 1 - len(dims)))))
    f.append(struct.pack("<IIII", ACT_ELU, flags, n_fix, history))
    f.append(struct.pack("<4f", CTRL_DT, STATE_DT, STEP_LIMIT_DEG, tilt_limit))
    f.append(struct.pack("<4f", TILT_TAU_S, TEMP_STOP_C, TEMP_START_C, CAP_MA))
    f.append(struct.pack("<3f", *cmd_max))
    f.append(struct.pack("<9f", *np.asarray(mount, dtype=float).ravel()))
    f.append(struct.pack("<12f", *c["signs"]))
    f.append(struct.pack("<12f", *c["scale"]))
    f.append(struct.pack("<12f", *band))
    f.append(struct.pack("<12f", *c["offsets_deg"]))
    f.append(struct.pack("<12i", *[int(v) for v in c["stand_dd"]]))
    f.append(struct.pack("<I", len(SERVO_CFG)))
    cfg = SERVO_CFG + [(0xFFFFFFFF, 0.0)] * (MAX_CFG - len(SERVO_CFG))
    for pid, val in cfg:
        f.append(struct.pack("<If", pid, val))
    h = b"".join(f)
    assert len(h) <= HEADER_SIZE, len(h)
    return h + b"\0" * (HEADER_SIZE - len(h))


def lf_sha256(path: Path) -> str:
    import hashlib
    return hashlib.sha256(path.read_bytes().replace(b"\r\n", b"\n")).hexdigest()


def recovery_policy(meta, scale, contract, contract_path):
    """A mini_pupper_4_recovery policy (meta kind walk / getup with its decoder): (flags, header extras, the DEC1 block).
    Refuses what its runner refuses (deploy/checks.py band_refusal, contract.calibration_refusal)."""
    kind = meta.get("kind") or "walk"
    if kind not in ("walk", "getup"):
        raise SystemExit(f"recovery policy kind {kind!r}")
    want = meta.get("contract_sha256")
    have = lf_sha256(contract_path)
    if want and want != have and want not in (contract.get("calibrations_superseded") or []):
        raise SystemExit(f"the policy was exported for contract {want[:12]}..., --contract is {have[:12]}...")
    band = meta.get("band_deg")                          # contract.band_deg_array: none, 1, 3 (per leg) or 12 values
    if band is None:
        band = [20.0 if i % 3 == 0 else 60.0 for i in range(12)]
    else:
        b = np.asarray(band, dtype=float).reshape(-1)
        b = np.full(12, b[0]) if b.size == 1 else np.tile(b, 4) if b.size == 3 else b
        if b.shape != (12,):
            raise SystemExit(f"band_deg {band}")
        band = [float(v) for v in b]
    bounds = meta.get("bounds")
    alpha = float(meta.get("action_filter_alpha", 0.0) or 0.0)
    off_deg = float(meta.get("stand_offset_abd_deg", 0.0) or 0.0)
    if not 0.0 <= alpha < 1.0:
        raise SystemExit(f"action_filter_alpha {alpha}")
    widest = 180.0 if kind == "getup" and bounds else 90.0
    if max(band) > widest or min(band) <= 0.0:
        raise SystemExit(f"band {max(band):g} deg: the runner takes at most {widest:g} (180 only for a get-up with bounds)")
    if kind == "walk" and any(b > (20.0 if i % 3 == 0 else 60.0) for i, b in enumerate(band)):
        raise SystemExit("a walk policy's band is wider than the walking band 20/60 deg")
    if off_deg and not bounds:
        raise SystemExit("a stand offset without decoder bounds")
    stand = [float(v) for v in contract["stand_policy_rad"]]
    tc = [[float(contract["tendon_coefs"][lg][f"{lg}_thigh"]), float(contract["tendon_coefs"][lg][f"{lg}_calf"])]
          for lg in LEGS]
    env = {}
    if bounds:
        if not bounds.get("frame", "").startswith("model: contract stand_policy_rad + rel"):
            raise SystemExit(f"bounds frame {bounds.get('frame')!r}")
        if [[float(a), float(b)] for a, b in bounds["tendon_coefs"]] != tc:
            raise SystemExit("bounds: tendon coefficients differ from the contract's")
        sdd = (bounds.get("info") or {}).get("contract_stand_dd")
        if sdd is not None and [int(v) for v in sdd] != [int(v) for v in contract["stand_dd"]]:
            raise SystemExit("bounds made for another calibration (stand dd differ): re-export the policy for this contract")
        env = bounds.get("hip_envelope") or {}
    n_env = [len(env[lg]["abd"]) if lg in env else 0 for lg in LEGS]
    r = REC_RULES
    getup = kind == "getup"
    phase_s = float(meta.get("getup_phase_s") or 0.0)
    if getup and not phase_s > 0.0:
        raise SystemExit("a get-up policy without its phase clock (meta getup_phase_s)")
    words = DEC_WORDS + 3 * sum(n_env)
    f = [struct.pack("<5I", DEC_MAGIC, DEC_VERSION, words, 1 if getup else 0, 1 if bounds else 0),
         struct.pack("<4I", *n_env), struct.pack("<I", r["max_getups"]), struct.pack("<f", alpha)]
    f.append(struct.pack("<12f", *[ABD_OUT_SIGN[i // 3] * off_deg if i % 3 == 0 else 0.0 for i in range(12)]))
    f.append(struct.pack("<12f", *stand))
    lo = [float(v) for v in bounds["lo"]] if bounds else [s_ - 10.0 for s_ in stand]
    hi = [float(v) for v in bounds["hi"]] if bounds else [s_ + 10.0 for s_ in stand]
    calf = [[float(a), float(b)] for a, b in bounds["calf_range"]] if bounds else [[-10.0, 10.0]] * 4
    f.append(struct.pack("<12f", *lo))
    f.append(struct.pack("<12f", *hi))
    f.append(struct.pack("<8f", *[v for row in tc for v in row]))
    f.append(struct.pack("<4f", *[c_[0] for c_ in calf]))
    f.append(struct.pack("<4f", *[c_[1] for c_ in calf]))
    f.append(struct.pack("<f", r["target_end_deg"]))
    f.append(struct.pack("<4f", *r["fall"]))
    f.append(struct.pack("<3f", *r["cf"]))
    f.append(struct.pack("<4f", *r["handback"]))
    f.append(struct.pack("<4f", r["getup_timeout_s"], phase_s, r["phase_restart_s"], r["phase_restart_tilt_deg"]))
    f.append(struct.pack("<2f", *((0.0, 0.0) if getup else r["walk_cur"])))
    f.append(struct.pack("<4f", *r["temp"]))
    f.append(struct.pack("<f", 0.0 if getup else r["heading_wz_max"]))
    # the hips' and knees' bounds sit this far inside their pots' track ends (limits.py): the firmware moves them for
    # the robot's own calibration (pol_bounds_calibrate)
    track_margin = max(0.0, float(((bounds or {}).get("info") or {}).get("track_margin_deg", 0.0) or 0.0)) if bounds else 0.0
    f.append(struct.pack("<f", track_margin))
    f.append(b"\0" * 24)
    assert len(b"".join(f)) == DEC_WORDS * 4, len(b"".join(f))
    for lg in LEGS:
        if lg in env:
            e = env[lg]
            a_ = np.asarray(e["abd"], dtype=np.float32)
            if np.any(np.diff(a_) <= 0):
                raise SystemExit(f"hip envelope {lg}: abduction not increasing in float32")
            for key in ("abd", "lo", "hi"):
                f.append(np.ascontiguousarray(np.asarray(e[key], dtype=np.float32), dtype="<f4").tobytes())
    block = b"".join(f)
    assert len(block) == 4 * words
    flags = FLAG_DECODER | FLAG_OMNI
    if getup:
        flags |= FLAG_GETUP | FLAG_GETUP_PHASE
        extra = dict(band=band, tilt_limit=180.0, cmd_max=(phase_s, 0.0, 0.0))
    else:
        if not meta.get("omni"):
            raise SystemExit("a recovery walker observes the whole command (meta omni)")
        extra = dict(band=band, tilt_limit=r["fall"][0], cmd_max=REC_CMD_MAX)
    print(f"recovery {kind}: band {band[0]:g}/{band[1]:g}/{band[2]:g} deg, filter {alpha:g}, stance offset {off_deg:g} deg, "
          f"bounds {'yes' if bounds else 'no'}{', hip envelope ' + str(n_env) if any(n_env) else ''}"
          f"{', phase ' + format(phase_s, 'g') + ' s' if getup else ''}")
    return flags, extra, block


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("policy", help="a policy directory with policy.npz (omniRL scripts/export_policy.py)")
    ap.add_argument("--contract", required=True, help="the contract JSON the policy was trained on")
    ap.add_argument("--mount", required=True, help="imu_mount.json (sensor -> body), or a mount name")
    ap.add_argument("--robot", help="sku1 or sku2: the robot it was trained for (default: the policy.npz "
                                    "metadata, written by omniRL's export since MP3_ROBOT)")
    ap.add_argument("--name", help="default: the policy directory's name")
    ap.add_argument("--fixtures", type=int, default=32)
    ap.add_argument("--reference", help="a climb policy's reference .npz (make_climb_reference.py; default: the run's "
                                        "config.json task.options.reference, or omniRL mp3_walk_policy/data/<its name>)")
    ap.add_argument("--residual-deg", type=float, help="a climb policy's correction range (default: the run's config)")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    pdir = Path(a.policy)
    d = np.load(pdir / "policy.npz", allow_pickle=False)
    meta = json.loads(str(d["meta"]))
    # the firmware knows the robots sku1 and sku2 (APP_POLICY_ROBOT); RLmp4's sku1v2 (SKU1's body, 390 g) is an sku1
    family = {"sku1v2": "sku1"}
    trained = meta.get("robot")
    robot = a.robot or trained
    if not robot:
        raise SystemExit("policy.npz does not say which robot it was trained for: give --robot sku1 or sku2")
    if a.robot and trained and family.get(a.robot, a.robot) != family.get(trained, trained):
        raise SystemExit(f"--robot {a.robot}, but the policy was trained for {trained}")
    robot = family.get(robot, robot)
    n = int(d["n_layers"])
    layers = [(d[f"W{i}"].astype(np.float32), d[f"b{i}"].astype(np.float32)) for i in range(n)]
    dims = [layers[0][0].shape[1]] + [w.shape[0] for w, _ in layers]
    if n > MAX_LAYERS:
        raise SystemExit(f"{n} layers: at most {MAX_LAYERS}")
    history = int(meta.get("history", 0) or 0)
    if not 0 <= history <= HISTORY_MAX:
        raise SystemExit(f"an observation history of {history}: the firmware takes at most {HISTORY_MAX}")
    if meta.get("obs_terms_extra"):
        raise SystemExit(f"observation terms {meta['obs_terms_extra']}: not implemented in the firmware")
    if dims[0] != 45 * max(history, 1) or dims[-1] != 12:
        raise SystemExit(f"network {dims}: the contract needs 45 inputs (x the history {history}) and 12 outputs")
    if meta.get("activation", "elu") != "elu":
        raise SystemExit(f"activation {meta.get('activation')}: only ELU is implemented")

    contract = json.loads(Path(a.contract).read_text())
    if Path(a.mount).exists():
        mount_name = json.loads(Path(a.mount).read_text())["mount"]
    else:
        mount_name = a.mount
    if mount_name not in IMU_MOUNTS:
        raise SystemExit(f"unknown IMU mount {mount_name!r}")
    c = {"signs": [float(v) for v in contract["signs"]],
         "offsets_deg": [float(v) for v in contract["offsets_deg"]],
         "stand_dd": contract["stand_dd"],
         "scale": [float(v) for v in d["scale"]]}

    k = max(0, min(a.fixtures, d["fixture_obs"].shape[0]))
    fobs = d["fixture_obs"][:k].astype(np.float32)
    fact = d["fixture_act"][:k].astype(np.float32)
    flags = FLAG_OMNI if meta.get("omni", False) else 0
    name = a.name or pdir.name
    kind = meta.get("kind", "walk")
    extra = {}
    climb_block = b""
    dec_block = b""
    recovery = any(k_ in meta for k_ in ("bounds", "action_filter_alpha", "stand_offset_abd_deg", "getup_phase_s"))
    if recovery:
        flags, extra, dec_block = recovery_policy(meta, d["scale"], contract, Path(a.contract))
    elif kind == "getup":
        # Its own range about the stand; no tilt stop (it starts lying down: the firmware ignores the tilt in
        # GETUP, and the walk's limit decides when a walk has fallen); no command.
        band = meta.get("band_deg")
        if not band or len(band) != 12 or not all(0.0 < float(v) <= 150.0 for v in band):
            raise SystemExit(f"a get-up policy needs its band_deg (12 values, 0-150): {band}")
        flags |= FLAG_GETUP
        extra = dict(band=[float(v) for v in band], tilt_limit=180.0, cmd_max=(0.0, 0.0, 0.0))
        if meta.get("phase_T_s"):
            T, up = float(meta["phase_T_s"]), float(meta.get("phase_up", 1.0))
            if not (0.5 <= T <= 20.0 and 0.0 <= up <= 1.0):
                raise SystemExit(f"a tracked get-up's phase: length {T} s, upright at {up}")
            flags |= FLAG_GETUP_PHASE
            extra["cmd_max"] = (T, up, 0.0)
    elif kind == "climb":
        # A residual climb (climb_track.py): its own band, the phase in the command inputs, and the reference
        # it corrects, carried in the file
        band = meta.get("band_deg")
        if not band or len(band) != 12 or not all(0.0 < float(v) <= 150.0 for v in band):
            raise SystemExit(f"a climb policy needs its band_deg (12 values, 0-150): {band}")
        T = float(meta.get("phase_T_s") or 0.0)
        if not 1.0 <= T <= 60.0:
            raise SystemExit(f"a climb policy's phase length: {T} s")
        opts = {}
        if (pdir / "config.json").exists():
            opts = json.loads((pdir / "config.json").read_text())["task"]["options"]
        res_deg = a.residual_deg if a.residual_deg is not None else float(opts.get("residual_deg", 0.0))
        if not 0.0 < res_deg <= 45.0:
            raise SystemExit(f"a climb policy's residual range: {res_deg} deg (--residual-deg)")
        ref_path = Path(a.reference) if a.reference else None
        if ref_path is None and opts.get("reference"):
            cand = [Path(opts["reference"])] + [Path(__file__).parent / "../../../omniRL/mp3_walk_policy/data" /
                                                Path(opts["reference"].replace("\\", "/")).name]
            ref_path = next((c for c in cand if c.exists()), None)
        if ref_path is None or not ref_path.exists():
            raise SystemExit(f"the climb's reference .npz: give --reference (the run's: {opts.get('reference')})")
        z = np.load(ref_path)
        cmd = np.asarray(z["cmd"], dtype=np.float32)
        if abs((cmd.shape[0] - 1) * float(z["dt"]) - T) > 1e-3 or abs(float(z["dt"]) - CTRL_DT) > 1e-6:
            raise SystemExit(f"the reference ({cmd.shape[0]} rows of {float(z['dt'])} s) does not match the "
                             f"policy's phase length {T} s")
        scale = np.asarray(d["scale"], dtype=np.float32)
        ref_units = np.clip(cmd / scale, -1.0, 1.0).astype(np.float32)          # teacher_action
        res_units = (np.full(12, np.radians(res_deg), dtype=np.float32) / scale).astype(np.float32)
        flags |= FLAG_CLIMB
        extra = dict(band=[float(v) for v in band], tilt_limit=CLIMB_TILT_LIMIT_DEG, cmd_max=(T, 0.0, res_deg))
        climb_block = (struct.pack("<II", cmd.shape[0], 0) + np.ascontiguousarray(res_units, dtype="<f4").tobytes()
                       + np.ascontiguousarray(ref_units, dtype="<f4").tobytes())
        print(f"climb: reference {ref_path.name}, {cmd.shape[0]} rows ({T:.2f} s), correction +-{res_deg:g} deg, "
              f"band {band[0]:g} deg, tilt stop {CLIMB_TILT_LIMIT_DEG:g} deg")
    elif kind != "walk":
        raise SystemExit(f"unknown policy kind {kind!r}")
    else:
        opts = {}
        if (pdir / "config.json").exists():
            opts = json.loads((pdir / "config.json").read_text()).get("task", {}).get("options", {}) or {}
        if opts.get("heading") == "loop":
            # a terrain / stairs walk: forward 0..vx_max, no sideways, the yaw rate from the heading loop
            if opts.get("heading_law", "pi") != "pi":
                raise SystemExit(f"heading law {opts.get('heading_law')!r}: the firmware's loop is the PI one")
            flags |= FLAG_HEADING
            wz = float(opts.get("heading_wz", 0.4))
            tl = min(np.degrees(float(opts.get("tilt_limit") or 1.0)), TERRAIN_TILT_LIMIT_DEG)
            extra = dict(cmd_max=(float(opts.get("vx_max", 0.12)), 0.0, wz), tilt_limit=float(tl))
            print(f"terrain walk: heading loop (PI, wz +-{wz:g} rad/s), forward up to {extra['cmd_max'][0]:g} m/s, "
                  f"tilt stop {tl:.0f} deg, history {history}")
    extra["history"] = history
    body = [header(name, robot, dims, flags, k, c, IMU_MOUNTS[mount_name], **extra)]
    for w, b in layers:
        body.append(np.ascontiguousarray(w, dtype="<f4").tobytes())
        body.append(np.ascontiguousarray(b, dtype="<f4").tobytes())
    body.append(np.ascontiguousarray(fobs, dtype="<f4").tobytes())
    body.append(np.ascontiguousarray(fact, dtype="<f4").tobytes())
    if flags & FLAG_CLIMB:
        body.append(climb_block)
    if flags & FLAG_DECODER:
        body.append(dec_block)
    blob = bytearray(b"".join(body))
    struct.pack_into("<I", blob, 8, len(blob))
    struct.pack_into("<I", blob, 12, zlib.crc32(bytes(blob[16:])) & 0xFFFFFFFF)
    Path(a.out).write_bytes(bytes(blob))
    what = ("recovery GET-UP (phase clock, decoder block)" if flags & FLAG_DECODER and flags & FLAG_GETUP
            else "recovery walk (omni, decoder block)" if flags & FLAG_DECODER
            else "CLIMB (residual on a trajopt reference)" if flags & FLAG_CLIMB
            else "terrain (heading loop)" if flags & FLAG_HEADING
            else "GET-UP (tracked motion)" if flags & FLAG_GETUP_PHASE else "GET-UP" if flags & FLAG_GETUP
            else "omni" if flags & FLAG_OMNI else "forward-only")
    print(f"{a.out}: {name} for {robot}, net {dims}, {what}, "
          f"mount {mount_name}, {k} fixtures, {len(blob)} bytes")


if __name__ == "__main__":
    main()
