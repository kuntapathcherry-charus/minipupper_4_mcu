# tools/policy -- RL walk policies for the robot itself

`main/rl` runs a walk policy on the ESP32-S3 (the app's RL Walk page,
`/v1/policy`). These tools get a trained policy into it and prove the robot
computes what training expects.

## Export a policy

From an omniRL policy directory (`policy.npz`, written by omniRL
`scripts/export_policy.py`):

```bash
python tools/policy/export_mpxp.py <omniRL>/policies/bam_t5ub06_stops_v1 \
    --contract <omniRL>/mp3_walk_policy/data/sku2_contract.json \
    --mount <omniRL>/mp3_walk_policy/data/imu_mount.json \
    --robot sku2 --out main/rl/policies/default.mpxp
```

`--robot` names the robot the contract describes. The firmware compares it
with its own (`APP_POLICY_ROBOT`, default `sku1`) and does not run a policy
for the other robot unless the app asks with "force".

`main/rl/policies/default.mpxp` is built into the firmware; replace it and
rebuild. A `main/rl/policies/getup.mpxp` next to it is built in too, as the
get-up policy (omniRL `docs/GETUP.md`).

## Check the arithmetic on a PC

`host_test/` compiles the robot's `policy_core.c` on the PC and runs it next to
omniRL's own numpy (`contract.py`, `export.py`) on 5 x 300 random steps:
observation, network and the servo targets it sends.

```bash
cd tools/policy/host_test
make OMNIRL=/path/to/omniRL POLICY=bam_t5ub06_stops_v1
```

Expected: `max |dd diff| 0` (the same servo targets every step) and errors of
about 1e-6 on the observation and the network.

## A climb policy (onto a box)

omniRL's climb (`climb_track.py --residual-deg`, the climb7 residual RL: a small correction the network adds to
trajopt's climb7_fast) exports like a walk, plus the reference it corrects, which goes into the file:

```bash
python scripts/export_policy.py runs/climb/<run>/model_4999.pt policies/climb7      # in omniRL
python tools/policy/export_mpxp.py <omniRL>/policies/climb7 \
    --contract <omniRL>/mp3_walk_policy/data/sku2_contract.json \
    --mount <omniRL>/mp3_walk_policy/data/imu_mount.json \
    --reference <omniRL>/mp3_walk_policy/data/climb7_fast_sku1_lidar.npz \
    --out main/rl/policies/climb.mpxp
```

`main/rl/policies/climb.mpxp` is built in when it is there: the RL Walk page's **Climb** (`/v1/policy`
`{"action":"climb"}`). The robot stands (2 s), holds 1 s, climbs (the motion, phase 0..1 in the command inputs),
holds 3 s on top and stops. Each step: `pol_climb_action()` = the reference's next servo command + the network's
correction (at most the run's `residual_deg`), then the walk's decoder. Its own tilt stop (55 deg) and the get-up's
current guard.

## Terrain and stairs walks (history, heading loop)

RLmp4's terrain and stairs walks (`train_terrain.py --history 10 --heading loop --heading-law pi`) take the last
10 observations (450 inputs, stacked per term as `contract.py HistoryStacker`) and were trained with their yaw-rate
command from a heading loop. `export_mpxp.py` writes the history into the header and sets `FLAG_HEADING` from the
policy's `config.json`; the firmware stacks the observations (`pol_hist_stack`) and closes the same PI loop
(`pol_heading_*`, as `runner.HeadingLoop`): the robot holds the heading it started with, and a turn from the app steers
it to a new heading that it then holds. Forward only (0 to `vx_max`), no sideways; the tilt stop is 55 deg.

```bash
python tools/policy/export_mpxp.py <RLmp4>/policies/sku1v2_stairs_narrow_4897 \
    --contract <omniRL>/mp3_walk_policy/data/sku2_contract.json \
    --mount <omniRL>/mp3_walk_policy/data/imu_mount.json \
    --name sku1v2_stairs_4897 --out main/rl/policies/sku1v2_stairs.mpxp
```

`main/rl/policies/sku1v2_stairs.mpxp` is built in as the walk "sku1v2_stairs" (the app's "Stairs · SKU1 v2").

## mini_pupper_4_recovery: the SKU2 walker and get-up

The recovery repository's two policies (`policies/walker`, `policies/getup`; its `docs/POLICY_CONTRACT.md`) run on the
ESP32 as its Pi runner (`deploy/runner.py`) runs them, with the Pi out of the loop. Their `policy.npz` meta has the
recovery decoder (`bounds`, `action_filter_alpha`, `stand_offset_abd_deg`, `getup_phase_s`); `export_mpxp.py` sees it,
checks what the recovery runner checks (the contract's hash, the bounds' calibration and tendon coefficients, the
band) and writes a DEC1 block after the fixtures (`FLAG_DECODER`, `policy_core.h mpxp_dec_t`): the target filter, the
stance offset, the joint bounds with the knee coupling and the hip envelope, and the runner's rules (fall, tilt
filter, hand-back, phase retry, time limit, guards).

```bash
R=C:/esp6/projects/mini_pupper_4_recovery
python tools/policy/export_mpxp.py $R/policies/walker --contract $R/src/mp4_recovery/data/sku2_contract.json \
    --mount $R/src/mp4_recovery/data/imu_mount.json --robot sku2 --name sku2_recovery_walker \
    --out main/rl/policies/sku2_recovery.mpxp
python tools/policy/export_mpxp.py $R/policies/getup --contract $R/src/mp4_recovery/data/sku2_contract.json \
    --mount $R/src/mp4_recovery/data/imu_mount.json --robot sku2 --name sku2_recovery_getup \
    --out main/rl/policies/getup.mpxp
```

`sku2_recovery.mpxp` is the walk "sku2_recovery" (the app's "Walk + get-up · SKU2"), `getup.mpxp` the get-up behind
**Get up** and "never stays down". On the robot (`APP_POLICY_ROBOT` sku2):

- **Calibration:** the robot's own (the page's "Use my calibration", on by default on an sku2 robot) or the file's (the
  recovery contract's, what its Pi runner uses). With the robot's own, the firmware moves the hips' and knees' bounds to
  20 deg inside the pots' track ends for those offsets (`pol_bounds_calibrate`, as the recovery repository's
  `make_contract.py` + `export_policy --from-policy` would) and keeps the physical limits (the abductions' measured
  range, the knee coupling, the hip envelope); each start runs the decoder self-test on it and refuses a calibration
  that puts a target within 15 deg of a track end.
- **Walk:** the walker's stand (abductions 12 deg wider), omni (vx, vy from the stick), the heading loop (wz 0.4 rad/s)
  while the stick does not turn. A fall (the complementary-filter tilt over 60 deg for 0.3 s, or over 45 deg turning
  faster than 2 rad/s) goes to the get-up at the next step with "never stays down" on, else limp.
- **Get-up:** its decoder seeded from the measured pose, the phase clock from 0 over 4 s (restarted after 1 s at its end
  if still tilted over 45 deg); 25 steps at < 12 deg, < 0.8 rad/s and < 10 deg rms from the stand hand back to the
  walker. At most 10 get-ups a run, 20 s each (then limp). No current guard in the get-up (1500 mA for 0.5 s while
  walking, as the recovery firmware), the temperature guard always on (menuconfig `APP_POLICY_TEMP_STOP_C`, default
  85 degC: stop there, limp after 1 s at 90, no start at 80; 0 turns every temperature check off); any stop during a
  get-up goes limp at once. The get-up's weights run 16-bit from PSRAM (190k weights).

`host_test`: `make recovery RECOVERY=<mini_pupper_4_recovery>` exports both and compares the C decoder, tilt filter,
fall detector, the observation -> network -> targets path and the bounds on other calibrations with the recovery
repository's numpy. Expected: `0 of 144000 targets differ`, `fall detector disagreements 0`, `dd differ 0/10800`,
`own calibration ... 0 of 72000 targets differ`.
