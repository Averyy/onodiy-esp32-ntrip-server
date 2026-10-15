/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "board_pins.h"

#include <stddef.h>

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
