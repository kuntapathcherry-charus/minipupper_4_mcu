/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Phase 2's missing half. `voice loopback` proved the codec by buffering a
 * whole clip locally; this streams frames over the real socket instead --
 * mic -> Opus -> binary WebSocket frame outbound, binary frame -> Opus ->
 * speaker inbound.
 *
 * Two ways in. `voice talk` is push-to-talk: the mic is opened, closed
 * again before any reply audio exists, and "listen" is manual-mode -- the
 * server does not run ASR until it sees {"type":"listen","state":"stop"}.
 * mpx_voice_talk_start_auto(), which mpx_voice_wake calls after the wake
 * word, is listen-mode "auto" instead: the server's VAD decides when you
 * have finished, and the first sign it has taken its turn ("stt", or "tts"
 * start) is what closes the microphone here. Either way capture and
 * playback never overlap on THIS component's account -- the reply plays
 * after the mic is closed. What may hold the mic open during a reply is
 * mpx_voice_wake's echo-cancelled front end, listening for barge-in.
 *
 * TX runs inline on its own task -- reading, encoding and sending share one
 * loop, since each blocking capture read already paces it at ~60ms/frame.
 * RX is two stages: the WebSocket event handler (mpx_voice_ws.c) just
 * copies each binary frame into this component's queue and returns
 * immediately, and a separate task here decodes and plays it. That split
 * exists because opus decode, like opus encode, wants real stack -- doing
 * it inline in the WebSocket client's own event task is how the Opus
 * loopback crash happened earlier in this project, on a task nowhere near
 * this hungry.
 */

#include "mpx_voice_link.h"

#include <inttypes.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mpx_audio.h"
#include "voice/voice_phone_audio.h"

static const char *TAG = "mpx_voice_stream";

/* Started at the same size as MPX_VOICE_TASK_STACK in mpx_voice_codec.cc
 * (28672) on the theory that talk_task and playback_task call into the same
 * Opus wrappers on their own stack, so they'd need the same headroom
 * loopback does. That theory was wrong for talk_task specifically: unlike
 * loopback, which only ever encodes and decodes locally, talk_task also
 * calls mpx_voice_send_binary() -- a synchronous TLS write, all the way
 * through esp_websocket_client into mbedtls's record-layer encryption --
 * from the very same stack frame each time around the loop, right after an
 * Opus encode call that alone was already sized to need most of a 28 KB
 * stack. Measured on real hardware: 28672 overflows within the first
 * `voice talk`, every time. Doubled with real margin left over rather than
 * inched up, because the failure mode of guessing wrong here again is a
 * silent memory-corruption crash, not a clean error return. PSRAM-backed
 * (see the allocation below), so the extra 28 KB costs nothing that
 * matters. */
#define VOICE_STREAM_TASK_STACK  (2048 * 28)
/* Downlink frames waiting to be played. The server does not pace TTS
 * audio: it sends each sentence as one burst, and a long sentence is far
 * more than the 24 frames (1.4 s) the first cut held --
 *
 *   W mpx_voice_stream: RX queue full -- dropping a downlink frame   (x30)
 *
 * -- which is what a reply that stutters and skips words sounds like.
 * 300 frames is 18 s of audio; the queue is pointers only (the packets
 * themselves are malloc'd, i.e. PSRAM), so the depth is nearly free. */
#define VOICE_STREAM_RX_DEPTH    300
/* Frames (60 ms each) to have in hand before a reply starts playing. */
#define VOICE_STREAM_PREBUFFER   7

/* Keep the voice talk capture in the same four-channel / 32-bit-slot layout
 * as the AFE front end. The paired I2S DMA rings stay allocated after
 * capture_stop(), but a two-channel open shrinks them to 16-bit slots; the
 * next AFE open then has to find twelve fresh 1920-byte DMA buffers.
 * Once music, TLS and the agent have fragmented internal RAM that fails even
 * with enough total bytes free. Reading all four channels costs a little PSRAM
 * scratch bandwidth, but avoids the reallocations and keeps barge-in available
 * while music plays.
 *
 * Pick slot 1, the microphone in the wake front end's measured "RMNN" layout.
 * Slot 0 is the amplifier's echo reference, not a microphone: selecting it
 * produces valid Opus frames containing almost no speech, so the server VAD
 * times out without an STT result even though IO21/PWA triggering works. The older
 * two-channel talk capture used slot 0 for its microphone; that numbering does
 * not carry over to this four-channel layout. See WAKE_FORMAT_4CH. */
#define TALK_CAPTURE_CHANNELS  4
#define TALK_CAPTURE_PICK      1

typedef struct {
    uint8_t *data;  /* NULL is the "tts finished" sentinel */
    size_t len;
    /* Tag at ingress, not after dequeue: abort + a new TTS start may clear
     * the global gate while playback is holding this frame. Its original
     * generation must remain attached so stale audio cannot pass later. */
    uint32_t generation;
    uint32_t reply_generation; /* NULL-data stop sentinels validate their reply */
} voice_rx_frame_t;

static QueueHandle_t s_rx_queue;
static bool s_playback_open;
static bool s_stream_ready;

static volatile bool s_talk_active;
static volatile bool s_talk_stop_requested;
static volatile bool s_talk_auto;        /* this talk is listen-mode "auto" */
static volatile bool s_talk_phone;       /* capture comes from the PWA, not I2S */
static volatile bool s_talk_barge;       /* AFE-cleaned PCM, listen-mode "barge_in" */
static volatile bool s_barge_accepted;   /* server confirmed speech and stopped old TTS */
static volatile bool s_drop_reply;       /* old reply aborted; drop packets until a new turn */
static volatile uint32_t s_playback_generation; /* invalidates a frame held outside the queue */
static mpx_voice_pcm_reader_t s_talk_reader;
static void *s_talk_reader_ctx;
static volatile bool s_server_turn;      /* the server answered -- stop streaming */
static volatile uint32_t s_tts_stops;    /* replies finished, see the header */
static volatile uint32_t s_reply_generation;
static volatile uint32_t s_reply_completions; /* increments only after queued audio drains */
static volatile uint32_t s_stop_reply_generation;
static volatile bool s_reply_active;     /* TTS started; remains set until queued audio drains */
static volatile bool s_stop_pending;     /* a "tts stop" arrived; close once the queue drains */
static SemaphoreHandle_t s_talk_done;
static SemaphoreHandle_t s_playback_lock; /* abort vs held-frame decode/output */
static uint32_t s_listen_counter;
static volatile uint32_t s_talk_listen_id;

/* ── setup ───────────────────────────────────────────────────────────────*/

static void playback_task(void *arg);

esp_err_t mpx_voice_stream_init(void)
{
    if (s_stream_ready) {
        return ESP_OK;
    }

    /* Queue storage in PSRAM: the slots are small, but internal RAM is the
     * one thing this board is short of (see the boot log's heap lines) and
     * a queue does not need to be fast memory. */
    s_rx_queue = xQueueCreateWithCaps(VOICE_STREAM_RX_DEPTH, sizeof(voice_rx_frame_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rx_queue) {
        s_rx_queue = xQueueCreateWithCaps(VOICE_STREAM_RX_DEPTH, sizeof(voice_rx_frame_t),
                                          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    ESP_RETURN_ON_FALSE(s_rx_queue, ESP_ERR_NO_MEM, TAG, "queue alloc failed");

    s_talk_done = xSemaphoreCreateBinary();
    if (!s_talk_done) {
        vQueueDeleteWithCaps(s_rx_queue);
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_playback_lock = xSemaphoreCreateMutex();
    if (!s_playback_lock) {
        vSemaphoreDelete(s_talk_done);
        vQueueDeleteWithCaps(s_rx_queue);
        s_talk_done = NULL;
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    /* AFE feed/fetch are pinned to core 1. Pin Opus decode to core 0 rather
     * than letting the scheduler pile it onto the already saturated AFE core;
     * the captured WDT trace showed IDLE0 while IDLE1 starved. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(playback_task, "voice_rx", VOICE_STREAM_TASK_STACK,
                                        NULL, 5, NULL, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCoreWithCaps(playback_task, "voice_rx", VOICE_STREAM_TASK_STACK,
                                 NULL, 5, NULL, 0, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        vSemaphoreDelete(s_playback_lock);
        vSemaphoreDelete(s_talk_done);
        vQueueDeleteWithCaps(s_rx_queue);
        s_playback_lock = NULL;
        s_talk_done = NULL;
        s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_stream_ready = true;
    ESP_LOGI(TAG, "streaming ready (talk/playback tasks up)");
    return ESP_OK;
}

/* ── RX: socket -> queue -> decode -> speaker ───────────────────────────── */

void mpx_voice_stream_rx_frame(const uint8_t *data, size_t len)
{
    if (!s_rx_queue || !data || !len) {
        return;
    }
    /* Raw capture has no echo reference, so before its server turn any
     * downlink is an old-reply tail and would feed the open microphone.
     * Processed barge-in capture is deliberately full duplex: AFE receives
     * the electrical playback reference and must keep hearing this reply. */
    if (s_drop_reply || (s_talk_active && !s_server_turn && !s_talk_barge)) {
        return;
    }
    uint8_t *copy = malloc(len);
    if (!copy) {
        ESP_LOGW(TAG, "OOM copying a %u-byte downlink frame -- dropped", (unsigned)len);
        return;
    }
    memcpy(copy, data, len);

    const voice_rx_frame_t frame = {
        .data = copy,
        .len = len,
        .generation = s_playback_generation,
        .reply_generation = s_reply_generation,
    };
    if (xQueueSend(s_rx_queue, &frame, 0) != pdTRUE) {
        ESP_LOGW(TAG, "RX queue full -- dropping a downlink frame");
        free(copy);
    }
}

void mpx_voice_stream_on_tts_start(bool explicit_start)
{
    if (s_playback_lock) {
        xSemaphoreTake(s_playback_lock, portMAX_DELAY);
    }
    /* sentence_start messages belong to the current reply. An explicit start,
     * or the first sentence after idle/abort, establishes a new generation. */
    if (explicit_start || !s_reply_active) {
        s_reply_generation++;
        s_stop_pending = false;
    }
    s_reply_active = true;
    if (s_playback_lock) {
        xSemaphoreGive(s_playback_lock);
    }
}

uint32_t mpx_voice_stream_reply_completion_count(void)
{
    return s_reply_completions;
}

bool mpx_voice_stream_reply_active(void)
{
    return s_reply_active;
}

void mpx_voice_stream_on_tts_stop(void)
{
    s_tts_stops++;
    if (!s_rx_queue) {
        return;
    }
    if (s_playback_lock) {
        xSemaphoreTake(s_playback_lock, portMAX_DELAY);
    }
    /* An aborted reply's delayed stop must not terminate a newer raw turn. */
    if (!s_reply_active) {
        if (s_playback_lock) {
            xSemaphoreGive(s_playback_lock);
        }
        return;
    }
    /* The flag is a fallback for a full queue dropping the sentinel. Both
     * are generation-tagged so an old drain cannot complete a newer reply. */
    s_stop_pending = true;
    s_stop_reply_generation = s_reply_generation;
    const voice_rx_frame_t sentinel = {
        .data = NULL,
        .len = 0,
        .reply_generation = s_reply_generation,
    };
    (void)xQueueSend(s_rx_queue, &sentinel, 0);
    if (s_playback_lock) {
        xSemaphoreGive(s_playback_lock);
    }
}

void mpx_voice_stream_on_barge_in_stop(void)
{
    s_tts_stops++;
    if (s_talk_barge) {
        s_barge_accepted = true;
    }
    mpx_voice_stream_abort_playback();
}

bool mpx_voice_stream_barge_accepted(void)
{
    return s_barge_accepted;
}

bool mpx_voice_stream_is_playing(void)
{
    return s_playback_open;
}

void mpx_voice_stream_on_stt(void)
{
    /* STT belongs to the new utterance and closes its uplink. Keep dropping
     * until new TTS start: an in-flight old audio packet must not restart
     * playback after the barge-in flush. */
    s_server_turn = true;
}

void mpx_voice_stream_on_listen_rearm(uint32_t listen_id)
{
    /* Ignored/empty ASR carries no transcript and must not touch UI state.
     * Its re-arm applies only to the capture that the server actually ended;
     * delayed ASR must never close a newer ordinary or barge-in turn. */
    if (s_talk_active && s_talk_barge && listen_id == s_talk_listen_id) {
        s_server_turn = true;
    }
}

void mpx_voice_stream_on_server_turn(void)
{
    /* A TTS start can still belong to the reply a barge-in capture is trying
     * to interrupt. Only STT proves that the new utterance reached its turn. */
    if (!s_talk_barge) {
        s_server_turn = true;
    }
    s_drop_reply = false;
}

bool mpx_voice_talk_server_took_turn(void)
{
    return s_server_turn;
}

uint32_t mpx_voice_stream_tts_stop_count(void)
{
    return s_tts_stops;
}

void mpx_voice_stream_abort_playback(void)
{
    if (!s_rx_queue) {
        return;
    }
    /* Serialize invalidation with the held frame's decode/output. If a write
     * is already in flight, wait for that bounded write; once this returns,
     * no frame approved before the abort can still reach the speaker. */
    if (s_playback_lock) {
        xSemaphoreTake(s_playback_lock, portMAX_DELAY);
    }
    /* Set the gate and invalidate a frame playback_task may be holding in
     * prebuffer before draining: in-flight old audio must not re-open the
     * speaker after this flush, even if a new tts start quickly clears RX. */
    s_playback_generation++;
    s_drop_reply = true;
    s_reply_active = false;
    s_stop_reply_generation = s_reply_generation;
    /* xQueueReset() would leak every heap-allocated packet still in the
     * queue. Drain instead, including any normal stop sentinel. */
    voice_rx_frame_t dropped;
    while (xQueueReceive(s_rx_queue, &dropped, 0) == pdTRUE) {
        free(dropped.data);
    }
    s_stop_pending = true;
    const voice_rx_frame_t sentinel = {
        .data = NULL,
        .len = 0,
        .reply_generation = s_reply_generation,
    };
    (void)xQueueSend(s_rx_queue, &sentinel, 0);
    /* The phone buffers its own playback ahead of real time, so resetting the
     * robot queue alone would leave the first words of the old reply playing
     * from the phone after a barge-in. */
    if (mpx_phone_audio_speaker_enabled()) mpx_phone_audio_stop_playback();
    if (s_playback_lock) {
        xSemaphoreGive(s_playback_lock);
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    voice_rx_frame_t frame;
    int16_t pcm[MPX_VOICE_DOWNLINK_FRAME];

    for (;;) {
        if (xQueueReceive(s_rx_queue, &frame, pdMS_TO_TICKS(100)) != pdTRUE) {
            /* Nothing queued. Serialize the fallback with a new TTS start
             * and validate its generation before completing the reply. */
            if (s_playback_lock) {
                xSemaphoreTake(s_playback_lock, portMAX_DELAY);
            }
            if (s_stop_pending && s_stop_reply_generation == s_reply_generation) {
                if (s_playback_open) {
                    mpx_audio_output_stop();
                    s_playback_open = false;
                }
                if (s_reply_active) {
                    s_reply_active = false;
                    s_reply_completions++;
                }
                s_stop_pending = false;
            }
            if (s_playback_lock) {
                xSemaphoreGive(s_playback_lock);
            }
            continue;
        }

        if (!frame.data) {
            if (s_playback_lock) {
                xSemaphoreTake(s_playback_lock, portMAX_DELAY);
            }
            const bool valid_stop = s_stop_pending &&
                                    s_stop_reply_generation == frame.reply_generation &&
                                    s_reply_generation == frame.reply_generation;
            if (valid_stop) {
                if (s_playback_open) {
                    mpx_audio_output_stop();
                    s_playback_open = false;
                }
                if (s_reply_active) {
                    s_reply_active = false;
                    s_reply_completions++;
                }
                s_stop_pending = false;
            }
            if (s_playback_lock) {
                xSemaphoreGive(s_playback_lock);
            }
            continue;
        }

        if (!s_playback_open) {
            /* First frame of a reply. Let a few more arrive before the
             * speaker starts: the server sends the opening of a reply in a
             * burst and then paces to real time, and with the I2S driver
             * clearing each DMA buffer as it goes (auto_clear), every gap
             * between frames is a gap in the audio -- the crackle heard at
             * the start of hands-free replies. ~250 ms of buffer absorbs
             * the jitter and is not a delay anyone notices. Raised to ~400 ms
             * (VOICE_STREAM_PREBUFFER, waiting at most 800 ms): over the
             * internet a 180 ms cushion still ran dry mid-word now and then,
             * and a sound cut off mid-wave is a click -- crackle. The server
             * sends a 1.2 s head start, so this fills at once. Prebuffer
             * outside the playback lock so abort is never delayed by it. */
            const bool phone_out = mpx_phone_audio_speaker_enabled();
            const int prebuffer = phone_out ? 2 : VOICE_STREAM_PREBUFFER;
            for (int i = 0; i < 32 && uxQueueMessagesWaiting(s_rx_queue) < prebuffer; i++) {
                vTaskDelay(pdMS_TO_TICKS(25));
            }
        }

        if (s_playback_lock) {
            xSemaphoreTake(s_playback_lock, portMAX_DELAY);
        }
        if (frame.generation != s_playback_generation || s_drop_reply) {
            /* abort_playback() ran before dequeue or while this frame was
             * prebuffering. The ingress generation tag closes the race where
             * a new reply clears s_drop_reply before this old frame is held. */
            if (s_playback_lock) {
                xSemaphoreGive(s_playback_lock);
            }
            free(frame.data);
            continue;
        }

        if (!s_playback_open) {
            /* MPX_VOICE_PLAYBACK_RATE, not mpx_voice_downlink_rate(): the
             * decoder was opened at the playback rate in mpx_voice_talk_start(),
             * so this is what the PCM actually is -- and it is the only rate
             * the I2S peripheral will accept while the AFE front end
             * holds the microphone open (see the header for the full story). */
            if (mpx_phone_audio_speaker_enabled()) {
                /* No I2S: the phone plays the PCM this task forwards below.
                 * Its own AudioContext handles buffering, so the same
                 * prebuffer is kept only to absorb the server's burst. */
                s_playback_open = true;
            } else if (mpx_audio_output_start(MPX_VOICE_PLAYBACK_RATE) == ESP_OK) {
                s_playback_open = true;
            } else {
                ESP_LOGW(TAG, "cannot open speaker for playback");
            }
        }

        if (s_playback_open) {
            const int n = mpx_voice_decode(frame.data, frame.len, pcm, MPX_VOICE_DOWNLINK_FRAME);
            if (n > 0) {
                if (mpx_phone_audio_speaker_enabled()) {
                    mpx_phone_audio_play(pcm, (size_t)n);
                } else {
                    (void)mpx_audio_output_write(pcm, (size_t)n);
                }
            } else if (n < 0) {
                ESP_LOGW(TAG, "decode failed: %d", n);
            }
        }
        if (s_playback_lock) {
            xSemaphoreGive(s_playback_lock);
        }

        free(frame.data);
    }
}

/* ── TX: mic -> encode -> binary frame -> socket ────────────────────────── */

static esp_err_t send_listen_state(const char *state, const char *mode)
{
    cJSON *root = cJSON_CreateObject();
    ESP_RETURN_ON_FALSE(root, ESP_ERR_NO_MEM, TAG, "out of memory");

    const char *sid = mpx_voice_session_id();
    if (sid && sid[0]) {
        cJSON_AddStringToObject(root, "session_id", sid);
    }
    cJSON_AddStringToObject(root, "type", "listen");
    cJSON_AddStringToObject(root, "state", state);
    if (strcmp(state, "start") == 0) {
        cJSON_AddNumberToObject(root, "listen_id", (double)s_talk_listen_id);
    }
    if (mode) {
        cJSON_AddStringToObject(root, "mode", mode);
    }

    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    ESP_RETURN_ON_FALSE(text, ESP_ERR_NO_MEM, TAG, "out of memory");

    const esp_err_t err = mpx_voice_send_json(text);
    free(text);
    return err;
}

/* ── End of speech, heard here ──────────────────────────────────────────
 * An auto-mode turn relies on the server's VAD to say "you are done" (stt or
 * tts start). When the gateway's VAD does not close the utterance, the turn
 * ran to its time limit every time (12 s, or 90 s on a follow-up) and only
 * the "stop" sent then made the server transcribe it. So the frames are also
 * measured here: once speech has been heard, END_SILENCE_FRAMES of quiet end
 * the turn with "stop" -- the server then transcribes at once. A room too
 * loud to tell speech from quiet simply never triggers it (the limit and the
 * server's own VAD still apply). */
#define DEV_VAD_MIN_RMS          250.0f   /* int16 RMS: never call quieter than this speech */
#define DEV_VAD_SPEECH_X         3.0f     /* speech: this many times the noise floor */
#define DEV_VAD_SPEECH_FRAMES    5        /* 300 ms of speech before the turn can end */
#define DEV_VAD_SETTLE_FRAMES    8        /* the first 480 ms only learn the room: the codec's start-up burst,
                                           * the start beep and the end of the robot's own reply are not you */
#define DEV_VAD_END_SILENCE_FRAMES 17     /* ~1 s of quiet after speech ends the turn (a pause to think does not) */

typedef struct {
    float floor;          /* the room's level (RMS), learned from the quiet frames */
    int frames;           /* frames measured */
    int speech;           /* speech frames heard */
    int quiet;            /* quiet frames in a row since the last speech frame */
} dev_vad_t;

static float frame_rms(const int16_t *pcm, size_t n)
{
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        acc += (double)pcm[i] * (double)pcm[i];
    }
    return n ? (float)__builtin_sqrt(acc / (double)n) : 0.0f;
}

/* One 60 ms frame; true when speech was heard and has now ended. */
static bool dev_vad_update(dev_vad_t *v, const int16_t *pcm, size_t n)
{
    const float rms = frame_rms(pcm, n);
    if (v->frames++ == 0) {
        v->floor = rms < 150.0f ? rms : 150.0f;   /* capped: you may already be talking in the first frame */
    }
    if (v->frames <= DEV_VAD_SETTLE_FRAMES) {
        if (rms < v->floor) v->floor = rms;       /* the quietest of the settling frames */
        return false;
    }
    const float thr = v->floor * DEV_VAD_SPEECH_X > DEV_VAD_MIN_RMS ? v->floor * DEV_VAD_SPEECH_X : DEV_VAD_MIN_RMS;
    if (rms > thr) {
        v->speech++;
        v->quiet = 0;
        return false;
    }
    /* quiet: follow the room's level (faster down than up) */
    v->floor += (rms - v->floor) * (rms < v->floor ? 0.3f : 0.05f);
    if (v->speech >= DEV_VAD_SPEECH_FRAMES) {
        v->quiet++;
    }
    return v->speech >= DEV_VAD_SPEECH_FRAMES && v->quiet >= DEV_VAD_END_SILENCE_FRAMES;
}

/* ── Which of the codec's channels is the microphone ─────────────────────
 * The talk capture opens the codec's 4 TDM channels and used to take a fixed
 * slot (TALK_CAPTURE_PICK). The slot order is not the same on every open:
 * some turns got the reference or an unused input, the server heard nothing,
 * and the conversation died after one reply on some boots and not others.
 * So every turn measures each channel's level (its RMS about its mean) and
 * streams the loudest, starting from TALK_CAPTURE_PICK and moving only to a
 * channel clearly louder (MIC_PICK_SWITCH_X, plus a margin) -- speech decides
 * it within a frame or two. The levels are logged so a layout change shows. */
#define MIC_PICK_MAX_CH     4
#define MIC_PICK_SWITCH_X   2.0f
#define MIC_PICK_MARGIN     50.0f
#define MIC_PICK_LOG_FRAME  5      /* log the levels once, 300 ms in */

static volatile uint8_t s_mic_slot = TALK_CAPTURE_PICK;   /* the last turn's microphone channel */

uint8_t mpx_voice_talk_mic_slot(void)
{
    return s_mic_slot;
}

typedef struct {
    float lvl[MIC_PICK_MAX_CH];
    uint8_t pick;
    uint32_t frames;
} mic_pick_t;

/* True when it moved to another channel this frame. */
static bool mic_pick_update(mic_pick_t *m, const int16_t *raw, size_t n, uint8_t nch)
{
    bool moved = false;
    if (nch > MIC_PICK_MAX_CH) nch = MIC_PICK_MAX_CH;
    for (uint8_t c = 0; c < nch; c++) {
        double sum = 0.0, sq = 0.0;
        for (size_t i = 0; i < n; i++) {
            const double x = raw[i * nch + c];
            sum += x;
            sq += x * x;
        }
        const double mean = sum / (double)n;
        const double var = sq / (double)n - mean * mean;
        const float rms = var > 0.0 ? (float)__builtin_sqrt(var) : 0.0f;
        m->lvl[c] = m->frames ? m->lvl[c] * 0.8f + rms * 0.2f : rms;
    }
    m->frames++;
    /* Only the microphones are candidates: this board's 4-channel capture is R,M,N,M (slot 0 the speaker's
     * reference, 2 unused), so music or the end of a reply on slot 0 can never be streamed as "you". */
    uint8_t best = m->pick;
    for (uint8_t c = 0; c < nch; c++) {
        if (nch == 4 && c != 1 && c != 3) continue;
        if (m->lvl[c] > m->lvl[best]) best = c;
    }
    if (best != m->pick && m->lvl[best] > MIC_PICK_SWITCH_X * m->lvl[m->pick] + MIC_PICK_MARGIN) {
        ESP_LOGI(TAG, "talk mic: slot %u -> %u (levels %.0f / %.0f / %.0f / %.0f)", (unsigned)m->pick,
                 (unsigned)best, (double)m->lvl[0], (double)m->lvl[1], (double)(nch > 2 ? m->lvl[2] : 0.0f),
                 (double)(nch > 3 ? m->lvl[3] : 0.0f));
        m->pick = best;
        moved = true;
        s_mic_slot = best;
    }
    if (m->frames == MIC_PICK_LOG_FRAME) {
        ESP_LOGI(TAG, "talk mic: slot %u (levels %.0f / %.0f / %.0f / %.0f)", (unsigned)m->pick,
                 (double)m->lvl[0], (double)m->lvl[1], (double)(nch > 2 ? m->lvl[2] : 0.0f),
                 (double)(nch > 3 ? m->lvl[3] : 0.0f));
    }
    return moved;
}

#if CONFIG_APP_VOICE_MIC_LOG_ENABLE
#define MIC_LOG_WAVE_BINS 12

static float mic_dbfs(float amplitude)
{
    return amplitude > 0.0f ? 20.0f * log10f(amplitude / 32768.0f) : -96.0f;
}

/* A compact envelope, not a sample-by-sample oscilloscope trace: each bin is
 * the peak of 5 ms from the normal 60 ms uplink frame. This shows speech-like
 * bursts while keeping the real-time task's serial output bounded. */
static void mic_log_frame(const int16_t *pcm, size_t n, uint32_t frames_sent, uint8_t slot,
                          const float *levels, const char *source)
{
    static const uint16_t thresholds[9] = { 50, 100, 250, 500, 1000, 2000, 4000, 8000, 16000 };
    static const char shades[] = " .:-=+*#%@";
    char wave[MIC_LOG_WAVE_BINS + 1];
    int frame_peak = 0;

    for (size_t bin = 0; bin < MIC_LOG_WAVE_BINS; bin++) {
        const size_t begin = bin * n / MIC_LOG_WAVE_BINS;
        const size_t end = (bin + 1) * n / MIC_LOG_WAVE_BINS;
        int peak = 0;
        for (size_t i = begin; i < end; i++) {
            const int sample = pcm[i];
            const int magnitude = sample < 0 ? -sample : sample;
            if (magnitude > peak) peak = magnitude;
        }
        if (peak > frame_peak) frame_peak = peak;
        size_t shade = 0;
        while (shade < 9 && peak >= thresholds[shade]) shade++;
        wave[bin] = shades[shade];
    }
    wave[MIC_LOG_WAVE_BINS] = '\0';

    const double rms_db = mic_dbfs(frame_rms(pcm, n));
    const double peak_db = mic_dbfs((float)frame_peak);
    ESP_LOGI(TAG, "voice mic id=%" PRIu32 " frame=%" PRIu32
                  " source=%s slot=%u rms=%.1f dBFS peak=%.1f dBFS"
                  " levels=%.0f/%.0f/%.0f/%.0f |%s|",
             s_talk_listen_id, frames_sent, source, (unsigned)slot, rms_db, peak_db,
             levels ? (double)levels[0] : 0.0, levels ? (double)levels[1] : 0.0,
             levels ? (double)levels[2] : 0.0, levels ? (double)levels[3] : 0.0, wave);
    mpx_voice_stream_broadcast_mic_debug(s_talk_listen_id, frames_sent, slot,
                                         (float)rms_db, (float)peak_db, levels, source, wave);
}
#endif

/* The stream moved to another microphone channel: its level is not the old one's, so start the speech count over
 * (a louder channel must not look like the start of speech, nor a quieter one like its end). */
static void dev_vad_new_channel(dev_vad_t *v)
{
    if (v->floor > 150.0f) v->floor = 150.0f;
    v->speech = 0;
    v->quiet = 0;
}

static void talk_task(void *arg)
{
    const uint32_t max_seconds = (uint32_t)(uintptr_t)arg;
    const uint32_t max_frames = max_seconds ? (max_seconds * 1000u / MPX_VOICE_FRAME_MS) : 0;
    bool phone = s_talk_phone;
    const bool barge_in = s_talk_barge;
    const mpx_voice_pcm_reader_t reader = s_talk_reader;
    void *reader_ctx = s_talk_reader_ctx;
    int16_t pcm[MPX_VOICE_UPLINK_FRAME];
    uint8_t packet[MPX_VOICE_PACKET_MAX];
    uint32_t frames_sent = 0;
    dev_vad_t vad = { 0 };
    bool vad_ended = false;
    mic_pick_t mic = { .pick = (s_mic_slot == 1 || s_mic_slot == 3) ? s_mic_slot : TALK_CAPTURE_PICK };   /* last turn's */
    int16_t *raw = NULL;          /* all the codec's channels, when it delivers more than one */
    uint8_t raw_ch = 1;

    if (!phone && !barge_in) {
        const esp_err_t open_err = mpx_audio_capture_start(MPX_VOICE_UPLINK_RATE,
                                                            TALK_CAPTURE_CHANNELS,
                                                            TALK_CAPTURE_PICK);
        if (open_err != ESP_OK) {
            ESP_LOGE(TAG, "capture start failed: %s", esp_err_to_name(open_err));
            s_talk_active = false;
            xSemaphoreGive(s_talk_done);
            vTaskDeleteWithCaps(NULL);
            return;
        }
    } else if (phone) {
        mpx_phone_audio_set_streaming(true);
    }
    if (!barge_in) {
        raw_ch = TALK_CAPTURE_CHANNELS;
        raw = heap_caps_malloc(sizeof(int16_t) * MPX_VOICE_UPLINK_FRAME * raw_ch, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!raw) {
            ESP_LOGW(TAG, "no memory to measure the mic channels -- slot %d fixed", TALK_CAPTURE_PICK);
        }
    }

    const bool auto_mode = s_talk_auto;
    const char *mode = barge_in ? "barge_in" : (auto_mode ? "auto" : "manual");
    (void)send_listen_state("start", mode);
    ESP_LOGI(TAG, "listening -- speak now (%s)",
             barge_in      ? "full-duplex, server VAD confirms barge-in" :
             auto_mode     ? "the server decides when you are done" :
             max_seconds    ? "will stop automatically" :
                              "run `voice talk stop` when done");

    while (!s_talk_stop_requested &&
           !(auto_mode && s_server_turn) &&
           (max_frames == 0 || frames_sent < max_frames)) {
        const bool want_phone = !barge_in && s_talk_phone && mpx_phone_audio_mic_enabled();
        if (want_phone != phone) {
            /* The PWA toggle may arrive after this task opened I2S. Move the
             * live turn rather than filling a phone ring nobody reads. */
            if (phone) mpx_phone_audio_set_streaming(false);
            else mpx_audio_capture_stop();
            phone = want_phone;
            if (phone) {
                mpx_phone_audio_set_streaming(true);
                ESP_LOGI(TAG, "live turn switched to phone microphone");
            } else {
                const esp_err_t open_err = mpx_audio_capture_start(
                    MPX_VOICE_UPLINK_RATE, TALK_CAPTURE_CHANNELS, TALK_CAPTURE_PICK);
                if (open_err != ESP_OK) {
                    ESP_LOGE(TAG, "robot microphone restart failed: %s",
                             esp_err_to_name(open_err));
                    break;
                }
                ESP_LOGI(TAG, "live turn switched to robot microphone");
            }
        }
        if (phone) {
            /* Timeout, not failure: the phone may simply not have sent the
             * next 60 ms yet, and a short read is padded by the ring's
             * staging buffer instead of truncated mid-frame. */
            if (mpx_phone_audio_read(pcm, MPX_VOICE_UPLINK_FRAME, 200) == 0) {
                continue;
            }
        } else if (barge_in) {
            const size_t got = reader(pcm, MPX_VOICE_UPLINK_FRAME, 200, reader_ctx);
            if (got == 0) {
                continue;
            }
            if (got != MPX_VOICE_UPLINK_FRAME) {
                ESP_LOGW(TAG, "processed capture returned a partial frame -- stopping");
                break;
            }
        } else if (raw && mpx_audio_capture_channels() == raw_ch) {
            if (mpx_audio_capture_read_raw(raw, MPX_VOICE_UPLINK_FRAME) != ESP_OK) {
                ESP_LOGW(TAG, "capture read failed -- stopping");
                break;
            }
            if (mic_pick_update(&mic, raw, MPX_VOICE_UPLINK_FRAME, raw_ch)) {
                dev_vad_new_channel(&vad);
            }
            for (size_t i = 0; i < MPX_VOICE_UPLINK_FRAME; i++) {
                pcm[i] = raw[i * raw_ch + mic.pick];
            }
        } else if (mpx_audio_capture_read(pcm, MPX_VOICE_UPLINK_FRAME) != ESP_OK) {
            ESP_LOGW(TAG, "capture read failed -- stopping");
            break;
        }
        const int packet_len = mpx_voice_encode(pcm, MPX_VOICE_UPLINK_FRAME, packet, sizeof(packet));
        if (packet_len > 0) {
            if (mpx_voice_send_binary(packet, (size_t)packet_len) != ESP_OK) {
                ESP_LOGW(TAG, "send failed -- socket down? stopping");
                break;
            }
        }
        frames_sent++;
#if CONFIG_APP_VOICE_MIC_LOG_ENABLE
        if (frames_sent % CONFIG_APP_VOICE_MIC_LOG_INTERVAL == 0) {
            mic_log_frame(pcm, MPX_VOICE_UPLINK_FRAME, frames_sent, mic.pick,
                          raw ? mic.lvl : NULL, phone ? "phone" : (barge_in ? "afe" : "robot"));
        }
#endif
        if (auto_mode && !barge_in && dev_vad_update(&vad, pcm, MPX_VOICE_UPLINK_FRAME)) {
            vad_ended = true;   /* you stopped talking: "stop" below makes the server transcribe now */
            break;
        }
    }

    if (raw) {
        heap_caps_free(raw);
    }
    if (phone) {
        mpx_phone_audio_set_streaming(false);
    } else if (!barge_in) {
        mpx_audio_capture_stop();
    }
    /* In auto mode the server has already decided the utterance is over by
     * the time it answers; telling it "stop" on top of that is at best
     * redundant and on some servers cancels the reply it is producing. Only
     * send it when WE ended the listen -- timeout, or an explicit stop. */
    if (!(auto_mode && s_server_turn)) {
        (void)send_listen_state("stop", NULL);
    }
    ESP_LOGI(TAG, "stopped listening (%" PRIu32 " frames sent, %.1fs%s)",
             frames_sent, (double)frames_sent * MPX_VOICE_FRAME_MS / 1000.0,
             (auto_mode && s_server_turn) ? ", server took its turn" :
             vad_ended ? ", end of speech heard here" : "");

    s_talk_active = false;
    s_talk_barge = false;
    s_talk_reader = NULL;
    s_talk_reader_ctx = NULL;
    xSemaphoreGive(s_talk_done);
    vTaskDeleteWithCaps(NULL);
}

static esp_err_t talk_start_common(uint32_t max_seconds, bool auto_mode, bool phone,
                                   bool barge_in, mpx_voice_pcm_reader_t reader, void *reader_ctx)
{
    ESP_RETURN_ON_FALSE(mpx_voice_state() == MPX_VOICE_READY, ESP_ERR_INVALID_STATE, TAG,
                        "not connected -- run `voice connect` first");
    ESP_RETURN_ON_FALSE(!s_talk_active, ESP_ERR_INVALID_STATE, TAG, "already talking");
    ESP_RETURN_ON_FALSE(!phone || mpx_phone_audio_mic_enabled(), ESP_ERR_INVALID_STATE, TAG,
                        "phone microphone is not enabled");
    ESP_RETURN_ON_FALSE(!barge_in || reader, ESP_ERR_INVALID_ARG, TAG,
                        "barge-in needs a processed PCM reader");
    ESP_RETURN_ON_FALSE(!barge_in || mpx_audio_capture_active(), ESP_ERR_INVALID_STATE, TAG,
                        "barge-in needs the AFE microphone active");
    /* The microphone is a single hardware stream (see mpx_audio_capture_active()'s
     * doc comment) -- if mpx_voice_wake's `wake start` already has it open,
     * talk_task's own capture_start() below would not fail, it would just
     * silently interleave reads with wake's, which is worse than an error
     * here. */
    ESP_RETURN_ON_FALSE(phone || barge_in || !mpx_audio_capture_active(), ESP_ERR_INVALID_STATE, TAG,
                        "microphone busy -- stop `wake` first (`wake stop`)");
    ESP_RETURN_ON_ERROR(mpx_voice_stream_init(), TAG, "stream init failed");

    /* Decode at MPX_VOICE_PLAYBACK_RATE regardless of what rate the server
     * said it encodes at (see the header): Opus resamples on the way out,
     * and the I2S peripheral will not run the speaker at a different rate
     * from the microphone the AFE front end keeps open. The loopback
     * test opens the codec at the same rates, so "already running" is the
     * same configuration either way. */
    if (!mpx_voice_codec_running()) {
        ESP_RETURN_ON_ERROR(mpx_voice_codec_start(MPX_VOICE_UPLINK_RATE, MPX_VOICE_PLAYBACK_RATE),
                            TAG, "codec start failed");
    }

    /* A previous talk that nobody waited for leaves its "done" token in the
     * semaphore; take it now so mpx_voice_talk_wait() below waits for THIS
     * talk and not a stale one. */
    (void)xSemaphoreTake(s_talk_done, 0);

    s_talk_stop_requested = false;
    s_talk_auto = auto_mode;
    s_talk_phone = phone;
    s_talk_barge = barge_in;
    s_talk_reader = reader;
    s_talk_reader_ctx = reader_ctx;
    s_barge_accepted = false;
    s_listen_counter++;
    if (s_listen_counter == 0) {
        s_listen_counter = 1;       /* zero means no usable generation ID */
    }
    s_talk_listen_id = s_listen_counter;
    /* After an interrupt, keep discarding in-flight old-reply packets across
     * the processed-talk handoff. Only the next tts start may reopen RX. */
    if (!barge_in) {
        s_drop_reply = false;
    }
    s_server_turn = false;
    s_talk_active = true;

    /* Encode shares core 0 with decode; AFE keeps core 1. See init above. */
    BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(talk_task, "voice_talk", VOICE_STREAM_TASK_STACK,
                                        (void *)(uintptr_t)max_seconds, 5, NULL, 0,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCoreWithCaps(talk_task, "voice_talk", VOICE_STREAM_TASK_STACK,
                                 (void *)(uintptr_t)max_seconds, 5, NULL, 0,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (ok != pdPASS) {
        s_talk_active = false;
        s_talk_barge = false;
        s_talk_reader = NULL;
        s_talk_reader_ctx = NULL;
        ESP_LOGE(TAG, "could not create the talk task: %d bytes of stack is "
                      "more than either heap has left", VOICE_STREAM_TASK_STACK);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t mpx_voice_talk_start(uint32_t max_seconds)
{
    return talk_start_common(max_seconds, false, false, false, NULL, NULL);
}

esp_err_t mpx_voice_talk_start_auto(uint32_t max_seconds)
{
    return talk_start_common(max_seconds, true, false, false, NULL, NULL);
}

esp_err_t mpx_voice_talk_start_phone(uint32_t max_seconds)
{
    /* Same auto listen mode as an IO21/PWA turn: the gateway's VAD decides
     * when the phone user stopped talking. */
    return talk_start_common(max_seconds, true, true, false, NULL, NULL);
}

esp_err_t mpx_voice_talk_start_barge_in(mpx_voice_pcm_reader_t reader, void *ctx,
                                         uint32_t max_seconds)
{
    return talk_start_common(max_seconds, true, false, true, reader, ctx);
}

esp_err_t mpx_voice_talk_use_phone(void)
{
    ESP_RETURN_ON_FALSE(s_talk_active, ESP_ERR_INVALID_STATE, TAG, "no active turn");
    ESP_RETURN_ON_FALSE(mpx_phone_audio_mic_enabled(), ESP_ERR_INVALID_STATE, TAG,
                        "phone microphone is not enabled");
    ESP_RETURN_ON_FALSE(!s_talk_barge, ESP_ERR_INVALID_STATE, TAG,
                        "AFE barge-in capture cannot switch to the phone");
    /* talk_task performs the I2S/ring handoff on its next 60 ms iteration. */
    s_talk_phone = true;
    return ESP_OK;
}

esp_err_t mpx_voice_talk_wait(uint32_t timeout_ms)
{
    if (!s_talk_active || !s_talk_done) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_talk_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    /* Put it back for anyone else waiting (mpx_voice_talk_stop() takes it
     * too); the next talk_start clears it. */
    (void)xSemaphoreGive(s_talk_done);
    return ESP_OK;
}

void mpx_voice_talk_stop(void)
{
    if (!s_talk_active) {
        return;
    }
    s_talk_stop_requested = true;
    /* The task notices within one capture read -- ~60ms -- so this is a
     * short wait, not a real block; the timeout is only so a wedged capture
     * driver cannot hang the console. */
    if (xSemaphoreTake(s_talk_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
        ESP_LOGW(TAG, "talk task did not confirm stop within 2s");
    }
}

bool mpx_voice_talk_active(void)
{
    return s_talk_active;
}
