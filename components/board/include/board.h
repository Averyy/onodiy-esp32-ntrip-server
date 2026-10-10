/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Board description: which pins and peripherals the station hardware uses.
 *
 * The single source of truth for wiring. Every other component asks this one
 * instead of reading pin Kconfig symbols directly, so adding a second board
 * variant later only touches this component.
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Static description of the board. Pin value -1 means "not connected". */
typedef struct {
    const char *name;          /**< Human-readable board name */
    int gnss_uart_num;         /**< UART controller for the GNSS link (never 0: console) */
    int gnss_rx_gpio;          /**< ESP32 RX  <- receiver COM1 TXD */
    int gnss_tx_gpio;          /**< ESP32 TX  -> receiver COM1 RXD */
    int gnss_reset_gpio;       /**< Receiver reset line, or -1 */
    int button_gpio;           /**< Recovery (BOOT) button, active low, or -1 */
    int status_led_gpio;       /**< Status LED, or -1 */
    bool sd_present;           /**< microSD slot available (reserved) */
    int sd_miso_gpio;          /**< SD pins, -1 when sd_present is false */
    int sd_mosi_gpio;
    int sd_sck_gpio;
    int sd_cs_gpio;
} board_desc_t;

/**
 * Validate the pin map (valid GPIOs, no clash with flash/PSRAM/console pins,
 * no duplicates), configure the button input, and put optional outputs (LED,
 * reset line) in a safe state.
 *
 * A call after a successful one returns ESP_OK at once (an early boot step
 * may run it before the normal init).
 *
 * @return ESP_OK, or ESP_ERR_INVALID_ARG if the configured pin map is unusable.
 */
esp_err_t board_init(void);

/**
 * Board description, before and after board_init(); never NULL. For labels
 * and logs any time; a component that drives pins checks board_is_valid()
 * first.
 */
const board_desc_t *board_get(void);

/**
 * True once board_init() accepted the pin map. False before that and after a
 * rejected map: components that configure pins (the GNSS UART in particular,
 * whose wrong pin could be an octal-PSRAM line) refuse to start, so a bad
 * Kconfig fails loudly instead of corrupting PSRAM.
 */
bool board_is_valid(void);

#ifdef __cplusplus
}
#endif
