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
#include <stddef.h>
#include <stdint.h>

#include "board_wiring.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Where the board description came from. */
typedef enum {
    BOARD_SOURCE_DEFAULT = 0,  /**< no board marker in the partition table: the original board */
    BOARD_SOURCE_MARKER = 1,   /**< the partition table's board.<id> marker */
} board_source_t;

#define BOARD_UNDRIVEN_MAX 4

/**
 * Description of the board this station runs on (board_init picks it from the
 * partition table, see board_table.h). Pin value -1 means "not connected".
 */
typedef struct {
    const char *id;            /**< "um982-alix", "um980-ntrx" */
    const char *name;          /**< Human-readable board name: "GNS NTRIP-X" */
    const char *antenna;       /**< where the GNSS antenna goes ("on the socket marked M"), or NULL */
    board_source_t source;
    unsigned flash_mb;         /**< the board's flash (its install's) */
    int gnss_uart_num;         /**< UART controller for the GNSS link (never 0: console) */
    int gnss_rx_gpio;          /**< ESP32 RX  <- the receiver's TXD */
    int gnss_tx_gpio;          /**< ESP32 TX  -> the receiver's RXD */
    int gnss_port;             /**< the receiver port at their other end: 1 = COM1 ... 3 = COM3 */
    int gnss_reset_gpio;       /**< Receiver reset line, or -1 */
    int button_gpio;           /**< Recovery (BOOT) button, active low, or -1 */
    int status_led_gpio;       /**< Status LED, or -1 */
    int rgb_gpio[3];           /**< RGB status LED: red, green, blue, or -1 */
    bool rgb_invert;           /**< the RGB LED lights on a low level */
    int wifi_led_gpio;         /**< Wi-Fi LED (lit high), or -1 */
    int gnss_led_gpio;         /**< GNSS LED (lit high), or -1 */
    int gnss_status_gpio;      /**< Input from the receiver, high = its status is good, or -1 */
    int undriven_gpio[BOARD_UNDRIVEN_MAX]; /**< reach parts whose use is not known: never driven or given out, -1 = none */
    bool sd_present;           /**< microSD slot available (reserved) */
    int sd_miso_gpio;          /**< SD pins, -1 when sd_present is false */
    int sd_mosi_gpio;
    int sd_sck_gpio;
    int sd_cs_gpio;
} board_desc_t;

/**
 * Pick the board (the partition table's board.<id> marker, cross-checked
 * with the chip's flash size; none: the original board), validate its pin
 * map (valid GPIOs, no clash with the flash/PSRAM, native USB or console
 * pins, no duplicates) and configure its inputs (the button, the receiver's
 * status). Outputs (LEDs) are left to their drivers; nothing else is driven.
 * A marker that cannot be used (another flash size, a board this firmware
 * does not know) keeps the receiver link closed and leaves every pin of the
 * board but the button alone (journalled, board_problem()).
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

/*
 * The receiver wiring. The board's is the default (on the original board the
 * build's Kconfig pins); one saved on the station (settings gnss/wiring, read
 * and written by the station component) replaces it from the next start, and
 * stays through app updates and a factory reset.
 */

/**
 * The saved wiring, given before board_init() (the app does at start). Used
 * when it passes board_wiring_check(), else the build's (logged). NULL: none.
 */
void board_use_wiring(const board_wiring_t *w);

/** The wiring this start uses. */
void board_wiring_active(board_wiring_t *out);

/** The board's own (the build's Kconfig pins on the original board). */
void board_wiring_default(board_wiring_t *out);

/**
 * Whether `w` can be used on this board: pins board_pin_for_link() that the
 * button, LEDs, receiver reset and status, SD card and the board's undriven
 * pins do not use, and a port of 1-3.
 * False with a sentence for the user in `why`.
 */
bool board_wiring_check(const board_wiring_t *w, char *why, size_t cap);

/**
 * Why the receiver link stays closed this start, a sentence: the board
 * choice failed (board_problem()), or the saved wiring fails
 * board_wiring_check() (board_wiring_refused()), so neither it nor the
 * board's pins are used. NULL when the link may open.
 */
const char *board_link_off_reason(void);

/** Why the board's pins are not used this start (a sentence), NULL when they are. A restart does not change it. */
const char *board_problem(void);

/**
 * The board choice for the journal (diag.h DIAG_EV_BOARD; board is below
 * diag, so the app writes it after board_init()): `a` the issue
 * (board_table.h board_issue_t), `b` the chip's flash in MB (bits 0-7, 0 =
 * not known) | the board's (bits 8-15). @return true when there is one.
 */
bool board_journal_note(int32_t *a, int32_t *b);

/** The saved wiring fails the check this start (the link stays closed until another one is saved and the station restarts). */
bool board_wiring_refused(void);

/** The pins the receiver link may use on this board, ascending (at most `cap`). @return their count. */
size_t board_link_pins(int8_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
