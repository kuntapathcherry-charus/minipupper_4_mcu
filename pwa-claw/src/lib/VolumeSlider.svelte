<script>
  /**
   * Speaker volume on the home screen (firmware: GET/POST /v1/voice "volume", 0..100).
   * The robot saves it, so it is the same level the voice tool and Settings -> AI & Voice show.
   * Hidden when the robot has no /v1/voice (voice not in the build) or no speaker.
   */
  let { online = true } = $props();

  let volume = $state(null);       // the robot's level; null = not shown
  let draft = $state(null);        // slider value while dragging
  let busy = $state(false);
  let error = $state("");
  let lastAudible = 60;            // what the mute button restores

  let shown = $derived(draft ?? volume ?? 0);

  async function load() {
    try {
      const r = await fetch("/v1/voice", { cache: "no-store" });
      if (!r.ok) { volume = null; return; }
      const d = await r.json();
      volume = d && d.speaker && typeof d.volume === "number" ? d.volume : null;
      if (volume) lastAudible = volume;
    } catch {
      /* keep what we had; the robot may be briefly unreachable */
    }
  }

  async function send(v) {
    error = "";
    busy = true;
    try {
      const r = await fetch("/v1/voice", {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ volume: v }),
      });
      const d = await r.json().catch(() => ({}));
      if (!r.ok) throw new Error(d.error || "HTTP " + r.status);
      volume = typeof d.volume === "number" ? d.volume : v;
      if (volume) lastAudible = volume;
    } catch (e) {
      error = e.message || "The robot did not take that.";
    }
    busy = false;
  }

  function commit() {
    if (draft === null) return;
    const v = draft;
    draft = null;
    send(v);
  }

  function toggleMute() {
    send(shown > 0 ? 0 : lastAudible || 60);
  }

  $effect(() => {
    if (online) load();
  });
</script>

{#if volume !== null}
  <section class="vol" aria-label="Speaker volume">
    <button type="button" class="mute" onclick={toggleMute} disabled={busy || !online}
            aria-label={shown > 0 ? "Mute the speaker" : "Unmute the speaker"}>
      <svg width="22" height="22" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2"
           stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
        <path d="M4 9v6h4l5 4V5L8 9H4z" fill="currentColor" />
        {#if shown === 0}
          <path d="M17 9l5 6M22 9l-5 6" />
        {:else}
          <path d="M16.5 8.5a5 5 0 0 1 0 7" />
          {#if shown > 50}<path d="M19 6a8.5 8.5 0 0 1 0 12" />{/if}
        {/if}
      </svg>
    </button>
    <input type="range" min="0" max="100" step="5" value={shown}
           style="--fill: {shown}%"
           disabled={!online}
           aria-label="Speaker volume"
           oninput={(e) => (draft = Number(e.currentTarget.value))}
           onchange={commit} />
    <span class="val">{shown}%</span>
  </section>
  {#if error}<p class="vol-error" role="status">{error}</p>{/if}
{/if}

<style>
  .vol {
    margin: 12px 14px 0;
    width: calc(100% - 28px);
    max-width: 432px;
    align-self: center;
    background: #fff;
    border: 1px solid var(--line);
    border-radius: 22px;
    padding: 8px 14px 8px 8px;
    display: flex; align-items: center; gap: 10px;
    flex-shrink: 0;
  }
  .mute {
    width: 40px; height: 40px; flex-shrink: 0; border-radius: 50%; border: none;
    background: var(--ground); color: var(--ink); cursor: pointer;
    display: grid; place-items: center;
  }
  .mute:disabled { opacity: 0.5; cursor: default; }
  .mute:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  input[type="range"] {
    flex: 1; min-width: 0; height: 28px; margin: 0; background: transparent;
    -webkit-appearance: none; appearance: none; cursor: pointer;
  }
  input[type="range"]::-webkit-slider-runnable-track {
    height: 8px; border-radius: 4px;
    background: linear-gradient(to right, var(--mpx-yellow) var(--fill), var(--ground) var(--fill));
  }
  input[type="range"]::-moz-range-track { height: 8px; border-radius: 4px; background: var(--ground); }
  input[type="range"]::-moz-range-progress { height: 8px; border-radius: 4px; background: var(--mpx-yellow); }
  input[type="range"]::-webkit-slider-thumb {
    -webkit-appearance: none; appearance: none; width: 24px; height: 24px; margin-top: -8px;
    border-radius: 50%; background: #fff; border: 2px solid var(--ink);
  }
  input[type="range"]::-moz-range-thumb {
    width: 20px; height: 20px; border-radius: 50%; background: #fff; border: 2px solid var(--ink);
  }
  input[type="range"]:disabled { opacity: 0.5; }
  input[type="range"]:focus-visible { outline: 3px solid var(--ink); outline-offset: 4px; border-radius: 4px; }
  .val { width: 44px; text-align: right; font-size: 14px; font-weight: 800; font-variant-numeric: tabular-nums; color: var(--ink); }
  .vol-error { margin: 4px auto 0; max-width: 432px; padding: 0 18px; font-size: 13px; color: #9B2C20; text-align: center; }
</style>
