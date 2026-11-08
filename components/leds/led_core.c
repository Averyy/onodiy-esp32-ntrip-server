/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "led_core.h"

#include <stddef.h>
#include <string.h>

static const led_rgb_t OFF = { 0, 0, 0 };
static const led_rgb_t WHITE = { 255, 255, 255 };
static const led_rgb_t RED = { 255, 0, 0 };
static const led_rgb_t GREEN = { 0, 255, 0 };
static const led_rgb_t BLUE = { 0, 0, 255 };
static const led_rgb_t YELLOW = { 255, 160, 0 };
static const led_rgb_t PINK = { 255, 0, 160 };

#define WIFI_DIM_DBM  (-90)
#define WIFI_FULL_DBM (-45)
#define WIFI_MIN_LEVEL 16u   /* still visible at the weakest signal */

/* The blinks of each colour after the white one: blue (Wi-Fi), then green (servers). */
static void codes(const led_state_t *s, unsigned n[2])
{
    n[0] = s->wifi == LED_WIFI_UP ? 2u : s->wifi == LED_WIFI_DOWN ? 1u : 0u;
    n[1] = s->servers == LED_SERVERS_ALL ? 2u : s->servers == LED_SERVERS_SOME ? 1u : 0u;
}

static uint32_t group_ms(unsigned blinks)
{
    return blinks == 0 ? 0u : blinks * LED_BLINK_ON_MS + (blinks - 1u) * LED_BLINK_OFF_MS + LED_GROUP_GAP_MS;
}

uint32_t led_cycle_ms(const led_state_t *s)
{
    unsigned n[2];
    codes(s, n);
    uint32_t ms = LED_BLINK_ON_MS + LED_GROUP_GAP_MS + group_ms(n[0]) + group_ms(n[1]);
    return ms < LED_CYCLE_MS ? LED_CYCLE_MS : ms;
}

led_rgb_t led_code_at(const led_state_t *s, uint32_t t)
{
    if (t < LED_BLINK_ON_MS) {
        return WHITE;
    }
    if (t < LED_BLINK_ON_MS + LED_GROUP_GAP_MS) {
        return OFF;
    }
    t -= LED_BLINK_ON_MS + LED_GROUP_GAP_MS;
    unsigned n[2];
    codes(s, n);
    const led_rgb_t colour[2] = { BLUE, GREEN };
    for (size_t g = 0; g < 2; g++) {
        uint32_t len = group_ms(n[g]);
        if (t < len) {
            uint32_t period = LED_BLINK_ON_MS + LED_BLINK_OFF_MS;
            return (t % period) < LED_BLINK_ON_MS && t < n[g] * period ? colour[g] : OFF;
        }
        t -= len;
    }
    return OFF;
}

void led_seq_init(led_seq_t *q, int64_t now_ms)
{
    memset(q, 0, sizeof(*q));
    q->cycle_at_ms = now_ms;
    q->red_until_ms = now_ms;
}

static led_rgb_t fast(led_rgb_t c, int64_t now_ms)
{
    return (now_ms / LED_FAST_MS) % 2 == 0 ? c : OFF;
}

led_rgb_t led_seq_color(led_seq_t *q, const led_state_t *s, int64_t now_ms)
{
    if (s == NULL) {
        /* Starting: white and red in turn until the first state. */
        return (now_ms / LED_BOOT_MS) % 2 == 0 ? WHITE : RED;
    }
    if (!q->started) {
        q->started = true;
        q->cycle = *s;
        q->cycle_at_ms = now_ms;
        q->cycle_ms = led_cycle_ms(s);
    }
    /* A bit that was clear and is set: a new alert (one cleared and raised again counts again). */
    if ((s->alerts & ~q->alerts_seen) != 0) {
        q->red_until_ms = now_ms + LED_ALERT_MS;
    }
    q->alerts_seen = s->alerts;

    if (now_ms - q->cycle_at_ms >= (int64_t)q->cycle_ms) {
        /* The next cycle shows the latest state; its start keeps the cadence unless the clock jumped. */
        int64_t next = q->cycle_at_ms + q->cycle_ms;
        q->cycle_at_ms = now_ms - next < (int64_t)LED_CYCLE_MS ? next : now_ms;
        q->cycle = *s;
        q->cycle_ms = led_cycle_ms(s);
    }

    if (s->button == LED_BUTTON_FACTORY) {
        return fast(PINK, now_ms);
    }
    if (s->button == LED_BUTTON_ADMIN) {
        return fast(YELLOW, now_ms);
    }
    if (now_ms < q->red_until_ms) {
        return fast(RED, now_ms);
    }
    return led_code_at(&q->cycle, (uint32_t)(now_ms - q->cycle_at_ms));
}

uint8_t led_wifi_level(bool online, int8_t rssi)
{
    if (!online) {
        return 0;
    }
    if (rssi == 0) {
        return 128;   /* 0 dBm: not read yet */
    }
    if (rssi <= WIFI_DIM_DBM) {
        return WIFI_MIN_LEVEL;
    }
    if (rssi >= WIFI_FULL_DBM) {
        return 255;
    }
    uint32_t span = (uint32_t)(WIFI_FULL_DBM - WIFI_DIM_DBM);
    uint32_t above = (uint32_t)(rssi - WIFI_DIM_DBM);
    return (uint8_t)(WIFI_MIN_LEVEL + (255u - WIFI_MIN_LEVEL) * above / span);
}

/* ---- a product's patterns ------------------------------------------------- */

static bool steps_valid_rgb(const leds_rgb_step_t *s, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        if (s[i].ms == 0) {
            return false;
        }
    }
    return true;
}

static bool steps_valid_level(const leds_level_step_t *s, uint8_t n)
{
    for (uint8_t i = 0; i < n; i++) {
        if (s[i].ms == 0) {
            return false;
        }
    }
    return true;
}

bool led_pattern_valid(const leds_pattern_t *p)
{
    return p != NULL && p->rgb_steps <= LEDS_PATTERN_STEPS && p->wifi_steps <= LEDS_PATTERN_STEPS &&
           p->gnss_steps <= LEDS_PATTERN_STEPS &&
           (p->gnss_mode == LEDS_GNSS_RECEIVER || p->gnss_mode == LEDS_GNSS_PATTERN) &&
           steps_valid_rgb(p->rgb, p->rgb_steps) && steps_valid_level(p->wifi, p->wifi_steps) &&
           steps_valid_level(p->gnss, p->gnss_steps);
}

bool led_pattern_equal(const leds_pattern_t *a, const leds_pattern_t *b)
{
    if (a->rgb_steps != b->rgb_steps || a->wifi_steps != b->wifi_steps || a->gnss_steps != b->gnss_steps ||
        a->gnss_mode != b->gnss_mode) {
        return false;
    }
    for (uint8_t i = 0; i < a->rgb_steps; i++) {
        const leds_rgb_step_t *x = &a->rgb[i];
        const leds_rgb_step_t *y = &b->rgb[i];
        if (x->r != y->r || x->g != y->g || x->b != y->b || x->ms != y->ms) {
            return false;
        }
    }
    for (uint8_t i = 0; i < a->wifi_steps; i++) {
        if (a->wifi[i].level != b->wifi[i].level || a->wifi[i].ms != b->wifi[i].ms) {
            return false;
        }
    }
    for (uint8_t i = 0; i < a->gnss_steps; i++) {
        if (a->gnss[i].level != b->gnss[i].level || a->gnss[i].ms != b->gnss[i].ms) {
            return false;
        }
    }
    return true;
}

led_rgb_t led_pattern_rgb(const leds_pattern_t *p, uint64_t t64)
{
    uint32_t total = 0;
    for (uint8_t i = 0; i < p->rgb_steps; i++) {
        total += p->rgb[i].ms;
    }
    if (total == 0) {
        return OFF;
    }
    uint32_t t = (uint32_t)(t64 % total);
    for (uint8_t i = 0; i < p->rgb_steps; i++) {
        if (t < p->rgb[i].ms) {
            return (led_rgb_t){ p->rgb[i].r, p->rgb[i].g, p->rgb[i].b };
        }
        t -= p->rgb[i].ms;
    }
    return OFF;
}

uint8_t led_pattern_level(const leds_level_step_t *steps, uint8_t n, uint64_t t64)
{
    uint32_t total = 0;
    for (uint8_t i = 0; i < n; i++) {
        total += steps[i].ms;
    }
    if (total == 0) {
        return 0;
    }
    uint32_t t = (uint32_t)(t64 % total);
    for (uint8_t i = 0; i < n; i++) {
        if (t < steps[i].ms) {
            return steps[i].level;
        }
        t -= steps[i].ms;
    }
    return 0;
}
