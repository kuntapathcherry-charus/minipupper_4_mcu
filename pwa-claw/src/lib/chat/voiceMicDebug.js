export const VOICE_MIC_HISTORY_LIMIT = 48;

/** Normalize the firmware's voice_mic event and reject incomplete telemetry. */
export function normalizeVoiceMicSample(data) {
  if (!data || data.type !== "voice_mic") return null;
  const rmsDb = Number(data.rms_db);
  const peakDb = Number(data.peak_db);
  if (!Number.isFinite(rmsDb) || !Number.isFinite(peakDb)) return null;
  const levels = Array.isArray(data.levels) ? data.levels.slice(0, 4).map(Number) : [];
  if (levels.length !== 4 || levels.some((level) => !Number.isFinite(level))) return null;
  return {
    listenId: Number(data.listen_id) || 0,
    frame: Number(data.frame) || 0,
    slot: Number(data.slot) || 0,
    rmsDb,
    peakDb,
    levels,
    source: typeof data.source === "string" ? data.source.slice(0, 16) : "robot",
    wave: typeof data.wave === "string" ? data.wave.slice(0, 32) : "",
  };
}

export function appendVoiceMicSample(samples, data) {
  const sample = normalizeVoiceMicSample(data);
  if (!sample) return samples;
  return [...samples, sample].slice(-VOICE_MIC_HISTORY_LIMIT);
}

/** Map the useful -70…0 dBFS range to bar heights for the live chart. */
export function micLevelPercent(rmsDb) {
  if (!Number.isFinite(rmsDb)) return 0;
  return Math.round(Math.max(0, Math.min(100, ((rmsDb + 70) / 70) * 100)));
}
