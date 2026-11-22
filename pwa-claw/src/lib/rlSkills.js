/**
 * RL skills: walk policies trained in simulation that run on the robot's own
 * ESP32-S3 (firmware main/rl/, GET/POST /v1/policy). Unlike Web and Move
 * skills they are not downloaded: each one names a policy built into the
 * firmware (main/rl/policies/), and the Store shows whether this robot has it.
 *
 *   policy   the firmware's built-in id (GET /v1/policy "builtins")
 *   robot    the robot it was trained for ("sku1" | "sku2")
 *   modes    how it can be run: "omni" (the stick: forward, sideways, turning)
 *            and/or "forward" (one button, a fixed forward speed)
 */
export const RL_SKILLS = [
  {
    id: "rl_sku1v2_omni_walk",
    policy: "walk",
    title: "SKU1 RL Omni Walk",
    tagline: "Walks in any direction, learned in simulation",
    description:
      "A walk learned with reinforcement learning on the SKU1 v2 robot at 405 g (sku1v2_405_walk_s2). " +
      "It runs on the robot's own chip at 50 Hz: steer it forward, sideways and around with the stick, " +
      "or let it walk straight ahead at a set speed.",
    robot: "sku1",
    modes: ["omni", "forward"],
    tags: ["walk", "omni"],
    author: "MangDang",
    price: "Free",
  },
];

export const ROBOT_NAME = { sku1: "SKU1", sku2: "SKU2" };
