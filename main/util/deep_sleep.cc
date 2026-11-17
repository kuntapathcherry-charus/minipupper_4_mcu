/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sleep (see deep_sleep.h).
 *
 * While the ESP sleeps its pins float, and the servo bus's chip selects and
 * clock go to the four AT32 driver boards, which stay powered. So before
 * sleeping the bus is parked: chip selects high (no board selected), clock
 * and MOSI low (SPI mode 0 idle), held through deep sleep. At the next boot
 * mpx_sleep_release_pins() lets go before driver_board_init() takes the pins.
 */
#include "util/deep_sleep.h"

#include <atomic>
#include <cstring>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"

#include "display/display_hw.h"
#include "robot/board_config.h"
#include "robot/robot.h"
#if CONFIG_APP_POLICY_ENABLE
#include "rl/mpx_policy.h"
#endif

static const char *TAG = "sleep";
static std::atomic<bool> s_going{false};

namespace {

struct ParkedPin { gpio_num_t pin; int level; };
const ParkedPin PARKED[] = {
    { SERVO_SPI_CS_FR_PIN, 1 }, { SERVO_SPI_CS_FL_PIN, 1 },
    { SERVO_SPI_CS_RR_PIN, 1 }, { SERVO_SPI_CS_RL_PIN, 1 },
    { SERVO_SPI_CLK_PIN, 0 },   { SERVO_SPI_MOSI_PIN, 0 },
};

// Wait up to timeout_ms for done() to be true, polling every 50 ms.
template <typename F>
bool wait_for(F done, int timeout_ms)
{
    for (int t = 0; t < timeout_ms; t += 50) {
        if (done()) return true;
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return done();
}

void park_servo_bus()
{
    for (const ParkedPin &p : PARKED) {
        gpio_config_t c = {};
        c.pin_bit_mask = 1ULL << p.pin;
        c.mode = GPIO_MODE_OUTPUT;
        gpio_config(&c);                       // takes the pad back from the SPI peripheral
        gpio_set_level(p.pin, p.level);
        gpio_hold_en(p.pin);
    }
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_en();
#endif
}

void sleep_task(void *)
{
    ESP_LOGW(TAG, "going to sleep: stop, lie down, servos off, deep sleep (WAKE button to start again)");
    vTaskDelay(pdMS_TO_TICKS(300));            // the HTTP reply goes out first

#if CONFIG_APP_POLICY_ENABLE
    // An RL run (or get-up / climb) owns the servos: end it the normal way first.
    mpx_policy_status_t st;
    mpx_policy_get_status(&st);
    if (st.state != MPX_POLICY_IDLE) {
        mpx_policy_stop("sleep");
        const bool idle = wait_for([] {
            mpx_policy_status_t s;
            mpx_policy_get_status(&s);
            return s.state == MPX_POLICY_IDLE;
        }, 10000);
        ESP_LOGI(TAG, "RL run %s", idle ? "ended" : "did not end in 10 s; sleeping anyway");
        (void)idle;
    }
#endif

    // Lie down and switch the servos off (the "Rest now" path). rest_now() only
    // acts from the stand, and a powered-down hold has to be woken first.
    const bool off = wait_for([] {
        if (robot::servos_off()) return true;
        if (robot::servos_holding()) robot::wake_servos();
        else if (!robot::servos_resting()) robot::rest_now();
        return false;
    }, 8000);
    ESP_LOGI(TAG, "servos %s", off ? "off" : "not confirmed off after 8 s; sleeping anyway");
    (void)off;

    // No more servo frames from here, then park the bus for the sleep.
    robot::servo_lock();
    vTaskDelay(pdMS_TO_TICKS(60));             // an exchange already started finishes
    park_servo_bus();

    display_hw_backlight(0);                   // its pull-up (R22) keeps it off asleep
    esp_wifi_stop();

    esp_sleep_enable_ext0_wakeup(WAKE_BUTTON_GPIO, 0);   // SW3 pulls IO21 low (R12 pull-up)
    ESP_LOGW(TAG, "deep sleep now");
    vTaskDelay(pdMS_TO_TICKS(50));
    esp_deep_sleep_start();
}

}  // namespace

extern "C" bool mpx_sleep_start(void)
{
    bool expected = false;
    if (!s_going.compare_exchange_strong(expected, true)) return false;
    if (xTaskCreate(sleep_task, "sleep", 4096, nullptr, 5, nullptr) != pdPASS) {
        s_going = false;
        return false;
    }
    return true;
}

extern "C" void mpx_sleep_release_pins(void)
{
#if SOC_GPIO_SUPPORT_HOLD_IO_IN_DSLP && !SOC_GPIO_SUPPORT_HOLD_SINGLE_IO_IN_DSLP
    gpio_deep_sleep_hold_dis();
#endif
    for (const ParkedPin &p : PARKED) gpio_hold_dis(p.pin);
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
        ESP_LOGI(TAG, "woken by the WAKE button");
    }
}
