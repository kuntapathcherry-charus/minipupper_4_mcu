/*
 * Phone-side audio for voice chat.
 *
 * The robot's own microphone is across the room during motion testing, so the
 * phone can take over either end of a voice turn:
 *   mic     — capture 16 kHz mono PCM and stream it as binary WebSocket frames
 *   speaker — play PCM frames the robot broadcasts for TTS
 *
 * PCM crosses the link uncompressed: 16 kHz * 2 bytes = 32 KB/s, which is
 * nothing on local Wi-Fi, and it avoids shipping an Opus decoder to the phone.
 * The robot still owns VAD, STT and TTS; this only moves the two endpoints.
 */

const TARGET_RATE = 16000;
const FRAME_SAMPLES = 960; // 60 ms at 16 kHz, matches the robot's uplink frame

export class PhoneAudio {
  /**
   * @param {object} options
   * @param {(buffer: ArrayBuffer) => void} options.sendBinary
   * @param {() => void} [options.onError]
   */
  constructor({ sendBinary, onError } = {}) {
    this.sendBinary = sendBinary;
    this.onError = onError;
    this.mic = false;
    this.speaker = false;
    this.stream = null;
    this.captureCtx = null;
    this.captureNode = null;
    this.workletLoaded = null;
    this.playCtx = null;
    this.sources = [];
    this.nextPlayAt = 0;
    this.pending = new Int16Array(0);
  }

  static get supported() {
    return (
      typeof window !== "undefined" &&
      !!navigator.mediaDevices?.getUserMedia &&
      typeof AudioContext !== "undefined"
    );
  }

  async setMic(on) {
    if (on) await this._startMic();
    else this._stopMic();
    this.mic = on;
  }

  async setSpeaker(on) {
    if (on) await this._ensurePlayContext();
    else this.interrupt();
    this.speaker = on;
  }

  /** Stop queued TTS immediately, e.g. on barge-in or when the turn ends. */
  interrupt() {
    for (const source of this.sources) {
      try {
        source.stop();
      } catch {
        /* already finished */
      }
    }
    this.sources = [];
    this.nextPlayAt = 0;
  }

  async disable() {
    this.mic = false;
    this.speaker = false;
    this._stopMic();
    this.interrupt();
    if (this.playCtx) {
      try {
        await this.playCtx.close();
      } catch {
        /* already closed */
      }
      this.playCtx = null;
    }
  }

  /** Queue one decoded frame of robot TTS. */
  play(pcm16) {
    if (!this.speaker || !this.playCtx || !pcm16?.length) return;
    const ctx = this.playCtx;
    /* A stalled phone (tab backgrounded, GC pause) must resynchronise rather
     * than try to play every late frame back-to-back. */
    if (this.nextPlayAt < ctx.currentTime) this.nextPlayAt = ctx.currentTime;
    const frame = ctx.createBuffer(1, pcm16.length, TARGET_RATE);
    const channel = frame.getChannelData(0);
    for (let i = 0; i < pcm16.length; i++) channel[i] = pcm16[i] / 32768;
    const source = ctx.createBufferSource();
    source.buffer = frame;
    source.connect(ctx.destination);
    // Half a frame of lead keeps a queued burst from starting late and
    // stuttering; after that, schedule back-to-back frames.
    const startAt = Math.max(ctx.currentTime + 0.03, this.nextPlayAt);
    source.start(startAt);
    this.nextPlayAt = startAt + frame.duration;
    this.sources.push(source);
    source.onended = () => {
      this.sources = this.sources.filter((s) => s !== source);
    };
  }

  async _startMic() {
    if (this.captureCtx) return;
    if (!PhoneAudio.supported) throw new Error("This browser cannot capture audio");

    try {
      this.stream = await navigator.mediaDevices.getUserMedia({
        audio: {
          channelCount: 1,
          echoCancellation: true,
          noiseSuppression: true,
          autoGainControl: true,
        },
      });

      this.captureCtx = new AudioContext({ sampleRate: TARGET_RATE });
      await this.captureCtx.resume();
      await this._loadWorklet();

      const source = this.captureCtx.createMediaStreamSource(this.stream);
      // Worklet does the decimation and Int16 conversion off the main thread,
      // so a busy Svelte render cannot drop microphone samples.
      this.captureNode = new AudioWorkletNode(this.captureCtx, "mpx-pcm-tap", {
        numberOfInputs: 1,
        numberOfOutputs: 1,
        channelCount: 1,
      });
      this.captureNode.port.onmessage = (event) => this._onCapture(event.data);
      // An unconnected output makes Chrome free the node, so route it through
      // a silent gain to the destination.
      const mute = this.captureCtx.createGain();
      mute.gain.value = 0;
      source.connect(this.captureNode).connect(mute).connect(this.captureCtx.destination);
    } catch (error) {
      this._stopMic();
      throw error;
    }
  }

  _onCapture(pcm) {
    if (!this.mic || !pcm?.length || !this.sendBinary) return;

    // The worklet emits whatever its render quantum produced, which is rarely
    // a whole 60 ms frame. Carry the tail into the next message; dropping it
    // would thin the audio every 128 samples.
    const joined = new Int16Array(this.pending.length + pcm.length);
    joined.set(this.pending, 0);
    joined.set(pcm, this.pending.length);

    let offset = 0;
    while (offset + FRAME_SAMPLES <= joined.length) {
      const frame = joined.subarray(offset, offset + FRAME_SAMPLES);
      this.sendBinary(frame.buffer.slice(frame.byteOffset, frame.byteOffset + frame.byteLength));
      offset += FRAME_SAMPLES;
    }
    this.pending = joined.slice(offset);
  }

  async _loadWorklet() {
    if (!this.workletLoaded) {
      const url = new URL("./pcm-worklet.js", import.meta.url);
      this.workletLoaded = this.captureCtx.audioWorklet.addModule(url);
    }
    await this.workletLoaded;
  }

  _stopMic() {
    if (this.captureNode) {
      this.captureNode.port.onmessage = null;
      this.captureNode.disconnect();
      this.captureNode = null;
    }
    this.pending = new Int16Array(0);
    for (const track of this.stream?.getTracks() || []) track.stop();
    this.stream = null;
    if (this.captureCtx) {
      this.captureCtx.close().catch(() => {});
      this.captureCtx = null;
      this.workletLoaded = null;
    }
  }

  async _ensurePlayContext() {
    if (this.playCtx) {
      await this.playCtx.resume();
      return;
    }
    this.playCtx = new AudioContext({ sampleRate: TARGET_RATE });
    await this.playCtx.resume();
  }
}

/** Request the robot to use this phone's mic/speaker. */
export function phoneAudioControl({ mic, speaker }) {
  return JSON.stringify({ type: "phone_audio", mic: !!mic, speaker: !!speaker });
}
