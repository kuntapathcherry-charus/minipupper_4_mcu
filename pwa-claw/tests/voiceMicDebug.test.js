import test from "node:test";
import assert from "node:assert/strict";
import {
  VOICE_MIC_HISTORY_LIMIT,
  appendVoiceMicSample,
  micLevelPercent,
  normalizeVoiceMicSample,
} from "../src/lib/chat/voiceMicDebug.js";

const event = {
  type: "voice_mic",
  listen_id: 17,
  frame: 14,
  source: "robot",
  slot: 1,
  rms_db: -28.04,
  peak_db: -9.2,
  levels: [12, 1834, 3, 1601],
  wave: "....--==**@@",
};

test("normalizes microphone telemetry for the voice panel", () => {
  assert.deepEqual(normalizeVoiceMicSample(event), {
    listenId: 17,
    frame: 14,
    slot: 1,
    rmsDb: -28.04,
    peakDb: -9.2,
    levels: [12, 1834, 3, 1601],
    source: "robot",
    wave: "....--==**@@",
  });
});

test("ignores incomplete microphone telemetry", () => {
  assert.equal(normalizeVoiceMicSample(null), null);
  assert.equal(normalizeVoiceMicSample({ ...event, rms_db: "bad" }), null);
  assert.equal(normalizeVoiceMicSample({ ...event, levels: [1, 2] }), null);
});

test("keeps only the recent microphone history", () => {
  let samples = [];
  for (let frame = 1; frame <= VOICE_MIC_HISTORY_LIMIT + 5; frame++) {
    samples = appendVoiceMicSample(samples, { ...event, frame });
  }
  assert.equal(samples.length, VOICE_MIC_HISTORY_LIMIT);
  assert.equal(samples[0].frame, 6);
  assert.equal(samples.at(-1).frame, VOICE_MIC_HISTORY_LIMIT + 5);
  const unchanged = appendVoiceMicSample(samples, { type: "voice_state" });
  assert.equal(unchanged, samples);
});

test("maps RMS dBFS to bounded chart heights", () => {
  assert.equal(micLevelPercent(-70), 0);
  assert.equal(micLevelPercent(-35), 50);
  assert.equal(micLevelPercent(0), 100);
  assert.equal(micLevelPercent(-96), 0);
  assert.equal(micLevelPercent(3), 100);
  assert.equal(micLevelPercent(Number.NaN), 0);
});
