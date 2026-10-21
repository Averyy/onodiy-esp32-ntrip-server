/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "led_core.h"
#include "unity.h"

typedef enum { C_OFF, C_WHITE, C_RED, C_GREEN, C_BLUE, C_YELLOW, C_PINK, C_OTHER } colour_t;

static colour_t name(led_rgb_t c)
{
    if (c.r == 0 && c.g == 0 && c.b == 0) {
        return C_OFF;
    }
    if (c.r == 255 && c.g == 255 && c.b == 255) {
        return C_WHITE;
    }
    if (c.r == 255 && c.g == 0 && c.b == 0) {
        return C_RED;
    }
    if (c.r == 0 && c.g == 255 && c.b == 0) {
        return C_GREEN;
    }
    if (c.r == 0 && c.g == 0 && c.b == 255) {
        return C_BLUE;
    }
    if (c.r == 255 && c.g > 0 && c.b == 0) {
        return C_YELLOW;
    }
    if (c.r == 255 && c.g == 0 && c.b > 0) {
        return C_PINK;
    }
    return C_OTHER;
}

/* One cycle as a string, a character per 50 ms: '.' off, W white, B blue, G green. */
static void cycle_text(const led_state_t *s, char *out, size_t cap)
{
    uint32_t len = led_cycle_ms(s);
    size_t n = 0;
    for (uint32_t t = 0; t < len && n + 1 < cap; t += 50) {
        colour_t c = name(led_code_at(s, t));
        out[n++] = c == C_OFF ? '.' : c == C_WHITE ? 'W' : c == C_BLUE ? 'B' : c == C_GREEN ? 'G' : '?';
    }
    out[n] = '\0';
}

TEST_CASE("leds: nothing configured is a white blink every 2 s (the quick guide's factory state)", "[leds]")
{
    led_state_t s = { 0 };
    TEST_ASSERT_EQUAL_UINT32(2000, led_cycle_ms(&s));
    char t[64];
    cycle_text(&s, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("WWW.....................................", t);
}

TEST_CASE("leds: white, then blue once or twice for Wi-Fi, then green once or twice for the servers", "[leds]")
{
    char t[64];
    led_state_t s = { .wifi = LED_WIFI_DOWN };
    cycle_text(&s, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("WWW.........BBB.........................", t);
    s.wifi = LED_WIFI_UP;
    cycle_text(&s, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("WWW.........BBB....BBB..................", t);
    s.servers = LED_SERVERS_SOME;
    cycle_text(&s, t, sizeof(t));   /* 2150 ms: longer than the shortest cycle */
    TEST_ASSERT_EQUAL_STRING("WWW.........BBB....BBB.........GGG.........", t);
    s.servers = LED_SERVERS_ALL;
    TEST_ASSERT_EQUAL_UINT32(150 + 450 + (150 + 200 + 150 + 450) * 2, led_cycle_ms(&s));
    cycle_text(&s, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("WWW.........BBB....BBB.........GGG....GGG.........", t);
    /* Servers without Wi-Fi: white, then green. */
    s.wifi = LED_WIFI_NONE;
    s.servers = LED_SERVERS_SOME;
    cycle_text(&s, t, sizeof(t));
    TEST_ASSERT_EQUAL_STRING("WWW.........GGG.........................", t);
}

TEST_CASE("leds: white and red in turn until the first state", "[leds]")
{
    led_seq_t q;
    led_seq_init(&q, 1000);
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, NULL, 1000)));
    TEST_ASSERT_EQUAL(C_RED, name(led_seq_color(&q, NULL, 1000 + LED_BOOT_MS)));
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, NULL, 1000 + 2 * LED_BOOT_MS)));
    /* The first state starts a cycle at once: its white blink. */
    led_state_t s = { .wifi = LED_WIFI_UP };
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, &s, 1001 + 2 * LED_BOOT_MS)));
}

TEST_CASE("leds: a state change shows from the next cycle, on its cadence", "[leds]")
{
    led_seq_t q;
    led_seq_init(&q, 0);
    led_state_t s = { .wifi = LED_WIFI_DOWN };
    (void)led_seq_color(&q, &s, 0);
    s.wifi = LED_WIFI_UP;
    /* Still the first cycle: one blue blink, no second. */
    TEST_ASSERT_EQUAL(C_BLUE, name(led_seq_color(&q, &s, 650)));
    TEST_ASSERT_EQUAL(C_OFF, name(led_seq_color(&q, &s, 950 + 50)));
    /* The next cycle (at 2000 ms) shows two. */
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, &s, 2010)));
    TEST_ASSERT_EQUAL(C_BLUE, name(led_seq_color(&q, &s, 2000 + 950 + 50)));
    TEST_ASSERT_EQUAL_INT64(2000, q.cycle_at_ms);
    /* A late tick (a jump past a whole cycle) starts the cycle then. */
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, &s, 9000)));
    TEST_ASSERT_EQUAL_INT64(9000, q.cycle_at_ms);
}

TEST_CASE("leds: a new alert flashes red for 2 s; one already shown does not again", "[leds]")
{
    led_seq_t q;
    led_seq_init(&q, 0);
    led_state_t s = { .wifi = LED_WIFI_UP };
    (void)led_seq_color(&q, &s, 0);
    s.alerts = 0x4;
    TEST_ASSERT_EQUAL(C_RED, name(led_seq_color(&q, &s, 3000)));
    TEST_ASSERT_EQUAL(C_OFF, name(led_seq_color(&q, &s, 3000 + LED_FAST_MS)));   /* fast flashing */
    TEST_ASSERT_EQUAL(C_RED, name(led_seq_color(&q, &s, 3000 + 2 * LED_FAST_MS)));
    /* After 2 s the codes again, the alert still set. */
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, &s, 6000)));
    /* Another bit: red again. Cleared and raised again: red again. */
    s.alerts = 0x6;
    TEST_ASSERT_EQUAL(C_RED, name(led_seq_color(&q, &s, 8000)));
    s.alerts = 0x2;
    (void)led_seq_color(&q, &s, 11000);
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, &s, 12000)));
    s.alerts = 0x6;
    TEST_ASSERT_EQUAL(C_RED, name(led_seq_color(&q, &s, 12000 + 2 * LED_FAST_MS)));
}

TEST_CASE("leds: the button's holds flash yellow, then pink, over everything", "[leds]")
{
    led_seq_t q;
    led_seq_init(&q, 0);
    led_state_t s = { .wifi = LED_WIFI_UP, .alerts = 1 };
    s.button = LED_BUTTON_ADMIN;
    TEST_ASSERT_EQUAL(C_YELLOW, name(led_seq_color(&q, &s, 0)));
    TEST_ASSERT_EQUAL(C_OFF, name(led_seq_color(&q, &s, LED_FAST_MS)));
    s.button = LED_BUTTON_FACTORY;
    TEST_ASSERT_EQUAL(C_PINK, name(led_seq_color(&q, &s, 2 * LED_FAST_MS)));
    /* Released during the alert's 2 s: the red flashing, then the codes. */
    s.button = LED_BUTTON_NONE;
    TEST_ASSERT_EQUAL(C_RED, name(led_seq_color(&q, &s, 4 * LED_FAST_MS)));
    TEST_ASSERT_EQUAL(C_WHITE, name(led_seq_color(&q, &s, 4000)));
}

TEST_CASE("leds: the Wi-Fi LED is off without an address, brighter with a stronger signal", "[leds]")
{
    TEST_ASSERT_EQUAL_UINT8(0, led_wifi_level(false, -50));
    TEST_ASSERT_EQUAL_UINT8(255, led_wifi_level(true, -40));
    TEST_ASSERT_EQUAL_UINT8(255, led_wifi_level(true, -45));
    TEST_ASSERT_EQUAL_UINT8(16, led_wifi_level(true, -95));
    TEST_ASSERT_EQUAL_UINT8(16, led_wifi_level(true, -90));
    TEST_ASSERT_EQUAL_UINT8(128, led_wifi_level(true, 0));   /* not read yet */
    uint8_t prev = 0;
    for (int dbm = -90; dbm <= -45; dbm++) {
        uint8_t v = led_wifi_level(true, (int8_t)dbm);
        TEST_ASSERT_TRUE(v >= prev);
        prev = v;
    }
}
