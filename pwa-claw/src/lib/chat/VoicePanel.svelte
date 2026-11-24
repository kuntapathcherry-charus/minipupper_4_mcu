<script>
  import AgentActivity from "./AgentActivity.svelte";
  import ChatMessage from "./ChatMessage.svelte";
  import VoiceMicMonitor from "./VoiceMicMonitor.svelte";
  import { isNearTranscriptBottom } from "./voiceTranscript.js";

  let {
    messages = [],
    micSamples = [],
    voiceState = "idle",
    busy = false,
    error = "",
    onStart = async () => {},
    onStop = async () => {},
    onClose = () => {},
  } = $props();

  let transcriptEl = $state(null);
  let followLatest = $state(true);
  let active = $derived(!["idle", "ended", "cleared"].includes(voiceState));
  let renderItems = $derived.by(() => {
    const items = [];
    for (let i = 0; i < messages.length; i++) {
      const message = messages[i];
      if (message.role === "step") {
        const previous = items[items.length - 1];
        if (previous?.kind === "activity") {
          previous.steps.push(message);
          previous.endIndex = i;
        } else {
          items.push({ kind: "activity", key: message.id, steps: [message], endIndex: i });
        }
      } else {
        items.push({ kind: "message", key: message.id, message, endIndex: i });
      }
    }
    return items;
  });

  function activityTitle(steps) {
    const names = [...new Set(steps.map((step) => step.toolName).filter(Boolean))];
    return names.length === 1 ? `MCP · ${names[0]}` : "MCP tool calls";
  }

  function handleTranscriptScroll() {
    if (!transcriptEl) return;
    followLatest = isNearTranscriptBottom(
      transcriptEl.scrollHeight,
      transcriptEl.scrollTop,
      transcriptEl.clientHeight,
    );
  }

  function jumpToLatest() {
    followLatest = true;
    transcriptEl?.scrollTo({ top: transcriptEl.scrollHeight });
  }

  $effect(() => {
    messages.length;
    const shouldFollow = followLatest;
    if (!transcriptEl || !shouldFollow) return;
    const el = transcriptEl;
    const frame = requestAnimationFrame(() => {
      el.scrollTo({ top: el.scrollHeight });
    });
    return () => cancelAnimationFrame(frame);
  });

  const stateLabels = {
    idle: "Ready",
    starting: "Connecting to voice gateway…",
    stopping: "Stopping…",
    listening: "Listening…",
    thinking: "Pupper is thinking…",
    speaking: "Pupper is speaking…",
    ended: "Stopped",
    cleared: "Ready",
  };

  function statusText(state) {
    return stateLabels[state] || "Listening…";
  }
</script>

<div class="voice-backdrop" role="presentation" onclick={onClose}></div>
<div class="voice-panel" role="dialog" aria-modal="true" aria-label="Live voice conversation">
  <div class="voice-head">
    <div>
      <span class="voice-kicker">MCP VOICE</span>
      <h2>Live conversation</h2>
    </div>
    <button type="button" class="close" aria-label="Close live conversation" title="Close" onclick={onClose}>
      <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true">
        <path d="m6 6 12 12M18 6 6 18"/>
      </svg>
    </button>
  </div>

  <div class="status-line">
    <span class="status-dot" class:active></span>
    <span>{statusText(voiceState)}</span>
  </div>

  <VoiceMicMonitor samples={micSamples} />

  <div class="transcript-shell">
    <!-- Keyboard focus makes the long MCP history scrollable without a pointer. -->
    <!-- svelte-ignore a11y_no_noninteractive_tabindex -->
    <div
      class="transcript"
      bind:this={transcriptEl}
      role="log"
      aria-live="polite"
      aria-label="Voice conversation and MCP activity history"
      tabindex="0"
      onscroll={handleTranscriptScroll}
    >
    {#if messages.length === 0}
      <div class="empty">
        <span class="empty-mic">
          <svg width="28" height="28" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true">
            <rect x="9" y="3" width="6" height="11" rx="3"/>
            <path d="M5 11a7 7 0 0 0 14 0M12 18v3M8 21h8"/>
          </svg>
        </span>
        <h3>No speech yet</h3>
        <p>Press Start, then speak to Pupper. Recognized speech and spoken replies appear here.</p>
      </div>
    {:else}
      {#each renderItems as item (item.key)}
        {#if item.kind === "activity"}
          <AgentActivity
            steps={item.steps}
            title={activityTitle(item.steps)}
            active={voiceState === "thinking" && item.endIndex === messages.length - 1}
            autoCollapse={true}
          />
        {:else}
          {@const chatMessage = { ...item.message, role: item.message.role === "user" ? "user" : "bot" }}
          <ChatMessage msg={chatMessage} />
        {/if}
      {/each}
    {/if}
    </div>
    {#if !followLatest && messages.length > 0}
      <button type="button" class="jump-latest" onclick={jumpToLatest}>
        <span aria-hidden="true">↓</span> Latest
      </button>
    {/if}
  </div>

  {#if error}<p class="error" role="alert">{error}</p>{/if}

  <div class="voice-actions">
    {#if active}
      <button type="button" class="action stop" disabled={busy} onclick={onStop}>
        {#if busy}<span class="spinner"></span>{/if}
        Stop
      </button>
    {:else}
      <button type="button" class="action start" disabled={busy} onclick={onStart}>
        {#if busy}<span class="spinner"></span>{/if}
        Start
      </button>
    {/if}
  </div>
  <p class="privacy">Live only · this transcript is not saved</p>
</div>

<style>
  .voice-backdrop {
    position: fixed;
    inset: 0;
    z-index: 80;
    background: rgba(20, 20, 18, 0.48);
    backdrop-filter: blur(3px);
  }

  .voice-panel {
    position: fixed;
    z-index: 81;
    left: 50%;
    bottom: max(16px, var(--safe-bottom));
    transform: translateX(-50%);
    width: min(560px, calc(100vw - 24px));
    height: min(760px, calc(100dvh - 32px - var(--safe-top)));
    min-height: 0;
    box-sizing: border-box;
    display: flex;
    flex-direction: column;
    overflow: hidden;
    padding: 22px;
    border: 1px solid var(--line);
    border-radius: 28px;
    background: var(--page);
    box-shadow: 0 24px 80px rgba(0, 0, 0, 0.25);
    color: var(--ink);
  }

  .voice-head { display: flex; align-items: flex-start; justify-content: space-between; gap: 16px; flex-shrink: 0; }
  .voice-kicker { color: var(--sub); font-size: 11px; font-weight: 800; letter-spacing: 0.14em; }
  h2 { margin: 3px 0 0; font-size: 24px; line-height: 1.2; }
  .close {
    width: 40px;
    height: 40px;
    display: grid;
    place-items: center;
    flex-shrink: 0;
    border: 0;
    border-radius: 50%;
    background: var(--ground);
    color: var(--ink);
    cursor: pointer;
  }

  .status-line { display: flex; align-items: center; gap: 8px; margin: 16px 0 10px; color: var(--sub); font-size: 13px; font-weight: 700; flex-shrink: 0; }
  .status-dot { width: 8px; height: 8px; border-radius: 50%; background: #929087; }
  .status-dot.active { background: #1E7B34; box-shadow: 0 0 0 4px rgba(30, 123, 52, 0.12); }

  .transcript-shell {
    position: relative;
    flex: 1;
    min-height: 0;
    display: flex;
    border: 1px solid var(--line);
    border-radius: 18px;
    background: var(--page);
  }

  .transcript {
    flex: 1;
    width: 100%;
    min-height: 0;
    overflow-y: auto;
    overscroll-behavior: contain;
    touch-action: pan-y;
    -webkit-overflow-scrolling: touch;
    scrollbar-gutter: stable;
    scrollbar-width: thin;
    scrollbar-color: var(--hair) transparent;
    padding: 8px 4px 16px;
    display: flex;
    flex-direction: column;
    gap: 6px;
    border-radius: inherit;
    background: var(--page);
  }

  .transcript:focus-visible { outline: 3px solid var(--ink); outline-offset: -3px; }
  .transcript::-webkit-scrollbar { width: 6px; }
  .transcript::-webkit-scrollbar-thumb { border-radius: 6px; background: var(--hair); }

  .jump-latest {
    position: absolute;
    right: 12px;
    bottom: 12px;
    display: flex;
    align-items: center;
    gap: 5px;
    min-height: 36px;
    padding: 7px 12px;
    border: 1px solid var(--line);
    border-radius: 18px;
    background: var(--ink);
    color: var(--mpx-yellow);
    box-shadow: 0 4px 14px rgba(0, 0, 0, 0.2);
    font-size: 12px;
    font-weight: 800;
    cursor: pointer;
  }

  .empty { flex: 1; min-height: 180px; display: flex; flex-direction: column; align-items: center; justify-content: center; padding: 14px; text-align: center; color: var(--sub); }
  .empty-mic { width: 56px; height: 56px; display: grid; place-items: center; border-radius: 50%; background: var(--page); color: var(--ink); }
  .empty h3 { margin: 14px 0 5px; color: var(--ink); font-size: 17px; }
  .empty p { max-width: 330px; margin: 0; font-size: 13px; line-height: 1.55; }

  .error { margin: 10px 2px 0; color: var(--danger, #b3261e); font-size: 13px; font-weight: 650; flex-shrink: 0; }
  .voice-actions { display: flex; margin-top: 16px; flex-shrink: 0; }
  .action {
    min-height: 50px;
    flex: 1;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 9px;
    border: 0;
    border-radius: 16px;
    background: var(--mpx-yellow);
    color: var(--ink);
    font-size: 16px;
    font-weight: 800;
    cursor: pointer;
  }
  .action.stop { background: var(--ink); color: var(--mpx-yellow); }
  .action:disabled { opacity: 0.5; cursor: default; }
  .spinner { width: 17px; height: 17px; border: 2px solid currentColor; border-right-color: transparent; border-radius: 50%; animation: spin 0.8s linear infinite; }
  .privacy { margin: 10px 0 -4px; text-align: center; color: var(--sub); font-size: 11px; flex-shrink: 0; }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  @keyframes spin { to { transform: rotate(360deg); } }

  @media (max-height: 600px) {
    .voice-panel { padding: 14px 18px; border-radius: 22px; }
    .status-line { margin: 8px 0; }
    .voice-actions { margin-top: 8px; }
    .privacy { display: none; }
  }

  @media (min-width: 768px) {
    .voice-panel { bottom: 50%; transform: translate(-50%, 50%); }
  }
</style>
