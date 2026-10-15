/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "board_pins.h"
#include "unity.h"

TEST_CASE("board pins: flash/PSRAM, native USB and the console are never a choice", "[board]")
{
    for (int g = 26; g <= 37; g++) {
        TEST_ASSERT_TRUE_MESSAGE(board_pin_reserved(g), "flash/PSRAM");
        TEST_ASSERT_EQUAL_STRING("flash/PSRAM", board_pin_owner(g));
    }
    /* The ESP32S3 USB-C port reaches the native USB lines with the plug one way round. */
    TEST_ASSERT_TRUE(board_pin_reserved(19));
    TEST_ASSERT_TRUE(board_pin_reserved(20));
    TEST_ASSERT_EQUAL_STRING("native USB (D-)", board_pin_owner(19));
    TEST_ASSERT_EQUAL_STRING("native USB (D+)", board_pin_owner(20));
    TEST_ASSERT_TRUE(board_pin_reserved(43));
    TEST_ASSERT_TRUE(board_pin_reserved(44));
    TEST_ASSERT_EQUAL_STRING("the UART0 console", board_pin_owner(43));
}

TEST_CASE("board pins: everything else is free, the default map included", "[board]")
{
    int reserved = 0;
    for (int g = -1; g <= 48; g++) {
        bool r = board_pin_reserved(g);
        TEST_ASSERT_EQUAL(r, board_pin_owner(g) != NULL);
        reserved += r ? 1 : 0;
    }
    TEST_ASSERT_EQUAL(12 + 2 + 2, reserved);
    /* The receiver link (GPIO1/GPIO2), the BOOT button (GPIO0), the SD defaults (4-7), the USB lines' neighbours. */
    const int free_pins[] = { -1, 0, 1, 2, 4, 5, 6, 7, 18, 21, 25, 38, 42, 45, 46, 47, 48 };
    for (size_t i = 0; i < sizeof(free_pins) / sizeof(free_pins[0]); i++) {
        TEST_ASSERT_FALSE(board_pin_reserved(free_pins[i]));
        TEST_ASSERT_NULL(board_pin_owner(free_pins[i]));
    }
}

TEST_CASE("board pins: the strapping pins are 0, 3, 45 and 46", "[board]")
{
    for (int g = -1; g <= 48; g++) {
        TEST_ASSERT_EQUAL(g == 0 || g == 3 || g == 45 || g == 46, board_pin_strapping(g));
    }
}
