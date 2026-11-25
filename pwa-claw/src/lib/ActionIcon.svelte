<script>
  /**
   * ActionIcon now renders a simple emoji based on the action id.
   * Built-in actions have predefined emojis; custom actions pass their emoji directly.
   */
  import PupperIcon, { has as hasPupper } from "./PupperIcon.svelte";

  let { id, emoji } = $props();

  const builtinEmojis = {
    advance: "🚶",
    back: "🔙",
    left: "◀️",
    right: "▶️",
    turnL: "↺",
    turnR: "↻",
    stanford: "🐕",
    step: "🦶",
    testspeed: "⚡",
    jump: "🤸",
    jumpfwd: "🏃",
    flegR: "🦵",
    flegL: "🦵",
    blegR: "🦵",
    blegL: "🦵",
    heightup: "⬆️",
    heightdown: "⬇️",
    moveLF: "↗️",
    moveRF: "↖️",
    moveLB: "↘️",
    moveRB: "↙️",
    sit: "🪑",
    stretch: "🧘",
    twerk: "💃",
    roll: "🔄",
    pitch: "📐",
    balance: "⚖️",
    init: "🏁",
    none: "⏹️",
    frontkick: "🦶",
    wiggle: "🐕",
    wiggleL: "◀️",
    wiggleR: "▶️",
    buttshrug: "🍑",
    buttshrugL: "◀️",
    buttshrugR: "▶️",
    bowback: "🙇",
    bodycycle: "🔄",
    headellipse: "🔄",
    mate: "🐾",
    alive: "💓",
    charshow: "🎭",
    wakeup: "😴",
    look: "👀",
    bow: "🎉",
    trot: "🐾",
    chase: "🌀",
    dig: "⛏️",
    scratch: "🐕",
    shake: "💦",
    cower: "😨",
    paw: "🤝",
    tripod: "🦮",
    situp: "🙌",
    rearclap: "👏",
    surrender: "🙆",
    sixseven: "🤚",
    pushup: "💪",
    runningman: "🏃",
    moonwalk: "🌙",
    spin90: "🔃",
    longjump: "🦘",
    farjump: "🚀",
    backflip: "🤸",
    backflip_k7_405_l2: "🤸",
    backflip_k7_405_l2d3: "🤸",
  };

  const displayEmoji = $derived(emoji || builtinEmojis[id] || "⚙️");
</script>

{#if hasPupper(id)}
  <!-- the app's own moves (ids with a drawing): the Mini Pupper 4 itself in that pose, even though their catalogue
       entry carries an emoji too; custom actions have their own ids and keep their emoji -->
  <span class="action-pupper"><PupperIcon pose={id} label={id} /></span>
{:else}
  <span class="action-emoji" role="img" aria-label={id}>{displayEmoji}</span>
{/if}

<style>
  .action-pupper { display: inline-flex; align-items: center; justify-content: center; line-height: 1; }
  .action-emoji {
    font-size: 1.8rem;
    line-height: 1;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    /* Slight text-stroke for legibility on colored backgrounds */
    filter: drop-shadow(0 1px 1px rgba(0,0,0,0.15));
  }
</style>
