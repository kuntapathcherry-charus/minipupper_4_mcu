<script>
  import { colors } from "./colors.js";
  import ActionIcon from "./ActionIcon.svelte";

  let {
    assignments = {},
    onSave,
    onBack,
    customActions = [],
    onAddAction,
    onDeleteAction,
  } = $props();

  const buttonPositions = [
    { id: 'action-top',    label: 'Top',    icon: 'triangle' },
    { id: 'action-right',  label: 'Right',  icon: 'circle' },
    { id: 'action-bottom', label: 'Bottom', icon: 'pentagon' },
    { id: 'action-left',   label: 'Left',   icon: 'square' },
  ];

  // ── Built-in actions with emoji + category ───────────────
  const builtinActions = [
    // ── Movement gaits ──
    { id: "scratch", label: "Scratch", emoji: "🐕", color: "#E08A2E", category: "gait" },
    { id: "look", label: "Look Around", emoji: "👀", color: "#E08A2E", category: "gait" },
    { id: "cower", label: "Scared", emoji: "😨", color: "#E08A2E", category: "gait" },
    { id: "paw", label: "Give Paw", emoji: "🤝", color: "#E08A2E", category: "gait" },
    { id: "surrender", label: "Hands Up", emoji: "🙆", color: "#C44569", category: "gait" },
    { id: "pushup", label: "Push-ups", emoji: "💪", color: "#C44569", category: "gait" },
    { id: "spin90", label: "Spin 90°", emoji: "🔃", color: "#D97A29", category: "gait" },
    { id: "jump", label: "Jump", emoji: "🤸", color: "#D97A29", category: "gait" },
    { id: "dig", label: "Dig", emoji: "⛏️", color: "#E08A2E", category: "gait" },
    { id: "backflip_k7_405_l2", label: "Backflip without screen", emoji: "🤸", color: "#117a65", category: "gait" },
    { id: "backflip_k7_405_l2d3", label: "Backflip safe with screen", emoji: "🤸", color: "#D14949", category: "gait" },
  ];

  // ── Merge built-in + custom actions ──────────────────────
  let allActions = $derived([...builtinActions, ...customActions]);

  // ── State ────────────────────────────────────────────────
  let selectedPosition = $state(buttonPositions[0].id);
  let searchQuery = $state("");
  let showGaitSection = $state(true);

  // Clone assignments for local editing — auto-saves on each change
  let localAssignments = $state({ ...assignments });

  // ── Derived ──────────────────────────────────────────────
  let selectedPos = $derived(
    buttonPositions.find(p => p.id === selectedPosition) ?? buttonPositions[0]
  );

  let filteredActions = $derived.by(() => {
    if (!searchQuery) return allActions;
    const q = searchQuery.toLowerCase();
    return allActions.filter(a =>
      a.label.toLowerCase().includes(q) ||
      a.emoji.includes(q)
    );
  });

  let gaitActions = $derived(filteredActions.filter(a => a.category === "gait"));
  let skillActions = $derived(filteredActions.filter(a => a.category !== "gait"));

  function getAssignedAction(posId) {
    const actionId = localAssignments[posId];
    if (!actionId) return null;
    return allActions.find(a => a.id === actionId) ?? null;
  }

  function selectPosition(posId) {
    selectedPosition = posId;
    searchQuery = "";
  }

  function assignAction(actionId) {
    const updated = { ...localAssignments, [selectedPosition]: actionId };
    localAssignments = updated;
    onSave?.(updated);
  }

  function clearAssignment(posId) {
    const next = { ...localAssignments };
    delete next[posId];
    localAssignments = next;
    onSave?.(next);
  }
</script>

{#snippet shape(icon)}
  <svg viewBox="0 0 24 24" width="22" height="22" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linejoin="round" aria-hidden="true">
    {#if icon === "triangle"}<path d="M12 4l9 16H3z"/>
    {:else if icon === "circle"}<circle cx="12" cy="12" r="8"/>
    {:else if icon === "square"}<rect x="4.5" y="4.5" width="15" height="15" rx="1"/>
    {:else}<path d="M12 3l9 6.5-3.4 10.5H6.4L3 9.5z"/>{/if}
  </svg>
{/snippet}

{#snippet tile(action, isCustom)}
  {@const isAssigned = localAssignments[selectedPosition] === action.id}
  <li class="tile">
    <button type="button" class="run" class:on={isAssigned} aria-pressed={isAssigned} onclick={() => assignAction(action.id)}>
      <span class="ico"><ActionIcon id={action.id} emoji={action.emoji} /></span>
      <span class="lbl">{action.label}</span>
    </button>
    {#if isAssigned}
      <span class="check" aria-hidden="true">
        <svg width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="3.4" stroke-linecap="round" stroke-linejoin="round"><path d="M5 13l4 4L19 7"/></svg>
      </span>
    {/if}
    {#if isCustom}
      <button type="button" class="del" onclick={() => onDeleteAction?.(action.id)} aria-label="Delete {action.label}">
        <svg width="13" height="13" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M6 6l12 12M18 6L6 18"/></svg>
      </button>
    {/if}
  </li>
{/snippet}

<div class="page">
  <header class="bar">
    <button type="button" class="round" onclick={onBack} aria-label="Back">
      <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M15 6l-6 6 6 6"/></svg>
    </button>
    <h1>Controller buttons</h1>
    <span class="spacer"></span>
  </header>

  <div class="slots-wrap">
    <div class="slots" role="tablist" aria-label="Controller button">
      {#each buttonPositions as pos}
        {@const assigned = getAssignedAction(pos.id)}
        <button
          type="button"
          role="tab"
          class="slot"
          class:on={selectedPosition === pos.id}
          aria-selected={selectedPosition === pos.id}
          aria-label="{pos.label} button: {assigned ? assigned.label : 'not set'}"
          onclick={() => selectPosition(pos.id)}
        >
          {@render shape(pos.icon)}
          <span class="slot-lbl" class:unset={!assigned}>{assigned ? assigned.label : "Not set"}</span>
        </button>
      {/each}
    </div>
  </div>

  <div class="scroll">
    <div class="inner">
      <label class="search">
        <svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><circle cx="11" cy="11" r="7"/><path d="M20 20l-4-4"/></svg>
        <span class="sr">Search moves</span>
        <input type="search" bind:value={searchQuery} placeholder="Search moves" />
      </label>

      <div class="hint-row">
        <p class="hint">Tap a move to put it on the <b>{selectedPos.label.toLowerCase()}</b> button.</p>
        {#if getAssignedAction(selectedPosition)}
          <button type="button" class="link" onclick={() => clearAssignment(selectedPosition)}>Clear</button>
        {/if}
      </div>

      {#if skillActions.length > 0}
        <h2>Your moves</h2>
        <ul class="grid">
          {#each skillActions as action (action.id)}
            {@render tile(action, action.category === "skill" || action.category === "custom")}
          {/each}
        </ul>
      {/if}

      {#if gaitActions.length > 0}
        <button type="button" class="sec-head" onclick={() => (showGaitSection = !showGaitSection)} aria-expanded={showGaitSection}>
          <h2>Built in</h2>
          <svg class="chev" class:down={showGaitSection} width="16" height="16" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M9 6l6 6-6 6"/></svg>
        </button>
        {#if showGaitSection}
          <ul class="grid">
            {#each gaitActions as action (action.id)}
              {@render tile(action, false)}
            {/each}
          </ul>
        {/if}
      {/if}

      {#if filteredActions.length === 0}
        <p class="hint center">No move called “{searchQuery}”.</p>
      {/if}

      <button type="button" class="add" onclick={() => onAddAction?.()}>
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.4" stroke-linecap="round" aria-hidden="true"><path d="M12 5v14M5 12h14"/></svg>
        <span>Add a move from a file</span>
      </button>
    </div>
  </div>
</div>

<style>
  .page { position: absolute; inset: 0; z-index: 50; display: flex; flex-direction: column; background: var(--page); color: var(--ink); }
  .bar {
    flex-shrink: 0; height: calc(64px + var(--safe-top)); padding: calc(10px + var(--safe-top)) 16px 0;
    display: flex; align-items: center; justify-content: space-between;
    max-width: var(--page-max); width: 100%; margin: 0 auto;
  }
  .bar h1 { font-size: 18px; font-weight: 800; }
  .spacer { width: 44px; }
  .round {
    width: 44px; height: 44px; border-radius: 22px; border: none; background: var(--ground); color: var(--ink);
    display: grid; place-items: center; cursor: pointer;
  }

  .slots-wrap { flex-shrink: 0; width: 100%; max-width: var(--page-max); margin: 0 auto; padding: 8px 16px 6px; }
  .slots { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 8px; }
  .slot {
    height: 72px; border-radius: 18px; border: 1.5px solid #E2DDD1; background: #fff; color: var(--ink);
    display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 4px; padding: 0 6px;
    cursor: pointer; transition: background var(--dur-fast), color var(--dur-fast), border-color var(--dur-fast);
  }
  .slot.on { background: var(--ink); border-color: var(--ink); color: var(--mpx-yellow); }
  .slot-lbl { font-size: 12px; font-weight: 700; max-width: 100%; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .slot.on .slot-lbl { color: #fff; }
  .slot-lbl.unset { color: var(--sub); font-weight: 600; }
  .slot.on .slot-lbl.unset { color: rgba(255, 255, 255, 0.6); }

  .scroll { flex: 1; min-height: 0; overflow-y: auto; }
  .inner { max-width: var(--page-max); margin: 0 auto; padding: 8px 16px calc(24px + var(--safe-bottom)); display: flex; flex-direction: column; gap: 10px; }

  .search { display: flex; align-items: center; gap: 8px; height: 46px; padding: 0 14px; border-radius: 23px; background: var(--ground); color: var(--sub); }
  .search input { flex: 1; min-width: 0; border: none; background: transparent; font-size: 16px; color: var(--ink); outline: none; }
  .search input::placeholder { color: var(--sub); }
  .search:focus-within { outline: 3px solid var(--ink); outline-offset: 2px; }

  .hint-row { display: flex; align-items: center; gap: 8px; min-height: 32px; }
  .hint { flex: 1; font-size: 13px; color: var(--sub); padding: 0 4px; }
  .hint b { color: var(--ink); }
  .hint.center { text-align: center; padding: 12px 4px; }
  .link { height: 32px; border: none; background: var(--ground); border-radius: 16px; padding: 0 12px; color: var(--ink); font-size: 13px; font-weight: 700; cursor: pointer; }

  h2 { margin: 8px 4px 0; font-size: 12px; font-weight: 800; letter-spacing: 0.12em; text-transform: uppercase; color: var(--sub); }
  .sec-head { display: flex; align-items: center; justify-content: space-between; border: none; background: none; padding: 0 4px 0 0; cursor: pointer; }
  .sec-head h2 { margin-top: 8px; }
  .chev { color: #A39F96; margin-top: 8px; transition: transform var(--dur-fast); }
  .chev.down { transform: rotate(90deg); }

  .grid { list-style: none; display: grid; grid-template-columns: repeat(auto-fill, minmax(96px, 1fr)); gap: 8px; }
  .tile { position: relative; }
  .run {
    width: 100%; min-height: 96px; border: 1px solid var(--line); border-radius: 18px; background: #fff; color: var(--ink);
    display: flex; flex-direction: column; align-items: center; justify-content: center; gap: 6px;
    padding: 12px 6px; cursor: pointer; transition: transform var(--dur-fast), background var(--dur-fast);
  }
  .run:active { transform: scale(0.97); }
  .run.on { background: var(--mpx-yellow); border-color: var(--mpx-yellow); }
  .ico { font-size: 24px; line-height: 1; }
  .lbl { font-size: 13px; font-weight: 700; text-align: center; line-height: 1.2; }
  .check {
    position: absolute; top: 6px; right: 6px; width: 22px; height: 22px; border-radius: 11px;
    background: var(--ink); color: var(--mpx-yellow); display: grid; place-items: center; pointer-events: none;
  }
  .del {
    position: absolute; top: 4px; left: 4px; width: 32px; height: 32px; border-radius: 16px; border: none;
    background: transparent; color: #B4402F; display: grid; place-items: center; cursor: pointer;
  }

  .add {
    margin-top: 6px; height: 56px; border-radius: 18px; border: 1.5px dashed #CFC9BB; background: #fff; color: var(--ink);
    display: flex; align-items: center; justify-content: center; gap: 8px; font-size: 15px; font-weight: 700; cursor: pointer;
  }
  button:focus-visible { outline: 3px solid var(--ink); outline-offset: 2px; }
  .sr { position: absolute; width: 1px; height: 1px; overflow: hidden; clip: rect(0 0 0 0); white-space: nowrap; }
</style>
