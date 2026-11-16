/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phone audio bridge: the PWA's microphone and speaker replace the robot's
 * ES7210/MAX98357A pair for a voice turn. The phone sends raw 16 kHz mono
 * PCM16 over the chat WebSocket; this module buffers it so the existing
 * talk_task can encode and upload it exactly as it would I2S capture. TTS
 * PCM is decoded on the robot as usual and then forwarded to the phone
 * instead of the I2S speaker.
 *
 * PCM rather than Opus on the phone link on purpose: encoding Opus in the
 * browser means shipping a WASM libopus, while the robot already has an
 * encoder and this link is local Wi-Fi. 16 kHz mono PCM is 32 KB/s, which
 * the AP handles alongside the existing chat traffic.
 *
 * Kept in C so mpx_voice_stream.c can call it; the module itself is C++
 * because it forwards decoded audio through network::chat_ws.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Enable or disable each direction. PCM is accepted only while a phone-owned
 * talk task is consuming it, so idle capture cannot fill the ring. */
void mpx_phone_audio_set_enabled(bool mic, bool speaker);
void mpx_phone_audio_set_streaming(bool streaming);
bool mpx_phone_audio_mic_enabled(void);
bool mpx_phone_audio_speaker_enabled(void);

/* Queue PCM received from the phone. `samples` is per-channel (mono). */
void mpx_phone_audio_push(const int16_t *pcm, size_t samples);

/* Copy exactly `samples` mono samples into `dst`. Returns `samples` when a
 * full frame is available, 0 on timeout. Partial frames are kept internally
 * so Opus always sees a complete 60 ms frame. */
size_t mpx_phone_audio_read(int16_t *dst, size_t samples, uint32_t timeout_ms);

/* Forward decoded TTS PCM to the phone speaker. No-op when the speaker is
 * not routed to the phone. */
void mpx_phone_audio_play(const int16_t *pcm, size_t samples);

/* Ask the phone to stop playing whatever it has queued. */
void mpx_phone_audio_stop_playback(void);

#ifdef __cplusplus
}
#endif
