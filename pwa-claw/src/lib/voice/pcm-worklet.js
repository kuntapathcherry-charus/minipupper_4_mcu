/*
 * Captures the phone microphone on the audio thread and posts 16 kHz mono
 * Int16 blocks. Resampling lives here because AudioWorkletGlobalScope knows
 * the real context rate (Chrome may ignore our 16 kHz request), and doing it
 * per block avoids carrying a fractional read position across postMessages.
 */

const TARGET_RATE = 16000;

class PcmTap extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ratio = sampleRate / TARGET_RATE;
    this.position = 0;
    this.previous = 0;
  }

  process(inputs) {
    const channel = inputs[0] && inputs[0][0];
    if (!channel || !channel.length) return true;

    const out = [];
    let position = this.position;
    while (position < channel.length) {
      const index = Math.floor(position);
      const fraction = position - index;
      const left = index === 0 ? this.previous : channel[index - 1];
      const right = channel[index] ?? 0;
      const value = left + (right - left) * fraction;
      out.push(value >= 1 ? 32767 : value <= -1 ? -32768 : (value * 32767) | 0);
      position += this.ratio;
    }

    this.previous = channel[channel.length - 1];
    this.position = position - channel.length;

    if (out.length) {
      const pcm = Int16Array.from(out);
      this.port.postMessage(pcm, [pcm.buffer]);
    }
    return true;
  }
}

registerProcessor("mpx-pcm-tap", PcmTap);
