/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The boards the firmware knows, and which one this station is (pure logic,
 * no IDF includes, host-tested).
 *
 * One app runs on every board; the install tells them apart. A board other
 * than the original is marked by an empty partition labelled "board.<id>"
 * (type 0x40, subtype 0x02) in its partition table. No marker means the
 * original board: every station deployed before the table existed, whose
 * partition tables never change. The physical flash size is a cross-check:
 * a marker on a chip of another flash size is refused (a self-built board
 * given another board's install must not drive that board's pins).
 *
 * A partition label has at most 16 characters, so a marked board's id has at
 * most 10.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_MARKER_PREFIX   "board."
#define BOARD_MARKER_TYPE     0x40
#define BOARD_MARKER_SUBTYPE  0x02
#define BOARD_LABEL_MAX       16u     /**< an ESP-IDF partition label */
#define BOARD_ID_MAX          (BOARD_LABEL_MAX - (sizeof(BOARD_MARKER_PREFIX) - 1u))
#define BOARD_KEEP_MAX        4u      /**< pins a board leaves undriven */

/** The original board's id: the board of a table without a marker. */
#define BOARD_ID_DEFAULT      "um982-alix"

/** One board. GPIO -1 = not on this board. */
typedef struct {
    const char *id;                    /**< "um980-ntrx"; the marker is "board." + id */
    const char *name;                  /**< for people: "GNS NTRIP-X" */
    const char *antenna;               /**< where the GNSS antenna goes, for "the antenna is <antenna>": "on the
                                            socket marked M"; NULL: no one place to name */
    uint8_t flash_mb;                  /**< the module's flash */
    int8_t uart_num;                   /**< the receiver link's UART (never 0: the console) */
    int8_t rx_gpio;                    /**< ESP32 RX <- the receiver's TXD */
    int8_t tx_gpio;                    /**< ESP32 TX -> the receiver's RXD */
    uint8_t port;                      /**< the receiver's port at their other end: 1 = COM1 */
    int8_t reset_gpio;                 /**< the receiver's reset line */
    int8_t button_gpio;                /**< the button, active low with a pull-up */
    int8_t status_led_gpio;            /**< a single status LED */
    int8_t rgb_gpio[3];                /**< an RGB status LED: red, green, blue */
    bool rgb_invert;                   /**< the RGB LED lights on a low level */
    int8_t wifi_led_gpio;              /**< a Wi-Fi LED, lit high, brightness by signal */
    int8_t gnss_led_gpio;              /**< a GNSS LED, lit high, following gnss_status_gpio */
    int8_t gnss_status_gpio;           /**< an input from the receiver (pull-down), high = its status is good */
    int8_t keep_undriven[BOARD_KEEP_MAX]; /**< pins wired to something unknown: never driven or given out */
} board_entry_t;

/** The boards, the original first. */
size_t board_table_count(void);
const board_entry_t *board_table_at(size_t i);

/** The board with this id, NULL when there is none. */
const board_entry_t *board_table_find(const char *id);

/** The board id of a marker label ("board.um980-ntrx" -> "um980-ntrx"), NULL when `label` is not a marker. */
const char *board_marker_id(const char *label);

/** Where this station's board came from. */
typedef enum {
    BOARD_FROM_DEFAULT = 0,            /**< no marker: the original board */
    BOARD_FROM_MARKER = 1,             /**< the partition table's marker */
} board_from_t;

/** What is wrong with the board choice (stored in the journal: never renumbered). */
typedef enum {
    BOARD_OK = 0,
    BOARD_BAD_FLASH = 1,               /**< a marked board on a chip of another flash size: link closed, nothing driven */
    BOARD_BAD_UNKNOWN = 2,             /**< a marker this firmware does not know: link closed, nothing driven */
    BOARD_BAD_TWO = 3,                 /**< more than one marker: link closed, nothing driven */
    BOARD_NOTE_FLASH = 4,              /**< the original board on a chip of another flash size: noted only */
} board_issue_t;

typedef struct {
    const board_entry_t *entry;        /**< the board: the marked one (also when its flash size differs), the
                                            original without a marker or with an unknown or second one; never NULL */
    board_from_t from;
    board_issue_t issue;
    bool trusted;                      /**< the entry's pins may be driven (issue is BOARD_OK or BOARD_NOTE_FLASH) */
    uint32_t flash_mb;                 /**< the chip's flash as read, 0 = not known */
    char why[112];                     /**< a sentence for the user when the link stays closed, else "" */
} board_choice_t;

/** How a board is shown (the API, the UI, the antenna hints). */
typedef struct {
    const char *id;
    const char *name;
    const char *antenna;               /**< NULL: no one place to name */
} board_label_t;

/**
 * How the board is shown. A marked board as its entry. Without a marker the
 * install is the same for every 16 MB board, so what it is shows at run
 * time: with the user's own saved wiring "Other ESP32-S3 board"; with a
 * UM980 (`model`, the receiver's, "" while unknown) the UM980 board; else
 * the original UM982 board (every deployed station).
 */
void board_label(const board_entry_t *entry, board_from_t from, bool own_wiring, const char *model,
                 board_label_t *out);

/**
 * Pick the board: `dflt` without a marker, the marked board when the table
 * has exactly one marker this firmware knows and the chip's flash
 * (`flash_bytes`, 0 = not known: not checked) is that board's.
 * `markers` are the marker partitions' labels.
 */
void board_choose(const board_entry_t *dflt, const char *const *markers, size_t n_markers, uint32_t flash_bytes,
                  board_choice_t *out);

#ifdef __cplusplus
}
#endif
