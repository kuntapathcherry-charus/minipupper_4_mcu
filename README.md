# mpv2onlyRL: reinforcement-learning walk on the ESP32-S3

Firmware for the MangDang Mini Pupper (MP4-ESP32-CORE board) that runs walk and
get-up policies trained in simulation **on the robot's own ESP32-S3**. There is
no Raspberry Pi and no PC in the loop: the network runs at 50 Hz on the chip,
the servo loop at 200 Hz, and the phone app starts it, steers it and reads its
logs.

## What it does

| | |
|---|---|
| **RL walk** | the SKU2 recovery walker: forward, sideways and turning from the app's stick |
| **Self-recovery** | if the robot ends up on its back or side it rolls over and stands up with the SKU2 get-up policy |
| **Always gets back up** | also outside a run: after a failed backflip or a push it waits until it lies still, then gets up by itself (switch on the RL walk page) |
| **Stairs** | the SKU2 stairs walk |
| **Backflips** | scripted backflip moves, including `backflip_k7_405_s3` |
| **Controller** | stick walk with L1 / R1 turns, at the same speeds as the mpx firmware |
| **Logs** | every run's log on the RL walk page, and a per-servo log of the last get-up (`getup_last.csv`) |
| **Safety** | stops on a big tilt, on a lost app (2 s), and optionally on hot servos |

The built-in policies live in `main/rl/policies/`:

| File | Policy | Page label |
|---|---|---|
| `default.mpxp` | `sku1v2_405_walk_s2`, the SKU1 v2 walk | Walk · SKU1 v2 · 405 g |
| `sku2_recovery.mpxp` | the SKU2 walker with the recovery decoder (the walk an sku2 robot boots with) | Walk + get-up · SKU2 |
| `getup.mpxp` | the SKU2 get-up | used by Get up / Always gets back up |
| `sku2_stairs.mpxp` | `sku2_stairs_narrow_v3`, the SKU2 stairs walk | Stairs · SKU2 |

For the SKU1 v2 policies instead (trained with the SKU2 recipes in
mini_pupper_4_sku1v2_rl), set `APP_POLICY_ROBOT` to `sku1` and put
`sku1v2_recovery.mpxp` and `getup_sku1.mpxp` in that folder.

## Build and flash

You need **ESP-IDF 5.5.x** and **Node.js** (LTS) with npm, for the phone app
that is built into the firmware.

```
git clone <this repo>
cd mpv2onlyRL

# 1. the phone app (once, and again after changing pwa-claw/)
cd pwa-claw
npm ci
npm run build
cd ..

# 2. the firmware (reconfigure twice on a fresh clone)
idf.py reconfigure
idf.py reconfigure
idf.py build flash monitor
```

`sdkconfig` is not in git; a fresh clone takes its settings (target
`esp32s3`, robot `sku2`, …) from `sdkconfig.defaults`. The first `reconfigure`
creates `sdkconfig`, the second picks the components that depend on it. After
that, `idf.py build flash monitor` is enough; after changing
`main/Kconfig.projbuild` or `sdkconfig.defaults`, run `idf.py reconfigure`
once first.

## First run

1. Connect your phone to the robot's hotspot (**MPX-Dog**) or put the robot on
   your Wi-Fi, and open the app.
2. **Settings → Calibration**: check the joints. The get-up needs room on
   the front thighs and calves, so a bad calibration is the most common reason
   it fails. **Get up** once on a soft surface first; the last get-up's
   per-servo log is on the RL walk page.
3. **Settings → RL walk**: run the **Self-test** once.
4. With the robot on a stand (feet off the ground), pick a policy, a time and
   **Start**. **Stop** ends the run.
5. Turn on **Always gets back up** if you want the robot to stand up by itself
   after any fall.

## Settings (`idf.py menuconfig` → App)

| Option | Default | What it does |
|---|---|---|
| `APP_POLICY_ROBOT` | `sku2` | this robot (`sku1` or `sku2`); a policy for the same robot uses your calibration |
| `APP_POLICY_AUTO_RIGHT` | on | always get back up, also outside a run |
| `APP_POLICY_TEMP_GUARD` | off | stop the walk on hot servos |
| `APP_POLICY_TEMP_STOP_C` | 85 | that temperature, in °C |
| `APP_POLICY_TEMP_IN_GETUP` | off | check the temperature during a get-up too |
| `APP_POLICY_WEIGHT_BITS` | 16 | weights on the robot: 32 (float), 16 or 8 bits; fewer bits leave more memory |

## Adding a policy

Policies are trained outside this repo and exported to the `.mpxp` format
with `tools/policy/export_mpxp.py` (it reads the `policy.npz` and writes the
network, the observation and action layout and the joint bounds, with a CRC).
Put the file in `main/rl/policies/`, add it to `main/CMakeLists.txt`, and
rebuild. The steps, and how to check a policy's arithmetic on a PC, are in
[tools/policy/README.md](tools/policy/README.md).

Every built-in policy is copied into PSRAM with the firmware, so keep only the
ones you use: more policies leave less memory for running one.

## HTTP API

| Route | |
|---|---|
| `GET /v1/policy` | status: state, loaded policy, built-ins, get-up counts, auto get-up, memory |
| `POST /v1/policy` | `{"action": ...}`: `run`, `stop`, `getup`, `cmd`, `keepalive`, `policy`, `settings`, `test`, `bench`, `weights` |
| `GET /v1/policy/log` | the last run's log |
| `GET /v1/policy/getup_log` | the last get-up, per servo (CSV) |
| `GET /stress` | the backflip stress-test page |

Details in [docs/RL.md](docs/RL.md#http-routes).

## Repository layout

```
main/             firmware (ESP-IDF app)
  rl/             policy runtime: network, decoder, get-up, run loop
  rl/policies/    built-in .mpxp policies
  robot/          servos, calibration, gait, scripted moves (backflips)
  network/        Wi-Fi, HTTP server, policy and stress-test routes
pwa-claw/         the phone app (Svelte), served by the robot
components/       local ESP-IDF components
third-party/      vendored esp-claw components
tools/policy/     export_mpxp.py and a host test of the network
docs/             RL.md (how the RL walk works), new-board bring-up
```

## Docs

| | |
|---|---|
| [docs/RL.md](docs/RL.md) | the RL walk: what is on, build, the code, policies, a run, memory, running without Wi-Fi, HTTP routes |
| [tools/policy/README.md](tools/policy/README.md) | export a policy (npz → mpxp) and check it on a PC |
| [docs/NEW_BOARD_BRINGUP.md](docs/NEW_BOARD_BRINGUP.md) | a new board, from scratch |

## License

Apache License 2.0, see [LICENSE](LICENSE).
