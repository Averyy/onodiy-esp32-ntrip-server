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

TEST_CASE("board pins: the receiver link may take 25 pins, none reserved, missing or strapping", "[board]")
{
    static const int expect[] = { 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48 };
    size_t n = 0;
    for (int g = -1; g <= 49; g++) {
        TEST_ASSERT_EQUAL((g >= 0 && g <= 21) || (g >= 26 && g <= 48), board_pin_exists(g));
        if (board_pin_for_link(g)) {
            TEST_ASSERT_TRUE(n < sizeof(expect) / sizeof(expect[0]));
            TEST_ASSERT_EQUAL(expect[n], g);
            n++;
        }
    }
    TEST_ASSERT_EQUAL(sizeof(expect) / sizeof(expect[0]), n);
}

TEST_CASE("board wiring: the check says why a wiring cannot be used", "[board]")
{
    const board_pin_use_t uses[] = { { "button", 0 }, { "status LED", -1 }, { "SD MISO", 4 } };
    const size_t n = sizeof(uses) / sizeof(uses[0]);
    char why[64];
    board_wiring_t w = { .rx_gpio = 1, .tx_gpio = 2, .port = 1 };
    TEST_ASSERT_FALSE(board_wiring_problem(&w, uses, n, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("", why);
    w = (board_wiring_t){ .rx_gpio = 17, .tx_gpio = 18, .port = 3 };
    TEST_ASSERT_FALSE(board_wiring_problem(&w, uses, n, why, sizeof(why)));

    static const struct {
        board_wiring_t w;
        const char *why;
    } bad[] = {
        { { 26, 2, 1 }, "GPIO26 is used by flash/PSRAM." },
        { { 1, 43, 1 }, "GPIO43 is used by the UART0 console." },
        { { 1, 3, 1 }, "GPIO3 is a start-up (strapping) pin." },
        { { 0, 2, 1 }, "GPIO0 is a start-up (strapping) pin." },
        { { 4, 2, 1 }, "GPIO4 is already the SD MISO pin." },
        { { 22, 2, 1 }, "The RX pin must be an ESP32-S3 GPIO (0-21, 26-48)." },
        { { 1, -1, 1 }, "The TX pin must be an ESP32-S3 GPIO (0-21, 26-48)." },
        { { 5, 5, 1 }, "RX and TX need two different pins." },
        { { 1, 2, 0 }, "The receiver port must be COM1, COM2 or COM3." },
        { { 1, 2, 4 }, "The receiver port must be COM1, COM2 or COM3." },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(board_wiring_problem(&bad[i].w, uses, n, why, sizeof(why)), bad[i].why);
        TEST_ASSERT_EQUAL_STRING(bad[i].why, why);
    }
    /* A -1 use (a pin not wired) takes nothing; no buffer for the reason is fine. */
    w = (board_wiring_t){ .rx_gpio = 5, .tx_gpio = 6, .port = 2 };
    TEST_ASSERT_FALSE(board_wiring_problem(&w, uses, n, NULL, 0));
    TEST_ASSERT_TRUE(board_wiring_problem(NULL, uses, n, why, sizeof(why)));
}

TEST_CASE("board wiring: the stored form round-trips; later versions may append", "[board]")
{
    const board_wiring_t w = { .rx_gpio = 48, .tx_gpio = 21, .port = 3 };
    uint8_t blob[8];
    TEST_ASSERT_EQUAL(0, board_wiring_encode(&w, blob, BOARD_WIRING_BLOB_LEN - 1));
    TEST_ASSERT_EQUAL(BOARD_WIRING_BLOB_LEN, board_wiring_encode(&w, blob, sizeof(blob)));
    const uint8_t want[] = { 1, 48, 21, 3 };
    TEST_ASSERT_EQUAL_MEMORY(want, blob, sizeof(want));
    board_wiring_t r;
    TEST_ASSERT_TRUE(board_wiring_decode(blob, BOARD_WIRING_BLOB_LEN, &r));
    TEST_ASSERT_TRUE(board_wiring_same(&w, &r));
    const uint8_t v2[] = { 2, 5, 6, 2, 0xAA };   /* a later version's extra field is ignored */
    TEST_ASSERT_TRUE(board_wiring_decode(v2, sizeof(v2), &r));
    TEST_ASSERT_EQUAL(5, r.rx_gpio);
    TEST_ASSERT_EQUAL(6, r.tx_gpio);
    TEST_ASSERT_EQUAL(2, r.port);
    const uint8_t v0[] = { 0, 5, 6, 2 };
    const uint8_t neg[] = { 1, 0xFF, 6, 2 };
    TEST_ASSERT_FALSE(board_wiring_decode(v0, sizeof(v0), &r));
    TEST_ASSERT_FALSE(board_wiring_decode(neg, sizeof(neg), &r));
    TEST_ASSERT_FALSE(board_wiring_decode(blob, BOARD_WIRING_BLOB_LEN - 1, &r));
    const board_wiring_t other = { .rx_gpio = 48, .tx_gpio = 21, .port = 2 };
    TEST_ASSERT_FALSE(board_wiring_same(&w, &other));
}
