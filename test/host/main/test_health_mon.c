/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Health monitor helpers: loss journal deltas, the link-loss window, caster
 * budgets, debounce, the BOOT button and the overall level. The clock starts
 * near the 32-bit millisecond wrap.
 */
#include <stdint.h>
#include <string.h>

#include "health_mon.h"
#include "unity.h"

#define T0 ((int64_t)UINT32_MAX - 2000)
#define SEC 1000
#define MIN (60 * SEC)

/* ---- loss journal ------------------------------------------------------------ */

TEST_CASE("health mon: the first sample is the baseline", "[health_mon]")
{
    hm_loss_t l;
    hm_loss_init(&l);
    uint64_t v[HM_LOSS_IDS] = { 5, 7, 0, 100, 2000, 1, 30, 2 };
    uint64_t inc[HM_LOSS_IDS];
    hm_loss_sample(&l, v, T0, inc);
    uint8_t id;
    int32_t d;
    TEST_ASSERT_FALSE(hm_loss_next(&l, &id, &d));
    for (unsigned i = 0; i < HM_LOSS_IDS; i++) {
        TEST_ASSERT_EQUAL_UINT64(0, inc[i]);
    }
}

TEST_CASE("health mon: one record per tick, oldest increase first, refused records merge", "[health_mon]")
{
    hm_loss_t l;
    hm_loss_init(&l);
    uint64_t v[HM_LOSS_IDS] = { 0 };
    hm_loss_sample(&l, v, T0, NULL);

    /* t1: FIFO overflow +1 and CRC +3; t2: buffer full +2 */
    v[0] = 1;
    v[3] = 3;
    uint64_t inc[HM_LOSS_IDS];
    hm_loss_sample(&l, v, T0 + SEC, inc);
    TEST_ASSERT_EQUAL_UINT64(1, inc[0]);
    TEST_ASSERT_EQUAL_UINT64(3, inc[3]);
    v[1] = 2;
    hm_loss_sample(&l, v, T0 + 2 * SEC, NULL);

    uint8_t id;
    int32_t d;
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(1, id);   /* same age as id 4: the lower id first */
    TEST_ASSERT_EQUAL_INT32(1, d);
    /* refused by the rate limit: nothing cleared; more overflows merge in */
    v[0] = 4;
    hm_loss_sample(&l, v, T0 + 3 * SEC, NULL);
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(1, id);
    TEST_ASSERT_EQUAL_INT32(4, d);
    hm_loss_written(&l, id, d);
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(4, id);
    TEST_ASSERT_EQUAL_INT32(3, d);
    hm_loss_written(&l, id, d);
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(2, id);
    TEST_ASSERT_EQUAL_INT32(2, d);
    hm_loss_written(&l, id, d);
    TEST_ASSERT_FALSE(hm_loss_next(&l, &id, &d));
}

TEST_CASE("health mon: large increases are clamped and the rest stays pending", "[health_mon]")
{
    hm_loss_t l;
    hm_loss_init(&l);
    uint64_t v[HM_LOSS_IDS] = { 0 };
    hm_loss_sample(&l, v, T0, NULL);
    v[4] = (uint64_t)INT32_MAX + 10u;   /* framer discarded bytes */
    hm_loss_sample(&l, v, T0 + SEC, NULL);
    uint8_t id;
    int32_t d;
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(5, id);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, d);
    hm_loss_written(&l, id, d);
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(5, id);
    TEST_ASSERT_EQUAL_INT32(10, d);
    hm_loss_written(&l, id, d);
    TEST_ASSERT_FALSE(hm_loss_next(&l, &id, &d));
}

TEST_CASE("health mon: a counter that went backwards is rebaselined", "[health_mon]")
{
    hm_loss_t l;
    hm_loss_init(&l);
    uint64_t v[HM_LOSS_IDS] = { 10, 0, 0, 0, 0, 0, 0, 0 };
    hm_loss_sample(&l, v, T0, NULL);
    v[0] = 3;
    hm_loss_sample(&l, v, T0 + SEC, NULL);
    uint8_t id;
    int32_t d;
    TEST_ASSERT_FALSE(hm_loss_next(&l, &id, &d));
    v[0] = 5;
    hm_loss_sample(&l, v, T0 + 2 * SEC, NULL);
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_INT32(2, d);
    hm_loss_written(&l, 0, 1);    /* ignored: no such id */
    hm_loss_written(&l, 9, 1);
    TEST_ASSERT_TRUE(hm_loss_next(&l, &id, &d));
    TEST_ASSERT_EQUAL_INT32(2, d);
}

/* ---- link-loss window ---------------------------------------------------------- */

typedef struct {
    hm_link_t w;
    int64_t now;
    uint64_t fifo, crc, frames;
    bool suspended;
} link_sim_t;

static void link_init(link_sim_t *s)
{
    memset(s, 0, sizeof(*s));
    s->now = T0;
    hm_link_init(&s->w, T0);
}

/* `seconds` ticks, adding `fps` frames and `crc_ps` CRC errors per second. */
static bool link_run(link_sim_t *s, int seconds, uint32_t fps, uint32_t crc_ps)
{
    bool a = false;
    for (int i = 0; i < seconds; i++) {
        s->frames += fps;
        s->crc += crc_ps;
        a = hm_link_tick(&s->w, s->fifo, s->crc, s->frames, s->suspended, s->now);
        s->now += SEC;
    }
    return a;
}

TEST_CASE("health mon: one FIFO overflow raises the link alert; it clears 30 min after leaving the window", "[health_mon]")
{
    link_sim_t s;
    link_init(&s);
    TEST_ASSERT_FALSE(link_run(&s, 120, 20, 0));
    s.fifo++;
    int64_t at = s.now;
    TEST_ASSERT_TRUE(link_run(&s, 1, 20, 0));
    /* in the window for 10-11 min, then 30 min more */
    int64_t cleared = 0;
    for (int i = 0; i < 60 * 60 && cleared == 0; i++) {
        if (!link_run(&s, 1, 20, 0)) {
            cleared = s.now - SEC;
        }
    }
    TEST_ASSERT_TRUE(cleared != 0);
    int64_t held = cleared - at;
    TEST_ASSERT_TRUE(held >= 40 * MIN);
    TEST_ASSERT_TRUE(held <= 41 * MIN + 2 * SEC);
}

TEST_CASE("health mon: the CRC rule needs more than 1 % with at least 100 frames", "[health_mon]")
{
    /* 99 frames, 50 errors: too few frames */
    link_sim_t s;
    link_init(&s);
    TEST_ASSERT_FALSE(link_run(&s, 2, 0, 0));
    s.frames += 99;
    s.crc += 50;
    TEST_ASSERT_FALSE(link_run(&s, 1, 0, 0));

    /* exactly 1 %: not above it */
    link_init(&s);
    link_run(&s, 1, 0, 0);
    s.frames += 1000;
    s.crc += 10;
    TEST_ASSERT_FALSE(link_run(&s, 1, 0, 0));
    s.crc += 1;
    TEST_ASSERT_TRUE(link_run(&s, 1, 0, 0));

    /* a background of 0.5 % over a day never alerts */
    link_init(&s);
    TEST_ASSERT_FALSE(link_run(&s, 24 * 3600, 200, 1));
    hm_link_window_t win;
    hm_link_window(&s.w, &win);
    TEST_ASSERT_TRUE(win.frames >= 10u * 60u * 200u);
    TEST_ASSERT_TRUE(win.frames <= 11u * 60u * 200u);
}

TEST_CASE("health mon: increases while suspended are not counted; the window rebaselines", "[health_mon]")
{
    link_sim_t s;
    link_init(&s);
    link_run(&s, 10, 20, 0);
    s.suspended = true;
    link_run(&s, 5, 20, 0);
    s.fifo += 3;            /* overflows counted during a raw session */
    s.crc += 500;
    link_run(&s, 5, 20, 0);
    s.suspended = false;
    TEST_ASSERT_FALSE(link_run(&s, 30, 20, 0));
    hm_link_window_t win;
    hm_link_window(&s.w, &win);
    TEST_ASSERT_EQUAL_UINT64(0, win.fifo);
    TEST_ASSERT_EQUAL_UINT64(0, win.crc);
    TEST_ASSERT_EQUAL_UINT64(9u * 20u + 29u * 20u, win.frames);   /* baseline and rebaseline ticks add nothing */
}

TEST_CASE("health mon: the window spans 10-11 minutes and survives a long gap", "[health_mon]")
{
    link_sim_t s;
    link_init(&s);
    link_run(&s, 1, 0, 0);
    s.fifo++;
    TEST_ASSERT_TRUE(link_run(&s, 1, 0, 0));
    link_run(&s, 10 * 60, 0, 0);
    hm_link_window_t win;
    hm_link_window(&s.w, &win);
    TEST_ASSERT_EQUAL_UINT64(1, win.fifo);   /* still inside */
    link_run(&s, 61, 0, 0);
    hm_link_window(&s.w, &win);
    TEST_ASSERT_EQUAL_UINT64(0, win.fifo);   /* out after at most 11 min */

    /* a tick after hours of silence starts a clean window */
    s.now += 5 * 3600 * SEC;
    link_run(&s, 1, 0, 0);
    hm_link_window(&s.w, &win);
    TEST_ASSERT_EQUAL_UINT64(0, win.fifo);
    TEST_ASSERT_EQUAL_UINT64(0, win.frames);
}

/* ---- caster budgets ------------------------------------------------------------- */

static hm_slot_in_t slot_in(uint8_t state, uint32_t age_s, uint32_t progress_ms)
{
    hm_slot_in_t in = {
        .enabled = true,
        .state = state,
        .state_age_s = age_s,
        .progress_age_ms = progress_ms,
        .connect_budget_s = 60,
        .send_timeout_ms = 10000,
    };
    return in;
}

TEST_CASE("health mon: caster budgets restart only the overrunning slot, once per budget", "[health_mon]")
{
    hm_slot_t s;
    hm_slot_init(&s);
    hm_slot_in_t in = slot_in(HM_CST_CONNECTING, 60, 100);
    TEST_ASSERT_FALSE(hm_slot_overrun(&s, &in, T0));
    in.state_age_s = 61;
    TEST_ASSERT_TRUE(hm_slot_overrun(&s, &in, T0));
    in.state_age_s = 90;
    TEST_ASSERT_FALSE(hm_slot_overrun(&s, &in, T0 + 30 * SEC));   /* cool-down */
    TEST_ASSERT_TRUE(hm_slot_overrun(&s, &in, T0 + 60 * SEC));

    /* a send that makes no progress for longer than the send timeout + 5 s */
    hm_slot_init(&s);
    in = slot_in(HM_CST_STREAMING, 3600, 15000);
    TEST_ASSERT_FALSE(hm_slot_overrun(&s, &in, T0));
    in.progress_age_ms = 15001;
    TEST_ASSERT_TRUE(hm_slot_overrun(&s, &in, T0));

    /* no progress at all in another state */
    hm_slot_init(&s);
    in = slot_in(HM_CST_BACKOFF, 10, 60000);
    TEST_ASSERT_FALSE(hm_slot_overrun(&s, &in, T0));
    in.progress_age_ms = 60001;
    TEST_ASSERT_TRUE(hm_slot_overrun(&s, &in, T0));

    /* disabled slots are never touched */
    hm_slot_init(&s);
    in = slot_in(HM_CST_CONNECTING, 1000, 1000000);
    in.enabled = false;
    TEST_ASSERT_FALSE(hm_slot_overrun(&s, &in, T0));
    in = slot_in(HM_CST_DISABLED, 1000, 1000000);
    TEST_ASSERT_FALSE(hm_slot_overrun(&s, &in, T0));
}

TEST_CASE("health mon: which caster states count as down", "[health_mon]")
{
    const uint8_t down[] = { HM_CST_WAIT_NETWORK, HM_CST_BACKOFF, HM_CST_AUTH_FAILED, HM_CST_CONFIG_ERROR,
                             HM_CST_BANNED };
    const uint8_t up[] = { HM_CST_DISABLED, HM_CST_WAIT_DATA, HM_CST_CONNECTING, HM_CST_STREAMING, HM_CST_PAUSED };
    for (unsigned i = 0; i < sizeof(down); i++) {
        hm_slot_in_t in = slot_in(down[i], 1, 1);
        TEST_ASSERT_TRUE(hm_slot_down(&in));
        in.enabled = false;
        TEST_ASSERT_FALSE(hm_slot_down(&in));
    }
    for (unsigned i = 0; i < sizeof(up); i++) {
        hm_slot_in_t in = slot_in(up[i], 1, 1);
        TEST_ASSERT_FALSE(hm_slot_down(&in));
    }
}

/* ---- debounce ------------------------------------------------------------------- */

TEST_CASE("health mon: debounce on and off times", "[health_mon]")
{
    hm_deb_t d;
    hm_deb_init(&d);
    int64_t t = T0;
    TEST_ASSERT_FALSE(hm_deb(&d, true, t, 60000, 10000));
    TEST_ASSERT_FALSE(hm_deb(&d, true, t + 59 * SEC, 60000, 10000));
    TEST_ASSERT_FALSE(hm_deb(&d, false, t + 59500, 60000, 10000));   /* a gap restarts it */
    TEST_ASSERT_FALSE(hm_deb(&d, true, t + 60 * SEC, 60000, 10000));
    TEST_ASSERT_TRUE(hm_deb(&d, true, t + 120 * SEC, 60000, 10000));
    TEST_ASSERT_TRUE(hm_deb(&d, false, t + 121 * SEC, 60000, 10000));
    TEST_ASSERT_TRUE(hm_deb(&d, false, t + 130 * SEC, 60000, 10000));
    TEST_ASSERT_FALSE(hm_deb(&d, false, t + 131 * SEC, 60000, 10000));
}

/* ---- BOOT button ------------------------------------------------------------------ */

static hm_btn_event_t press_for(hm_button_t *b, int64_t *t, int seconds, hm_btn_event_t *reached)
{
    hm_btn_event_t last_reached = HM_BTN_NONE;
    for (int i = 0; i <= seconds; i++) {
        hm_btn_event_t e = hm_button(b, true, *t, 10000, 20000);
        if (e == HM_BTN_ADMIN_REACHED || e == HM_BTN_FACTORY_REACHED) {
            TEST_ASSERT_TRUE(e != last_reached);   /* each announced once */
            last_reached = e;
        } else {
            TEST_ASSERT_EQUAL_INT(HM_BTN_NONE, e);
        }
        *t += SEC;
    }
    if (reached != NULL) {
        *reached = last_reached;
    }
    hm_btn_event_t r = hm_button(b, false, *t, 10000, 20000);
    *t += SEC;
    return r;
}

TEST_CASE("health mon: BOOT button actions are chosen by the hold time, on release", "[health_mon]")
{
    hm_button_t b;
    hm_button_init(&b);
    int64_t t = T0;
    TEST_ASSERT_EQUAL_INT(HM_BTN_NONE, hm_button(&b, false, t, 10000, 20000));
    t += SEC;
    hm_btn_event_t reached;
    TEST_ASSERT_EQUAL_INT(HM_BTN_NONE, press_for(&b, &t, 3, &reached));
    TEST_ASSERT_EQUAL_INT(HM_BTN_NONE, reached);
    TEST_ASSERT_EQUAL_INT(HM_BTN_ADMIN, press_for(&b, &t, 12, &reached));
    TEST_ASSERT_EQUAL_INT(HM_BTN_ADMIN_REACHED, reached);
    TEST_ASSERT_EQUAL_INT(HM_BTN_FACTORY, press_for(&b, &t, 25, &reached));
    TEST_ASSERT_EQUAL_INT(HM_BTN_FACTORY_REACHED, reached);
    TEST_ASSERT_EQUAL_INT(HM_BTN_ADMIN, press_for(&b, &t, 10, NULL));     /* exactly the admin hold */
    TEST_ASSERT_EQUAL_INT(HM_BTN_NONE, press_for(&b, &t, 9, NULL));
}

TEST_CASE("health mon: a BOOT button held since boot does nothing until released", "[health_mon]")
{
    hm_button_t b;
    hm_button_init(&b);
    int64_t t = T0;
    for (int i = 0; i < 120; i++) {
        TEST_ASSERT_EQUAL_INT(HM_BTN_NONE, hm_button(&b, true, t, 10000, 20000));
        t += SEC;
    }
    TEST_ASSERT_EQUAL_INT(HM_BTN_NONE, hm_button(&b, false, t, 10000, 20000));   /* stuck, then let go */
    t += SEC;
    TEST_ASSERT_EQUAL_INT(HM_BTN_ADMIN, press_for(&b, &t, 11, NULL));
}

/* ---- level -------------------------------------------------------------------------- */

TEST_CASE("health mon: overall level is the worst of the ladder and degraded", "[health_mon]")
{
    TEST_ASSERT_EQUAL_UINT8(0, hm_level(0, false));
    TEST_ASSERT_EQUAL_UINT8(1, hm_level(0, true));
    TEST_ASSERT_EQUAL_UINT8(1, hm_level(1, false));
    TEST_ASSERT_EQUAL_UINT8(2, hm_level(2, true));
    TEST_ASSERT_EQUAL_UINT8(3, hm_level(3, true));
    TEST_ASSERT_EQUAL_UINT8(3, hm_level(3, false));
}
