/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "board_pins.h"

#include <stddef.h>
#include <stdio.h>

const char *board_pin_owner(int gpio)
{
    if (gpio >= 26 && gpio <= 37) {
        return "flash/PSRAM";
    }
    if (gpio == 19) {
        return "native USB (D-)";
    }
    if (gpio == 20) {
        return "native USB (D+)";
    }
    if (gpio == 43 || gpio == 44) {
        return "the UART0 console";
    }
    return NULL;
}

bool board_pin_reserved(int gpio)
{
    return board_pin_owner(gpio) != NULL;
}

bool board_pin_strapping(int gpio)
{
    return gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46;
}

bool board_pin_exists(int gpio)
{
    return (gpio >= 0 && gpio <= 21) || (gpio >= 26 && gpio <= 48);
}

bool board_pin_for_link(int gpio)
{
    return board_pin_exists(gpio) && !board_pin_reserved(gpio) && !board_pin_strapping(gpio);
}

static bool pin_problem(const char *role, int gpio, const board_pin_use_t *uses, size_t n_uses, char *why, size_t cap)
{
    if (!board_pin_exists(gpio)) {
        snprintf(why, cap, "The %s pin must be an ESP32-S3 GPIO (0-21, 26-48).", role);
        return true;
    }
    const char *owner = board_pin_owner(gpio);
    if (owner != NULL) {
        snprintf(why, cap, "GPIO%d is used by %s.", gpio, owner);
        return true;
    }
    if (board_pin_strapping(gpio)) {
        snprintf(why, cap, "GPIO%d is a start-up (strapping) pin.", gpio);
        return true;
    }
    for (size_t i = 0; i < n_uses; i++) {
        if (uses[i].gpio == gpio) {
            if (uses[i].what == NULL) {
                snprintf(why, cap, "GPIO%d is wired to another part of the board.", gpio);
            } else {
                snprintf(why, cap, "GPIO%d is already the %s pin.", gpio, uses[i].what);
            }
            return true;
        }
    }
    return false;
}

bool board_wiring_problem(const board_wiring_t *w, const board_pin_use_t *uses, size_t n_uses, char *why,
                          size_t cap)
{
    char spare[8];
    if (why == NULL || cap == 0) {
        why = spare;
        cap = sizeof(spare);
    }
    why[0] = '\0';
    if (w == NULL) {
        snprintf(why, cap, "No wiring given.");
        return true;
    }
    if (pin_problem("RX", w->rx_gpio, uses, n_uses, why, cap) || pin_problem("TX", w->tx_gpio, uses, n_uses, why, cap)) {
        return true;
    }
    if (w->rx_gpio == w->tx_gpio) {
        snprintf(why, cap, "RX and TX need two different pins.");
        return true;
    }
    if (w->port < BOARD_PORT_MIN || w->port > BOARD_PORT_MAX) {
        snprintf(why, cap, "The receiver port must be COM1, COM2 or COM3.");
        return true;
    }
    return false;
}

size_t board_wiring_encode(const board_wiring_t *w, uint8_t *out, size_t cap)
{
    if (w == NULL || out == NULL || cap < BOARD_WIRING_BLOB_LEN) {
        return 0;
    }
    out[0] = BOARD_WIRING_BLOB_V1;
    out[1] = (uint8_t)w->rx_gpio;
    out[2] = (uint8_t)w->tx_gpio;
    out[3] = w->port;
    return BOARD_WIRING_BLOB_LEN;
}

bool board_wiring_decode(const uint8_t *in, size_t len, board_wiring_t *out)
{
    if (in == NULL || out == NULL || len < BOARD_WIRING_BLOB_LEN || in[0] < BOARD_WIRING_BLOB_V1 || in[1] > 127 ||
        in[2] > 127) {
        return false;
    }
    out->rx_gpio = (int8_t)in[1];
    out->tx_gpio = (int8_t)in[2];
    out->port = in[3];
    return true;
}

bool board_wiring_same(const board_wiring_t *a, const board_wiring_t *b)
{
    return a->rx_gpio == b->rx_gpio && a->tx_gpio == b->tx_gpio && a->port == b->port;
}
