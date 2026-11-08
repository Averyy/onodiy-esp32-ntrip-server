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
 *
 * Another product shows its own feedback with leds_set_pattern() instead
 * (leds_pattern.h): the same driver, timer, dimming and pins, so one owner
 * per pin; the station's codes and overlays (alerts, button holds) are then
 * off. leds_set() goes back to the station's codes.
 */
#pragma once

#include "esp_err.h"
#include "leds_pattern.h"
#include "leds_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/** After board_init(): configure the board's LED pins and start the timer. ESP_OK on a board without LEDs. */
esp_err_t leds_init(void);

/** What to show from now on (health, once a second): the station's codes. */
void leds_set(const led_state_t *s);

/**
 * A product's own patterns from now on, in place of the station's codes
 * (copied; any task, also before leds_init(): they show once it runs). A
 * pattern equal to the one showing keeps its phase, so a product may set
 * it again every second; a different one starts from its first steps.
 * ESP_ERR_INVALID_ARG (nothing changes) for NULL, more than
 * LEDS_PATTERN_STEPS steps, a step of 0 ms or an unknown GNSS mode. ESP_OK
 * on a board without LEDs (nothing is driven).
 */
esp_err_t leds_set_pattern(const leds_pattern_t *p);

#ifdef __cplusplus
}
#endif
