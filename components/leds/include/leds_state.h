/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * What the board's LEDs show (pure types, no IDF includes, also in the host
 * tests). Health fills it once a second (leds_set).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LED_WIFI_NONE = 0,     /**< no Wi-Fi network configured (setup hotspot only) */
    LED_WIFI_DOWN,         /**< configured, not connected */
    LED_WIFI_UP,           /**< configured and connected (an address) */
} led_wifi_t;

typedef enum {
    LED_SERVERS_NONE = 0,  /**< no NTRIP server enabled */
    LED_SERVERS_SOME,      /**< enabled, not all connected */
    LED_SERVERS_ALL,       /**< every enabled one connected */
} led_servers_t;

typedef enum {
    LED_BUTTON_NONE = 0,
    LED_BUTTON_ADMIN,      /**< held past the admin-reset time */
    LED_BUTTON_FACTORY,    /**< held past the factory-reset time */
} led_button_t;

/** What the LEDs show, as health last saw it. */
typedef struct {
    led_wifi_t wifi;
    led_servers_t servers;
    led_button_t button;
    uint32_t alerts;       /**< health's alert bits */
    bool online;           /**< the Wi-Fi LED: the station has an address */
    int8_t rssi;           /**< dBm, for the Wi-Fi LED's brightness */
} led_state_t;

#ifdef __cplusplus
}
#endif
