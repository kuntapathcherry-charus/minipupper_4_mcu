/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 */

#include "voice/voice_phone_audio.h"

#include "sdkconfig.h"

#if CONFIG_APP_VOICE_PHONE_AUDIO

#include <atomic>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

#include "mpx_voice_link.h"
#include "network/chat_ws.h"

static const char *TAG = "phone_audio";

/* Two seconds of 16 kHz mono PCM. The phone streams in 60 ms chunks, so this
 * only has to absorb Wi-Fi jitter between talk_task reads; it is not a
 * recording buffer. PSRAM: internal RAM is the scarce resource. */
static constexpr size_t kRingBytes = 16000 * 2 * 2;

static std::atomic<bool> s_mic_enabled{false};
static std::atomic<bool> s_speaker_enabled{false};
static std::atomic<bool> s_streaming{false};

static RingbufHandle_t s_ring;
static uint8_t s_stage[MPX_VOICE_UPLINK_FRAME * 2];
static size_t s_stage_len;
static unsigned s_dropped;

static bool ensure_ring(void)
{
    if (s_ring) return true;
    s_ring = xRingbufferCreateWithCaps(kRingBytes, RINGBUF_TYPE_BYTEBUF,
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ring) {
        s_ring = xRingbufferCreateWithCaps(kRingBytes, RINGBUF_TYPE_BYTEBUF,
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!s_ring) {
        ESP_LOGE(TAG, "cannot allocate the %u-byte phone audio ring", (unsigned)kRingBytes);
    }
    return s_ring != nullptr;
}

static void clear_ring(void)
{
    s_stage_len = 0;
    if (!s_ring) return;
    size_t sz = 0;
    void *item;
    while ((item = xRingbufferReceiveUpTo(s_ring, &sz, 0, sizeof(s_stage))) != nullptr) {
        vRingbufferReturnItem(s_ring, item);
    }
}

void mpx_phone_audio_set_enabled(bool mic, bool speaker)
{
    if (mic) (void)ensure_ring();
    if (!mic) s_streaming.store(false);
    s_mic_enabled.store(mic);
    s_speaker_enabled.store(speaker);
    ESP_LOGI(TAG, "phone audio: mic=%d speaker=%d", mic ? 1 : 0, speaker ? 1 : 0);
}

void mpx_phone_audio_set_streaming(bool streaming)
{
    /* Only talk_task calls this, which keeps s_stage single-writer. Drop
     * everything on both edges: audio captured before/after a turn is stale. */
    s_streaming.store(false);
    clear_ring();
    s_streaming.store(streaming && s_mic_enabled.load());
}

bool mpx_phone_audio_mic_enabled(void) { return s_mic_enabled.load(); }
bool mpx_phone_audio_speaker_enabled(void) { return s_speaker_enabled.load(); }

void mpx_phone_audio_push(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples || !s_mic_enabled.load() ||
        !s_streaming.load() || !ensure_ring()) return;

    /* A WebSocket frame may end mid-sample if the browser is buggy or the
     * frame was truncated; drop the stray byte rather than shifting every
     * following sample by one. */
    size_t bytes = samples * sizeof(int16_t);
    if (bytes & 1u) bytes--;
    if (!bytes) return;

    if (xRingbufferSend(s_ring, pcm, bytes, 0) != pdTRUE) {
        if ((s_dropped++ % 50u) == 0u) {
            ESP_LOGW(TAG, "phone audio ring full -- dropped %u bytes", (unsigned)bytes);
        }
    }
}

size_t mpx_phone_audio_read(int16_t *dst, size_t samples, uint32_t timeout_ms)
{
    if (!dst || !samples || !s_ring) return 0;

    const size_t want = samples * sizeof(int16_t);
    TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start = xTaskGetTickCount();

    while (s_stage_len < want) {
        size_t got = 0;
        void *item = xRingbufferReceiveUpTo(s_ring, &got, timeout, want - s_stage_len);
        if (!item) break;
        if (got > want - s_stage_len) got = want - s_stage_len;
        std::memcpy(s_stage + s_stage_len, item, got);
        s_stage_len += got;
        vRingbufferReturnItem(s_ring, item);

        /* Only wait once per call: the caller loops at 60 ms, so re-arming
         * the timeout would make a stop request wait far too long. */
        timeout = 0;
        if (xTaskGetTickCount() - start > pdMS_TO_TICKS(1000)) break;
    }

    if (s_stage_len < want) return 0;

    std::memcpy(dst, s_stage, want);
    s_stage_len = 0;
    return samples;
}

void mpx_phone_audio_play(const int16_t *pcm, size_t samples)
{
    if (!pcm || !samples || !s_speaker_enabled.load()) return;
    network::broadcast_voice_binary_to_pwa(reinterpret_cast<const uint8_t *>(pcm),
                                           samples * sizeof(int16_t));
}

void mpx_phone_audio_stop_playback(void)
{
    network::broadcast_voice_to_pwa(R"({"type":"phone_audio","event":"stop"})");
}

#else  /* !CONFIG_APP_VOICE_PHONE_AUDIO */

/* Keep callers linkable in the default end-user profile, without allocating
 * the ring or accepting phone PCM. The UI is hidden by status_json's
 * phone_audio capability flag and chat_ws rejects its control messages. */
void mpx_phone_audio_set_enabled(bool, bool) {}
void mpx_phone_audio_set_streaming(bool) {}
bool mpx_phone_audio_mic_enabled(void) { return false; }
bool mpx_phone_audio_speaker_enabled(void) { return false; }
void mpx_phone_audio_push(const int16_t *, size_t) {}
size_t mpx_phone_audio_read(int16_t *, size_t, uint32_t) { return 0; }
void mpx_phone_audio_play(const int16_t *, size_t) {}
void mpx_phone_audio_stop_playback(void) {}

#endif  /* CONFIG_APP_VOICE_PHONE_AUDIO */
