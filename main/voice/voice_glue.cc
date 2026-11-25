/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * See voice_glue.h.
 */

#include "voice_glue.h"

#include <cstdio>
#include <cstring>
#include <atomic>

#include "agent/agent_config.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "voice_glue";

namespace voice_glue {

bool voice_wanted()
{
    /* Voice is the gateway's in both AI modes; only "off" closes the mic. */
    return agent_config::mode() != agent_config::Mode::Off;
}

}  // namespace voice_glue

#if CONFIG_APP_VOICE_ENABLE

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include "cJSON.h"
#include "cap_music.h"
#include "esp_heap_caps.h"
#include "mpx_audio.h"
#include "mpx_robot.h"
#include "mpx_voice_link.h"
#include "mpx_voice_wake.h"
#include "network/chat_ws.h"
#include "voice_brain.h"
#include "voice_tools.h"

#if CONFIG_APP_DISPLAY_ENABLE
#include "cap_display.h"
#endif

extern "C" void mpx_voice_stream_broadcast_mic_debug(
    uint32_t listen_id, uint32_t frame, uint8_t slot, float rms_db, float peak_db,
    const float *levels, const char *source, const char *wave)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "voice_mic");
    cJSON_AddNumberToObject(root, "listen_id", listen_id);
    cJSON_AddNumberToObject(root, "frame", frame);
    cJSON_AddStringToObject(root, "source", source);
    cJSON_AddNumberToObject(root, "slot", slot);
    cJSON_AddNumberToObject(root, "rms_db", rms_db);
    cJSON_AddNumberToObject(root, "peak_db", peak_db);
    cJSON *channel_levels = cJSON_AddArrayToObject(root, "levels");
    for (int i = 0; i < 4; i++) {
        cJSON_AddItemToArray(channel_levels, cJSON_CreateNumber(levels ? levels[i] : 0.0f));
    }
    cJSON_AddStringToObject(root, "wave", wave);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        network::broadcast_voice_to_pwa(json);
        cJSON_free(json);
    }
}

namespace {

enum Event : uint8_t { EV_MASTER_ON = 1, EV_MASTER_OFF };

QueueHandle_t s_events;
bool          s_started;
bool          s_wake_ready;        /* mpx_voice_wake_init() succeeded */
volatile bool s_voice_resume;      /* hands-free was on when the Pi took over */

/* Voice held back for a skill (voice_glue::hold()). Depth, not a flag: the
 * automatic hold a skill run takes and a manual one from POST /v1/voice
 * {"hold":true} can be live together, and only the last release restarts
 * listening. Touched from the skill task, the event task and httpd. */
/* Depth, not a flag, and touched from three tasks (the skill task, the voice
 * event task, httpd): an atomic because hold() and release() are the only
 * writers, and the read-modify-write has to be one step -- two holders both
 * reading depth 0 must not both end up taking a fresh hold. */
std::atomic<int> s_hold_depth{0};
bool         s_hold_restore_auto;  /* hands-free was listening when it started */
bool         s_hold_restore_wake;  /* ... or the front end was up by hand       */
bool         s_hold_restore_afe;   /* policy destroyed the reserved front end */
int          s_task_priority = 5;  /* wake_feed/wake_fetch, see set_task_priority */
StaticSemaphore_t s_hold_mutex_buf;
SemaphoreHandle_t s_hold_mutex;
portMUX_TYPE s_hold_init_mux = portMUX_INITIALIZER_UNLOCKED;

/* MCP conversations live on the gateway, so voice_brain's local history
 * events never fire. Mirror only those gateway turns to the PWA; the panel
 * keeps them in memory and nothing is written to LittleFS or localStorage. */
void broadcast_gateway_turn(const char *role, const char *text)
{
    if (mpx_voice_device_brain() || !text || !text[0]) return;
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "voice_live");
    cJSON_AddStringToObject(root, "role", role);
    cJSON_AddStringToObject(root, "text", text);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        network::broadcast_voice_to_pwa(json);
        cJSON_free(json);
    }
}

void broadcast_gateway_state(const char *state)
{
    if (mpx_voice_device_brain()) return;
    cJSON *root = cJSON_CreateObject();
    if (!root) return;
    cJSON_AddStringToObject(root, "type", "voice_live_state");
    cJSON_AddStringToObject(root, "state", state);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        network::broadcast_voice_to_pwa(json);
        cJSON_free(json);
    }
}

void voice_heard(const char *text)
{
#if CONFIG_APP_DISPLAY_ENABLE
    cap_display_voice_heard(text);
#endif
    broadcast_gateway_turn("user", text);
}

void voice_saying(const char *text)
{
#if CONFIG_APP_DISPLAY_ENABLE
    cap_display_voice_saying(text);
#endif
    broadcast_gateway_turn("assistant", text);
}

void voice_emotion(const char *name)
{
#if CONFIG_APP_DISPLAY_ENABLE
    cap_display_voice_emotion(name);
#else
    (void)name;
#endif
}

void voice_reply_done()
{
#if CONFIG_APP_DISPLAY_ENABLE
    cap_display_voice_reply_done();
#endif
    broadcast_gateway_state("listening");
}

void voice_session_ended()
{
#if CONFIG_APP_DISPLAY_ENABLE
    cap_display_voice_session_ended();
#endif
    broadcast_gateway_state("ended");
}

void lock_hold()
{
    portENTER_CRITICAL(&s_hold_init_mux);
    if (!s_hold_mutex) s_hold_mutex = xSemaphoreCreateMutexStatic(&s_hold_mutex_buf);
    portEXIT_CRITICAL(&s_hold_init_mux);
    xSemaphoreTake(s_hold_mutex, portMAX_DELAY);
}

void unlock_hold()
{
    xSemaphoreGive(s_hold_mutex);
}

bool hands_free_wanted()
{
    return voice_glue::voice_wanted() && s_wake_ready;
}

esp_err_t configure_brain()
{
    if (voice_brain::wanted()) {
        const esp_err_t err = voice_brain::start();
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "local voice brain unavailable (%s); using the gateway's MCP brain",
                 esp_err_to_name(err));
    } else {
        voice_brain::stop();
    }
    return voice_tools::start();
}

esp_err_t hands_free_start_locked()
{
    if (s_hold_depth > 0) {
        /* A skill has the robot. Remember, and let the last release() start
         * it -- starting now would fight the skill for the microphone and
         * the CPU, which is the whole point of the hold. */
        s_hold_restore_auto = true;
        ESP_LOGI(TAG, "hands-free listening waits: voice is held for a skill");
        return ESP_OK;
    }
    if (!hands_free_wanted()) return ESP_ERR_INVALID_STATE;
    if (mpx_robot_pi_in_control()) {
        /* The Pi has the robot: voice stays off until it lets go. */
        s_voice_resume = true;
        ESP_LOGI(TAG, "hands-free listening waits: the Raspberry Pi is in control");
        return ESP_OK;
    }
    return mpx_voice_wake_auto_start();
}

/* The Pi link task must not block, so the stopping (microphone, display
 * lock inside music/voice teardown) happens here. */
void events_task(void *)
{
    uint8_t ev;
    for (;;) {
        if (xQueueReceive(s_events, &ev, portMAX_DELAY) != pdTRUE) continue;
        switch (ev) {
        case EV_MASTER_ON:
            ESP_LOGW(TAG, "Raspberry Pi (ROS) in control: voice and music off");
            if (cap_music_active()) cap_music_stop();
            if (mpx_voice_wake_auto_active()) {
                s_voice_resume = true;
                mpx_voice_wake_auto_stop();
            }
            break;
        case EV_MASTER_OFF:
            if (s_voice_resume) {
                s_voice_resume = false;
                /* Through hands_free_start_locked() rather than
                 * mpx_voice_wake_auto_start() directly, so a skill that is
                 * running when the Pi lets go keeps its hold. */
                if (hands_free_start_locked() != ESP_OK) {
                    ESP_LOGW(TAG, "hands-free listening did not restart -- POST /v1/voice {\"hands_free\":true}");
                }
            }
            break;
        default:
            break;
        }
    }
}

}  // namespace

namespace voice_glue {

bool available()
{
    return mpx_audio_have_mic();
}

esp_err_t apply_mode()
{
    ESP_LOGW(TAG, "AI mode -> %s", agent_config::mode_name(agent_config::mode()));
    if (!s_started) return ESP_OK;
    if (!voice_wanted()) {
        voice_brain::stop();
        if (cap_music_active()) cap_music_stop();
        mpx_voice_wake_auto_stop();
        mpx_voice_disconnect();
        s_voice_resume = false;
        /* Nothing to come back to: an "off" while a skill is running must not
         * make the skill's release() turn listening back on. */
        s_hold_restore_auto = false;
        s_hold_restore_wake = false;
        return ESP_OK;
    }
    if (!available()) return ESP_ERR_NOT_SUPPORTED;
    if (configure_brain() != ESP_OK) {
        ESP_LOGW(TAG, "voice brain did not start");
    }
    /* Off -> on: the front end was never built if the robot booted with
     * voice off, so this goes through the init-if-needed path. While a skill
     * holds voice this defers (hands_free_start()/locked) and the release
     * starts it. */
    if (mpx_voice_wake_auto_active()) return ESP_OK;
    return hands_free_start();
}

void prepare()
{
    if (s_wake_ready || !available()) return;

    /* Reserve the large ESP-SR allocations before ESP-Claw. In this esp-sr
     * version an OOM inside AFE/model creation dereferences a null
     * buffer, so merely checking create_from_config's return is too late. */
    mpx_voice_wake_set_task_priority(s_task_priority);
    if (mpx_voice_wake_init() != ESP_OK) {
        ESP_LOGW(TAG, "voice front end reservation failed");
        return;
    }
    s_wake_ready = true;
    ESP_LOGI(TAG, "voice front end reserved: internal RAM %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

void start()
{
    if (s_started) return;
    s_started = true;

    if (!available()) {
        ESP_LOGW(TAG, "no microphone: voice unavailable");
        return;
    }

    /* The Pi events queue first: a Pi taking control during boot must be
     * seen even before the front end is up. */
    s_events = xQueueCreate(4, sizeof(uint8_t));
    // PSRAM stack (XIP is on, see PORT_NOTES "The PSRAM-stack rule").
    if (!s_events || xTaskCreateWithCaps(events_task, "voice_ev", 4096, nullptr, 3, nullptr,
                                         MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGW(TAG, "voice events task did not start");
    }

    /* One hook table feeds both the optional face and the MCP live transcript. */
    static const mpx_voice_ui_hooks_t voice_hooks = {
        .heard         = voice_heard,
        .saying        = voice_saying,
        .emotion       = voice_emotion,
        .reply_done    = voice_reply_done,
        .session_ended = voice_session_ended,
    };
    mpx_voice_set_ui_hooks(&voice_hooks);

    /* Music works in every mode -- with voice off it is the app's
     * (POST /v1/voice {"music":...}). */
    cap_music_init();

    /* The front end's task priority, before anything starts a task so the
     * first start already uses the caller's choice. */
    mpx_voice_wake_set_task_priority(s_task_priority);

    if (!voice_wanted()) {
        ESP_LOGW(TAG, "AI is off: voice disabled, the microphone stays closed");
        return;
    }

    /* Device brain: ESP-Claw answers and the gateway is STT/TTS. Server
     * brain: the gateway's model answers through the MCP tool bridge. */
    if (configure_brain() != ESP_OK) {
        ESP_LOGW(TAG, "voice brain did not start -- voice turns cannot use tools");
    }

    /* prepare() normally reserved the AFE before the agent. This call also
     * keeps direct/retry users working; s_afe_ready prevents a second AFE. */
    if (mpx_voice_wake_init() != ESP_OK) {
        ESP_LOGW(TAG, "voice front-end init did not complete -- voice unavailable this boot");
        return;
    }
    s_wake_ready = true;
    ESP_LOGI(TAG, "after voice front-end init: internal RAM %u free, %u largest; PSRAM %u free, %u largest",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

#if CONFIG_APP_VOICE_AUTO_LISTEN
    /* Hands-free from here on. The socket opens on the first IO21/PWA trigger, by
     * which time Wi-Fi has had every chance to come up. */
    esp_err_t listen_err = hands_free_start_locked();
    if (listen_err != ESP_OK) {
        ESP_LOGW(TAG, "hands-free listening did not start (%s) -- POST /v1/voice {\"hands_free\":true} to retry",
                 esp_err_to_name(listen_err));
    }
#endif
}

void on_master_changed(bool active)
{
    const uint8_t ev = active ? EV_MASTER_ON : EV_MASTER_OFF;
    if (s_events) (void)xQueueSend(s_events, &ev, 0);
}

esp_err_t hands_free_start()
{
    if (!s_started || !available()) return ESP_ERR_NOT_SUPPORTED;
    if (!voice_wanted()) return ESP_ERR_INVALID_STATE;
    if (s_hold_depth > 0) {
        /* Do not spend the model load now: release() will come back through
         * here when the skill is done. */
        s_hold_restore_auto = true;
        ESP_LOGI(TAG, "hands-free listening waits: voice is held for a skill");
        return ESP_OK;
    }
    if (!s_wake_ready) {
        if (mpx_voice_wake_init() != ESP_OK) return ESP_FAIL;
        s_wake_ready = true;
    }
    return hands_free_start_locked();
}

void hands_free_stop()
{
    s_voice_resume = false;
    mpx_voice_wake_auto_stop();
}

esp_err_t wake_now()
{
    const esp_err_t err = hands_free_start();
    if (err != ESP_OK) return err;
    return mpx_voice_wake_trigger();
}

esp_err_t hold(const char *why)
{
    const char *reason = (why && why[0]) ? why : "skill";

    lock_hold();
    int expected = 0;
    if (!s_hold_depth.compare_exchange_strong(expected, 1)) {
        // Only the thread that changes 0 -> 1 performs teardown and records
        // what to restore. Skills, policies and manual holds may arrive together.
        s_hold_depth.fetch_add(1);
        ESP_LOGI(TAG, "voice already held (%d): %s", s_hold_depth.load(), reason);
        unlock_hold();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "voice held for %s (mode %s)", reason,
             agent_config::mode_name(agent_config::mode()));
    if (!s_started) {
        unlock_hold();
        return ESP_OK;   /* nothing is up yet to hold back */
    }

    /* A live turn first: cut the reply and drop the gateway socket, so the
     * skill is not sharing the CPU with whatever model is on the other end
     * of it, and so the server is not left streaming into a device that has
     * stopped listening. */
    if (mpx_voice_stream_reply_active() || mpx_voice_stream_is_playing()) {
        mpx_voice_stream_abort_playback();
    }
    /* A skill must leave the control socket up in either brain mode: the
     * final reply is sent back through it after mpx_run_skill returns. The
     * wake front end, microphone, playback and music are still held below.
     * Pi takeover and manual/policy holds keep the full disconnect. */
    if (strcmp(reason, "skill") != 0) {
        mpx_voice_disconnect();
    }

    /* The speaker is shared: music waiting for it would take it from the
     * skill (or from whatever the skill plays). */
    if (cap_music_active()) {
        cap_music_stop();
    }

    /* Then the microphone. Hands-free first -- it owns the front end and
     * would otherwise restart it behind us -- then a front end started by
     * hand. Both are restarted by release(). */
    esp_err_t err = ESP_OK;
    if (mpx_voice_wake_auto_active()) {
        s_hold_restore_auto = true;
        err = mpx_voice_wake_auto_stop();
    } else if (mpx_voice_wake_active()) {
        s_hold_restore_wake = true;
        err = mpx_voice_wake_stop();
    }
    unlock_hold();
    return err;
}

esp_err_t release_front_end()
{
    lock_hold();
    if (s_hold_depth.load() == 0) {
        unlock_hold();
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t err = mpx_voice_wake_deinit();
    if (err == ESP_OK) {
        s_hold_restore_afe = s_wake_ready;
        s_wake_ready = false;
    }
    unlock_hold();
    return err;
}

void release(const char *why)
{
    const char *reason = (why && why[0]) ? why : "skill";

    lock_hold();
    if (s_hold_depth.load() == 0) {
        ESP_LOGW(TAG, "voice released for %s without a hold -- ignored", reason);
        unlock_hold();
        return;
    }
    if (s_hold_depth.fetch_sub(1) > 1) {
        ESP_LOGI(TAG, "voice still held (%d): %s released", s_hold_depth.load(), reason);
        unlock_hold();
        return;
    }

    const bool restore_auto = s_hold_restore_auto;
    const bool restore_wake = s_hold_restore_wake;
    const bool restore_afe = s_hold_restore_afe;
    s_hold_restore_auto = false;
    s_hold_restore_wake = false;
    s_hold_restore_afe = false;

    /* Restore the boot-time reservation even when no capture was active or
     * AI is currently off; a later mode change must not find a null AFE. */
    if (restore_afe) prepare();
    if (!s_started) {
        unlock_hold();
        return;
    }
    if (!voice_wanted()) {
        ESP_LOGI(TAG, "voice released (%s): AI mode is off, nothing to restart", reason);
        unlock_hold();
        return;
    }

    /* hands_free_start() keeps the AI mode's and the Pi's vetoes, and does the
     * model load if this hold began before the front end was ever built. */
    if (restore_auto && !mpx_voice_wake_auto_active()) {
        const esp_err_t err = hands_free_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "hands-free did not restart after %s (%s) -- POST /v1/voice {\"hands_free\":true}",
                     reason, esp_err_to_name(err));
        }
    } else if (restore_wake && !mpx_voice_wake_active()) {
        const esp_err_t err = mpx_voice_wake_start();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "voice trigger loop did not restart after %s (%s)",
                     reason, esp_err_to_name(err));
        }
    }
    unlock_hold();
}

bool held()
{
    return s_hold_depth > 0;
}

esp_err_t set_task_priority(int prio)
{
    if (prio < 1 || prio > 24) return ESP_ERR_INVALID_ARG;
    s_task_priority = prio;
    mpx_voice_wake_set_task_priority(prio);
    ESP_LOGI(TAG, "voice task priority -> %d", prio);
    return ESP_OK;
}

int task_priority()
{
    return s_task_priority;
}

esp_err_t provision()
{
    if (!available()) return ESP_ERR_NOT_SUPPORTED;
    return mpx_voice_provision(nullptr);
}

std::string status_json()
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "available", available());
    cJSON_AddStringToObject(o, "mode", agent_config::mode_name(agent_config::mode()));
    cJSON_AddBoolToObject(o, "mic", mpx_audio_have_mic());
    cJSON_AddBoolToObject(o, "speaker", mpx_audio_have_speaker());
#ifdef CONFIG_APP_VOICE_PHONE_AUDIO
    cJSON_AddBoolToObject(o, "phone_audio", true);
#else
    cJSON_AddBoolToObject(o, "phone_audio", false);
#endif
    cJSON_AddNumberToObject(o, "volume", mpx_audio_get_volume());
    cJSON_AddBoolToObject(o, "provisioned", mpx_voice_stored_url()[0] != '\0');
    cJSON_AddStringToObject(o, "gateway", MPX_VOICE_DEFAULT_OTA_URL);
    cJSON_AddStringToObject(o, "link", mpx_voice_state_name());
    cJSON_AddStringToObject(o, "hands_free", mpx_voice_wake_state_name());
    /* A skill (or the app) is holding voice back: this is why hands_free can
     * read "off" while the owner expects voice triggers to be armed. */
    cJSON_AddBoolToObject(o, "held", s_hold_depth.load() > 0);
    cJSON_AddNumberToObject(o, "hold_depth", s_hold_depth.load());
    cJSON_AddNumberToObject(o, "task_priority", mpx_voice_wake_task_priority());
    {
        int turn_s = 0, followup_s = 0;
        mpx_voice_wake_get_listen_times(&turn_s, &followup_s);
        cJSON_AddNumberToObject(o, "listen_s", turn_s);
        cJSON_AddNumberToObject(o, "followup_s", followup_s);
    }
    cJSON_AddBoolToObject(o, "listening", mpx_voice_wake_active());
    cJSON_AddBoolToObject(o, "talking", mpx_voice_talk_active());
    cJSON_AddBoolToObject(o, "playing_reply", mpx_voice_stream_is_playing());
    cJSON_AddBoolToObject(o, "pi_in_control", mpx_robot_pi_in_control());
    {
        float level = 0, ring = 0;
        int vad = 0;
        mpx_voice_wake_last_seen(&level, &vad, &ring);
        cJSON *afe = cJSON_AddObjectToObject(o, "afe");
        cJSON_AddBoolToObject(afe, "ready", s_wake_ready);
        cJSON_AddNumberToObject(afe, "level_dbfs", (double)level);
        cJSON_AddBoolToObject(afe, "speech", vad != 0);
        cJSON_AddNumberToObject(afe, "ringbuf_free", (double)ring);
    }
    {
        char line[200];
        cap_music_describe(line, sizeof(line));
        cJSON *music = cJSON_AddObjectToObject(o, "music");
        cJSON_AddBoolToObject(music, "active", cap_music_active());
        cJSON_AddStringToObject(music, "title", cap_music_title());
        cJSON_AddNumberToObject(music, "level", cap_music_level());
        cJSON_AddStringToObject(music, "state", line);
    }
    char *s = cJSON_PrintUnformatted(o);
    std::string out = s ? s : "{}";
    cJSON_free(s);
    cJSON_Delete(o);
    return out;
}

}  // namespace voice_glue

#else  /* !CONFIG_APP_VOICE_ENABLE */

namespace voice_glue {

bool available() { return false; }
esp_err_t apply_mode() { return voice_wanted() ? ESP_ERR_NOT_SUPPORTED : ESP_OK; }
void prepare() {}
void start() { ESP_LOGI(TAG, "voice not compiled in (CONFIG_APP_VOICE_ENABLE=n)"); }
void on_master_changed(bool) {}
esp_err_t hands_free_start() { return ESP_ERR_NOT_SUPPORTED; }
void hands_free_stop() {}
esp_err_t wake_now() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t provision() { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t hold(const char *) { return ESP_OK; }
esp_err_t release_front_end() { return ESP_OK; }
void release(const char *) {}
bool held() { return false; }
esp_err_t set_task_priority(int) { return ESP_ERR_NOT_SUPPORTED; }
int task_priority() { return 0; }
std::string status_json() { return "{\"available\":false,\"mode\":\"off\"}"; }

}  // namespace voice_glue

#endif
