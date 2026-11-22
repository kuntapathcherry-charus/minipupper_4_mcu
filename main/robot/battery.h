/* battery.h - the battery voltage, read from the AT32 servo boards.
 *
 * The MP4-ESP32-CORE has no battery sense of its own (no divider from Vbat+
 * to an ESP32 pin). Each AT32 servo board measures its VIN (= Vbat+, the
 * servo connectors sit on it) with a divider on PA5 and, with AT32 firmware
 * that has it, answers live value 14 (DB_LIVE_SUPPLY_VOLTAGE_V) in volts.
 * Older AT32 firmware answers that id with something else (it clamps an
 * unknown id to 0, the pot reading), so only plausible voltages count: a board
 * without the feature is simply left out, and with no board reporting the
 * voltage is unknown and nothing is refused because of it.
 *
 * A low-priority task reads the boards every 10 s while nothing timing
 * critical runs (an RL run, a table move, a jump, the Pi in control).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     valid;          /* at least one board reported a plausible VIN          */
    float    volts;          /* the mean of the boards that did                       */
    int      percent;        /* 0..100 from a Li-ion curve (APP_BATTERY_CELLS), -1 if !valid */
    uint8_t  boards;         /* bit b: board b (0..3) reported a plausible VIN        */
    float    board_v[4];     /* each board's reading, NAN where none                 */
    uint32_t age_ms;         /* since that reading                                    */
} mpx_battery_t;

/** Start the sampler task (after the robot is up). Safe to call once. */
void mpx_battery_start(void);

/** Read the boards now (a few ms on the servo bus). False when none reports a voltage. */
bool mpx_battery_refresh(void);

/** The last reading. False (and out->valid false) when there is none. */
bool mpx_battery_get(mpx_battery_t *out);

/**
 * Why a backflip may not run now, or NULL when it may: refused above
 * CONFIG_APP_BACKFLIP_MAX_MV (a full battery makes the servos stronger and the
 * flip over-rotates). Unknown voltage = allowed. refresh: read the boards first
 * when the last reading is older than 15 s (only from a task that may block a
 * few ms, never from the gait task).
 */
const char *mpx_battery_backflip_refusal(char *buf, int len, bool refresh);

/** {"valid":..,"volts":..,"percent":..,"boards":[..],"age_ms":..,"backflip_max_v":..} into buf. */
int mpx_battery_json(char *buf, int len);

#ifdef __cplusplus
}
#endif
