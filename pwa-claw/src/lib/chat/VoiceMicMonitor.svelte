<script>
  import { micLevelPercent } from "./voiceMicDebug.js";

  let { samples = [] } = $props();
  let latest = $derived(samples.length ? samples[samples.length - 1] : null);

  function formatDb(value) {
    return Number.isFinite(value) ? value.toFixed(1) : "—";
  }
</script>

{#if latest}
  <section class="mic-monitor" aria-label="Live microphone diagnostics">
    <div class="monitor-head">
      <span class="monitor-title">MIC MONITOR</span>
      <span class="source">{latest.source} · slot {latest.slot}</span>
    </div>

    <div
      class="chart"
      role="img"
      aria-label={`Recent microphone RMS, latest ${formatDb(latest.rmsDb)} dBFS`}
    >
      {#each samples as sample}
        <span
          class="bar"
          class:has-signal={sample.rmsDb >= -45}
          style={`height: ${Math.max(3, micLevelPercent(sample.rmsDb))}%`}
          title={`Frame ${sample.frame}: ${formatDb(sample.rmsDb)} dBFS`}
        ></span>
      {/each}
    </div>

    <div class="envelope" aria-label={`Current amplitude envelope ${latest.wave}`}>{latest.wave}</div>

    <div class="metrics">
      <span>RMS <strong>{formatDb(latest.rmsDb)}</strong> dBFS</span>
      <span>Peak <strong>{formatDb(latest.peakDb)}</strong> dBFS</span>
      <span class="channels">Channels <strong>{latest.levels.join(" / ")}</strong></span>
      <span class="frame">Turn {latest.listenId} · frame {latest.frame}</span>
    </div>
  </section>
{/if}

<style>
  .mic-monitor {
    flex-shrink: 0;
    margin: 0 0 10px;
    padding: 11px 12px;
    border: 1px solid var(--line);
    border-radius: 16px;
    background: var(--ground);
  }

  .monitor-head { display: flex; align-items: center; justify-content: space-between; gap: 8px; }
  .monitor-title { color: var(--sub); font-size: 10px; font-weight: 800; letter-spacing: 0.12em; }
  .source { color: var(--sub); font-size: 11px; font-weight: 650; }

  .chart {
    height: 34px;
    display: flex;
    align-items: end;
    gap: 2px;
    margin-top: 8px;
    padding: 3px 4px;
    border-radius: 7px;
    background: var(--page);
    overflow: hidden;
  }

  .bar { flex: 1; min-width: 2px; border-radius: 2px 2px 0 0; background: #929087; }
  .bar.has-signal { background: #1E7B34; }

  .envelope {
    min-height: 17px;
    margin-top: 4px;
    color: var(--ink);
    font-family: ui-monospace, SFMono-Regular, Menlo, monospace;
    font-size: 15px;
    line-height: 17px;
    letter-spacing: 0.16em;
    text-align: center;
    white-space: pre;
    overflow: hidden;
  }

  .metrics {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 3px 12px;
    margin-top: 5px;
    color: var(--sub);
    font-size: 10px;
    font-variant-numeric: tabular-nums;
  }

  .metrics strong { color: var(--ink); font-weight: 700; }
  .channels, .frame { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }

  @media (max-width: 380px) {
    .metrics { grid-template-columns: 1fr; }
  }
</style>
