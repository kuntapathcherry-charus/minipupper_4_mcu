---
{
  "name": "mpx_robot",
  "description": "Drive the MangDang quadruped: walk, turn, hold a pose, jump, backflip, and read gait, IMU and servo state.",
  "author": "MangDang",
  "metadata": {
    "cap_groups": [
      "cap_robot"
    ],
    "manage_mode": "readonly"
  }
}
---


# MangDang quadruped

A four-legged robot with twelve servos, three per leg. You can move it, pose
it, and read its state. You cannot see through it — it has no camera.

## Before moving it

Call `robot_get_state` first if you have not already this conversation. It
returns the movement names that actually exist on this robot, which is the
list to choose from — do not invent names, and do not assume a name from
another robot works here.

It also reports the hottest servo. Above about 60 °C, stop and say so rather
than continuing to move; these are hobby servos in a small robot and heat is
how they die.

## Movements

`robot_move` takes either a name or an analog drive.

**Walking.** `advance`, `back`, `left`, `right` are the simple gaits.
`stanford` is the good one — a proper trot ported from StanfordQuadruped, and
what you should reach for when someone says "walk". `turnL` and `turnR` spin
in place. `moveLF`, `moveRF`, `moveLB`, `moveRB` walk diagonally.

**Analog drive.** Instead of a name, give `forward`, `strafe` and `turn`
between -1 and 1. This is the same path the joystick in the app uses: any
input starts the trot, and it scales with the configured speed. Positive
strafe is left; positive turn is left. Use this when someone wants continuous
or proportional motion ("walk forward slowly") rather than a fixed move.

**Duration and stopping.** `duration_ms` works with either form, up to 10000.
For a continuous move it keeps the motion running for that long, then stops
it in the neutral stand (`none`) — not trotting in place. It defaults to
3000, so plain "go forward" is a complete bounded task. Always set it for a
different bounded request such as "forward for 5 seconds" (`forward: 1,
duration_ms: 5000`). Explicit `duration_ms: 0` holds until another
`robot_move` call; use that only for a deliberately sustained move. Send
`movement: "none"` to stop, or `movement: "init"` to return to the neutral
pose. One-shot moves such as jumps and backflips end by themselves. A
skill-provided movement owns the robot until the skill ends, so do not attach
a duration to it.

**Looking.** `lookup`, `lookdown`, `lookleft`, `lookright`, and the diagonals
`lookul`, `lookur`, `lookll`, `looklr`. These hold a pose rather than walking.
`robot_pose` does the same thing with exact angles.

**Body.** `stretch`, `heightup`, `heightdown`, `balance`, `roll`, `pitch`.

**Expressive.** `twerk`, `wiggle`, `wiggleL`, `wiggleR`, `buttshrug`,
`buttshrugL`, `buttshrugR`, `frontkick`, `bowback`, `bodycycle`,
`headellipse`. Reach for these when someone asks the robot to react, celebrate
or be playful — that is what they are for.

**Legs.** `flegL`, `flegR`, `blegL`, `blegR` lift one paw. Good for a wave or
a handshake.

**Jumps and flips.** `jump` and `jumpfwd` do what they say. `longjump` hops
forward; `farjump` is the longest hop. Backflips are built in: `backflip` is
the one to use by default -- it is `backflip_k7_405_l2d3`, the flip that keeps
the screen off the floor. `backflip_k7_405_l2` is the same flip without that
care for the screen; use it only when asked for the one "without screen".
Older variants (`backflip1`, `backflip_stable`, `backflip_clean`,
`backflip_stable_v0`, `backflip_clean_opt`, `backflip_stable_opt`,
`backflip_k7`, `backflip_k7h`) were tuned for a lighter robot: do not pick
them yourself. If someone asks for a backflip, do it -- it exists; do not say
the robot cannot. These moves land hard, so mention a soft floor and a charged
battery, and do not run one right after another. On a battery fuller than the
backflip limit (12.0 V, `battery` in `robot_get_state`) a backflip is
refused with the reason: tell the user it needs to walk a little first.

**Other.** `init` returns to the neutral standing pose. `step` marks time in
place. `none` stops.

Installed skills can add their own movement names; those work here too, and
`robot_get_state` does not list them — `mpx_list_skills` does. A built-in name
always wins over a skill's.

## Poses

`robot_pose` holds a body attitude with the feet planted. Roll is limited to
±25°, pitch to ±20°, yaw to ±30°, and values past that are clamped rather than
refused. Positive pitch raises the nose.

`speed_dps` controls how fast it gets there. 0 snaps instantly, which looks
mechanical; 60–120 reads as natural movement. For anything expressive, use a
speed.

## Settings

`robot_set_config` changes how walking feels and persists it to flash:

| Field | Meaning | Sensible range |
|---|---|---|
| `period` | ms per gait phase — lower is faster | 60–120 |
| `height` | body height, mm | 60–90 |
| `up_height` | how far a foot lifts, mm | 5–20 |
| `stride` | step length, mm | 5–20 |
| `tilt` | body tilt, degrees | -10–10 |
| `speed_mm_s` | trot speed | 30–120 |

Every field is optional and omitted fields keep their current value, so you
can change one thing without knowing the rest. Read the state first anyway —
it is better to say "I raised the height from 70 to 85" than to guess.

Larger `stride` and lower `period` together make the robot unstable. If it
starts stumbling, raise `period` before anything else.

## What to tell the user

Say what the robot did, not what you called. "It's doing a little dance" is
better than "I invoked robot_move with movement=twerk". If a movement was
refused because a skill is running, say that — it is a real and temporary
condition, and `mpx_stop_skill` clears it.
