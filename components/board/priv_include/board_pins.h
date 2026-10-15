/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Which ESP32-S3 pins the station may not give to anything else (pure
 * logic, no IDF includes, host-tested).
 */
#pragma once

#include <stdbool.h>

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

#ifdef __cplusplus
}
#endif
