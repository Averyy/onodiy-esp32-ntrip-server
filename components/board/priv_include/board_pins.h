/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Which ESP32-S3 pins the station may not give to anything else, and the
 * checks and stored form of the receiver wiring (pure logic, no IDF
 * includes, host-tested).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "board_wiring.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * What already uses `gpio` on the board, or NULL when nothing does: the SPI
 * flash and octal PSRAM of the N16R8 module (GPIO26-37), the native USB data
 * lines (GPIO19 D-, GPIO20 D+: the ESP32S3 USB-C port reaches them with the
 * plug one way round) and the UART0 console (GPIO43 TX, GPIO44 RX: the
 * CH343P bridge the other way round). A text for the log.
 */
const char *board_pin_owner(int gpio);

/** A pin board_pin_owner() names: never a choice for anything else. */
bool board_pin_reserved(int gpio);

/** A strapping pin, sampled at reset (GPIO0, 3, 45, 46). */
bool board_pin_strapping(int gpio);

/** A GPIO the ESP32-S3 has: 0-21 and 26-48 (there is no GPIO22-25). */
bool board_pin_exists(int gpio);

/**
 * A pin the receiver link may be given at run time: it exists, nothing on
 * the module uses it and it is not a strapping pin (the receiver's TXD idles
 * high and would set one at every reset).
 */
bool board_pin_for_link(int gpio);

/** One of the board's other pins, which the receiver link may not take. */
typedef struct {
    const char *what;        /**< "button", ... */
    int gpio;                /**< -1 = not wired (ignored) */
} board_pin_use_t;

/**
 * Whether `w` can be used: both pins board_pin_for_link(), different, none
 * of `uses`, and a port of 1-3. False with the reason in `why` (a sentence
 * for the user, e.g. "GPIO0 is already the button pin.").
 */
bool board_wiring_problem(const board_wiring_t *w, const board_pin_use_t *uses, size_t n_uses, char *why,
                          size_t cap);

#ifdef __cplusplus
}
#endif
