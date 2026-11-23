/* battery.cc - the battery voltage from the AT32 servo boards; see battery.h. */
#include "robot/battery.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "robot/driver_board.h"
#include "robot/robot.h"
#if CONFIG_APP_POLICY_ENABLE
#include "rl/mpx_policy.h"
#endif

#ifndef CONFIG_APP_BACKFLIP_MAX_MV
#define CONFIG_APP_BACKFLIP_MAX_MV 12000
#endif
#ifndef CONFIG_APP_BATTERY_CELLS
#define CONFIG_APP_BATTERY_CELLS 3
#endif

namespace {

const char *TAG = "battery";

constexpr float VIN_MIN = 5.0f;     // below: not a voltage (old AT32 firmware answers the pot reading)
constexpr float VIN_MAX = 17.0f;    // the divider's full scale is 16.2 V
constexpr int   SAMPLE_MS = 10000;
constexpr int   STALE_MS = 15000;   // a backflip check reads again past this
constexpr int64_t REPROBE_US = 300LL * 1000000;   // a board without the voltage is asked again every 5 min

// Per board: does its AT32 firmware report the voltage? Older firmware does not (it answers the pot reading for
// an unknown id, or no echo). Such a board is asked once, quietly (driver_board_get_live_quiet: a failure does not
// mute the board for the real configuration requests), and then only every REPROBE_US.
enum class Vin : uint8_t { UNKNOWN, YES, NO };
Vin     s_vin[4] = { Vin::UNKNOWN, Vin::UNKNOWN, Vin::UNKNOWN, Vin::UNKNOWN };
int64_t s_vin_probe_us[4] = {};

portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
mpx_battery_t s_bat = { false, 0.0f, -1, 0, { NAN, NAN, NAN, NAN }, 0 };
int64_t s_read_us = 0;
bool s_logged_none = false;
TaskHandle_t s_task = nullptr;

// One Li-ion cell at rest: volts -> percent (a typical 18650 curve). Under load
// the voltage sags, so read it with the robot standing.
struct Pt { float v; float pct; };
constexpr Pt CURVE[] = {
    { 3.20f, 0 }, { 3.30f, 2 }, { 3.40f, 6 }, { 3.50f, 12 }, { 3.60f, 22 }, { 3.65f, 30 },
    { 3.70f, 40 }, { 3.75f, 48 }, { 3.80f, 56 }, { 3.90f, 68 }, { 4.00f, 80 }, { 4.10f, 90 },
    { 4.15f, 95 }, { 4.20f, 100 },
};

int percent_of(float pack_v)
{
    const float cell = pack_v / static_cast<float>(CONFIG_APP_BATTERY_CELLS);
    constexpr int N = sizeof(CURVE) / sizeof(CURVE[0]);
    if (cell <= CURVE[0].v) return 0;
    if (cell >= CURVE[N - 1].v) return 100;
    for (int i = 1; i < N; ++i) {
        if (cell <= CURVE[i].v) {
            const float u = (cell - CURVE[i - 1].v) / (CURVE[i].v - CURVE[i - 1].v);
            return static_cast<int>(std::lround(CURVE[i - 1].pct + u * (CURVE[i].pct - CURVE[i - 1].pct)));
        }
    }
    return 100;
}

// Something timing critical owns the servo bus: no config traffic now.
bool bus_busy()
{
    if (robot::host::active() || robot::master_active()) return true;   // an RL run / a host, the Pi
#if CONFIG_APP_POLICY_ENABLE
    if (mpx_policy_active()) return true;                                // an RL run, also while it gets ready
#endif
    const robot::GaitCmd cmd = robot::current_gait_cmd();
    if (cmd == robot::GaitCmd::Jump) return true;
    return static_cast<int>(cmd) >= robot::GAIT_BF_BASE;               // backflips and table moves
}

void sampler(void *)
{
    vTaskDelay(pdMS_TO_TICKS(3000));    // let the boards come up first
    for (;;) {
        if (!bus_busy()) mpx_battery_refresh();
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_MS));
    }
}

}  // namespace

extern "C" bool mpx_battery_refresh(void)
{
    mpx_battery_t b = { false, 0.0f, -1, 0, { NAN, NAN, NAN, NAN }, 0 };
    float sum = 0.0f;
    int n = 0;
    const int64_t now = esp_timer_get_time();
    for (int board = 0; board < 4; ++board) {
        if (s_vin[board] == Vin::NO && now - s_vin_probe_us[board] < REPROBE_US) continue;
        float v = NAN;
        // One servo per board (logical 1, 4, 7, 10): VIN is the same for its three.
        const bool ok = driver_board_get_live_quiet(board * 3 + 1, DB_LIVE_SUPPLY_VOLTAGE_V, &v) &&
                        std::isfinite(v) && v >= VIN_MIN && v <= VIN_MAX;
        if (ok) {
            s_vin[board] = Vin::YES;
            b.board_v[board] = v;
            b.boards |= static_cast<uint8_t>(1u << board);
            sum += v;
            ++n;
        } else if (s_vin[board] != Vin::YES) {
            // Never answered with a voltage: older AT32 firmware. Ask again only in 5 min.
            if (s_vin[board] == Vin::UNKNOWN)
                ESP_LOGI(TAG, "servo board %d does not report its supply voltage (older AT32 firmware)", board);
            s_vin[board] = Vin::NO;
            s_vin_probe_us[board] = now;
        }
    }
    if (n > 0) {
        b.valid = true;
        b.volts = sum / static_cast<float>(n);
        b.percent = percent_of(b.volts);
    } else if (!s_logged_none) {
        s_logged_none = true;
        ESP_LOGI(TAG, "no servo board reports its supply voltage (AT32 firmware without live value 14): "
                      "battery unknown, backflips not limited by it");
    }
    portENTER_CRITICAL(&s_mux);
    // Keep the last good reading when this round got none (a board busy, the bus taken).
    if (b.valid || !s_bat.valid) {
        s_bat = b;
        s_read_us = esp_timer_get_time();
    }
    portEXIT_CRITICAL(&s_mux);
    if (b.valid) s_logged_none = false;
    return b.valid;
}

extern "C" bool mpx_battery_get(mpx_battery_t *out)
{
    portENTER_CRITICAL(&s_mux);
    *out = s_bat;
    const int64_t t = s_read_us;
    portEXIT_CRITICAL(&s_mux);
    out->age_ms = t ? static_cast<uint32_t>((esp_timer_get_time() - t) / 1000) : 0;
    return out->valid;
}

extern "C" void mpx_battery_start(void)
{
    if (s_task) return;
    if (xTaskCreatePinnedToCore(sampler, "battery", 3072, nullptr, 2, &s_task, 0) != pdPASS) {
        ESP_LOGW(TAG, "no battery task (out of memory?): the voltage is read only on demand");
        s_task = nullptr;
    }
}

extern "C" const char *mpx_battery_backflip_refusal(char *buf, int len, bool refresh)
{
    if (CONFIG_APP_BACKFLIP_MAX_MV <= 0) return nullptr;
    mpx_battery_t b;
    mpx_battery_get(&b);
    if (refresh && (!b.valid || b.age_ms > STALE_MS) && !bus_busy()) {
        mpx_battery_refresh();
        mpx_battery_get(&b);
    }
    if (!b.valid) return nullptr;                       // unknown: allowed, as before
    const float max_v = CONFIG_APP_BACKFLIP_MAX_MV / 1000.0f;
    if (b.volts <= max_v) return nullptr;
    std::snprintf(buf, len, "battery too full for a backflip: %.2f V, the limit is %.2f V "
                            "(walk a little first)", b.volts, max_v);
    return buf;
}

extern "C" int mpx_battery_json(char *buf, int len)
{
    mpx_battery_t b;
    mpx_battery_get(&b);
    int n = std::snprintf(buf, len, R"({"valid":%s,"volts":%.2f,"percent":%d,"cells":%d,"boards":[)",
                          b.valid ? "true" : "false", b.valid ? b.volts : 0.0, b.percent,
                          CONFIG_APP_BATTERY_CELLS);
    for (int i = 0; i < 4 && n < len; ++i) {
        if (std::isfinite(b.board_v[i])) n += std::snprintf(buf + n, len - n, "%s%.2f", i ? "," : "", b.board_v[i]);
        else n += std::snprintf(buf + n, len - n, "%snull", i ? "," : "");
    }
    if (n < len) {
        n += std::snprintf(buf + n, len - n, R"(],"age_ms":%lu,"backflip_max_v":%.2f})",
                           static_cast<unsigned long>(b.age_ms), CONFIG_APP_BACKFLIP_MAX_MV / 1000.0);
    }
    return n;
}
