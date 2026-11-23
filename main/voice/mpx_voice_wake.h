/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Voice input uses esp-sr's acoustic front end (AEC + VAD) for
 * echo-cancelled barge-in. WakeNet is disabled: IO21_WAKE or the PWA starts
 * each conversation, and no microphone task runs while idle. A second
 * physical IO21_WAKE press stops the active conversation and re-arms idle.
 *
 * Speech barge-in is preferred when the gateway acknowledges
 * features.barge_in: AFE-cleaned microphone PCM streams throughout the
 * reply in listen mode "barge_in", and server VAD confirms speech before
 * cancelling old playback. STT at the utterance endpoint closes the uplink.
 * Opus/TLS runs in the normal talk task, never in the DSP fetch task. The
 * reader joins AFE chunks into exact 60 ms frames (no zero-padding), and a
 * mutex serializes ring reset/drain against the producer. Ignored/empty ASR
 * returns a capture-scoped listen/rearm event with its original listen_id;
 * delayed results cannot stop a newer capture or trigger transcript UI.
 *
 * PWA Interrupt remains available with every gateway: it sends
 * {"type":"abort","reason":"user_requested"} and stops local playback
 * immediately before starting the next turn. Physical IO21_WAKE instead
 * toggles the whole conversation off when pressed during talk or reply.
 *
 * Hands-free (mpx_voice_wake_auto_start(), and the boot default under
 * CONFIG_APP_VOICE_AUTO_LISTEN) waits for IO21_WAKE/PWA, streams a turn
 * until server VAD ends it, plays the reply, and offers a follow-up.
 * The socket is opened, or reopened after the server's idle timeout, on
 * demand.
 *
 * Capture is a single hardware stream. Raw talks and AFE never read it at
 * once: the hands-free task owns every handoff. During full-duplex reply,
 * AFE remains the only I2S reader and feeds cleaned PCM through a PSRAM
 * ring to the Opus task. `wake` and `voice talk` still refuse to start over
 * each other.
 *
 * Playback runs at 16 kHz (MPX_VOICE_PLAYBACK_RATE), not the 24 kHz the
 * server encodes at, because the speaker and the microphone share one I2S
 * peripheral and it will not run the two directions at different rates --
 * and barge-in needs the microphone open during the reply.
 *
 * Four-channel capture is R,M,N,M: electrical speaker reference, two real
 * mics, and one unused slot. Default MMNR processing reorders this to
 * M,M,N,R for dual-microphone array + AEC; runtime fallback is RMNN
 * single-mic AEC, and two-channel MN hardware fallback has no AEC. AEC
 * quality, CPU and heap still require validation on the physical board.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads esp-sr's models from the "model" partition and builds the AFE with
 * WakeNet disabled. Done once at boot; does NOT open the microphone. The
 * mic stays free for `voice talk` until a reply needs echo-cancelled
 * barge-in. Idempotent. */
esp_err_t mpx_voice_wake_init(void);
/* Release AFE and the barge ring after voice_glue has stopped and
 * joined their tasks. A later init recreates them for the next voice session. */
esp_err_t mpx_voice_wake_deinit(void);

/* Opens the microphone and starts the feed/fetch tasks. Refuses with
 * ESP_ERR_INVALID_STATE if mpx_audio_capture_active() is already true
 * (something else -- normally `voice talk` -- holds the mic) or if
 * mpx_voice_wake_init() has not run yet. */
esp_err_t mpx_voice_wake_start(void);

/* Stops the feed/fetch tasks and releases the microphone. Waits for both
 * tasks to actually exit before returning, so it is safe to call
 * mpx_voice_talk_start() immediately afterwards. Safe to call when not
 * running. */
esp_err_t mpx_voice_wake_stop(void);

bool mpx_voice_wake_active(void);

/* Priority for the two front-end (DSP) tasks -- the ones that run esp-sr's
 * AEC/VAD on the audio path during replies. Applies at once to tasks that
 * are up, and to the next mpx_voice_wake_start()
 * otherwise; 1..24, anything else is ignored. The hands-free orchestrator
 * ("wake_auto") keeps its own priority: it only waits, and starving it would
 * make the loop late to hand the microphone over. */
void mpx_voice_wake_set_task_priority(int prio);

/* What that setter last accepted (the priority the tasks start with). */
int mpx_voice_wake_task_priority(void);

/* Hands-free conversation loop, described in the file header. Runs
 * mpx_voice_wake_init() itself if needed. Idempotent. Needs `voice
 * provision` to have been run once on this device; it reports and returns
 * to idle rather than failing if it has not. */
esp_err_t mpx_voice_wake_auto_start(void);

/* Leave hands-free mode: cuts short any talk in progress, stops the auto
 * task, and releases the microphone. Safe to call when not running. */
esp_err_t mpx_voice_wake_auto_stop(void);

bool mpx_voice_wake_auto_active(void);

/* Software trigger for the PWA Start/Interrupt control. The auto loop owns
 * the microphone handoff, so this only posts a trigger; it barges into a
 * playing reply rather than ending the session. ESP_ERR_INVALID_STATE when
 * hands-free is off. */
esp_err_t mpx_voice_wake_trigger(void);

/* True while the hands-free loop is in a conversation turn: after an
 * IO21/PWA trigger, while the microphone streams to the server, and while
 * the reply arrives and plays. False while idle. */
bool mpx_voice_wake_conversing(void);

/* Asked when a reply has finished: return true to skip the follow-up turn
 * and go straight back to idle (cap_music: a song is waiting for the
 * speaker). NULL = always offer the follow-up. */
typedef bool (*mpx_voice_wake_gate_fn)(void);
void mpx_voice_wake_set_followup_gate(mpx_voice_wake_gate_fn skip_followup);

/* What the front end last saw, for /v1/voice (mpv2-claw's `wake info`):
 * input level in dBFS, VAD state (1 = speech), ring buffer free fraction.
 * Zeros until the first fetch. */
void mpx_voice_wake_last_seen(float *volume_dbfs, int *vad, float *ringbuf_free);

/* The hands-free loop as a word for status replies: "off", "ready",
 * "talking", "replying". */
const char *mpx_voice_wake_state_name(void);

/* mpx-dog has no serial console: mpv2-claw's `wake` command
 * (register_wake_command, with its `wake levels` channel probe) is not
 * here. The 4-channel layout it measured is baked into WAKE_FORMAT_4CH. */

#ifdef __cplusplus
}
#endif
