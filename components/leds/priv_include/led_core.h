/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The status LED's blink codes and the Wi-Fi LED's brightness (pure logic,
 * no IDF includes, injected clock, host-tested).
 *
 * The codes are the ones the GNS NTRIP-X's printed quick guide explains, so
 * its instructions stay true:
 *  - every cycle starts with one white blink; a cycle lasts at least
 *    LED_CYCLE_MS (setup hotspot only, nothing configured: white every 2 s);
 *  - then blue: one blink when Wi-Fi is configured but not connected, two
 *    when connected;
 *  - then green: one blink when NTRIP servers are enabled but not all
 *    connected, two when all are;
 *  - fast red for LED_ALERT_MS when an alert is raised (a health alert bit
 *    that was clear);
 *  - while starting, white and red in turn;
 *  - while the button is held: fast yellow from the admin-reset hold
 *    (release: admin password cleared, recovery hotspot), fast pink from
 *    the factory-reset hold (release: factory reset).
 * A state change shows from the next cycle; the overlays (button, alert)
 * at once.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "leds_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LED_BLINK_ON_MS   150u   /**< one blink of a code */
#define LED_BLINK_OFF_MS  200u   /**< between the blinks of a double code */
#define LED_GROUP_GAP_MS  450u   /**< dark between two colours' codes */
#define LED_CYCLE_MS      2000u  /**< shortest cycle */
#define LED_FAST_MS       100u   /**< fast flashing: this long on, this long off */
#define LED_ALERT_MS      2000u  /**< red after a new alert */
#define LED_BOOT_MS       250u   /**< each colour of the start pattern */

typedef struct {
    uint8_t r, g, b;       /**< 0-255, before any inversion */
} led_rgb_t;

typedef struct {
    bool started;          /**< a state was given: the start pattern ends */
    led_state_t cycle;     /**< the state the running cycle shows */
    int64_t cycle_at_ms;
    uint32_t cycle_ms;
    uint32_t alerts_seen;
    int64_t red_until_ms;
} led_seq_t;

void led_seq_init(led_seq_t *q, int64_t now_ms);

/** The status LED's colour now. `s`: the latest state, NULL while starting (no state yet). */
led_rgb_t led_seq_color(led_seq_t *q, const led_state_t *s, int64_t now_ms);

/** One cycle's length for a state (the codes, at least LED_CYCLE_MS). */
uint32_t led_cycle_ms(const led_state_t *s);

/** The colour `t` ms into a cycle for state `s` (the plain codes, no overlay). */
led_rgb_t led_code_at(const led_state_t *s, uint32_t t);

/**
 * The Wi-Fi LED's brightness, 0-255: off without an address, else by signal (dim at -90 dBm, full from -45 dBm;
 * half while the signal is not read yet, rssi 0).
 */
uint8_t led_wifi_level(bool online, int8_t rssi);

#ifdef __cplusplus
}
#endif
