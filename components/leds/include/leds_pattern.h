/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * A product's own LED feedback (pure types, no IDF includes, also in the
 * host tests): for each of the board's LEDs a short pattern that loops, in
 * place of the station's blink codes (leds_set_pattern). The product decides
 * what each pattern means; the driver only plays them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LEDS_PATTERN_STEPS 8u   /**< steps per pattern at most */

/** The status (RGB) LED: a colour held for ms. */
typedef struct {
    uint8_t r, g, b;   /**< 0-255 each, before the board's inversion (the driver's) */
    uint16_t ms;       /**< 1..65535 */
} leds_rgb_step_t;

/** The Wi-Fi LED (brightness) or the GNSS LED (0 off, anything else on): a level held for ms. */
typedef struct {
    uint8_t level;
    uint16_t ms;       /**< 1..65535 */
} leds_level_step_t;

/** What the GNSS LED shows. */
typedef enum {
    LEDS_GNSS_RECEIVER = 0,   /**< the receiver's status input, as the station shows it (on boards that have one) */
    LEDS_GNSS_PATTERN,        /**< the pattern below */
} leds_gnss_mode_t;

/**
 * One pattern per LED; each loops on its own from its first step. A pattern
 * of 0 steps keeps that LED dark. A board without a given LED ignores its
 * part.
 */
typedef struct {
    leds_rgb_step_t rgb[LEDS_PATTERN_STEPS];
    uint8_t rgb_steps;
    leds_level_step_t wifi[LEDS_PATTERN_STEPS];
    uint8_t wifi_steps;
    leds_gnss_mode_t gnss_mode;
    leds_level_step_t gnss[LEDS_PATTERN_STEPS];
    uint8_t gnss_steps;
} leds_pattern_t;

#ifdef __cplusplus
}
#endif
