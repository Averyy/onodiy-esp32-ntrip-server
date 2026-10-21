/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "board_pins.h"
#include "board_table.h"
#include "unity.h"

#define MB (1024u * 1024u)

static const board_entry_t *orig(void)
{
    return board_table_find(BOARD_ID_DEFAULT);
}

TEST_CASE("board table: the original board is today's Kconfig defaults, first in the table", "[board]")
{
    const board_entry_t *e = orig();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_PTR(board_table_at(0), e);
    TEST_ASSERT_EQUAL_STRING("UM982 board (AliExpress)", e->name);
    TEST_ASSERT_EQUAL_STRING("on the socket marked M", e->antenna);   /* the board has two: M and S */
    TEST_ASSERT_EQUAL(16, e->flash_mb);
    /* components/board/Kconfig: UART1, RX GPIO1, TX GPIO2, COM1, no reset line, the BOOT button on GPIO0, no LED. */
    TEST_ASSERT_EQUAL(1, e->uart_num);
    TEST_ASSERT_EQUAL(1, e->rx_gpio);
    TEST_ASSERT_EQUAL(2, e->tx_gpio);
    TEST_ASSERT_EQUAL(1, e->port);
    TEST_ASSERT_EQUAL(-1, e->reset_gpio);
    TEST_ASSERT_EQUAL(0, e->button_gpio);
    TEST_ASSERT_EQUAL(-1, e->status_led_gpio);
    for (size_t i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(-1, e->rgb_gpio[i]);
    }
    TEST_ASSERT_EQUAL(-1, e->wifi_led_gpio);
    TEST_ASSERT_EQUAL(-1, e->gnss_led_gpio);
    TEST_ASSERT_EQUAL(-1, e->gnss_status_gpio);
    for (size_t i = 0; i < BOARD_KEEP_MAX; i++) {
        TEST_ASSERT_EQUAL(-1, e->keep_undriven[i]);
    }
}

TEST_CASE("board table: the GNS NTRIP-X as found on the unit", "[board]")
{
    const board_entry_t *e = board_table_find("um980-ntrx");
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_STRING("GNS NTRIP-X", e->name);
    TEST_ASSERT_EQUAL_STRING("on the port marked GNSS", e->antenna);
    TEST_ASSERT_EQUAL(8, e->flash_mb);
    TEST_ASSERT_EQUAL(1, e->uart_num);
    TEST_ASSERT_EQUAL(16, e->rx_gpio);
    TEST_ASSERT_EQUAL(15, e->tx_gpio);
    TEST_ASSERT_EQUAL(1, e->port);
    TEST_ASSERT_EQUAL(-1, e->reset_gpio);
    TEST_ASSERT_EQUAL(0, e->button_gpio);
    TEST_ASSERT_EQUAL(6, e->rgb_gpio[0]);
    TEST_ASSERT_EQUAL(5, e->rgb_gpio[1]);
    TEST_ASSERT_EQUAL(4, e->rgb_gpio[2]);
    TEST_ASSERT_TRUE(e->rgb_invert);
    TEST_ASSERT_EQUAL(7, e->wifi_led_gpio);
    TEST_ASSERT_EQUAL(8, e->gnss_led_gpio);
    TEST_ASSERT_EQUAL(11, e->gnss_status_gpio);
    const int8_t keep[] = { 1, 2, 40, 41 };
    TEST_ASSERT_EQUAL_INT8_ARRAY(keep, e->keep_undriven, BOARD_KEEP_MAX);
}

/* Every pin a board names, -1 skipped. */
static size_t board_pins_of(const board_entry_t *e, int *out)
{
    const int all[] = { e->rx_gpio, e->tx_gpio, e->reset_gpio, e->button_gpio, e->status_led_gpio, e->rgb_gpio[0],
                        e->rgb_gpio[1], e->rgb_gpio[2], e->wifi_led_gpio, e->gnss_led_gpio, e->gnss_status_gpio,
                        e->keep_undriven[0], e->keep_undriven[1], e->keep_undriven[2], e->keep_undriven[3] };
    size_t n = 0;
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        if (all[i] >= 0) {
            out[n++] = all[i];
        }
    }
    return n;
}

TEST_CASE("board table: every board's pins exist, are free of the module's and of each other, ids fit a marker",
          "[board]")
{
    TEST_ASSERT_TRUE(board_table_count() >= 2);
    for (size_t b = 0; b < board_table_count(); b++) {
        const board_entry_t *e = board_table_at(b);
        TEST_ASSERT_EQUAL_PTR(e, board_table_find(e->id));
        TEST_ASSERT_NOT_EQUAL(0, e->uart_num);   /* UART0 is the console */
        TEST_ASSERT_TRUE(e->port >= 1 && e->port <= 3);
        /* "board." + id fits a 16-character partition label (the original board is never marked, its id fits too). */
        TEST_ASSERT_TRUE_MESSAGE(strlen(e->id) <= BOARD_ID_MAX, e->id);
        int pins[16];
        size_t n = board_pins_of(e, pins);
        for (size_t i = 0; i < n; i++) {
            TEST_ASSERT_TRUE(board_pin_exists(pins[i]));
            TEST_ASSERT_FALSE_MESSAGE(board_pin_reserved(pins[i]), e->id);
            for (size_t j = 0; j < i; j++) {
                TEST_ASSERT_NOT_EQUAL_MESSAGE(pins[j], pins[i], e->id);
            }
        }
        /* The receiver link may use its board's own pins (the button is the only strapping pin a board uses). */
        TEST_ASSERT_TRUE(board_pin_for_link(e->rx_gpio));
        TEST_ASSERT_TRUE(board_pin_for_link(e->tx_gpio));
    }
    TEST_ASSERT_EQUAL(10u, BOARD_ID_MAX);
}

TEST_CASE("board marker: a label board.<id>, nothing else", "[board]")
{
    TEST_ASSERT_EQUAL_STRING("um980-ntrx", board_marker_id("board.um980-ntrx"));
    TEST_ASSERT_EQUAL_STRING("x", board_marker_id("board.x"));
    TEST_ASSERT_NULL(board_marker_id("board."));
    TEST_ASSERT_NULL(board_marker_id("board"));
    TEST_ASSERT_NULL(board_marker_id("Board.um980-ntrx"));
    TEST_ASSERT_NULL(board_marker_id("storage"));
    TEST_ASSERT_NULL(board_marker_id(""));
    TEST_ASSERT_NULL(board_marker_id(NULL));
    TEST_ASSERT_NULL(board_table_find("board.um980-ntrx"));
    TEST_ASSERT_NULL(board_table_find(NULL));
}

TEST_CASE("board choice: no marker is the original board, whatever the flash", "[board]")
{
    board_choice_t c;
    board_choose(orig(), NULL, 0, 16u * MB, &c);
    TEST_ASSERT_EQUAL_PTR(orig(), c.entry);
    TEST_ASSERT_EQUAL(BOARD_FROM_DEFAULT, c.from);
    TEST_ASSERT_EQUAL(BOARD_OK, c.issue);
    TEST_ASSERT_TRUE(c.trusted);
    TEST_ASSERT_EQUAL(16, c.flash_mb);
    TEST_ASSERT_EQUAL_STRING("", c.why);

    /* Every deployed station has no marker: another flash size (or one not read) never closes its link. */
    board_choose(orig(), NULL, 0, 8u * MB, &c);
    TEST_ASSERT_EQUAL_PTR(orig(), c.entry);
    TEST_ASSERT_EQUAL(BOARD_NOTE_FLASH, c.issue);
    TEST_ASSERT_TRUE(c.trusted);
    TEST_ASSERT_EQUAL(8, c.flash_mb);
    TEST_ASSERT_EQUAL_STRING("", c.why);
    board_choose(orig(), NULL, 0, 0, &c);
    TEST_ASSERT_EQUAL(BOARD_OK, c.issue);
    TEST_ASSERT_EQUAL(0, c.flash_mb);
}

TEST_CASE("board choice: the NTRIP-X marker on 8 MB flash, or not read", "[board]")
{
    const char *m[] = { "board.um980-ntrx" };
    board_choice_t c;
    board_choose(orig(), m, 1, 8u * MB, &c);
    TEST_ASSERT_EQUAL_STRING("um980-ntrx", c.entry->id);
    TEST_ASSERT_EQUAL(BOARD_FROM_MARKER, c.from);
    TEST_ASSERT_EQUAL(BOARD_OK, c.issue);
    TEST_ASSERT_TRUE(c.trusted);
    TEST_ASSERT_EQUAL_STRING("", c.why);
    board_choose(orig(), m, 1, 0, &c);
    TEST_ASSERT_EQUAL_STRING("um980-ntrx", c.entry->id);
    TEST_ASSERT_TRUE(c.trusted);
}

TEST_CASE("board choice: a marker on another flash size, an unknown or a second marker closes the link", "[board]")
{
    const char *m[] = { "board.um980-ntrx" };
    board_choice_t c;
    /* A self-built N16R8 given the NTRIP-X install must not drive the NTRIP-X's pins. */
    board_choose(orig(), m, 1, 16u * MB, &c);
    TEST_ASSERT_EQUAL_STRING("um980-ntrx", c.entry->id);   /* named, so the user sees which install it got */
    TEST_ASSERT_EQUAL(BOARD_FROM_MARKER, c.from);
    TEST_ASSERT_EQUAL(BOARD_BAD_FLASH, c.issue);
    TEST_ASSERT_FALSE(c.trusted);
    TEST_ASSERT_EQUAL_STRING("The install is for the GNS NTRIP-X (8 MB flash), but this chip has 16 MB.", c.why);
    board_choose(orig(), m, 1, 4u * MB, &c);
    TEST_ASSERT_EQUAL(BOARD_BAD_FLASH, c.issue);

    const char *u[] = { "board.rtk-z9" };
    board_choose(orig(), u, 1, 8u * MB, &c);
    TEST_ASSERT_EQUAL_PTR(orig(), c.entry);
    TEST_ASSERT_EQUAL(BOARD_FROM_MARKER, c.from);
    TEST_ASSERT_EQUAL(BOARD_BAD_UNKNOWN, c.issue);
    TEST_ASSERT_FALSE(c.trusted);
    TEST_ASSERT_EQUAL_STRING("This firmware does not know the board \"rtk-z9\". Update it or install it again.", c.why);

    const char *two[] = { "board.um980-ntrx", "board.um980-ntrx" };
    board_choose(orig(), two, 2, 8u * MB, &c);
    TEST_ASSERT_EQUAL_PTR(orig(), c.entry);
    TEST_ASSERT_EQUAL(BOARD_BAD_TWO, c.issue);
    TEST_ASSERT_FALSE(c.trusted);
    TEST_ASSERT_EQUAL_STRING("The partition table marks 2 boards. Install the firmware for this board again.", c.why);
    TEST_ASSERT_TRUE(strlen(c.why) < sizeof(c.why) - 1u);
}

TEST_CASE("board choice: the journal's issue numbers keep their meaning", "[board]")
{
    TEST_ASSERT_EQUAL(0, BOARD_OK);
    TEST_ASSERT_EQUAL(1, BOARD_BAD_FLASH);
    TEST_ASSERT_EQUAL(2, BOARD_BAD_UNKNOWN);
    TEST_ASSERT_EQUAL(3, BOARD_BAD_TWO);
    TEST_ASSERT_EQUAL(4, BOARD_NOTE_FLASH);
}

TEST_CASE("board wiring: the NTRIP-X's LEDs, status input and undriven pins are not the link's", "[board]")
{
    const board_entry_t *e = board_table_find("um980-ntrx");
    const board_pin_use_t uses[] = {
        { "button", e->button_gpio },          { "red LED", e->rgb_gpio[0] },     { "green LED", e->rgb_gpio[1] },
        { "blue LED", e->rgb_gpio[2] },        { "Wi-Fi LED", e->wifi_led_gpio }, { "GNSS LED", e->gnss_led_gpio },
        { "receiver status", e->gnss_status_gpio }, { NULL, e->keep_undriven[0] }, { NULL, e->keep_undriven[1] },
        { NULL, e->keep_undriven[2] },         { NULL, e->keep_undriven[3] },
    };
    const size_t n = sizeof(uses) / sizeof(uses[0]);
    char why[64];
    board_wiring_t w = { .rx_gpio = 16, .tx_gpio = 15, .port = 1 };
    TEST_ASSERT_FALSE(board_wiring_problem(&w, uses, n, why, sizeof(why)));
    /* The original board's pins are the NTRIP-X's unused flow-control pair. */
    w = (board_wiring_t){ .rx_gpio = 1, .tx_gpio = 2, .port = 1 };
    TEST_ASSERT_TRUE(board_wiring_problem(&w, uses, n, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO1 is wired to another part of the board.", why);
    w = (board_wiring_t){ .rx_gpio = 16, .tx_gpio = 6, .port = 1 };
    TEST_ASSERT_TRUE(board_wiring_problem(&w, uses, n, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO6 is already the red LED pin.", why);
    w = (board_wiring_t){ .rx_gpio = 11, .tx_gpio = 15, .port = 1 };
    TEST_ASSERT_TRUE(board_wiring_problem(&w, uses, n, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO11 is already the receiver status pin.", why);
    w = (board_wiring_t){ .rx_gpio = 16, .tx_gpio = 41, .port = 1 };
    TEST_ASSERT_TRUE(board_wiring_problem(&w, uses, n, why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("GPIO41 is wired to another part of the board.", why);
}
