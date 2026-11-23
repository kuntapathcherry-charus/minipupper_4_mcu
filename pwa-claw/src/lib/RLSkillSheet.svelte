<script>
  /**
   * One RL skill, opened from the Store or Skills: a walk policy that runs on
   * the robot itself (firmware /v1/policy, the same runtime as Settings → RL
   * walk). Pick how to run it, the speed, the weights' precision and for how
   * long, then Start / Stop. While it runs this sheet steers it (the robot's
   * dead-man stops it within 2 s if the app goes quiet); closing it stops it.
   *
   * @prop skill   an entry of RL_SKILLS (rlSkills.js)
   * @prop onClose () => void
   */
  import { untrack } from "svelte";
  import { ROBOT_NAME } from "./rlSkills.js";

  let { skill, onClose } = $props();

  let st = $state(null);            // GET /v1/policy; { available: false } without the RL walk
  let reachable = $state(true);
  let busy = $state(false);
  let msg = $state("");
  let msgBad = $state(false);

  // How to run it
  let mode = $state(untrack(() => skill.modes?.[0] || "omni"));   // "omni" | "forward"
  const FWD_SPEEDS = [
    { v: 0.05, label: "Slow" },
    { v: 0.08, label: "Normal" },
    { v: 0.12, label: "Fast" },
  ];
  let fwdSpeed = $state(0.08);
  let speedPct = $state(60);        // omni: the share of the policy's top speed the stick gives
  const DURATIONS = [
    { s: 10, label: "10 s" },
    { s: 30, label: "30 s" },
    { s: 120, label: "2 min" },
    { s: 0, label: "Until Stop" },
  ];
  let seconds = $state(30);
  const BITS = [
    { b: 32, label: "32-bit", note: "Exact" },
    { b: 16, label: "16-bit", note: "Default" },
    { b: 8, label: "8-bit", note: "Smallest" },
  ];
  let ownCal = $state(true);

  // The stick (omni): x, y, turn in -1..1; y up = forward, x / turn right = +
  let sx = $state(0), sy = $state(0), turn = $state(0);
  let padEl = $state(null);
  let dragging = false;

  const available = $derived(!!st && st.available !== false);
  const has = $derived(available && Array.isArray(st.builtins) && st.builtins.includes(skill.policy));
  const mine = $derived(has && !!st.loaded && st.builtin === skill.policy);
  const active = $derived(!!st?.active);
  const ours = $derived(active && mine);
  const max = $derived(mine && Array.isArray(st.cmd_max) ? st.cmd_max : [0.12, 0.1, 0.6]);
  const omniOk = $derived(!mine || !!st.omni);
  const otherRobot = $derived(mine && !!st.robot && !!st.this_robot && st.robot !== st.this_robot);
  const scale = $derived(speedPct / 100);
  const cmd = $derived(mode === "forward"
    ? [Math.min(fwdSpeed, max[0]), 0, 0]
    : [
        +(sy * max[0] * scale).toFixed(3),
        omniOk ? +(-sx * max[1] * scale).toFixed(3) : 0,
        omniOk ? +(-turn * max[2] * scale).toFixed(3) : 0,
      ]);
  const PHASE = {
    idle: "Ready",
    prep: "Getting ready · keep it still",
    ramp_in: "Moving to the stand",
    hold_in: "Holding the stand",
    run: "Walking",
    ramp_out: "Stopping",
    hold_out: "Stopping",
    getup: "Getting up",
    climb: "Climbing",
  };

  function say(m, bad = false) { msg = m; msgBad = bad; }

  async function call(body) {
    const r = await fetch("/v1/policy", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    const j = await r.json().catch(() => null);
    if (j && j.available !== undefined) st = j;
    return { r, j };
  }

  async function load() {
    try {
      const r = await fetch("/v1/policy", { cache: "no-store", signal: AbortSignal.timeout(6000) });
      if (r.status === 404) { st = { available: false }; return; }
      st = await r.json();
      reachable = true;
    } catch {
      reachable = false;
    }
  }

  // Open: read the robot, then load this skill's policy if the robot is idle with another one.
  async function init() {
    await load();
    if (has && !st.active && !mine) await selectPolicy();
    if (mine) ownCal = !!st.own_calibration;
  }
  $effect(() => { untrack(() => init()); });

  async function selectPolicy() {
    busy = true;
    say(`Loading ${skill.title}…`);
    try {
      const { r, j } = await call({ action: "policy", name: skill.policy });
      if (r.ok) say("");
      else say(j?.error || "Could not load the policy.", true);
    } catch {
      say("The robot did not answer.", true);
    }
    busy = false;
  }

  // Idle: refresh every 2 s. Running: the stick loop below keeps st fresh.
  $effect(() => {
    if (active) return;
    const t = setInterval(load, 2000);
    return () => clearInterval(t);
  });

  // While our run is on: the command every 250 ms (the robot's dead-man).
  let inFlight = false;
  $effect(() => {
    if (!ours) return;
    const t = setInterval(async () => {
      if (inFlight) return;
      inFlight = true;
      try {
        await call({ action: "cmd", vx: cmd[0], vy: cmd[1], wz: cmd[2] });
      } catch {
        say("Lost the robot. It stops by itself within 2 s.", true);
      }
      inFlight = false;
    }, 250);
    return () => clearInterval(t);
  });

  // "Starting…" is old news once it walks or stops.
  let lastPhase = "";
  $effect(() => {
    const ph = st?.phase || "";
    if (ph !== lastPhase && (ph === "run" || (lastPhase && ph === "idle")) && !msgBad) msg = "";
    lastPhase = ph;
  });

  async function setBits(b) {
    if (busy || active || !mine || st.weight_bits === b) return;
    busy = true;
    say(`Switching to ${b}-bit weights…`);
    try {
      const { r, j } = await call({ action: "weights", bits: b });
      if (r.ok) say(`It now runs with ${b}-bit weights.`);
      else say(j?.error || "Could not switch the weights.", true);
    } catch {
      say("The robot did not answer.", true);
    }
    busy = false;
  }

  async function start() {
    if (busy || active) return;
    if (!mine) { await selectPolicy(); if (!mine) return; }
    busy = true;
    sx = sy = turn = 0;
    try {
      const { r, j } = await call({
        action: "run", seconds, vx: 0, vy: 0, wz: 0, hold: 2, dry: false,
        // A skill runs on any robot: one trained for another model (SKU1 on an SKU2 build) is let through.
        force: otherRobot, log: false, own_calibration: ownCal,
      });
      if (r.ok) say("Starting. Keep the robot still for 1 s.");
      else say(j?.error ? `The robot said no: ${j.error}` : `The robot said no (HTTP ${r.status}).`, true);
    } catch {
      say("The robot did not answer.", true);
    }
    busy = false;
  }

  async function stop() {
    sx = sy = turn = 0;
    try {
      await call({ action: "stop" });
      say("Stopping: it ramps to the stand, then hands back to the normal gait.");
    } catch {
      say("The robot did not answer. It stops by itself within 2 s.", true);
    }
  }

  async function close() {
    if (ours) await stop();
    onClose?.();
  }

  // The stick pad
  function setFromPointer(e) {
    const r = padEl.getBoundingClientRect();
    let x = ((e.clientX - r.left) / r.width) * 2 - 1;
    let y = -(((e.clientY - r.top) / r.height) * 2 - 1);
    const m = Math.hypot(x, y);
    if (m > 1) { x /= m; y /= m; }
    sx = +x.toFixed(2);
    sy = +y.toFixed(2);
  }
  function padDown(e) {
    dragging = true;
    try { e.currentTarget.setPointerCapture(e.pointerId); } catch {}
    setFromPointer(e);
  }
  function padMove(e) { if (dragging) setFromPointer(e); }
  function padUp() { dragging = false; sx = 0; sy = 0; }

  function onKey(e) { if (e.key === "Escape") close(); }

  const kb = (n) => (n >= 1024 * 1024 ? `${(n / 1048576).toFixed(1)} MB` : `${Math.round(n / 1024)} KB`);
</script>

<svelte:window onkeydown={onKey} />

<div class="scrim" role="presentation" onclick={close}></div>
<div class="sheet" role="dialog" aria-modal="true" aria-labelledby="rl-sheet-title">
  <div class="grab" aria-hidden="true"><span></span></div>
  <div class="body">
    <header class="head">
      <span class="art" aria-hidden="true">
        <svg width="30" height="30" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" stroke-linecap="round" stroke-linejoin="round"><circle cx="6" cy="6" r="2"/><circle cx="18" cy="6" r="2"/><circle cx="12" cy="18" r="2"/><path d="M7.5 7.5l3.5 8.5M16.5 7.5L13 16M8 6h8"/></svg>
      </span>
      <div class="title">
        <span class="kicker">RL skill · runs on the robot</span>
        <h2 id="rl-sheet-title">{skill.title}</h2>
        <span class="sub">Trained for {ROBOT_NAME[skill.robot] || skill.robot} · {skill.author}</span>
      </div>
      <button type="button" class="close" onclick={close} aria-label="Close">
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>
      </button>
    </header>

    <p class="desc">{skill.description}</p>

    {#if !st}
      <p class="muted">{reachable ? "Asking the robot…" : "Can't reach the robot. Is it on and on the same Wi‑Fi?"}</p>
    {:else if !available}
      <p class="msg bad">This firmware was built without the RL walk, so RL skills can't run on it.</p>
    {:else if !has}
      <p class="msg bad">This robot's firmware doesn't include this policy ({skill.policy}). Flash a build with it to run it.</p>
    {:else}
      <div class="status" class:on={ours}>
        <span class="dot" class:walk={ours && st.phase === "run"} class:busy={active && !(ours && st.phase === "run")}></span>
        <span class="status-txt">
          <b>{active ? (ours ? PHASE[st.phase] || st.phase : "Another RL run is on") : mine ? "Ready" : "Not loaded yet"}</b>
          <span>{mine ? `${st.name} · ${st.weight_bits}-bit · ${kb(st.weights_bytes || 0)}` : "Loads when you start"}{ours && st.phase === "run" ? ` · ${cmd[0].toFixed(2)} m/s` : ""}</span>
        </span>
      </div>

      {#if !active}
        <section>
          <h3>How to run it</h3>
          <div class="seg" role="radiogroup" aria-label="How to run it">
            {#if skill.modes.includes("omni")}
              <button type="button" role="radio" aria-checked={mode === "omni"} class:on={mode === "omni"} onclick={() => (mode = "omni")}>
                <b>Steer it</b><span>Stick: forward, sideways, turn</span>
              </button>
            {/if}
            {#if skill.modes.includes("forward")}
              <button type="button" role="radio" aria-checked={mode === "forward"} class:on={mode === "forward"} onclick={() => (mode = "forward")}>
                <b>Walk forward</b><span>Straight ahead, one speed</span>
              </button>
            {/if}
          </div>
        </section>

        <section>
          <h3>Speed</h3>
          {#if mode === "forward"}
            <div class="chips" role="radiogroup" aria-label="Forward speed">
              {#each FWD_SPEEDS as f}
                <button type="button" role="radio" aria-checked={fwdSpeed === f.v} class:on={fwdSpeed === f.v} onclick={() => (fwdSpeed = f.v)}>
                  {f.label}<span>{Math.min(f.v, max[0]).toFixed(2)} m/s</span>
                </button>
              {/each}
            </div>
          {:else}
            <label class="slider">
              <span class="slider-row"><span>Top speed</span><b>{speedPct}%</b></span>
              <input type="range" min="20" max="100" step="10" bind:value={speedPct} style="--fill: {(speedPct - 20) / 0.8}%" aria-label="Top speed" />
              <span class="hint">Full stick: {(max[0] * scale).toFixed(2)} m/s forward{omniOk ? `, ${(max[1] * scale).toFixed(2)} m/s sideways, ${(max[2] * scale).toFixed(1)} rad/s turning` : ""}</span>
            </label>
          {/if}
        </section>

        <section>
          <h3>Weights</h3>
          <div class="chips" role="radiogroup" aria-label="Weight precision">
            {#each BITS as b}
              <button type="button" role="radio" aria-checked={st.weight_bits === b.b} class:on={mine && st.weight_bits === b.b} disabled={busy || !mine} onclick={() => setBits(b.b)}>
                {b.label}<span>{b.note}</span>
              </button>
            {/each}
          </div>
          <p class="hint">Fewer bits leave more memory for the robot; the walk is checked against training each time.{mine && st.weights_worst_deg !== undefined ? ` Now: ${Number(st.weights_worst_deg).toFixed(2)}° worst joint error.` : ""}</p>
        </section>

        <section>
          <h3>For how long</h3>
          <div class="chips" role="radiogroup" aria-label="Run time">
            {#each DURATIONS as d}
              <button type="button" role="radio" aria-checked={seconds === d.s} class:on={seconds === d.s} onclick={() => (seconds = d.s)}>{d.label}</button>
            {/each}
          </div>
        </section>

        <section class="group">
          <div class="row">
            <span class="row-txt">
              <b>Use my calibration</b>
              <span>{ownCal ? "Your robot's offsets (Settings → Calibration)" : "The offsets saved in the policy"}</span>
            </span>
            <button type="button" class="switch" class:on={ownCal} role="switch" aria-checked={ownCal} aria-label="Use my calibration" onclick={() => (ownCal = !ownCal)}><span class="knob"></span></button>
          </div>
        </section>

        <p class="safety">First runs: robot on a stand, feet off the ground. It stands up, holds, then walks.</p>
      {:else if ours && mode === "omni"}
        <section class="drive">
          <div
            class="pad"
            bind:this={padEl}
            role="slider"
            tabindex="0"
            aria-label="Walk direction"
            aria-valuenow={Math.round(sy * 100)}
            onpointerdown={padDown}
            onpointermove={padMove}
            onpointerup={padUp}
            onpointercancel={padUp}
          >
            <span class="knob-pad" style="transform: translate({sx * 60}px, {-sy * 60}px)"></span>
            <span class="pad-label">Drag to walk</span>
          </div>
          {#if omniOk}
            <label class="slider">
              <span class="slider-row"><span>Turn</span><b>{turn === 0 ? "—" : turn < 0 ? "left" : "right"}</b></span>
              <input type="range" min="-1" max="1" step="0.05" bind:value={turn} onpointerup={() => (turn = 0)} onkeyup={() => (turn = 0)} aria-label="Turn" class="turn" />
            </label>
          {/if}
        </section>
      {:else if ours}
        <p class="muted">Walking forward at {cmd[0].toFixed(2)} m/s{seconds ? ` for ${seconds} s` : " until you press Stop"}.</p>
      {:else}
        <p class="muted">Another RL run is on (from the RL walk page or ROS). Stop it there first.</p>
      {/if}

      {#if msg}<p class="msg" class:bad={msgBad} role="status">{msg}</p>{/if}
    {/if}
  </div>

  {#if has}
    <footer class="foot">
      {#if ours}
        <button type="button" class="go stop" onclick={stop}>Stop</button>
      {:else}
        <button type="button" class="go" disabled={busy || active} onclick={start}>
          {busy ? "…" : mode === "forward" ? `Start · walk forward ${Math.min(fwdSpeed, max[0]).toFixed(2)} m/s` : "Start · steer it"}
        </button>
      {/if}
    </footer>
  {/if}
</div>

<style>
  .scrim { position: fixed; inset: 0; z-index: 70; background: rgba(17, 17, 17, 0.38); animation: fade var(--dur-med) var(--ease-out); }
  .sheet {
    position: fixed; left: 0; right: 0; bottom: 0; z-index: 71;
    max-width: var(--page-max); margin: 0 auto;
    max-height: 92dvh;
    display: flex; flex-direction: column;
    background: #fff;
    border-radius: 28px 28px 0 0;
    box-shadow: 0 -18px 40px rgba(0, 0, 0, 0.2);
    padding-bottom: var(--safe-bottom);
    animation: rise var(--dur-slow) var(--ease-spring);
  }
  @keyframes rise { from { transform: translateY(100%); } to { transform: translateY(0); } }
  @keyframes fade { from { opacity: 0; } to { opacity: 1; } }
  .grab { height: 22px; flex-shrink: 0; display: grid; place-items: center; }
  .grab span { width: 40px; height: 5px; border-radius: 3px; background: #D9D4C7; }
  .body { flex: 1; min-height: 0; overflow-y: auto; padding: 2px 16px 12px; display: flex; flex-direction: column; gap: 16px; }

  .head { display: flex; align-items: flex-start; gap: 12px; }
  .art { width: 56px; height: 56px; flex-shrink: 0; border-radius: 16px; background: var(--ink); color: var(--mpx-yellow); display: grid; place-items: center; }
  .title { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; }
  .kicker { font-size: 11px; font-weight: 800; letter-spacing: 0.06em; text-transform: uppercase; color: #8A8578; }
  .title h2 { font-size: 21px; font-weight: 850; letter-spacing: -0.01em; line-height: 1.15; }
  .sub { font-size: 13px; color: #6F6A60; }
  .close { width: 40px; height: 40px; flex-shrink: 0; border-radius: 20px; border: none; background: var(--ground); color: var(--ink); display: grid; place-items: center; cursor: pointer; }
  .close:focus-visible, .go:focus-visible, .seg button:focus-visible, .chips button:focus-visible, .switch:focus-visible, .pad:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .desc { font-size: 14px; line-height: 1.45; color: #3F3C37; }
  .muted { font-size: 14px; color: #6F6A60; }

  .status { display: flex; align-items: center; gap: 12px; background: var(--ground); border-radius: 16px; padding: 12px 14px; }
  .status.on { background: var(--mpx-yellow-soft); }
  .dot { width: 10px; height: 10px; flex-shrink: 0; border-radius: 50%; background: #BDB8AB; }
  .dot.walk { background: var(--mpx-ok); box-shadow: 0 0 0 4px rgba(34, 197, 94, 0.2); }
  .dot.busy { background: var(--mpx-warn); }
  .status-txt { display: flex; flex-direction: column; gap: 1px; min-width: 0; font-size: 13px; color: #6F6A60; }
  .status-txt b { font-size: 15px; color: var(--ink); }

  section { display: flex; flex-direction: column; gap: 8px; }
  h3 { font-size: 13px; font-weight: 800; color: #6F6A60; letter-spacing: 0.02em; }
  .seg { display: grid; grid-auto-flow: column; grid-auto-columns: minmax(0, 1fr); gap: 8px; }
  .seg button { min-height: 60px; padding: 10px 12px; border-radius: 16px; border: 1.5px solid var(--line); background: #fff; color: var(--ink); text-align: left; display: flex; flex-direction: column; gap: 2px; cursor: pointer; }
  .seg button b { font-size: 15px; }
  .seg button span { font-size: 12px; color: #6F6A60; }
  .seg button.on { border-color: var(--ink); background: var(--ink); color: #fff; }
  .seg button.on span { color: #D8D4CA; }
  .chips { display: flex; gap: 8px; flex-wrap: wrap; }
  .chips button { min-height: 44px; padding: 6px 14px; border-radius: 14px; border: none; background: var(--ground); color: var(--ink); font-size: 14px; font-weight: 750; display: flex; flex-direction: column; align-items: center; justify-content: center; cursor: pointer; }
  .chips button span { font-size: 11px; font-weight: 600; color: #6F6A60; }
  .chips button.on { background: var(--ink); color: #fff; }
  .chips button.on span { color: #D8D4CA; }
  .chips button:disabled { opacity: 0.5; cursor: default; }
  .hint { font-size: 12px; color: #6F6A60; line-height: 1.4; }

  .slider { display: flex; flex-direction: column; gap: 6px; }
  .slider-row { display: flex; justify-content: space-between; font-size: 14px; font-weight: 700; }
  input[type="range"] { width: 100%; height: 28px; margin: 0; background: transparent; -webkit-appearance: none; appearance: none; cursor: pointer; }
  input[type="range"]::-webkit-slider-runnable-track { height: 8px; border-radius: 4px; background: linear-gradient(to right, var(--mpx-yellow) var(--fill, 50%), var(--ground) var(--fill, 50%)); }
  input[type="range"].turn::-webkit-slider-runnable-track { background: var(--ground); }
  input[type="range"]::-moz-range-track { height: 8px; border-radius: 4px; background: var(--ground); }
  input[type="range"]::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 24px; height: 24px; margin-top: -8px; border-radius: 50%; background: #fff; border: 2px solid var(--ink); }
  input[type="range"]::-moz-range-thumb { width: 20px; height: 20px; border-radius: 50%; background: #fff; border: 2px solid var(--ink); }

  .group { background: var(--ground); border-radius: 16px; padding: 4px 14px; gap: 0; }
  .row { display: flex; align-items: center; gap: 12px; padding: 10px 0; }
  .row-txt { flex: 1; min-width: 0; display: flex; flex-direction: column; gap: 2px; font-size: 12px; color: #6F6A60; }
  .row-txt b { font-size: 14px; color: var(--ink); }
  .switch { width: 50px; height: 30px; flex-shrink: 0; border-radius: 15px; border: none; background: #CFCABD; position: relative; cursor: pointer; transition: background var(--dur-fast); }
  .switch.on { background: var(--ink); }
  .knob { position: absolute; top: 3px; left: 3px; width: 24px; height: 24px; border-radius: 50%; background: #fff; transition: transform var(--dur-fast); }
  .switch.on .knob { transform: translateX(20px); }
  .safety { font-size: 12px; color: #8A6D00; background: var(--mpx-yellow-soft); border-radius: 12px; padding: 10px 12px; }

  .drive { align-items: center; gap: 14px; }
  .pad { width: 180px; height: 180px; border-radius: 50%; background: var(--ground); border: 2px solid var(--line); position: relative; display: grid; place-items: center; touch-action: none; cursor: grab; }
  .knob-pad { width: 60px; height: 60px; border-radius: 50%; background: var(--ink); box-shadow: 0 6px 14px rgba(0, 0, 0, 0.25); transition: transform 60ms linear; }
  .pad-label { position: absolute; bottom: -24px; font-size: 12px; color: #6F6A60; white-space: nowrap; }
  .drive .slider { width: 100%; margin-top: 10px; }

  .msg { font-size: 13px; padding: 10px 12px; border-radius: 12px; background: var(--ground); color: var(--ink); }
  .msg.bad { background: #FBE9E6; color: #9B2C20; }

  .foot { flex-shrink: 0; padding: 10px 16px 14px; border-top: 1px solid var(--line); }
  .go { width: 100%; height: 54px; border-radius: 27px; border: none; background: var(--mpx-yellow); color: var(--ink); font-size: 16px; font-weight: 850; cursor: pointer; }
  .go:disabled { opacity: 0.5; cursor: default; }
  .go.stop { background: #D14949; color: #fff; }
</style>
