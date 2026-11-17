/*
 * SPDX-FileCopyrightText: 2026 MangDang
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sleep: the robot's "off" button. This board cannot switch its own power
 * (the 5 V regulator U9's EN is tied to the battery, sheet 01), so the
 * nearest thing is: stop any RL run, lie down, servos off, screen off, then
 * the ESP32-S3 in deep sleep. The WAKE button (SW3, IO21) or RESET (SW1)
 * starts it again (a normal boot). The regulator, the four driver boards and
 * the servos stay powered and still draw a little from the battery.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Start going to sleep (in its own task) and return at once, so the HTTP
 *  reply can go out first. Returns false if a sleep is already under way. */
bool mpx_sleep_start(void);

/** At boot, before the servo bus starts: let go of the pins held through the
 *  last deep sleep (a no-op after any other reset). */
void mpx_sleep_release_pins(void);

#ifdef __cplusplus
}
#endif
