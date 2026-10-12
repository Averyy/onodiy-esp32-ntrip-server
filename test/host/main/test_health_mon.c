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
    int64_t now = T0;
    TEST_ASSERT_FALSE(hm_loss_next(&l, now, &id, &d));
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
    int64_t now = T0 + 2 * SEC;
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(1, id);   /* same age as id 4: the lower id first */
    TEST_ASSERT_EQUAL_INT32(1, d);
    /* refused by the rate limit: nothing cleared; more overflows merge in */
    v[0] = 4;
    hm_loss_sample(&l, v, T0 + 3 * SEC, NULL);
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(1, id);
    TEST_ASSERT_EQUAL_INT32(4, d);
    hm_loss_written(&l, id, d, now);
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(4, id);
    TEST_ASSERT_EQUAL_INT32(3, d);
    hm_loss_written(&l, id, d, now);
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(2, id);
    TEST_ASSERT_EQUAL_INT32(2, d);
    hm_loss_written(&l, id, d, now);
    TEST_ASSERT_FALSE(hm_loss_next(&l, now, &id, &d));
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
    int64_t now = T0 + SEC;
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(5, id);
    TEST_ASSERT_EQUAL_INT32(INT32_MAX, d);
    hm_loss_written(&l, id, d, now);
    TEST_ASSERT_FALSE(hm_loss_next(&l, now + SEC, &id, &d));   /* the counter's next record waits */
    now += HM_LOSS_MIN_INTERVAL_MS;
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_UINT8(5, id);
    TEST_ASSERT_EQUAL_INT32(10, d);
    hm_loss_written(&l, id, d, now);
    TEST_ASSERT_FALSE(hm_loss_next(&l, now, &id, &d));
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
    int64_t now = T0 + 2 * SEC;
    TEST_ASSERT_FALSE(hm_loss_next(&l, now, &id, &d));
    v[0] = 5;
    hm_loss_sample(&l, v, T0 + 2 * SEC, NULL);
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_INT32(2, d);
    hm_loss_written(&l, 0, 1, now);    /* ignored: no such id */
    hm_loss_written(&l, 9, 1, now);
    TEST_ASSERT_TRUE(hm_loss_next(&l, now, &id, &d));
    TEST_ASSERT_EQUAL_INT32(2, d);
}

TEST_CASE("health mon: a counter rising for hours gives one record a minute, others still get theirs", "[health_mon]")
{
    /* Framer discards rise every second for 3 h (a garbled line); FIFO
     * overflows now and then. A journal that takes every record. */
    hm_loss_t l;
    hm_loss_init(&l);
    uint64_t v[HM_LOSS_IDS] = { 0 };
    int64_t now = T0;
    hm_loss_sample(&l, v, now, NULL);
    unsigned recs[HM_LOSS_IDS + 1] = { 0 };
    uint64_t journalled[HM_LOSS_IDS + 1] = { 0 };
    for (int t = 1; t <= 3 * 3600; t++) {
        now += SEC;
        v[4] += 100;
        if (t % 600 == 0) {
            v[0] += 1;
        }
        hm_loss_sample(&l, v, now, NULL);
        uint8_t id;
        int32_t d;
        if (hm_loss_next(&l, now, &id, &d)) {
            recs[id]++;
            journalled[id] += (uint64_t)d;
            hm_loss_written(&l, id, d, now);
        }
    }
    TEST_ASSERT_TRUE(recs[5] <= 3 * 60 + 1);
    TEST_ASSERT_TRUE(recs[5] >= 3 * 60 - 1);
    TEST_ASSERT_EQUAL_UINT32(18, recs[1]);
    /* Nothing is lost: what is not journalled yet is pending. */
    TEST_ASSERT_EQUAL_UINT64(v[4], journalled[5] + l.pending[4]);
    TEST_ASSERT_EQUAL_UINT64(v[0], journalled[1] + l.pending[0]);
}

TEST_CASE("health mon: a low heap reboots once, never for a start-up dip or after a quick one", "[health_mon]")
{
    const uint32_t lim = 8192;
    hm_heap_t hp;
    /* A dip at run time: one reboot. */
    hm_heap_init(&hp, false, UINT32_MAX);
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 40000, lim));
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 9000, lim));
    TEST_ASSERT_TRUE(hm_heap_reboot_due(&hp, 8000, lim));
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 7000, lim));

    /* The mark was already below the limit when the monitor started (a
     * start-up peak): every boot would repeat it, so no reboot for it... */
    hm_heap_init(&hp, false, UINT32_MAX);
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 6000, lim));
    for (int i = 0; i < 100; i++) {
        TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 6000, lim));
    }
    /* ...but a further fall at run time does. */
    TEST_ASSERT_TRUE(hm_heap_reboot_due(&hp, 5000, lim));

    /* The previous boot rebooted for low heap 10 min after its start: hold. */
    hm_heap_init(&hp, true, 600);
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 40000, lim));
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 1000, lim));
    TEST_ASSERT_TRUE(hp.held);
    /* After a long session, or another reboot reason, it may again. */
    hm_heap_init(&hp, true, HM_HEAP_REBOOT_SPACING_S);
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 40000, lim));
    TEST_ASSERT_TRUE(hm_heap_reboot_due(&hp, 1000, lim));
    hm_heap_init(&hp, false, 5);
    TEST_ASSERT_FALSE(hm_heap_reboot_due(&hp, 40000, lim));
    TEST_ASSERT_TRUE(hm_heap_reboot_due(&hp, 1000, lim));

    /* A start-up dip on every boot: boots loop at most... never. */
    unsigned reboots = 0;
    bool prev_low = false;
    uint32_t prev_up = UINT32_MAX;
    for (int boot = 0; boot < 50; boot++) {
        hm_heap_init(&hp, prev_low, prev_up);
        bool r = false;
        for (int t = 0; t < 3600 && !r; t++) {
            r = hm_heap_reboot_due(&hp, 7000, lim);
        }
        reboots += r ? 1u : 0u;
        prev_low = r;
        prev_up = r ? 5u : UINT32_MAX;
    }
    TEST_ASSERT_EQUAL_UINT32(0, reboots);
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

/* ---- power lost --------------------------------------------------------------------- */

#define PT0 1800000000LL   /* 2027-01-15, UTC s */
#define PH 3600LL
#define PDAY 86400LL
#define RESET_PLANNED 1
#define RESET_CRASH 2
#define EV_OTHER 8         /* any record of a boot after its boot record */

/* One boot: its boot record (no clock yet) and, `start` > 0, a later record
 * of the same boot with a trusted clock 100 s into it. */
static void pboot(hm_power_t *p, int cls, int64_t start)
{
    hm_power_feed(p, HM_EV_BOOT, cls, 0, 0);
    hm_power_feed(p, EV_OTHER, 0, 0, 5);
    if (start > 0) {
        hm_power_feed(p, EV_OTHER, 0, start + 100, 100);
    }
}

TEST_CASE("health mon: three power-on boots within 24 h raise power lost; it clears 24 h after the last", "[health_mon]")
{
    hm_power_t p;
    hm_power_init(&p);
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0));
    pboot(&p, HM_RESET_POWER_ON, PT0);
    pboot(&p, HM_RESET_POWER_ON, PT0 + 1 * PH);
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + 2 * PH));
    pboot(&p, HM_RESET_POWER_ON, PT0 + 2 * PH);
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0 + 2 * PH + 200));
    /* the first boot leaves the window, the alert stays: 24 h without one clears it */
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0 + 25 * PH));
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0 + 2 * PH + PDAY - 1));
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + 2 * PH + PDAY));

    /* boots of other kinds count neither for nor against it */
    hm_power_init(&p);
    pboot(&p, HM_RESET_POWER_ON, PT0);
    pboot(&p, RESET_CRASH, PT0 + 1 * PH);
    pboot(&p, RESET_PLANNED, PT0 + 2 * PH);
    pboot(&p, HM_RESET_POWER_ON, PT0 + 3 * PH);
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + 4 * PH));
    pboot(&p, HM_RESET_POWER_ON, PT0 + 5 * PH);
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0 + 6 * PH));

    /* one a day is not "within 24 h" */
    hm_power_init(&p);
    for (int i = 0; i < 5; i++) {
        pboot(&p, HM_RESET_POWER_ON, PT0 + i * (PDAY + PH));
    }
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + 4 * (PDAY + PH) + PH));
}

TEST_CASE("health mon: power lost stays up while power-on boots keep coming less than 24 h apart", "[health_mon]")
{
    hm_power_t p;
    hm_power_init(&p);
    pboot(&p, HM_RESET_POWER_ON, PT0);
    pboot(&p, HM_RESET_POWER_ON, PT0 + 1 * PH);
    pboot(&p, HM_RESET_POWER_ON, PT0 + 2 * PH);
    pboot(&p, HM_RESET_POWER_ON, PT0 + 22 * PH);
    pboot(&p, HM_RESET_POWER_ON, PT0 + 44 * PH);
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0 + 60 * PH));
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + 44 * PH + PDAY));
    /* a new run needs three again */
    pboot(&p, HM_RESET_POWER_ON, PT0 + 80 * PH);
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + 81 * PH));
}

TEST_CASE("health mon: power-on boots without a trusted clock count as inside the window", "[health_mon]")
{
    hm_power_t p;
    hm_power_init(&p);
    /* a boot dated two days ago, then three boots that never had a clock */
    pboot(&p, RESET_PLANNED, PT0 - 2 * PDAY);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, HM_RESET_POWER_ON, 0);
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0));
    TEST_ASSERT_FALSE(hm_power_alert(&p, 0));
    pboot(&p, HM_RESET_POWER_ON, 0);
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0));
    TEST_ASSERT_TRUE(hm_power_alert(&p, 0));   /* no clock now either: three undated boots */

    /* a later trusted time dates them: older than 24 h, they no longer count */
    pboot(&p, RESET_PLANNED, PT0 - PDAY - PH);
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0));
    TEST_ASSERT_FALSE(hm_power_alert(&p, 0));

    /* placed at the start of the next dated boot, inside the window: they count */
    hm_power_init(&p);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, HM_RESET_POWER_ON, PT0);
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0 + PH));
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + PDAY));
}

TEST_CASE("health mon: the running boot is dated from the clock, so the alert clears 24 h after it", "[health_mon]")
{
    /* Three short power-on sessions, none of them with a dated record; the
     * third one then runs for weeks without writing a dated record either. */
    hm_power_t p;
    hm_power_init(&p);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, HM_RESET_POWER_ON, 0);
    const int64_t start = PT0;   /* the running boot started here (now minus its uptime) */
    hm_power_running(&p, start + 600, 600);
    TEST_ASSERT_EQUAL_INT64(start, p.boot[2].t);
    TEST_ASSERT_EQUAL_INT64(start, p.boot[0].ub);   /* the earlier ones came before it */
    TEST_ASSERT_TRUE(hm_power_alert(&p, start + 600));
    hm_power_running(&p, start + PDAY - 1, (uint32_t)(PDAY - 1));
    TEST_ASSERT_TRUE(hm_power_alert(&p, start + PDAY - 1));
    hm_power_running(&p, start + PDAY, (uint32_t)PDAY);
    TEST_ASSERT_FALSE(hm_power_alert(&p, start + PDAY));
    hm_power_running(&p, start + 20 * PDAY, (uint32_t)(20 * PDAY));
    TEST_ASSERT_FALSE(hm_power_alert(&p, start + 20 * PDAY));

    /* Weeks apart, each session undated: the running boot's start shows they
     * were not within 24 h of it. */
    hm_power_init(&p);
    pboot(&p, RESET_PLANNED, start - 30 * PDAY);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, RESET_PLANNED, start - 15 * PDAY);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, RESET_PLANNED, start - 2 * PDAY);
    pboot(&p, HM_RESET_POWER_ON, 0);
    hm_power_running(&p, start + 60, 60);
    TEST_ASSERT_FALSE(hm_power_alert(&p, start + 60));

    /* A running boot that is not a power-on boot only bounds the earlier ones. */
    hm_power_init(&p);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, HM_RESET_POWER_ON, 0);
    pboot(&p, RESET_PLANNED, 0);
    hm_power_running(&p, start + 60, 60);
    TEST_ASSERT_EQUAL_INT64(start, p.boot[2].ub);
    TEST_ASSERT_TRUE(hm_power_alert(&p, start + 60));
    TEST_ASSERT_FALSE(hm_power_alert(&p, start + PDAY));

    /* No trusted clock: nothing changes. */
    hm_power_init(&p);
    pboot(&p, HM_RESET_POWER_ON, 0);
    hm_power_running(&p, 0, 600);
    TEST_ASSERT_EQUAL_INT64(0, p.boot[0].t);
    TEST_ASSERT_EQUAL_INT64(0, p.boot[0].ub);
}

TEST_CASE("health mon: a boot record with its own clock dates the boot; old boots fall out", "[health_mon]")
{
    hm_power_t p;
    hm_power_init(&p);
    for (int i = 0; i < 3; i++) {
        hm_power_feed(&p, HM_EV_BOOT, HM_RESET_POWER_ON, PT0 + i * PH + 2, 2);
    }
    TEST_ASSERT_TRUE(hm_power_alert(&p, PT0 + 3 * PH));
    TEST_ASSERT_EQUAL_INT64(PT0 + PH, p.boot[1].t);
    /* many more boots than kept: the newest decide */
    for (int i = 0; i < 40; i++) {
        pboot(&p, HM_RESET_POWER_ON, PT0 + 10 * PDAY + i * 2 * PDAY);
    }
    TEST_ASSERT_EQUAL_UINT8(HM_POWER_BOOTS, p.n);
    TEST_ASSERT_FALSE(hm_power_alert(&p, PT0 + 10 * PDAY + 39 * 2 * PDAY + PH));
}

/* ---- GNSS ladder restart hold ---------------------------------------------------- */

#define HOUR (60 * MIN)

/* A configured station well past its first hour, nobody around, no episode. */
static hm_hold_in_t hold_in(int64_t now)
{
    hm_hold_in_t in = {
        .now_ms = now,
        .episode = false,
        .settings_available = true,
        .setup_done = true,
        .ap_clients = 0,
        .admin_at_ms = HM_NEVER,
        .admin_window_ms = 300u * SEC,
        .presence_max_ms = 30u * MIN,
        .station_time_s = 2u * 3600u,
        .power_on_grace_s = 3600u,
        .heard = true,
    };
    return in;
}

/* Run once a second from `from` for `ms`; returns the last hold. */
static uint8_t hold_run(hm_hold_t *h, hm_hold_in_t *in, int64_t from, int64_t ms, uint32_t *left)
{
    uint8_t hold = 0;
    for (int64_t t = 0; t <= ms; t += SEC) {
        in->now_ms = from + t;
        hold = hm_reboot_hold(h, in, left);
    }
    return hold;
}

TEST_CASE("health mon: restart hold reasons", "[health_mon]")
{
    hm_hold_t h;
    hm_hold_init(&h);
    uint32_t left = 0;
    hm_hold_in_t in = hold_in(T0);
    TEST_ASSERT_EQUAL_UINT8(0, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, left);

    in.setup_done = false;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_SETUP, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, left);   /* setup is not capped */
    in.settings_available = false;                /* nothing can be stored: no setup hold */
    TEST_ASSERT_EQUAL_UINT8(0, hm_reboot_hold(&h, &in, NULL));

    in = hold_in(T0);
    in.ap_clients = 1;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_AP_CLIENT, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(1800, left);

    /* power_on: early, nothing heard; it ends with the grace at the latest */
    in = hold_in(T0);
    in.station_time_s = 27;
    in.heard = false;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_POWER_ON, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(3600 - 27, left);
    /* a receiver heard this boot (RTCM or a valid text line): no hold, 6a at once */
    in.heard = true;
    TEST_ASSERT_EQUAL_UINT8(0, hm_reboot_hold(&h, &in, NULL));
    /* past the grace: no hold either */
    in.heard = false;
    in.station_time_s = 3600;
    TEST_ASSERT_EQUAL_UINT8(0, hm_reboot_hold(&h, &in, NULL));
}

TEST_CASE("health mon: the admin window expires at 300 s", "[health_mon]")
{
    hm_hold_t h;
    hm_hold_init(&h);
    hm_hold_in_t in = hold_in(T0);
    in.admin_at_ms = T0;
    in.now_ms = T0;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN, hm_reboot_hold(&h, &in, NULL));
    in.now_ms = T0 + 299 * SEC;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN, hm_reboot_hold(&h, &in, NULL));
    in.now_ms = T0 + 300 * SEC;
    TEST_ASSERT_EQUAL_UINT8(0, hm_reboot_hold(&h, &in, NULL));
    /* a new request opens it again */
    in.admin_at_ms = in.now_ms;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN, hm_reboot_hold(&h, &in, NULL));
}

TEST_CASE("health mon: ap_client and admin are capped at 30 min per ladder episode", "[health_mon]")
{
    hm_hold_t h;
    hm_hold_init(&h);
    uint32_t left = 0;
    hm_hold_in_t in = hold_in(T0);
    in.episode = true;
    in.ap_clients = 1;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_AP_CLIENT, hold_run(&h, &in, T0, 10 * MIN, &left));
    TEST_ASSERT_EQUAL_UINT32(20 * 60, left);

    /* the client leaves for a while: the time held so far stays used */
    in.ap_clients = 0;
    TEST_ASSERT_EQUAL_UINT8(0, hold_run(&h, &in, T0 + 10 * MIN + SEC, 30 * MIN, NULL));

    /* the admin page takes over the rest: together at most 30 min */
    int64_t t1 = T0 + 41 * MIN;
    in.admin_at_ms = t1;
    in.now_ms = t1;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_TRUE(left <= 20 * 60 && left >= 20 * 60 - 2);
    in.ap_clients = 1;
    uint8_t hold = 0;
    int64_t t = t1;
    for (; t < t1 + 25 * MIN; t += SEC) {
        in.now_ms = t;
        in.admin_at_ms = t;   /* the page pings every second */
        hold = hm_reboot_hold(&h, &in, &left);
        if (hold == 0) {
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT8(0, hold);
    TEST_ASSERT_TRUE(t - t1 >= 19 * MIN && t - t1 <= 20 * MIN + 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, left);

    /* setup still holds past the limit: it is not capped */
    in.setup_done = false;
    in.now_ms = t + SEC;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_SETUP, hm_reboot_hold(&h, &in, &left));

    /* a ladder reset ends the episode: the next one has the full 30 min */
    in.setup_done = true;
    in.episode = false;
    in.now_ms = t + 2 * SEC;
    (void)hm_reboot_hold(&h, &in, NULL);
    in.episode = true;
    in.now_ms = t + 3 * SEC;
    in.admin_at_ms = in.now_ms;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_AP_CLIENT | HM_HOLD_ADMIN, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(1800, left);

    /* a restart starts afresh too (the state is RAM only) */
    hm_hold_init(&h);
    in.now_ms = t + 4 * SEC;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_AP_CLIENT | HM_HOLD_ADMIN, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(1800, left);
}

TEST_CASE("health mon: presence outside a ladder episode uses none of the limit", "[health_mon]")
{
    hm_hold_t h;
    hm_hold_init(&h);
    uint32_t left = 0;
    hm_hold_in_t in = hold_in(T0);
    in.ap_clients = 2;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_AP_CLIENT, hold_run(&h, &in, T0, 2 * HOUR, &left));
    TEST_ASSERT_EQUAL_UINT32(1800, left);
}

TEST_CASE("health mon: first setup holds for days", "[health_mon]")
{
    hm_hold_t h;
    hm_hold_init(&h);
    hm_hold_in_t in = hold_in(T0);
    in.episode = true;
    in.setup_done = false;
    in.station_time_s = 0;
    in.heard = false;
    uint8_t hold = 0;
    for (int64_t t = 0; t <= 72 * HOUR; t += MIN) {
        in.now_ms = T0 + t;
        in.station_time_s = (uint32_t)(t / SEC);
        hold = hm_reboot_hold(&h, &in, NULL);
        TEST_ASSERT_TRUE((hold & HM_HOLD_SETUP) != 0);
    }
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_SETUP, hold);   /* power_on is over after its hour */
}

TEST_CASE("health mon: first setup holds only while no caster is enabled", "[health_mon]")
{
    hm_hold_t h;
    hm_hold_init(&h);
    uint32_t left = 0;
    hm_hold_in_t in = hold_in(T0);
    in.episode = true;
    in.setup_done = false;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_SETUP, hm_reboot_hold(&h, &in, &left));
    /* the Onocoy step saved and enabled its slot before setup was marked done:
     * the station streams, so a restart it needs is not held for the setup */
    in.casters_enabled = true;
    in.now_ms = T0 + SEC;
    TEST_ASSERT_EQUAL_UINT8(0, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, left);
    /* a person on the page still holds it, within the presence limit */
    in.now_ms = T0 + 2 * SEC;
    in.admin_at_ms = in.now_ms;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_TRUE(left <= 1800 && left >= 1798);
}

TEST_CASE("health mon: the hold ends at the latest when its last reason does", "[health_mon]")
{
    hm_hold_t h;
    hm_hold_init(&h);
    uint32_t left = 0;
    hm_hold_in_t in = hold_in(T0);
    in.episode = true;
    in.station_time_s = 8 * 60;      /* 52 min of the power-on grace left */
    in.heard = false;
    in.admin_at_ms = T0;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN | HM_HOLD_POWER_ON, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(52 * 60, left);   /* not the 30 min presence limit */

    /* past most of the grace, the presence limit is the later end */
    in.station_time_s = 3600 - 60;
    in.now_ms = T0 + SEC;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN | HM_HOLD_POWER_ON, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_TRUE(left > 60 && left <= 1800);

    /* first setup has no limit: no end to name */
    in.setup_done = false;
    in.now_ms = T0 + 2 * SEC;
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_SETUP | HM_HOLD_ADMIN | HM_HOLD_POWER_ON, hm_reboot_hold(&h, &in, &left));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, left);
}

TEST_CASE("health mon: the status shows the restart hold only during a ladder episode", "[health_mon]")
{
    hm_hold_view_t v;
    /* a healthy station with the admin page open: nothing is pending */
    hm_hold_view(false, HM_HOLD_ADMIN, 1800, false, 900, &v);
    TEST_ASSERT_EQUAL_UINT8(0, v.hold);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, v.hold_left_s);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, v.cap_wait_s);

    hm_hold_view(true, HM_HOLD_ADMIN, 1700, false, 900, &v);
    TEST_ASSERT_EQUAL_UINT8(HM_HOLD_ADMIN, v.hold);
    TEST_ASSERT_EQUAL_UINT32(1700, v.hold_left_s);
    TEST_ASSERT_EQUAL_UINT32(900, v.cap_wait_s);

    /* the cap allows a restart now: no wait to show */
    hm_hold_view(true, 0, UINT32_MAX, true, 0, &v);
    TEST_ASSERT_EQUAL_UINT8(0, v.hold);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, v.cap_wait_s);
}
