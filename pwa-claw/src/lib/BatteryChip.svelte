<script>
  /**
   * The battery on Home (firmware GET /v1/robot/battery, read from the AT32 servo
   * boards). Percent by default, tap for volts. Hidden while no servo board
   * reports the voltage (older AT32 firmware) or the firmware has no such route.
   */
  let { online = true } = $props();

  let bat = $state(null);          // the last reply with valid: true
  let showVolts = $state(false);

  async function load() {
    try {
      const r = await fetch("/v1/robot/battery", { cache: "no-store" });
      if (!r.ok) { bat = null; return; }
      const j = await r.json();
      bat = j && j.valid ? j : null;
    } catch { /* keep the last reading while the robot is briefly away */ }
  }

  $effect(() => {
    if (!online) return;
    load();
    const t = setInterval(load, 15000);
    return () => clearInterval(t);
  });

  const pct = $derived(bat ? Math.max(0, Math.min(100, bat.percent)) : 0);
  const low = $derived(!!bat && pct <= 20);
  const full = $derived(!!bat && bat.backflip_max_v > 0 && bat.volts > bat.backflip_max_v);
</script>

{#if bat}
  <button
    type="button"
    class="bat"
    class:low
    onclick={() => (showVolts = !showVolts)}
    title={full ? `Above ${bat.backflip_max_v.toFixed(1)} V: backflips wait until it drops` : ""}
    aria-label="Battery {bat.volts.toFixed(1)} volts, about {pct} percent{full ? ', too full for backflips' : ''}"
  >
    <svg width="22" height="14" viewBox="0 0 22 14" aria-hidden="true">
      <rect x="1" y="1" width="17" height="12" rx="3" fill="none" stroke="currentColor" stroke-width="1.8" />
      <rect x="19" y="4.5" width="2" height="5" rx="1" fill="currentColor" />
      <rect x="3" y="3" width={Math.max(1, 13 * pct / 100)} height="8" rx="1.5" fill="currentColor" class="lvl" />
    </svg>
    <span>{showVolts ? `${bat.volts.toFixed(1)} V` : `${pct}%`}</span>
  </button>
{/if}

<style>
  .bat {
    height: 36px; padding: 0 10px; border-radius: 18px; border: none;
    background: var(--ground); color: var(--ink);
    display: inline-flex; align-items: center; gap: 6px;
    font-size: 13px; font-weight: 800; font-variant-numeric: tabular-nums; cursor: pointer; white-space: nowrap;
  }
  .bat .lvl { color: #1E9E4A; }
  .bat.low { background: #FBE9E6; color: #9B2C20; }
  .bat.low .lvl { color: #D14949; }
  .bat:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
</style>
