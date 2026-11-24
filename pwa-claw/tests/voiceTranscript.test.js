import assert from "node:assert/strict";
import test from "node:test";

import {
  isNearTranscriptBottom,
  TRANSCRIPT_FOLLOW_THRESHOLD_PX,
} from "../src/lib/chat/voiceTranscript.js";

test("voice transcript follows at the bottom and within the threshold", () => {
  assert.equal(isNearTranscriptBottom(1000, 600, 400), true);
  assert.equal(isNearTranscriptBottom(1000, 504, 400), true);
  assert.equal(TRANSCRIPT_FOLLOW_THRESHOLD_PX, 96);
});

test("voice transcript stops following when the reader scrolls into history", () => {
  assert.equal(isNearTranscriptBottom(1000, 503, 400), false);
  assert.equal(isNearTranscriptBottom(1000, 0, 400), false);
});

test("short and invalid transcripts remain safe to follow", () => {
  assert.equal(isNearTranscriptBottom(300, 0, 400), true);
  assert.equal(isNearTranscriptBottom(Number.NaN, 0, 400), true);
});
