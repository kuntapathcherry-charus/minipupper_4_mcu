export const TRANSCRIPT_FOLLOW_THRESHOLD_PX = 96;

/** Keep new voice events from pulling a reader away from older MCP details. */
export function isNearTranscriptBottom(scrollHeight, scrollTop, clientHeight) {
  if (![scrollHeight, scrollTop, clientHeight].every(Number.isFinite)) return true;
  return scrollHeight - scrollTop - clientHeight <= TRANSCRIPT_FOLLOW_THRESHOLD_PX;
}
