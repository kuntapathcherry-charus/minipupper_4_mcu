/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See include/mpx_voice_wake.h for what this is, its gate, and what is
 * deliberately not wired up yet.
 *
 * Shape of it: esp-sr's AFE (Audio Front End) does AEC + VAD in one
 * pipeline over the mic/reference feed. WakeNet is deliberately disabled:
 * IO21_WAKE or the PWA starts each conversation; another physical button
 * press stops it. Two tasks, the
 * pattern esp-sr's own examples use and for the reason they use it: feed()
 * enqueues raw audio into the AFE's internal ring buffer and returns
 * quickly, fetch() blocks (its own documented 2000ms timeout) pulling
 * enhanced audio and detection results back out, and those two rates are
 * not the same thing -- combining them into one task risks whichever one
 * is momentarily behind stalling the other.
 */

#include "mpx_voice_wake.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "board_config.h"
#include "mpx_audio.h"
#include "mpx_voice_link.h"
#include "voice/voice_phone_audio.h"
#include "sdkconfig.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* From main/Kconfig.projbuild. The fallback is for a build whose sdkconfig
 * predates the symbol (ninja does not always re-run kconfgen for a new
 * Kconfig entry -- `idf.py reconfigure` does), so a stale sdkconfig costs
 * a default, not a compile error. */
#ifndef CONFIG_APP_VOICE_AUTO_MAX_SECONDS
#define CONFIG_APP_VOICE_AUTO_MAX_SECONDS 12
#endif
#ifndef CONFIG_APP_VOICE_BARGE_IN
#define CONFIG_APP_VOICE_BARGE_IN 1
#endif

static const char *TAG = "mpx_voice_wake";

/* Capture layout -- MEASURED, with `wake levels 4`, on the MP4 ESP32 CORE
 * board, 2026-09-04. The ES7210's four inputs are, per the schematic, two
 * microphones on MIC1/MIC2, the amplifier's OUTP/OUTN (the acoustic echo
 * reference) on MIC3, and nothing on MIC4. What the AFE actually receives
 * is a 4-channel capture that esp_codec_dev implements as two 32-bit I2S
 * slots, and the 16-bit samples come out of that in a different order
 * from the one board_devices.yaml lists:
 *
 *   index 0   quiet 204 / tone 4388   electrical, no room noise, x21 with
 *                                     the tone: the ECHO REFERENCE
 *   index 1   quiet 1714 / tone 505   a microphone -- the same one the
 *                                     2-channel `voice talk` capture uses
 *                                     as its channel 0 (same quiet level)
 *   index 2   quiet 202 / tone 1      the unused input
 *   index 3   quiet 2013 / tone 533   the other microphone
 *
 * Two earlier layouts were wrong in instructive ways. 2 channels as "MR"
 * handed AEC the second microphone as the reference, and it subtracted the
 * voice from itself. 4 channels as "MNRN" (the yaml order) handed the old
 * front end
 * the reference as the microphone: `wake debug` showed a flat -69 dBFS
 * while speaking, i.e. the ~200 RMS of an idle amplifier feed. Hence the
 * insistence on measuring: `wake levels` prints the table above and says
 * whether it still matches this format.
 *
 * Single microphone plus electrical reference ("RMNN") preserves the FD
 * nonlinear echo cancellation needed for barge-in without the dual-mic BSS
 * pass. FD MMNR kept the opposite core saturated until its idle watchdog
 * fired even with WakeNet disabled. Raw DMA order is R,M,N,M, already the
 * RMNN prefix AFE consumes; the unused fourth slot is ignored.
 *
 * If the codec will not open 4 channels, fall back to 2 channels as "MN":
 * button/PWA triggering still works, but with no reference there is no echo
 * cancellation, so barge-in cannot hear you over the robot's own voice. */
#define WAKE_FORMAT_4CH        "RMNN"
#define WAKE_FORMAT_2CH        "MN"

/* SR mode has linear AEC only. Its residual speaker echo is enough to trip
 * the gateway's VAD and cancel the reply that caused it. FD mode adds the
 * nonlinear suppression needed while microphone and speaker run together. */
#define WAKE_AFE_TYPE          AFE_TYPE_FD

static uint8_t     s_capture_channels;   /* 4 or 2, decided by init */
static const char *s_input_format;

/* Enhanced AFE output waiting for the Opus task. PSRAM only: 16 KiB holds
 * 500 ms of 16 kHz mono PCM without consuming scarce internal/DMA RAM. */
#define WAKE_BARGE_RING_BYTES  (16 * 1024)
static RingbufHandle_t s_barge_ring;
static SemaphoreHandle_t s_barge_lock; /* fetch send vs auto-task reset/drain */
static volatile bool s_barge_streaming;
static int16_t s_barge_staged[MPX_VOICE_UPLINK_FRAME];
static size_t s_barge_staged_count;
static uint32_t s_barge_dropped;

/* PSRAM-backed, generously sized on purpose -- see mpx_voice_stream.c's own
 * VOICE_STREAM_TASK_STACK comment for why this project no longer inches
 * stack sizes up after a guess turns out wrong. feed() and fetch() run
 * esp-sr's DSP (AEC and VAD) on whichever task calls them,
 * not on a stack of the library's own choosing.
 *
 * Reserve both stacks statically in PSRAM. MMNR consumes considerably more
 * heap than RMNN; on the board its first start left only a 24 KiB internal
 * block, and both dynamic PSRAM/internal task creation attempts failed.
 * Static storage removes that boot-time heap dependency without shrinking
 * the DSP stacks. The TCBs remain small static internal-RAM objects. */
#define WAKE_FEED_TASK_STACK   (2048 * 10)
#define WAKE_FETCH_TASK_STACK  (2048 * 16)
EXT_RAM_BSS_ATTR static StackType_t
    s_feed_stack[WAKE_FEED_TASK_STACK / sizeof(StackType_t)];
EXT_RAM_BSS_ATTR static StackType_t
    s_fetch_stack[WAKE_FETCH_TASK_STACK / sizeof(StackType_t)];
static StaticTask_t s_feed_tcb;
static StaticTask_t s_fetch_tcb;

#define WAKE_TASK_WAIT_MS      3000  /* > fetch()'s own 2000ms internal timeout */

static srmodel_list_t *s_models;
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static bool s_afe_ready;

static volatile bool s_run;
static bool s_active;
static SemaphoreHandle_t s_feed_done;
static SemaphoreHandle_t s_fetch_done;
/* The two DSP tasks, and the priority they run at. Kept so
 * mpx_voice_wake_set_task_priority() can reach a front end that is already
 * running -- a priority that only takes effect on the next start() would be
 * "restart the robot to change it", which is not what the caller wants. */
static TaskHandle_t s_feed_task;
static TaskHandle_t s_fetch_task;
static int          s_task_priority = 5;   /* what the tasks have always run at */

/* Hands-free mode -- see mpx_voice_wake_auto_start() below. */
#define WAKE_AUTO_TASK_STACK   6144   /* internal RAM: it only waits and orchestrates */
#define WAKE_AUTO_BEEP_HZ      1000
#define WAKE_AUTO_BEEP_MS      120
#define WAKE_AUTO_END_BEEP_START_HZ  660
#define WAKE_AUTO_END_BEEP_END_HZ    440
#define WAKE_AUTO_END_BEEP_MS        180
/* IO21_WAKE (SW3, momentary to ground) and the PWA are the only explicit
 * conversation triggers. The button is polled from the hands-free task's
 * waits rather than through an interrupt because those waits already run
 * every 50-500 ms and a button does not need better than that. */
#define WAKE_BUTTON_POLL_MS    50

static volatile bool s_auto_run;
static bool s_auto_active;
static volatile bool s_feed_failed;    /* capture read died under feed_task */

/* `wake debug on`: one line a second from the fetch loop with what the AFE
 * is actually seeing -- input level in dBFS, VAD state, ring buffer load.
 * The first question when barge-in is not heard is whether audio reaches
 * the AFE at all, and this answers it without a recording. */
static volatile bool s_debug;
static float s_last_volume_dbfs;
static int   s_last_vad;
static float s_last_ringbuf_free;
static SemaphoreHandle_t s_detected;   /* PWA trigger -> auto task */
static SemaphoreHandle_t s_auto_done;

/* ── init: load models, build the AFE instance once ─────────────────────── */

esp_err_t mpx_voice_wake_init(void)
{
    if (s_afe_ready) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(mpx_audio_have_mic(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "no microphone on this board");

    s_models = esp_srmodel_init("model");
    ESP_RETURN_ON_FALSE(s_models, ESP_FAIL, TAG,
                        "esp_srmodel_init failed -- is the `model` partition "
                        "flashed? (CONFIG_APP_VOICE_ENABLE needs `idf.py flash`, "
                        "not just an app flash, the first time it's turned on)");

    /* Probe the capture layout (see the comment above WAKE_FORMAT_4CH).
     * Opening and closing the codec once at boot is cheap; building the AFE
     * for a layout the codec then refuses is not. */
    if (mpx_audio_capture_active()) {
        ESP_LOGW(TAG, "microphone busy during init -- assuming a 4-channel capture");
        s_capture_channels = 4;
        s_input_format = WAKE_FORMAT_4CH;
    } else if (mpx_audio_capture_start(MPX_VOICE_UPLINK_RATE, 4, 0) == ESP_OK) {
        mpx_audio_capture_stop();
        s_capture_channels = 4;
        s_input_format = WAKE_FORMAT_4CH;
    } else {
        s_capture_channels = 2;
        s_input_format = WAKE_FORMAT_2CH;
        ESP_LOGW(TAG, "codec will not open 4 channels -- no echo reference, so "
                      "barge-in cannot hear you while the robot is talking");
    }
    ESP_LOGI(TAG, "capture: %u channels, AFE input format \"%s\" (single-mic FD AEC)",
             (unsigned)s_capture_channels, s_input_format);

    afe_config_t *afe_config = afe_config_init(s_input_format, s_models,
                                               WAKE_AFE_TYPE, AFE_MODE_LOW_COST);
    if (!afe_config) {
        ESP_LOGE(TAG, "afe_config_init failed");
        goto fail;
    }
    /* Keep the DSP needed for echo-cancelled barge-in, but do not create or
     * run a wake-word engine. Conversations start only from IO21_WAKE/PWA. */
    afe_config->wakenet_init = false;

    /* Internal RAM is the scarce resource on this board (see the
     * CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL comment in
     * boards/santy_control/sdkconfig.defaults.board) -- AFE's own ring
     * buffers and per-algorithm state are exactly the kind of large,
     * long-lived allocation that has no business competing with the LCD's
     * DMA buffer for it. */
    afe_config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    /* Core 1, as xiaozhi-esp32 does: core 0 carries the main task, LVGL,
     * Wi-Fi and the agent, and a starved AFE task drops audio silently
     * (ringbuff_free_pct in the fetch result is the tell). The calling tasks
     * are pinned below too: this preference does not set their affinity. */
    afe_config->afe_perferred_core = 1;

    s_afe_handle = esp_afe_handle_from_config(afe_config);
    if (!s_afe_handle) {
        ESP_LOGE(TAG, "esp_afe_handle_from_config failed");
        goto fail;
    }

    s_afe_data = s_afe_handle->create_from_config(afe_config);
    /* The public config API is caller-owned; AFE copies it during create. */
    afe_config_free(afe_config);
    afe_config = NULL;
    if (!s_afe_data) {
        ESP_LOGE(TAG, "create_from_config failed");
        goto fail;
    }

#if CONFIG_APP_VOICE_BARGE_IN
    if (!s_barge_ring) {
        s_barge_ring = xRingbufferCreateWithCaps(WAKE_BARGE_RING_BYTES,
                                                 RINGBUF_TYPE_BYTEBUF,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_barge_ring && !s_barge_lock) {
        s_barge_lock = xSemaphoreCreateMutex();
    }
    if (!s_barge_ring || !s_barge_lock) {
        ESP_LOGW(TAG, "no memory for barge-in PCM ring; PWA Interrupt and IO21 stop remain available");
    }
#endif

    s_afe_ready = true;
    ESP_LOGI(TAG, "AFE ready: %d Hz, %d feed channel(s), %d samples/channel "
                  "per feed, %d samples/channel per fetch",
             s_afe_handle->get_samp_rate(s_afe_data),
             s_afe_handle->get_feed_channel_num(s_afe_data),
             s_afe_handle->get_feed_chunksize(s_afe_data),
             s_afe_handle->get_fetch_chunksize(s_afe_data));
    s_afe_handle->print_pipeline(s_afe_data);
    return ESP_OK;

fail:
    if (afe_config) afe_config_free(afe_config);
    if (s_afe_data && s_afe_handle) s_afe_handle->destroy(s_afe_data);
    s_afe_data = NULL;
    s_afe_handle = NULL;
    s_afe_ready = false;
    if (s_models) esp_srmodel_deinit(s_models);
    s_models = NULL;
    return ESP_FAIL;
}

esp_err_t mpx_voice_wake_deinit(void)
{
    /* voice_glue must hold voice and join feed/fetch/auto/talk before this.
     * Destroying AFE underneath fetch is a use-after-free, not a memory fix. */
    if (s_feed_task || s_fetch_task || s_active || s_auto_active || s_auto_run ||
        s_barge_streaming || mpx_voice_stream_is_playing()) {
        ESP_LOGE(TAG, "AFE teardown refused: voice activity is still running");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_afe_data && s_afe_handle) s_afe_handle->destroy(s_afe_data);
    s_afe_data = NULL;
    s_afe_handle = NULL;
    s_afe_ready = false;
    if (s_models) esp_srmodel_deinit(s_models);
    s_models = NULL;
    if (s_barge_ring) {
        vRingbufferDeleteWithCaps(s_barge_ring);
        s_barge_ring = NULL;
    }
    if (s_barge_lock) {
        vSemaphoreDelete(s_barge_lock);
        s_barge_lock = NULL;
    }
    s_barge_streaming = false;
    ESP_LOGI(TAG, "AFE released for policy use; PSRAM %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_OK;
}

/* ── feed: mic -> raw capture -> afe->feed() ─────────────────────────────── */

static void feed_task(void *arg)
{
    (void)arg;
    const int chunk = s_afe_handle->get_feed_chunksize(s_afe_data);
    const int channels = s_afe_handle->get_feed_channel_num(s_afe_data);

    if (chunk <= 0 || chunk > MPX_AUDIO_MAX_FRAMES || channels != (int)s_capture_channels) {
        ESP_LOGE(TAG, "feed chunk/channel mismatch (chunk=%d, channels=%d, "
                      "expected <= %d frames, %u channels) -- not feeding",
                 chunk, channels, MPX_AUDIO_MAX_FRAMES, (unsigned)s_capture_channels);
        xSemaphoreGive(s_feed_done);
        vTaskDelete(NULL);
        return;
    }

    int16_t *buf = heap_caps_malloc((size_t)chunk * channels * sizeof(int16_t),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = heap_caps_malloc((size_t)chunk * channels * sizeof(int16_t), MALLOC_CAP_DEFAULT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "no memory for the feed buffer -- not feeding");
        xSemaphoreGive(s_feed_done);
        vTaskDelete(NULL);
        return;
    }

    while (s_run) {
        if (mpx_audio_capture_read_raw(buf, (size_t)chunk) != ESP_OK) {
            ESP_LOGW(TAG, "raw capture read failed -- stopping feed");
            /* Take fetch_task down with us rather than leaving it polling
             * an AFE nobody feeds ("Ringbuffer of AFE is empty" every
             * 200 ms, forever). The hands-free task sees s_feed_failed
             * and restarts the front end; `wake info` shows it otherwise. */
            s_feed_failed = true;
            s_run = false;
            break;
        }
        /* AFE takes slot 1 as its microphone ("RMNN"). This board has a second microphone on slot 3, and the
         * talk turns measure which one is louder (mpx_voice_talk_mic_slot): when it is slot 3, hand AFE that one,
         * so barge-in hears you as well as an ordinary turn does. */
        if (channels == 4 && mpx_voice_talk_mic_slot() == 3) {
            for (int i = 0; i < chunk; i++) {
                const int16_t t = buf[i * 4 + 1];
                buf[i * 4 + 1] = buf[i * 4 + 3];
                buf[i * 4 + 3] = t;
            }
        }
        s_afe_handle->feed(s_afe_data, buf);
    }

    free(buf);
    xSemaphoreGive(s_feed_done);
    vTaskDelete(NULL);
}

/* ── fetch: afe->fetch() -> VAD result -> barge-in ─────────────────────── */

static void send_abort_voice_trigger(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }
    const char *sid = mpx_voice_session_id();
    if (sid && sid[0]) {
        cJSON_AddStringToObject(root, "session_id", sid);
    }
    cJSON_AddStringToObject(root, "type", "abort");
    cJSON_AddStringToObject(root, "reason", "user_requested");

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!text) {
        return;
    }
    if (mpx_voice_send_json(text) != ESP_OK) {
        ESP_LOGW(TAG, "could not send abort -- socket down?");
    }
    free(text);
}

static size_t barge_pcm_read(int16_t *pcm, size_t frames, uint32_t timeout_ms, void *ctx)
{
    (void)ctx;
    if (!pcm || !frames || !s_barge_ring) {
        return 0;
    }
    const size_t want = frames * sizeof(int16_t);
    const TickType_t timeout = pdMS_TO_TICKS(timeout_ms);
    const TickType_t start = xTaskGetTickCount();

    /* ReceiveUpTo splits byte-buffer items, so neither short AFE chunks nor
     * a coalesced chunk larger than one Opus frame can lose samples. Return
     * only complete frames; talk_task must never zero-pad a partial read. */
    while (s_barge_staged_count * sizeof(int16_t) < want) {
        TickType_t wait = 0;
        if (timeout > 0) {
            TickType_t elapsed = xTaskGetTickCount() - start;
            if (elapsed >= timeout) {
                break;
            }
            wait = timeout - elapsed;
        }
        size_t got = 0;
        void *item = xRingbufferReceiveUpTo(s_barge_ring, &got, wait,
                                            want - s_barge_staged_count * sizeof(int16_t));
        if (!item) {
            break;
        }
        memcpy((uint8_t *)s_barge_staged + s_barge_staged_count * sizeof(int16_t), item, got);
        s_barge_staged_count += got / sizeof(int16_t);
        vRingbufferReturnItem(s_barge_ring, item);
    }
    if (s_barge_staged_count < frames) {
        return 0;
    }
    memcpy(pcm, s_barge_staged, want);
    s_barge_staged_count = 0;
    return frames;
}

static esp_err_t wake_barge_talk_start(uint32_t max_seconds)
{
    ESP_RETURN_ON_FALSE(s_barge_ring && s_barge_lock && mpx_voice_barge_in_supported() &&
                        mpx_audio_capture_active(), ESP_ERR_NOT_SUPPORTED, TAG,
                        "gateway or capture does not support speech barge-in");

    xSemaphoreTake(s_barge_lock, portMAX_DELAY);
    s_barge_streaming = false;
    size_t bytes = 0;
    void *item;
    while ((item = xRingbufferReceive(s_barge_ring, &bytes, 0)) != NULL) {
        vRingbufferReturnItem(s_barge_ring, item);
    }
    s_barge_staged_count = 0;
    s_barge_dropped = 0;
    xSemaphoreGive(s_barge_lock);

    esp_err_t err = mpx_voice_talk_start_barge_in(barge_pcm_read, NULL, max_seconds);
    if (err == ESP_OK) {
        /* Server-side VAD owns interruption; WakeNet is never enabled. */
        s_barge_streaming = true;
    }
    return err;
}

static void wake_barge_talk_stop(void)
{
    if (s_barge_lock) {
        xSemaphoreTake(s_barge_lock, portMAX_DELAY);
    }
    s_barge_streaming = false;
    if (s_barge_lock) {
        xSemaphoreGive(s_barge_lock);
    }
    if (mpx_voice_talk_active()) {
        mpx_voice_talk_stop();
    }
}

static void fetch_task(void *arg)
{
    (void)arg;

    while (s_run) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res) {
            continue;  /* fetch()'s own 2000ms timeout re-checks s_run for us */
        }
        if (res->ret_value != 0) {
            /* Negative/nonzero ret_value is esp-sr's own "something is
             * wrong with this frame" signal -- not fatal to the pipeline,
             * just skip acting on this result. */
            continue;
        }

        s_last_volume_dbfs = res->data_volume;
        s_last_vad = (int)res->vad_state;
        s_last_ringbuf_free = res->ringbuff_free_pct;
        if (s_debug) {
            static TickType_t last;
            const TickType_t now = xTaskGetTickCount();
            if (now - last >= pdMS_TO_TICKS(1000)) {
                last = now;
                ESP_LOGI(TAG, "afe: level %.0f dBFS  vad %s  ringbuf free %.0f%%",
                         (double)res->data_volume,
                         res->vad_state == VAD_SPEECH ? "SPEECH " : "silence",
                         (double)res->ringbuff_free_pct * 100.0);
            }
        }

        if (s_barge_lock) {
            xSemaphoreTake(s_barge_lock, portMAX_DELAY);
        }
        if (s_barge_streaming && res->data && res->data_size > 0) {
            BaseType_t stored = xRingbufferSend(s_barge_ring, res->data,
                                                (size_t)res->data_size, 0);
            if (stored != pdTRUE && ++s_barge_dropped % 100 == 1) {
                ESP_LOGW(TAG, "barge-in PCM ring overrun (%lu fetch chunks dropped)",
                         (unsigned long)s_barge_dropped);
            }
        }
        if (s_barge_lock) {
            xSemaphoreGive(s_barge_lock);
        }

    }

    xSemaphoreGive(s_fetch_done);
    vTaskDelete(NULL);
}

/* ── start/stop ──────────────────────────────────────────────────────────── */

esp_err_t mpx_voice_wake_start(void)
{
    ESP_RETURN_ON_FALSE(s_afe_ready, ESP_ERR_INVALID_STATE, TAG,
                        "call mpx_voice_wake_init() first");
    if (s_active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_FALSE(!mpx_audio_capture_active(), ESP_ERR_INVALID_STATE, TAG,
                        "microphone busy -- stop `voice talk` first");

    ESP_RETURN_ON_ERROR(mpx_audio_capture_start(MPX_VOICE_UPLINK_RATE,
                                                s_capture_channels, 0),
                        TAG, "capture start failed");

    /* Whatever the AFE still holds from before the microphone was handed
     * away (the start cue or the start of a reply) is
     * stale now; old audio must not leak into the next barge-in capture.
     * Optional in the interface, hence the NULL check. */
    if (s_afe_handle->reset_buffer) {
        s_afe_handle->reset_buffer(s_afe_data);
    }

    if (!s_feed_done) {
        s_feed_done = xSemaphoreCreateBinary();
    }
    if (!s_fetch_done) {
        s_fetch_done = xSemaphoreCreateBinary();
    }
    if (!s_feed_done || !s_fetch_done) {
        mpx_audio_capture_stop();
        return ESP_ERR_NO_MEM;
    }

    s_run = true;
    s_feed_failed = false;

    /* xTaskCreateStatic() leaves both DSP tasks unpinned. FD feed() alone
     * can then occupy core 0 until IDLE0 trips the task watchdog.
     * Keep feed/fetch on the core requested in afe_config and leave core 0
     * for Wi-Fi, Opus, LVGL and the rest of the firmware. */
    s_feed_task = xTaskCreateStaticPinnedToCore(feed_task, "wake_feed",
                                                WAKE_FEED_TASK_STACK / sizeof(StackType_t),
                                                NULL, (UBaseType_t)s_task_priority,
                                                s_feed_stack, &s_feed_tcb, 1);
    if (!s_feed_task) {
        s_run = false;
        mpx_audio_capture_stop();
        ESP_LOGE(TAG, "could not create the static-PSRAM feed task");
        return ESP_ERR_NO_MEM;
    }

    s_fetch_task = xTaskCreateStaticPinnedToCore(fetch_task, "wake_fetch",
                                                 WAKE_FETCH_TASK_STACK / sizeof(StackType_t),
                                                 NULL, (UBaseType_t)s_task_priority,
                                                 s_fetch_stack, &s_fetch_tcb, 1);
    if (!s_fetch_task) {
        /* feed_task is already running -- stop it the normal way rather
         * than leaving it orphaned. */
        s_run = false;
        xSemaphoreTake(s_feed_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS));
        s_feed_task = NULL;
        mpx_audio_capture_stop();
        ESP_LOGE(TAG, "could not create the static-PSRAM fetch task");
        return ESP_ERR_NO_MEM;
    }

    s_active = true;
    ESP_LOGI(TAG, "AFE feed/fetch tasks up");
    return ESP_OK;
}

esp_err_t mpx_voice_wake_stop(void)
{
    if (!s_active) return ESP_OK;
    s_run = false;

    /* Clear each self-deleted task handle only after its completion semaphore
     * confirms exit. A timeout leaves the front end marked active so a later
     * deinit cannot destroy AFE underneath a still-running DSP task. */
    if (s_feed_task) {
        if (xSemaphoreTake(s_feed_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS)) == pdTRUE) {
            s_feed_task = NULL;
        } else {
            ESP_LOGW(TAG, "feed task did not confirm stop within %d ms", WAKE_TASK_WAIT_MS);
        }
    }
    if (s_fetch_task) {
        if (xSemaphoreTake(s_fetch_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS)) == pdTRUE) {
            s_fetch_task = NULL;
        } else {
            ESP_LOGW(TAG, "fetch task did not confirm stop within %d ms", WAKE_TASK_WAIT_MS);
        }
    }
    if (s_feed_task || s_fetch_task) return ESP_ERR_TIMEOUT;

    mpx_audio_capture_stop();
    s_active = false;
    ESP_LOGI(TAG, "stopped");
    return ESP_OK;
}

bool mpx_voice_wake_active(void)
{
    return s_active;
}

void mpx_voice_wake_set_task_priority(int prio)
{
    if (prio < 1 || prio > 24) {
        ESP_LOGW(TAG, "ignoring priority %d: the front-end tasks take 1..24", prio);
        return;
    }
    s_task_priority = prio;
    if (s_feed_task) {
        vTaskPrioritySet(s_feed_task, (UBaseType_t)prio);
    }
    if (s_fetch_task) {
        vTaskPrioritySet(s_fetch_task, (UBaseType_t)prio);
    }
    ESP_LOGI(TAG, "front-end task priority -> %d%s", prio, s_active ? " (live)" : "");
}

int mpx_voice_wake_task_priority(void)
{
    return s_task_priority;
}

/* ── hands-free: IO21/PWA trigger -> talk -> reply -> ready ────────────── */

/* Short cues generated here rather than through mpx_audio_play_tone(), whose
 * resolution is whole seconds. Played with the microphone closed, so no echo
 * path exists. Accumulating phase makes the end cue a true falling tone rather
 * than a frequency-labelled sine whose pitch would sweep in the wrong way. */
static void wake_auto_tone(uint32_t start_hz, uint32_t end_hz, uint32_t duration_ms,
                           double amplitude)
{
    if (!mpx_audio_have_speaker()) {
        return;
    }
    const size_t frames = (size_t)MPX_VOICE_PLAYBACK_RATE * duration_ms / 1000;
    int16_t *buf = malloc(frames * sizeof(int16_t));
    if (!buf) {
        return;
    }
    const size_t ramp = frames / 8;
    double phase = 0.0;
    for (size_t i = 0; i < frames; i++) {
        double env = 1.0;
        if (i < ramp) {
            env = (double)i / (double)ramp;
        } else if (i >= frames - ramp) {
            env = (double)(frames - 1 - i) / (double)ramp;
        }
        const double t = frames > 1 ? (double)i / (double)(frames - 1) : 0.0;
        const double hz = start_hz + ((double)end_hz - start_hz) * t;
        phase += 2.0 * M_PI * hz / MPX_VOICE_PLAYBACK_RATE;
        buf[i] = (int16_t)(amplitude * env * sin(phase));
    }
    if (mpx_audio_output_start(MPX_VOICE_PLAYBACK_RATE) == ESP_OK) {
        size_t off = 0;
        while (off < frames) {
            const size_t n = (frames - off) > MPX_AUDIO_MAX_FRAMES ? MPX_AUDIO_MAX_FRAMES : (frames - off);
            if (mpx_audio_output_write(buf + off, n) != ESP_OK) {
                break;
            }
            off += n;
        }
        mpx_audio_output_stop();
    }
    free(buf);
}

static void wake_auto_beep(void)
{
    wake_auto_tone(WAKE_AUTO_BEEP_HZ, WAKE_AUTO_BEEP_HZ, WAKE_AUTO_BEEP_MS, 8000.0);
}

static void wake_auto_end_beep(void)
{
    /* Roughly half the start cue's amplitude: audible, but not another alert. */
    wake_auto_tone(WAKE_AUTO_END_BEEP_START_HZ, WAKE_AUTO_END_BEEP_END_HZ,
                   WAKE_AUTO_END_BEEP_MS, 4200.0);
}

/* Seconds to keep listening for a follow-up after a reply, with no new
 * button/PWA trigger. The server's VAD ends it the moment you speak, and
 * the server ends the whole session (says goodbye, closes the socket) after ITS idle limit,
 * which also ends this listen -- so in practice this is only a backstop.
 * Long on purpose: xiaozhi-esp32 keeps listening after a reply until the
 * server says goodbye, and a short window here (it was 8 s) meant that a
 * pause to think became "the robot stopped listening". */
#define WAKE_FOLLOWUP_SECONDS   90
/* Upper bound on waiting for a reply to arrive and finish. */
#define WAKE_REPLY_TIMEOUT_MS   60000
/* After WE end a listen ("stop": end of speech heard here, or the time limit)
 * the server only then transcribes it: its answer (stt / tts start) takes a
 * second or two. Wait this long for it before calling the turn "nothing
 * heard" -- giving up at once lost replies and ended the conversation. */
#define WAKE_LATE_ANSWER_MS     6000

static void wake_button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << WAKE_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (gpio_config(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "WAKE button on GPIO %d could not be configured", WAKE_BUTTON_GPIO);
    }
}

typedef enum {
    WAKE_TRIGGER_NONE,
    WAKE_TRIGGER_BUTTON,
    WAKE_TRIGGER_SOFTWARE,
} wake_trigger_t;

static bool s_button_was_down;
static uint8_t s_button_debounce;

/* Require two matching samples (100 ms) before changing pressed/released
 * state. Bounce must not turn one actuation into a start plus a stop. */
static bool wake_auto_poll_button(void)
{
    const bool down = gpio_get_level(WAKE_BUTTON_GPIO) == 0;
    if (down == s_button_was_down) {
        s_button_debounce = 0;
        return false;
    }
    if (++s_button_debounce < 2) {
        return false;
    }
    s_button_debounce = 0;
    s_button_was_down = down;
    if (!down) {
        return false;
    }
    ESP_LOGI(TAG, "IO21_WAKE pressed");
    /* Make fetch() leave promptly; the auto task owns all teardown. */
    s_run = false;
    return true;
}

static wake_trigger_t wake_auto_wait_trigger(uint32_t ms)
{
    for (uint32_t waited = 0; waited < ms; waited += WAKE_BUTTON_POLL_MS) {
        if (xSemaphoreTake(s_detected, pdMS_TO_TICKS(WAKE_BUTTON_POLL_MS)) == pdTRUE) {
            return WAKE_TRIGGER_SOFTWARE;
        }
        if (wake_auto_poll_button()) {
            return WAKE_TRIGGER_BUTTON;
        }
    }
    return WAKE_TRIGGER_NONE;
}

/* End the session but leave the auto loop armed for the next button press. */
static void wake_auto_stop_conversation(void)
{
    ESP_LOGI(TAG, "hands-free: IO21_WAKE stopped the conversation");
    /* Discard only triggers already queued at the stop edge. A newer PWA
     * Start during teardown must survive and start the requested session. */
    (void)xSemaphoreTake(s_detected, 0);
    mpx_voice_stream_abort_playback();
    (void)mpx_voice_talk_stop();
    (void)mpx_voice_talk_wait(1000);
    mpx_voice_disconnect();
    /* A late TTS event before socket teardown can re-arm playback. Abort
     * once more after disconnect so no buffered late audio can restart. */
    mpx_voice_stream_abort_playback();
}

/* Stop the front end, waiting first for a barge-in's playback teardown --
 * see the note in the REPLY state for why the order matters. */
static void wake_auto_release_mic(void)
{
    for (int i = 0; i < 40 && mpx_voice_stream_is_playing(); i++) {
        vTaskDelay(pdMS_TO_TICKS(25));
    }
    mpx_voice_wake_stop();
}

typedef enum {
    WAKE_ST_LISTEN,   /* idle, waiting for IO21_WAKE or the PWA */
    WAKE_ST_TALK,     /* microphone streaming to the server */
    WAKE_ST_REPLY,    /* reply arriving/playing; front end up for barge-in */
} wake_auto_state_t;

/* The loop's state, for mpx_voice_wake_conversing(), and the music
 * player's say on follow-up turns (mpx_voice_wake_set_followup_gate()). */
static volatile wake_auto_state_t s_auto_state = WAKE_ST_LISTEN;
static mpx_voice_wake_gate_fn s_followup_gate;

bool mpx_voice_wake_conversing(void)
{
    return s_auto_active && s_auto_state != WAKE_ST_LISTEN;
}

void mpx_voice_wake_set_followup_gate(mpx_voice_wake_gate_fn skip_followup)
{
    s_followup_gate = skip_followup;
}

static void wake_auto_task(void *arg)
{
    (void)arg;
    const uint32_t max_secs = CONFIG_APP_VOICE_AUTO_MAX_SECONDS;
    wake_auto_state_t state = WAKE_ST_LISTEN;
    bool follow_up = false;   /* this TALK needs no new trigger and no beep */
    bool reply_already_done = false; /* reply fully drained during TALK transition */
    uint32_t reply_completions_before = 0;

    while (s_auto_run) {
        s_auto_state = state;
        switch (state) {

        case WAKE_ST_LISTEN: {
            /* No audio task or model inference runs while idle. IO21_WAKE and
             * the PWA semaphore are the only ways out of this state. */
            if (wake_auto_wait_trigger(500) == WAKE_TRIGGER_NONE) {
                break;
            }
            wake_auto_release_mic();
            follow_up = false;
            state = WAKE_ST_TALK;
            break;
        }

        case WAKE_ST_TALK: {
            reply_completions_before = mpx_voice_stream_reply_completion_count();
            /* A session. The server drops idle sockets after a minute or
             * so, so reconnecting here is the normal case. */
            if (mpx_voice_state() != MPX_VOICE_READY) {
                /* Zero setup: the MangDang gateway hands out a URL and a
                 * token with no activation code. Provision on first use, when
                 * a new image changes the OTA endpoint, and again if the stored
                 * token stopped working (the server's secret changed). */
                const bool had_url = mpx_voice_stored_url()[0];
                if (mpx_voice_provision_needed()) {
                    ESP_LOGI(TAG, "hands-free: %s -- provisioning against %s",
                             had_url ? "gateway configuration changed" : "first use",
                             MPX_VOICE_DEFAULT_OTA_URL);
                    const esp_err_t provision_err = mpx_voice_provision(NULL);
                    if (provision_err != ESP_OK && had_url) {
                        /* Do not strand a unit when the new endpoint is briefly
                         * unreachable. Retry provisioning on every reconnect;
                         * the old credentials remain a fallback until then. */
                        ESP_LOGW(TAG, "hands-free: provisioning failed (%s) -- trying stored gateway",
                                 esp_err_to_name(provision_err));
                    }
                }
                if (!mpx_voice_stored_url()[0]) {
                    ESP_LOGW(TAG, "hands-free: not provisioned (no network?) -- press WAKE "
                                  "or use the PWA again later, or run `voice provision`");
                    state = WAKE_ST_LISTEN;
                    break;
                }
                ESP_LOGI(TAG, "hands-free: connecting");
                esp_err_t err = mpx_voice_connect(NULL, NULL);
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free: connect failed (%s) -- provisioning again",
                             esp_err_to_name(err));
                    if (mpx_voice_provision(NULL) == ESP_OK) {
                        err = mpx_voice_connect(NULL, NULL);
                    }
                }
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free: connect failed (%s) -- back to ready",
                             esp_err_to_name(err));
                    state = WAKE_ST_LISTEN;
                    break;
                }
            }

            ESP_LOGI(TAG, "hands-free: turn (%s), internal RAM %u free / %u largest",
                     follow_up ? "follow-up" : "button/PWA trigger",
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            if (!follow_up) {
                wake_auto_beep();
            } else {
                ESP_LOGI(TAG, "hands-free: your turn -- no new trigger needed until the server ends the session");
            }
            const uint32_t secs = follow_up ? WAKE_FOLLOWUP_SECONDS : max_secs;
            /* When the PWA owns the microphone, the SAME turn runs off the
             * phone's PCM ring instead of I2S -- connection, provisioning,
             * follow-ups and barge-in stay in this loop either way. */
            const esp_err_t err = mpx_phone_audio_mic_enabled()
                                      ? mpx_voice_talk_start_phone(secs)
                                      : mpx_voice_talk_start_auto(secs);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "hands-free: talk failed (%s) -- back to ready",
                         esp_err_to_name(err));
                wake_auto_release_mic();
                state = WAKE_ST_LISTEN;
                break;
            }
            /* Physical stop and PWA Interrupt are consumed here, so neither
             * can remain queued and interrupt a later reply unexpectedly. */
            wake_trigger_t trigger = WAKE_TRIGGER_NONE;
            for (uint32_t waited = 0; mpx_voice_talk_active() && waited < secs * 1000u + 3000u; waited += 250) {
                trigger = wake_auto_wait_trigger(250);
                if (trigger != WAKE_TRIGGER_NONE) {
                    break;
                }
                if (!s_auto_run) {
                    mpx_voice_talk_stop();
                    break;
                }
            }
            if (trigger == WAKE_TRIGGER_BUTTON) {
                wake_auto_stop_conversation();
                wake_auto_release_mic();
                wake_auto_end_beep();
                follow_up = false;
                state = WAKE_ST_LISTEN;
                break;
            }
            if (trigger == WAKE_TRIGGER_SOFTWARE) {
                mpx_voice_talk_stop();
                (void)mpx_voice_talk_wait(1000);
                wake_auto_release_mic();
                follow_up = false;
                state = WAKE_ST_TALK;
                break;
            }
            if (mpx_voice_talk_server_took_turn()) {
                /* PWA Interrupt or a fast gateway can start TTS before raw
                 * uplink teardown is observed. Preserve that reply even if
                 * it finishes during this handoff. */
                (void)mpx_voice_talk_wait(1000);
                reply_already_done = mpx_voice_stream_reply_completion_count() !=
                                     reply_completions_before;
                state = WAKE_ST_REPLY;
                break;
            }
            if (mpx_voice_talk_active()) {
                /* A reply can cross the follow-up deadline by a few frames.
                 * Give on_server_turn() time to stop raw uplink before
                 * deciding that this turn had no answer. */
                (void)mpx_voice_talk_wait(1000);
            }
            if (mpx_voice_talk_active()) {
                mpx_voice_talk_stop();
                (void)mpx_voice_talk_wait(1000);
            }
            bool stop_pressed = false;
            for (uint32_t waited = 0; !mpx_voice_talk_server_took_turn() && waited < WAKE_LATE_ANSWER_MS;
                 waited += WAKE_BUTTON_POLL_MS) {
                if (wake_auto_poll_button()) {
                    stop_pressed = true;
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(WAKE_BUTTON_POLL_MS));
            }
            if (stop_pressed) {
                wake_auto_stop_conversation();
                wake_auto_release_mic();
                wake_auto_end_beep();
                follow_up = false;
                state = WAKE_ST_LISTEN;
                break;
            }
            if (!mpx_voice_talk_server_took_turn()) {
                /* Nothing was said, or nothing the server could use. */
                ESP_LOGI(TAG, "hands-free: nothing heard -- ready for IO21_WAKE/PWA");
                wake_auto_release_mic();
                wake_auto_end_beep();
                state = WAKE_ST_LISTEN;
                break;
            }
            reply_already_done = mpx_voice_stream_reply_completion_count() !=
                                 reply_completions_before;
            state = WAKE_ST_REPLY;
            break;
        }

        case WAKE_ST_REPLY: {
            /* Keep AFE running ahead of the reply. With a negotiated
             * barge_in gateway, continuously stream its echo-cancelled PCM;
             * the gateway cancels old TTS only after its VAD confirms speech.
             * Older gateways retain PWA Interrupt; IO21_WAKE stops the reply. */
            const uint32_t completions_before = reply_completions_before;
            bool finished = reply_already_done;
            reply_already_done = false;
            if (!finished && !s_active && mpx_voice_wake_start() != ESP_OK) {
                ESP_LOGW(TAG, "hands-free: front end did not restart -- no barge-in this reply");
            }
            bool barge_talk = false;
            bool interrupted = false;
            if (!finished && s_active && !mpx_phone_audio_mic_enabled()) {
                esp_err_t err = wake_barge_talk_start(WAKE_REPLY_TIMEOUT_MS / 1000);
                if (err == ESP_OK) {
                    barge_talk = true;
                    ESP_LOGI(TAG, "hands-free: full-duplex barge-in armed");
                } else if (err != ESP_ERR_NOT_SUPPORTED) {
                    ESP_LOGW(TAG, "hands-free: barge-in stream failed (%s); PWA Interrupt/IO21 stop remain available",
                             esp_err_to_name(err));
                }
            }
            wake_trigger_t trigger = WAKE_TRIGGER_NONE;
            bool ended = false;
            for (uint32_t waited = 0; s_auto_run && waited < WAKE_REPLY_TIMEOUT_MS; waited += 250) {
                trigger = wake_auto_wait_trigger(250);
                if (trigger != WAKE_TRIGGER_NONE || finished) {
                    /* Even an already-drained reply polls once, so a queued
                     * PWA Interrupt takes its branch instead of becoming a
                     * stale trigger for a later session. */
                    break;
                }
                if (barge_talk &&
                    (mpx_voice_stream_barge_accepted() ||
                     (mpx_voice_talk_server_took_turn() && !mpx_voice_talk_active()))) {
                    /* A confirmed onset cancels old playback, then STT ends
                     * the new utterance. A barge-mode follow-up can instead
                     * reach STT while no old TTS exists, so there is no stop
                     * event and barge_accepted stays false. Its completed
                     * STT/rearm is equally a turn boundary: arm the next
                     * reply instead of leaving a producer with no consumer. */
                    interrupted = true;
                    if (!mpx_voice_talk_active()) {
                        finished = true;
                        break;
                    }
                } else if (mpx_voice_stream_reply_completion_count() != completions_before) {
                    /* Only a valid, fully drained stop completes a reply.
                     * Aborted old replies and prebuffering never satisfy this.
                     * A barge stop takes the branch above and must not truncate
                     * the user's new utterance. */
                    finished = true;
                    break;
                }
                /* Session gone -- the server said goodbye (after its idle
                 * timeout, or after you did) or the socket dropped. Do not
                 * sit here for a reply that is not coming, and do not offer
                 * a follow-up turn on a dead session: let whatever is still
                 * playing finish, then go back to idle. */
                if (mpx_voice_state() != MPX_VOICE_READY && !mpx_voice_stream_is_playing()) {
                    ended = true;
                    break;
                }
                if (s_feed_failed) {
                    ESP_LOGW(TAG, "hands-free: microphone read died -- restarting the front end");
                    wake_barge_talk_stop();
                    barge_talk = false;
                    mpx_voice_wake_stop();
                    (void)mpx_voice_wake_start();
                }
            }
            if (!s_auto_run) {
                wake_barge_talk_stop();
                break;
            }
            if (trigger == WAKE_TRIGGER_BUTTON) {
                /* Physical WAKE is a toggle: stop this session and stay
                 * armed, rather than turning the press into a new turn. */
                wake_barge_talk_stop();
                wake_auto_stop_conversation();
                wake_auto_release_mic();
                wake_auto_end_beep();
                follow_up = false;
                state = WAKE_ST_LISTEN;
            } else if (trigger == WAKE_TRIGGER_SOFTWARE) {
                /* PWA Interrupt already aborted playback and told the
                 * server. Stop processed uplink before handing the capture
                 * hardware to an ordinary raw-mic turn. */
                wake_barge_talk_stop();
                wake_auto_release_mic();
                follow_up = false;
                state = WAKE_ST_TALK;
            } else if (finished && interrupted) {
                /* STT/rearm closed the utterance; keep AFE up and arm
                 * barge-in for the replacement reply without an I2S restart.
                 */
                wake_barge_talk_stop();
                state = WAKE_ST_REPLY;
            } else if (finished && s_followup_gate && s_followup_gate()) {
                wake_barge_talk_stop();
                /* Music is waiting for the speaker (it was just asked for,
                 * or this question interrupted it): no follow-up turn --
                 * close the mic so the music can play. */
                wake_auto_release_mic();
                ESP_LOGI(TAG, "hands-free: music waiting -- ready for IO21_WAKE/PWA");
                state = WAKE_ST_LISTEN;
            } else if (finished && mpx_voice_state() == MPX_VOICE_READY) {
                /* Reply done: hand the mic straight back for a follow-up. */
                wake_barge_talk_stop();
                wake_auto_release_mic();
                follow_up = true;
                state = WAKE_ST_TALK;
            } else if (finished || ended) {
                wake_barge_talk_stop();
                /* The gateway can close immediately after its goodbye TTS.
                 * Wait for that playback above, close the mic, then give the
                 * session a distinct, softer falling-tone ending. */
                wake_auto_release_mic();
                wake_auto_end_beep();
                ESP_LOGI(TAG, "hands-free: session ended -- ready for IO21_WAKE/PWA");
                state = WAKE_ST_LISTEN;
            } else {
                wake_barge_talk_stop();
                wake_auto_release_mic();
                wake_auto_end_beep();
                ESP_LOGW(TAG, "hands-free: no reply within %d s -- ready for IO21_WAKE/PWA",
                         WAKE_REPLY_TIMEOUT_MS / 1000);
                state = WAKE_ST_LISTEN;
            }
            break;
        }
        }
    }

    if (s_active) {
        mpx_voice_wake_stop();
    }
    s_auto_state = WAKE_ST_LISTEN;
    s_auto_active = false;
    xSemaphoreGive(s_auto_done);
    vTaskDelete(NULL);
}

esp_err_t mpx_voice_wake_auto_start(void)
{
    if (s_auto_active) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(mpx_voice_wake_init(), TAG, "init failed");

    if (!s_detected) {
        s_detected = xSemaphoreCreateBinary();
    }
    if (!s_auto_done) {
        s_auto_done = xSemaphoreCreateBinary();
    }
    ESP_RETURN_ON_FALSE(s_detected && s_auto_done, ESP_ERR_NO_MEM, TAG, "out of memory");
    /* Drain anything left from a previous run. */
    (void)xSemaphoreTake(s_detected, 0);
    (void)xSemaphoreTake(s_auto_done, 0);

    wake_button_init();

    s_auto_run = true;
    s_auto_active = true;
    /* Internal stack on purpose: this task only orchestrates, and a task
     * whose stack lives in PSRAM cannot be the one that ends up in a flash
     * write (NVS, from provisioning) with the cache disabled. */
    if (xTaskCreate(wake_auto_task, "wake_auto", WAKE_AUTO_TASK_STACK, NULL, 4, NULL) != pdPASS) {
        s_auto_run = false;
        s_auto_active = false;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "hands-free: IO21_WAKE/PWA starts talk; press IO21_WAKE again to stop");
    return ESP_OK;
}

esp_err_t mpx_voice_wake_auto_stop(void)
{
    if (!s_auto_active) return ESP_OK;
    s_auto_run = false;
    /* If it is mid-talk, cut that short so the task can leave the loop. */
    s_barge_streaming = false;
    mpx_voice_talk_stop();
    if (xSemaphoreTake(s_auto_done, pdMS_TO_TICKS(WAKE_TASK_WAIT_MS + 5000)) != pdTRUE) {
        ESP_LOGW(TAG, "hands-free task did not confirm stop");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool mpx_voice_wake_auto_active(void)
{
    return s_auto_active;
}

esp_err_t mpx_voice_wake_trigger(void)
{
    if (!s_auto_active || !s_detected) return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "PWA Start/Interrupt voice trigger");
    if (mpx_voice_stream_reply_active() || mpx_voice_stream_is_playing()) {
        mpx_voice_stream_abort_playback();
        send_abort_voice_trigger();
    }
    /* Match the physical button: make fetch() leave its loop promptly, then
     * wake the orchestrator. It remains the sole owner of the mic handoff. */
    s_run = false;
    xSemaphoreGive(s_detected);
    return ESP_OK;
}

/* ── status, for /v1/voice ───────────────────────────────────────────────── */

void mpx_voice_wake_last_seen(float *volume_dbfs, int *vad, float *ringbuf_free)
{
    if (volume_dbfs) {
        *volume_dbfs = s_last_volume_dbfs;
    }
    if (vad) {
        *vad = s_last_vad == (int)VAD_SPEECH ? 1 : 0;
    }
    if (ringbuf_free) {
        *ringbuf_free = s_last_ringbuf_free;
    }
}

const char *mpx_voice_wake_state_name(void)
{
    if (!s_auto_active) {
        return "off";
    }
    switch (s_auto_state) {
        case WAKE_ST_TALK:  return "talking";
        case WAKE_ST_REPLY: return "replying";
        default:            return "ready";
    }
}

