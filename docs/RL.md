# On-device RL policies

Policies trained in simulation run on the robot's ESP32-S3 and drive the
twelve servos without a Raspberry Pi. **Settings → RL walk** starts and steers
a policy, selects the built-in networks and reads timing logs.

## Integration with the full firmware

`CONFIG_APP_POLICY_ENABLE` is on in `sdkconfig.defaults`; the normal agent,
skills, Lua, display, audio, voice and Wi-Fi features stay enabled. A policy
takes the same counted `voice_glue::hold()` as a skill before it touches the
robot: active voice work and music stop, wake-word detection is suspended,
and local agent chat and new music requests are refused. The hold is acquired
and released on the low-priority `pol_after` worker, never on the 200 Hz servo
tick. The final release restores the voice state that existed before the run.

A policy refuses to overlap a skill, ROS host or another raw-servo
controller. `robot::host::begin()` parks an ordinary gait or move when the
run starts; no separate `gait=none` command is needed. New skills are
likewise refused while a policy owns the robot. After a run, the ordinary
gait and voice service resume.

All policy choices are under `idf.py menuconfig` → *MPX Dog App
Configuration*. `APP_POLICY_ROBOT` is `sku2` in the committed defaults.
Unattended automatic get-up is deliberately **off** by default; enable
`APP_POLICY_AUTO_RIGHT` or use the page's switch only after supervised tests.

## Build

The normal `firmware-dev` build includes the PWA and embedded policies. From
the dev container with ESP-IDF 5.5.4:

```
idf.py -DIDF_TARGET=esp32s3 build
```

For a fresh checkout, install PWA dependencies with `npm ci` in `pwa-claw/`
first. The firmware build rebuilds `pwa-claw/www/` when its inputs change.
Do not use the old RL profile that compiled audio, voice and the agent out.
Flashing and physical tests are a separate, supervised step.

## The code

| | |
|---|---|
| `main/rl/policy_core.c` | the network and the observation/action contract (plain C, also built on the PC by `tools/policy/host_test`) |
| `main/rl/policy_run.cc` | a run: start, the 50 Hz loop, stops, logs, the size test |
| `main/network/http_policy.cc` | `/v1/policy` |
| `pwa-claw/src/lib/RLWalkPage.svelte` | the app's RL walk page |
| `main/rl/policies/*.mpxp` | the built-in policies |

## Policies

Built in (`main/rl/policies/`):

| File | Policy | For |
|---|---|---|
| `default.mpxp` | the basic omnidirectional walk | SKU2 |
| `sku2_recovery.mpxp` | mini_pupper_4_recovery walker, "Walk + get-up · SKU2" | SKU2 |
| `sku2_stairs.mpxp` | SKU2 stairs/terrain walk | SKU2 |
| `getup.mpxp` | mini_pupper_4_recovery get-up (`sku2_recovery_getup`) | SKU2 |

The build also detects the other optional RL asset names in
`main/CMakeLists.txt` if a matching `.mpxp` is added later; absent files are
not embedded.

The basic walk is omnidirectional: forward, sideways and turning commands
are available through **Omni (stick)**; **Walk forward** holds one forward
speed. Recovery and stairs networks carry their own decoder and command
contract in the `.mpxp` file.

At boot the robot picks the first one made for itself (`APP_POLICY_ROBOT`,
`sku1` or `sku2`, default `sku2`); the page switches between them. A policy
for the other robot needs "Run a policy for …" on the page: another body can
do anything on this one.

**Walk + get-up · SKU2** (mini_pupper_4_recovery): with "never stays down" on, it walks, gets up after a fall and
walks on, as the recovery repository's Pi runner does, on the ESP32 alone. "Use my calibration" runs it on the robot's
own offsets (the joint limits near the pots' track ends follow them); off, on the recovery contract's. The walk's
temperature guard is on (`APP_POLICY_TEMP_STOP_C`, default 85 degC; 0 = off; none during the get-up unless
`APP_POLICY_TEMP_IN_GETUP`) and there is no current guard while getting up; any stop during a get-up goes limp.
"Always gets back up" (`APP_POLICY_AUTO_RIGHT`, off by default; the page's switch until the next restart) works
outside any run: with no move playing and no host holding the robot, lying still on its back or a side (over
60 deg) for 0.3 s starts the get-up at once, which ends standing; at most 3 tries, 3 s apart, until it has been upright
for 2 s again. Details: [tools/policy/README.md](../tools/policy/README.md#mini_pupper_4_recovery-the-sku2-walker-and-get-up).

A new policy: export it from omniRL with `tools/policy/export_mpxp.py` into
`main/rl/policies/` and rebuild ([tools/policy/README.md](../tools/policy/README.md),
which also has the PC test of the arithmetic).

## A run

Servo configuration, 1 s still for the gyro bias, ramp to the stand, walk,
ramp back, and the gait takes over again. It stops by itself on a big tilt, a
silent servo board, a leg that stops following, or when the page stops
talking to it for 2 s (the stick is also the dead-man). First runs: robot on a
stand, feet off the ground.

The temperature guard (`APP_POLICY_TEMP_GUARD`) is off by default: the hottest
servo is still shown and logged. Watch it.

**The loop.** The 5 ms servo tick runs on core 1. The network has its own
task (`pol_net`, core 1, just below the tick): the tick hands it the
observation every 20 ms (50 Hz) and keeps reading the servos and the IMU while
it computes; the first tick after it is done sends its action. A network done
within one tick acts at once; a bigger one may take most of the 20 ms and acts
5 ms later for each tick it spans (train with that delay). Still busy at the
next step: a late step; two in a row stop the run.

**Weights: 32, 16 or 8 bits** (`APP_POLICY_WEIGHT_BITS`, default 16). The
file holds floats; the robot can round them to 16- or 8-bit integers (one
scale per row), so each step reads half or a quarter of the bytes. The
self-test compares the result with training's own answers and falls back to
floats if the rounding is too large (16-bit: 0.01° at worst; 8-bit: 0.4° mean,
2.3° at worst). The page's **Network** card switches it until the next boot.
The weights go to internal RAM only while `APP_POLICY_INTERNAL_RESERVE_KB`
(96 KB) stays free for Wi-Fi; otherwise PSRAM. Persistent inference scratch
and the policy/network task stacks always use PSRAM. Only the low-priority
log/audio worker keeps an internal stack because it writes LittleFS.

## How big a network fits: the size test

**RL walk → Size test → How big a network fits?** times networks of the walk's
shape from 31k weights up (random weights, nothing moves, about 10 s) and says
which fit and how much later than today's they act. Run it once more after a
walk on the stand: then it counts the real servo exchange too.

## Running without Wi-Fi

**RL walk → Without Wi-Fi** (walk forward for 10 s, 30 s or 2 min). The robot
answers the app, then switches its radio off for the whole run: no Wi-Fi
interrupts, no packets. When the run ends it:

1. prints a summary on the serial port (steps, late ticks, step period, tick,
   network and servo-exchange times, tilt, tracking, temperature, current);
2. saves the full log to flash, `/fs/rl_last_run.csv`;
3. switches Wi-Fi back on (its hotspot, and your router if one is saved).

The page counts down, knocks until the robot answers, then shows the log as
after any run, marked `WI-FI OFF`. Nothing can stop the robot from the app
during such a run: it stops by itself when the time is up, on a big tilt or if
a leg stops following. Run it on the stand first. At most 2 minutes; the radio
comes back at the latest 30 s after the run should have ended.

On the robot's own hotspot your phone may move to another network meanwhile:
join the robot's again and the page carries on. If the page was closed, open
RL walk again: **Show the last saved log** reads the copy on flash (it
survives a reboot).

The size test can run without Wi-Fi as well (**Size test → Without Wi-Fi**).
Run it once each way with the same bits to see what Wi-Fi costs the network.

## HTTP routes

| Route | |
|---|---|
| `GET /v1/policy` | state, the loaded policy, the last run's numbers |
| `POST /v1/policy` | `{"action":"run","seconds":10,"vx":0.05,…}` (`"offline":true`: Wi-Fi off for the run), `getup`, `cmd` (the stick, also the dead-man), `keepalive`, `stop`, `test` (self-test), `policy` (switch the walk), `weights` (32/16/8), `bench` (the size test) |
| `GET /v1/policy/log` | the last logged run, CSV, one line per 20 ms step |
| `GET /v1/policy/bench` | the size test's result |
