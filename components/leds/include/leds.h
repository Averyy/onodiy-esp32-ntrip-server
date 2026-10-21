/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The board's LEDs (components/board: a board without LEDs has nothing to
 * drive, and every call is a no-op):
 *  - the status LED (RGB) shows the blink codes of led_core.h;
 *  - the Wi-Fi LED is lit while the station has an address, brighter with a
 *    stronger signal;
 *  - the GNSS LED follows the receiver's status input (lit while it is
 *    high: the receiver has a position fix).
 * A 25 ms timer updates them. Until the first leds_set() the status LED shows
 * the start pattern.
 */
#pragma once

#include "esp_err.h"
#include "leds_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** After board_init(): configure the board's LED pins and start the timer. ESP_OK on a board without LEDs. */
esp_err_t leds_init(void);

/** What to show from now on (health, once a second). */
void leds_set(const led_state_t *s);

#ifdef __cplusplus
}
#endif
