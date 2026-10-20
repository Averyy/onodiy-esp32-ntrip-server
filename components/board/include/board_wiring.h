/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * How the receiver is wired to the ESP32 (pure type, no IDF includes, also
 * in the host tests).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOARD_PORT_MIN 1u   /**< the receiver's COM1 */
#define BOARD_PORT_MAX 3u   /**< its COM3 */

/** The ESP32 pins of the receiver link and the receiver port at their other end. */
typedef struct {
    int8_t rx_gpio;          /**< ESP32 RX <- the receiver's TXD */
    int8_t tx_gpio;          /**< ESP32 TX -> the receiver's RXD */
    uint8_t port;            /**< the receiver's port: 1 = COM1 ... 3 = COM3 */
} board_wiring_t;

#define BOARD_WIRING_BLOB_V1  1u
#define BOARD_WIRING_BLOB_LEN 4u

/**
 * The stored form (settings gnss/wiring): v1 {u8 version = 1, u8 rx GPIO,
 * u8 tx GPIO, u8 port}. Later versions only append fields; a reader ignores
 * what it does not know.
 * @return bytes written (BOARD_WIRING_BLOB_LEN), 0 if `cap` is too small.
 */
size_t board_wiring_encode(const board_wiring_t *w, uint8_t *out, size_t cap);

/** Read a stored wiring (its values unchecked: board_wiring_check). False when it is not one. */
bool board_wiring_decode(const uint8_t *in, size_t len, board_wiring_t *out);

/** Two wirings with the same pins and port. */
bool board_wiring_same(const board_wiring_t *a, const board_wiring_t *b);

#ifdef __cplusplus
}
#endif
