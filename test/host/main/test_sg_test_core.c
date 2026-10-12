/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signal-group comparison core: start refusals, the measuring order, a whole
 * run with its sample maths, refusals and failures of a switch, keep, stop,
 * the 24 h timeout, restarts from every state through the stored record,
 * the save cadence and damaged records. The "station" here takes the core's
 * switch requests and reports the applies the way station.c does.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "gnss_rtcm3.h"
#include "sg_test_core.h"
#include "unity.h"

#define T0   1000000LL
#define U0   1790000000LL
#define SEC  1000LL
#define MIN  (60 * SEC)
#define HOUR (60 * MIN)

typedef struct {
    sgt_t t;
    int64_t now;
    int64_t unix_s;
    unsigned switches;       /* switches the station queued */
    station_sg_t switched;   /* the last one */
    unsigned saves;
    sgt_sample_t good;       /* handed in at every due sample */
    bool streaming;
    bool auto_ok;            /* every queued switch succeeds at once */
} fx_t;

static fx_t s_fx;

static station_sg_t G(uint8_t m, uint8_t s)
{
    return (station_sg_t){ .master = m, .slave = s };
}

static bool same(station_sg_t a, station_sg_t b)
{
    return a.master == b.master && a.slave == b.slave;
}

static sgt_guard_t ok_guard(station_sg_t running)
{
    sgt_guard_t g;
    memset(&g, 0, sizeof(g));
    g.settings_ok = true;
    g.autocfg = true;
    g.supported = true;
    g.identity_confirmed = true;
    g.streaming = true;
    g.position_locked = true;
    g.running = running;
    return g;
}

/* Galileo on four bands, GPS on three, QZSS on four (never best). */
static void sample_fill(sgt_sample_t *s, uint8_t gal_l1, uint8_t gal_cnr)
{
    memset(s, 0, sizeof(*s));
    s->streaming = true;
    s->epoch_age_ms = 400;
    s->sys_sats[GNSS_SYS_GALILEO] = gal_l1;
    s->band_sats[GNSS_SYS_GALILEO][GNSS_BAND_L1] = gal_l1;
    s->band_sats[GNSS_SYS_GALILEO][GNSS_BAND_L5] = 9;
    s->band_sats[GNSS_SYS_GALILEO][GNSS_BAND_L2] = 8;
    s->band_sats[GNSS_SYS_GALILEO][GNSS_BAND_L6] = 7;
    s->cnr_dbhz[GNSS_SYS_GALILEO][GNSS_BAND_L1] = gal_cnr;
    s->cnr_dbhz[GNSS_SYS_GALILEO][GNSS_BAND_L5] = 47;
    s->signal_mask[GNSS_SYS_GALILEO] = (1u << (2 - 1)) | (1u << (22 - 1)) | (1u << (15 - 1)) | (1u << (9 - 1));
    s->sys_sats[GNSS_SYS_GPS] = 9;
    s->band_sats[GNSS_SYS_GPS][GNSS_BAND_L1] = 9;
    s->band_sats[GNSS_SYS_GPS][GNSS_BAND_L2] = 8;
    s->band_sats[GNSS_SYS_GPS][GNSS_BAND_L5] = 5;
    s->signal_mask[GNSS_SYS_GPS] = (1u << (2 - 1)) | (1u << (16 - 1)) | (1u << (22 - 1));
    s->sys_sats[GNSS_SYS_QZSS] = 2;
    for (unsigned b = 0; b < 4; b++) {
        s->band_sats[GNSS_SYS_QZSS][b] = 2;
    }
    s->sys_sats[GNSS_SYS_SBAS] = 3;   /* never listed */
    s->band_sats[GNSS_SYS_SBAS][GNSS_BAND_L1] = 3;
}

static fx_t *fx_new(void)
{
    fx_t *x = &s_fx;
    memset(x, 0, sizeof(*x));
    sgt_init(&x->t);
    x->now = T0;
    x->unix_s = U0;
    x->streaming = true;
    x->auto_ok = true;
    sample_fill(&x->good, 10, 44);
    return x;
}

static void fx_apply(fx_t *x, bool ok, bool rejected, bool read_known, station_sg_t read)
{
    sgt_apply_t a;
    memset(&a, 0, sizeof(a));
    a.ok = ok;
    a.rejected = rejected;
    a.read_known = read_known;
    a.read = read;
    strcpy(a.reason, ok ? "" : rejected ? "PARSING FAILD NO MATCHING FUNC" : "timeout");
    sgt_apply_done(&x->t, &a, x->now, x->unix_s);
}

/* The station's side of one step: take a switch request (and, with auto_ok, its successful apply). */
static void fx_station(fx_t *x)
{
    if (x->t.want_switch) {
        x->switched = x->t.target;
        sgt_switch_queued(&x->t);
        x->switches++;
        if (x->auto_ok) {
            fx_apply(x, true, false, true, x->switched);
        }
    }
    if (x->t.save_due) {
        sgt_saved(&x->t, x->now);
        x->saves++;
    }
}

static void fx_run(fx_t *x, int64_t ms)
{
    int64_t end = x->now + ms;
    while (x->now < end) {
        x->now += SEC;
        x->unix_s++;
        bool due = sgt_sample_due(&x->t, x->now);
        sgt_tick(&x->t, x->streaming, due ? &x->good : NULL, x->now, x->unix_s);
        fx_station(x);
    }
}

static int fx_start(fx_t *x, station_sg_t running, station_sg_t a, station_sg_t b, uint32_t phase_s)
{
    sgt_guard_t g = ok_guard(running);
    station_sg_t groups[2] = { a, b };
    int e = sgt_start(&x->t, &g, groups, phase_s, x->now);
    fx_station(x);
    return e;
}

static unsigned notes_of(fx_t *x, uint8_t event, sgt_note_t *last)
{
    sgt_note_t n[SGT_NOTES];
    unsigned c = sgt_take_notes(&x->t, n, SGT_NOTES);
    unsigned k = 0;
    for (unsigned i = 0; i < c; i++) {
        if (n[i].event == event) {
            k++;
            if (last != NULL) {
                *last = n[i];
            }
        }
    }
    return k;
}

/* ---- refusals -------------------------------------------------------------------- */

TEST_CASE("sg test: every start refusal, arguments first", "[sg_test]")
{
    fx_t *x = fx_new();
    station_sg_t groups[2] = { G(3, 6), G(7, 0) };
    sgt_guard_t g = ok_guard(G(3, 6));

    station_sg_t same_pair[2] = { G(3, 6), G(3, 6) };
    TEST_ASSERT_EQUAL(STATION_SGT_E_GROUPS, sgt_start(&x->t, &g, same_pair, 1800, x->now));
    TEST_ASSERT_EQUAL(STATION_SGT_E_PHASE, sgt_start(&x->t, &g, groups, 59, x->now));
    TEST_ASSERT_EQUAL(STATION_SGT_E_PHASE, sgt_start(&x->t, &g, groups, 7201, x->now));

    static const struct {
        size_t off;
        bool value;
        int err;
    } k[] = {
        { offsetof(sgt_guard_t, settings_ok), false, STATION_SGT_E_SETTINGS },
        { offsetof(sgt_guard_t, first_apply_held), true, STATION_SGT_E_FIRST_APPLY_HELD },
        { offsetof(sgt_guard_t, autocfg), false, STATION_SGT_E_AUTOCFG_OFF },
        { offsetof(sgt_guard_t, maintenance), true, STATION_SGT_E_MAINTENANCE },
        { offsetof(sgt_guard_t, journal_open), true, STATION_SGT_E_JOURNAL_OPEN },
        { offsetof(sgt_guard_t, safe_mode), true, STATION_SGT_E_SAFE_MODE },
        { offsetof(sgt_guard_t, supported), false, STATION_SGT_E_UNSUPPORTED },
        { offsetof(sgt_guard_t, identity_confirmed), false, STATION_SGT_E_IDENTITY },
        { offsetof(sgt_guard_t, streaming), false, STATION_SGT_E_NOT_STREAMING },
        { offsetof(sgt_guard_t, config_failed), true, STATION_SGT_E_CONFIG_FAILED },
        { offsetof(sgt_guard_t, position_locked), false, STATION_SGT_E_NOT_LOCKED },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        sgt_guard_t b = ok_guard(G(3, 6));
        *(bool *)((uint8_t *)&b + k[i].off) = k[i].value;
        TEST_ASSERT_EQUAL_MESSAGE(k[i].err, sgt_start(&x->t, &b, groups, 1800, x->now), "guard");
        TEST_ASSERT_FALSE(sgt_running(&x->t));
        TEST_ASSERT_FALSE(x->t.want_switch);
    }
    /* Running already. */
    TEST_ASSERT_EQUAL(STATION_SGT_OK, sgt_start(&x->t, &g, groups, 1800, x->now));
    TEST_ASSERT_EQUAL(STATION_SGT_E_RUNNING, sgt_start(&x->t, &g, groups, 1800, x->now));
    /* Keep only while deciding; stop only while running. */
    TEST_ASSERT_EQUAL(STATION_SGT_E_NOT_DECIDING, sgt_keep(&x->t, 0, x->now, x->unix_s));
    fx_t *y = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_E_IDLE, sgt_stop(&y->t, y->now, y->unix_s));
}

/* ---- order and a whole run ---------------------------------------------------------- */

TEST_CASE("sg test: the running group is measured first; otherwise A then B", "[sg_test]")
{
    fx_t *x = fx_new();
    /* B runs now: measured first, no switch. */
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(7, 0), G(3, 6), 60));
    TEST_ASSERT_EQUAL(STATION_SGT_SETTLING, x->t.state);
    TEST_ASSERT_EQUAL_UINT(0, x->switches);
    TEST_ASSERT_EQUAL_UINT8(1, x->t.order[0]);
    sgt_note_t n;
    TEST_ASSERT_EQUAL_UINT(1, notes_of(x, SGT_EV_STARTED, &n));
    TEST_ASSERT_EQUAL_INT32(sgt_note_a(SGT_EV_STARTED, G(3, 6)), n.a);
    TEST_ASSERT_EQUAL_INT32(60, n.b);
    fx_run(x, 2 * MIN + 30 * SEC);
    TEST_ASSERT_EQUAL_UINT(1, x->switches);   /* only to A for phase 1 */
    TEST_ASSERT_TRUE(same(G(7, 0), x->switched));

    /* Neither runs now: A first (a switch), then B (another). */
    x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(4, 5), G(3, 6), G(7, 0), 60));
    TEST_ASSERT_EQUAL_UINT8(0, x->t.order[0]);
    TEST_ASSERT_EQUAL_UINT(1, x->switches);
    TEST_ASSERT_TRUE(same(G(3, 6), x->switched));
    TEST_ASSERT_EQUAL(STATION_SGT_SETTLING, x->t.state);
    TEST_ASSERT_EQUAL_UINT(1, notes_of(x, SGT_EV_SWITCHED, &n));
    TEST_ASSERT_EQUAL_INT32(0, n.b);   /* phase 0 */

    /* A runs now: A first. */
    x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 60));
    TEST_ASSERT_EQUAL_UINT8(0, x->t.order[0]);
    TEST_ASSERT_EQUAL_UINT(0, x->switches);
}

TEST_CASE("sg test: a whole run to DECIDING with the sample maths; only good samples count", "[sg_test]")
{
    fx_t *x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 120));
    /* Settling: a minute, nothing measured. */
    fx_run(x, 59 * SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_SETTLING, x->t.state);
    station_sg_test_t v;
    sgt_view(&x->t, x->now, &v);
    TEST_ASSERT_EQUAL(0, v.phase);
    TEST_ASSERT_EQUAL_UINT32(1 + 120, v.left_s);
    TEST_ASSERT_TRUE(v.have_running);
    TEST_ASSERT_TRUE(same(G(3, 6), v.running));
    fx_run(x, SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_MEASURING, x->t.state);

    /* Good samples alternate between 10 and 12 Galileo L1 satellites (CNR
     * 44 and 46); stale ones and ones taken while not streaming (with other
     * figures) are left out. */
    unsigned good = 0;
    unsigned k = 0;
    while (x->t.state == STATION_SGT_MEASURING && x->t.phase == 0) {
        x->now += SEC;
        if (!sgt_sample_due(&x->t, x->now)) {
            sgt_tick(&x->t, true, NULL, x->now, x->unix_s);
            fx_station(x);
            continue;
        }
        sgt_sample_t s;
        bool streaming = true;
        if (k % 4 == 3) {
            sample_fill(&s, 30, 20);
            s.epoch_age_ms = 5000;   /* stale */
        } else if (k % 4 == 2) {
            sample_fill(&s, 30, 20);
            s.streaming = false;
            streaming = false;
        } else {
            sample_fill(&s, good % 2 == 0 ? 10 : 12, good % 2 == 0 ? 44 : 46);
            good++;
        }
        k++;
        sgt_tick(&x->t, streaming, &s, x->now, x->unix_s);
        fx_station(x);
    }
    TEST_ASSERT_EQUAL_UINT(12, good);
    TEST_ASSERT_EQUAL_UINT32(12, x->t.acc[0].samples);
    TEST_ASSERT_EQUAL_UINT8(STATION_SGT_RES_DONE, x->t.acc[0].status);
    TEST_ASSERT_EQUAL_UINT8(1, x->t.phase);
    TEST_ASSERT_EQUAL(STATION_SGT_SETTLING, x->t.state);   /* switched to B and settling */
    TEST_ASSERT_TRUE(same(G(7, 0), x->switched));

    sgt_view(&x->t, x->now, &v);
    const station_sgt_result_t *r = &v.results[0];
    TEST_ASSERT_EQUAL_UINT32(120, r->measured_s);
    TEST_ASSERT_EQUAL_UINT8(GNSS_SYS_GALILEO, r->best_sys);
    TEST_ASSERT_EQUAL_UINT8(4, r->best_bands);
    TEST_ASSERT_EQUAL_UINT8(3, r->nsys);   /* GPS, Galileo, QZSS; never SBAS */
    TEST_ASSERT_EQUAL_UINT8(GNSS_SYS_GPS, r->sys[0].sys);
    TEST_ASSERT_EQUAL_UINT8(GNSS_SYS_GALILEO, r->sys[1].sys);
    TEST_ASSERT_EQUAL_UINT8(GNSS_SYS_QZSS, r->sys[2].sys);
    const station_sgt_sys_t *gal = &r->sys[1];
    TEST_ASSERT_EQUAL_UINT16(110, gal->sats_x10);
    TEST_ASSERT_EQUAL_UINT8(4, gal->nband);
    TEST_ASSERT_EQUAL_UINT8(GNSS_BAND_L1, gal->band[0].band);
    TEST_ASSERT_EQUAL_UINT16(110, gal->band[0].sats_x10);
    TEST_ASSERT_EQUAL_UINT16(450, gal->band[0].cnr_x10);
    TEST_ASSERT_EQUAL_HEX32(1u << (2 - 1), gal->band[0].signal_mask);
    TEST_ASSERT_EQUAL_UINT8(GNSS_BAND_L2, gal->band[1].band);
    TEST_ASSERT_EQUAL_UINT16(0, gal->band[1].cnr_x10);   /* no CNR measured */
    TEST_ASSERT_EQUAL_HEX32(1u << (15 - 1), gal->band[1].signal_mask);
    TEST_ASSERT_EQUAL_UINT8(GNSS_BAND_L5, gal->band[2].band);
    TEST_ASSERT_EQUAL_UINT16(90, gal->band[2].sats_x10);
    TEST_ASSERT_EQUAL_UINT16(470, gal->band[2].cnr_x10);
    TEST_ASSERT_EQUAL_UINT8(GNSS_BAND_L6, gal->band[3].band);
    TEST_ASSERT_EQUAL_UINT8(4, r->sys[2].nband);   /* QZSS: four bands, not best */
    TEST_ASSERT_EQUAL_UINT8(STATION_SGT_RES_MEASURING, v.results[1].status);

    /* Phase 1 to its end: deciding, on B. */
    fx_run(x, 60 * SEC + 120 * SEC + 5 * SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_DECIDING, x->t.state);
    sgt_view(&x->t, x->now, &v);
    TEST_ASSERT_EQUAL(-1, v.phase);
    TEST_ASSERT_TRUE(same(G(7, 0), v.running));
    TEST_ASSERT_UINT32_WITHIN(10, SGT_DECIDE_S, v.decide_left_s);
    TEST_ASSERT_EQUAL_UINT8(STATION_SGT_RES_DONE, v.results[1].status);
    TEST_ASSERT_EQUAL_UINT32(120, v.results[1].measured_s);
    TEST_ASSERT_EQUAL_UINT(1, x->switches);
}

TEST_CASE("sg test: epoch gaps count only while measuring good samples", "[sg_test]")
{
    fx_t *x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 60));
    x->good.epoch_gaps = 50;   /* before the phase: not this group's */
    fx_run(x, 70 * SEC);       /* settled; the first sample sets the base */
    x->good.epoch_gaps = 53;
    fx_run(x, 10 * SEC);
    x->good.epoch_gaps = 2;    /* gnss_obs started over: no negative count */
    fx_run(x, 10 * SEC);
    x->good.epoch_gaps = 3;
    fx_run(x, 10 * SEC);
    TEST_ASSERT_EQUAL_UINT32(3 + 0 + 1, x->t.acc[0].epoch_gaps);
}

/* ---- refusals and failures of a switch ---------------------------------------------- */

TEST_CASE("sg test: a group refused in phase 0 or in phase 1 ends the test as rejected, back to the group before",
          "[sg_test]")
{
    /* Phase 0: 7 0 is refused outright; the receiver still runs 3 6. */
    fx_t *x = fx_new();
    x->auto_ok = false;
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(7, 0), G(5, 0), 60));
    TEST_ASSERT_EQUAL(STATION_SGT_SWITCHING, x->t.state);
    TEST_ASSERT_TRUE(same(G(7, 0), x->switched));
    (void)notes_of(x, 0, NULL);
    fx_apply(x, false, true, true, G(3, 6));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);   /* it runs 3 6 already: no switch back */
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_REJECTED, x->t.outcome);
    TEST_ASSERT_EQUAL_UINT8(STATION_SGT_RES_REJECTED, x->t.acc[0].status);
    TEST_ASSERT_NOT_NULL(strstr(x->t.reason, "refused signal group 7 0"));
    sgt_note_t n[SGT_NOTES];
    unsigned c = sgt_take_notes(&x->t, n, SGT_NOTES);
    TEST_ASSERT_EQUAL_UINT(2, c);
    TEST_ASSERT_EQUAL_UINT8(SGT_EV_REFUSED, n[0].event);
    TEST_ASSERT_EQUAL_INT32(sgt_note_a(SGT_EV_REFUSED, G(7, 0)), n[0].a);
    TEST_ASSERT_EQUAL_INT32((3 << 8) | 6, n[0].b);
    TEST_ASSERT_EQUAL_UINT8(SGT_EV_ENDED, n[1].event);
    TEST_ASSERT_EQUAL_INT32(sgt_note_a(SGT_EV_ENDED, G(3, 6)), n[1].a);
    TEST_ASSERT_EQUAL_INT32(STATION_SGT_OUT_REJECTED, n[1].b);
    station_sg_test_t v;
    sgt_view(&x->t, x->now, &v);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_REJECTED, v.outcome);
    TEST_ASSERT_EQUAL_UINT32(0, v.ended_age_s);
    TEST_ASSERT_EQUAL_INT64(x->unix_s, v.ended_unix);

    /* Phase 1: A (3 6, running) measured, then B (7 0) refused after its
     * restart: the receiver read back 3 6, which is the group from before. */
    x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 60));
    x->auto_ok = false;
    fx_run(x, 2 * MIN + 30 * SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_SWITCHING, x->t.state);
    TEST_ASSERT_EQUAL_UINT8(1, x->t.phase);
    fx_apply(x, false, true, true, G(3, 6));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_REJECTED, x->t.outcome);
    TEST_ASSERT_EQUAL_UINT8(STATION_SGT_RES_DONE, x->t.acc[0].status);
    TEST_ASSERT_EQUAL_UINT8(STATION_SGT_RES_REJECTED, x->t.acc[1].status);

    /* Phase 1 with A switched to first (4 5 ran before): the refusal of B
     * leaves A running, so the test switches back to 4 5. */
    x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(4, 5), G(3, 6), G(7, 0), 60));
    x->auto_ok = false;
    fx_run(x, 2 * MIN + 30 * SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_SWITCHING, x->t.state);
    unsigned sw = x->switches;
    fx_apply(x, false, true, true, G(3, 6));
    TEST_ASSERT_EQUAL(STATION_SGT_RESTORING, x->t.state);
    fx_station(x);
    TEST_ASSERT_EQUAL_UINT(sw + 1, x->switches);
    TEST_ASSERT_TRUE(same(G(4, 5), x->switched));
    fx_apply(x, true, false, true, G(4, 5));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_REJECTED, x->t.outcome);
}

TEST_CASE("sg test: other switch failures and ten minutes without streaming end the test as failed", "[sg_test]")
{
    /* An apply that times out. */
    fx_t *x = fx_new();
    x->auto_ok = false;
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(4, 5), G(3, 6), G(7, 0), 60));
    fx_apply(x, false, false, false, G(0, 0));
    TEST_ASSERT_EQUAL(STATION_SGT_RESTORING, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_FAILED, x->t.outcome);
    fx_station(x);
    TEST_ASSERT_TRUE(same(G(4, 5), x->switched));
    fx_apply(x, true, false, true, G(4, 5));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_NOT_NULL(strstr(x->t.reason, "failed"));

    /* A switch that never finishes. */
    x = fx_new();
    x->auto_ok = false;
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(4, 5), G(3, 6), G(7, 0), 60));
    fx_run(x, 10 * MIN - SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_SWITCHING, x->t.state);
    fx_run(x, SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_RESTORING, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_FAILED, x->t.outcome);
    TEST_ASSERT_TRUE(same(G(4, 5), x->switched));   /* the restore always switches */
    fx_run(x, 10 * MIN);                             /* the restore never finishes either */
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_FAILED, x->t.outcome);

    /* Ten minutes without streaming while measuring (a short outage is fine). */
    x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 3600));
    fx_run(x, 5 * MIN);
    x->streaming = false;
    fx_run(x, 9 * MIN);
    x->streaming = true;
    fx_run(x, MIN);
    TEST_ASSERT_EQUAL(STATION_SGT_MEASURING, x->t.state);
    x->streaming = false;
    fx_run(x, 10 * MIN);   /* the first silent second starts the count */
    TEST_ASSERT_EQUAL(STATION_SGT_MEASURING, x->t.state);
    fx_run(x, SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);   /* 3 6 runs: nothing to switch back */
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_FAILED, x->t.outcome);
    TEST_ASSERT_NOT_NULL(strstr(x->t.reason, "did not stream"));
    TEST_ASSERT_EQUAL_UINT8(STATION_SGT_RES_DONE, x->t.acc[0].status);   /* what was measured stays */
}

/* ---- keep, stop, timeout -------------------------------------------------------------- */

static fx_t *to_deciding(station_sg_t running, station_sg_t a, station_sg_t b)
{
    fx_t *x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, running, a, b, 60));
    fx_run(x, 5 * MIN);
    TEST_ASSERT_EQUAL(STATION_SGT_DECIDING, x->t.state);
    return x;
}

TEST_CASE("sg test: keep switches only when the kept group is not the one running", "[sg_test]")
{
    /* 3 6 measured first, 7 0 last (running now). Keep 7 0 (index 1): no switch. */
    fx_t *x = to_deciding(G(3, 6), G(3, 6), G(7, 0));
    unsigned sw = x->switches;
    TEST_ASSERT_EQUAL(STATION_SGT_OK, sgt_keep(&x->t, 1, x->now, x->unix_s));
    fx_station(x);
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_KEPT, x->t.outcome);
    TEST_ASSERT_EQUAL_UINT(sw, x->switches);
    station_sg_test_t v;
    sgt_view(&x->t, x->now, &v);
    TEST_ASSERT_TRUE(v.have_kept);
    TEST_ASSERT_TRUE(same(G(7, 0), v.kept));

    /* Keep 3 6 (index 0): one switch back. */
    x = to_deciding(G(3, 6), G(3, 6), G(7, 0));
    sw = x->switches;
    x->auto_ok = false;
    TEST_ASSERT_EQUAL(STATION_SGT_OK, sgt_keep(&x->t, 0, x->now, x->unix_s));
    TEST_ASSERT_EQUAL(STATION_SGT_RESTORING, x->t.state);
    sgt_view(&x->t, x->now, &v);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_KEPT, v.outcome);   /* decided while restoring */
    fx_station(x);
    TEST_ASSERT_EQUAL_UINT(sw + 1, x->switches);
    TEST_ASSERT_TRUE(same(G(3, 6), x->switched));
    fx_apply(x, true, false, true, G(3, 6));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_KEPT, x->t.outcome);
    TEST_ASSERT_EQUAL_INT8(0, x->t.kept);
    TEST_ASSERT_EQUAL(STATION_SGT_E_NOT_DECIDING, sgt_keep(&x->t, 0, x->now, x->unix_s));
    x = to_deciding(G(3, 6), G(3, 6), G(7, 0));
    TEST_ASSERT_EQUAL(STATION_SGT_E_GROUPS, sgt_keep(&x->t, 2, x->now, x->unix_s));
}

TEST_CASE("sg test: stop from every state goes back to the group from before", "[sg_test]")
{
    for (uint8_t st = STATION_SGT_SWITCHING; st <= STATION_SGT_RESTORING; st++) {
        fx_t *x = fx_new();
        x->auto_ok = st != STATION_SGT_SWITCHING;
        TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(4, 5), G(3, 6), G(7, 0), 60));
        if (st == STATION_SGT_SETTLING) {
            fx_run(x, 10 * SEC);
        } else if (st == STATION_SGT_MEASURING) {
            fx_run(x, 70 * SEC);
        } else if (st == STATION_SGT_DECIDING) {
            fx_run(x, 5 * MIN);
        } else if (st == STATION_SGT_RESTORING) {
            fx_run(x, 5 * MIN);
            x->auto_ok = false;
            TEST_ASSERT_EQUAL(STATION_SGT_OK, sgt_keep(&x->t, 0, x->now, x->unix_s));
            fx_station(x);
        }
        TEST_ASSERT_EQUAL_MESSAGE(st, x->t.state, "state reached");
        x->auto_ok = false;
        TEST_ASSERT_EQUAL(STATION_SGT_OK, sgt_stop(&x->t, x->now, x->unix_s));
        TEST_ASSERT_EQUAL(STATION_SGT_RESTORING, x->t.state);
        TEST_ASSERT_EQUAL(STATION_SGT_OUT_CANCELLED, x->t.outcome);
        TEST_ASSERT_TRUE(same(G(4, 5), x->t.target));
        if (x->t.switch_queued) {
            fx_apply(x, true, false, true, x->t.applying);   /* the switch that was under way ends first */
        }
        fx_station(x);
        TEST_ASSERT_TRUE(same(G(4, 5), x->switched));
        fx_apply(x, true, false, true, G(4, 5));
        TEST_ASSERT_EQUAL_MESSAGE(STATION_SGT_IDLE, x->t.state, "ended");
        TEST_ASSERT_EQUAL(STATION_SGT_OUT_CANCELLED, x->t.outcome);
        TEST_ASSERT_TRUE(same(G(4, 5), x->t.cur));
    }
    /* Stop with the running group measured first, while still on it: nothing to switch. */
    fx_t *x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 60));
    fx_run(x, 30 * SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_OK, sgt_stop(&x->t, x->now, x->unix_s));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_EQUAL_UINT(0, x->switches);
}

TEST_CASE("sg test: no choice within 24 h of running time restores the group from before", "[sg_test]")
{
    fx_t *x = to_deciding(G(4, 5), G(3, 6), G(7, 0));
    fx_run(x, 24 * HOUR - 10 * MIN);
    TEST_ASSERT_EQUAL(STATION_SGT_DECIDING, x->t.state);
    x->auto_ok = false;
    fx_run(x, 10 * MIN);
    TEST_ASSERT_EQUAL(STATION_SGT_RESTORING, x->t.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_TIMED_OUT, x->t.outcome);
    TEST_ASSERT_TRUE(same(G(4, 5), x->switched));
    fx_apply(x, true, false, true, G(4, 5));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, x->t.state);
    TEST_ASSERT_NOT_NULL(strstr(x->t.reason, "No choice"));
}

/* ---- persistence -------------------------------------------------------------------- */

TEST_CASE("sg test: a restart from every state resumes from the stored record", "[sg_test]")
{
    static uint8_t buf[SGT_REC_MAX];
    static sgt_t back;
    for (uint8_t st = STATION_SGT_IDLE; st <= STATION_SGT_RESTORING; st++) {
        fx_t *x = fx_new();
        x->auto_ok = st != STATION_SGT_SWITCHING;
        TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(4, 5), G(3, 6), G(7, 0), 300));
        if (st == STATION_SGT_SETTLING) {
            fx_run(x, 10 * SEC);
        } else if (st == STATION_SGT_MEASURING) {
            fx_run(x, 60 * SEC + 150 * SEC);   /* 15 samples */
        } else if (st == STATION_SGT_DECIDING) {
            fx_run(x, 20 * MIN);
        } else if (st == STATION_SGT_RESTORING || st == STATION_SGT_IDLE) {
            fx_run(x, 20 * MIN);
            x->auto_ok = st == STATION_SGT_IDLE;
            TEST_ASSERT_EQUAL(STATION_SGT_OK, sgt_keep(&x->t, 0, x->now, x->unix_s));
            fx_station(x);
        }
        TEST_ASSERT_EQUAL_MESSAGE(st, x->t.state, "state reached");
        uint32_t samples = x->t.acc[0].samples;
        station_sg_test_t before;
        sgt_view(&x->t, x->now, &before);
        size_t len = sgt_encode(&x->t, x->now, buf, sizeof(buf));
        TEST_ASSERT_GREATER_THAN_UINT(0, len);

        /* The new boot: its own clock. */
        int64_t boot = 5000;
        sgt_init(&back);
        TEST_ASSERT_TRUE(sgt_decode(&back, buf, len, boot));
        TEST_ASSERT_EQUAL_UINT32(samples, back.acc[0].samples);
        TEST_ASSERT_EQUAL_UINT8(x->t.outcome, back.outcome);
        TEST_ASSERT_TRUE(same(back.groups[0], G(3, 6)));
        TEST_ASSERT_TRUE(same(back.before, G(4, 5)));
        switch (st) {
        case STATION_SGT_SWITCHING:
        case STATION_SGT_RESTORING:
            TEST_ASSERT_EQUAL(st, back.state);
            TEST_ASSERT_TRUE(back.want_switch);   /* the switch is asked for again */
            TEST_ASSERT_TRUE(same(x->t.target, back.target));
            break;
        case STATION_SGT_SETTLING:
        case STATION_SGT_MEASURING:
            TEST_ASSERT_EQUAL(STATION_SGT_SETTLING, back.state);   /* settles again */
            TEST_ASSERT_EQUAL_INT64(boot + SGT_SETTLE_S * 1000, back.settle_until_ms);
            break;
        case STATION_SGT_DECIDING: {
            TEST_ASSERT_EQUAL(st, back.state);
            station_sg_test_t v;
            sgt_view(&back, boot, &v);
            TEST_ASSERT_EQUAL_UINT32(before.decide_left_s, v.decide_left_s);   /* keeps counting down */
            break;
        }
        default: {
            TEST_ASSERT_EQUAL(STATION_SGT_IDLE, back.state);
            station_sg_test_t v;
            sgt_view(&back, boot, &v);
            TEST_ASSERT_EQUAL(STATION_SGT_OUT_KEPT, v.outcome);
            TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, v.ended_age_s);   /* not this boot */
            TEST_ASSERT_EQUAL_INT64(before.ended_unix, v.ended_unix);
            break;
        }
        }
    }

    /* Measured time kept: 15 samples before, 15 more after settling again end a 300 s phase. */
    fx_t *x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 300));
    fx_run(x, 60 * SEC + 150 * SEC);
    TEST_ASSERT_EQUAL_UINT32(15, x->t.acc[0].samples);
    size_t len = sgt_encode(&x->t, x->now, buf, sizeof(buf));
    TEST_ASSERT_TRUE(sgt_decode(&x->t, buf, len, x->now));
    fx_run(x, 59 * SEC);
    TEST_ASSERT_EQUAL(STATION_SGT_SETTLING, x->t.state);
    fx_run(x, 150 * SEC + 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(30, x->t.acc[0].samples);
    TEST_ASSERT_EQUAL_UINT8(1, x->t.phase);
}

TEST_CASE("sg test: the record is saved at transitions, every 5 min measuring, every 10 min deciding", "[sg_test]")
{
    fx_t *x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 3600));
    TEST_ASSERT_EQUAL_UINT(1, x->saves);   /* the start */
    fx_run(x, 60 * SEC);
    TEST_ASSERT_EQUAL_UINT(2, x->saves);   /* measuring begins */
    fx_run(x, 5 * MIN - SEC);
    TEST_ASSERT_EQUAL_UINT(2, x->saves);
    fx_run(x, SEC);
    TEST_ASSERT_EQUAL_UINT(3, x->saves);
    fx_run(x, 10 * MIN);
    TEST_ASSERT_EQUAL_UINT(5, x->saves);

    /* Deciding: to_deciding() ends a minute into it (saved when it began). */
    x = to_deciding(G(3, 6), G(3, 6), G(7, 0));
    unsigned at = x->saves;
    fx_run(x, 9 * MIN - SEC);
    TEST_ASSERT_EQUAL_UINT(at, x->saves);
    fx_run(x, SEC);
    TEST_ASSERT_EQUAL_UINT(at + 1, x->saves);
    fx_run(x, HOUR);
    TEST_ASSERT_EQUAL_UINT(at + 7, x->saves);
}

TEST_CASE("sg test: a short, damaged or old record is refused; an unreadable one ends as interrupted", "[sg_test]")
{
    static uint8_t buf[SGT_REC_MAX];
    fx_t *x = fx_new();
    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 300));
    fx_run(x, 3 * MIN);
    size_t len = sgt_encode(&x->t, x->now, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 100 && len <= SGT_REC_MAX);
    TEST_ASSERT_EQUAL_UINT(0, sgt_encode(&x->t, x->now, buf, 100));   /* does not fit */
    len = sgt_encode(&x->t, x->now, buf, sizeof(buf));

    static sgt_t t;
    sgt_init(&t);
    TEST_ASSERT_FALSE(sgt_decode(&t, buf, len - 1, x->now));   /* short */
    TEST_ASSERT_FALSE(sgt_decode(&t, buf, 0, x->now));
    buf[0] = SGT_REC_VERSION + 1;                              /* another version */
    TEST_ASSERT_FALSE(sgt_decode(&t, buf, len, x->now));
    buf[0] = 0;
    TEST_ASSERT_FALSE(sgt_decode(&t, buf, len, x->now));
    buf[0] = SGT_REC_VERSION;
    buf[1] = 9;                                                /* no such state */
    TEST_ASSERT_FALSE(sgt_decode(&t, buf, len, x->now));
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, t.state);             /* unchanged */
    buf[1] = STATION_SGT_MEASURING;
    TEST_ASSERT_TRUE(sgt_decode(&t, buf, len, x->now));

    sgt_interrupted(&t, x->now, x->unix_s);
    station_sg_test_t v;
    sgt_view(&t, x->now, &v);
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, v.state);
    TEST_ASSERT_EQUAL(STATION_SGT_OUT_INTERRUPTED, v.outcome);
    TEST_ASSERT_FALSE(v.have_test);
    sgt_note_t n;
    TEST_ASSERT_EQUAL_UINT(1, sgt_take_notes(&t, &n, 1));
    TEST_ASSERT_EQUAL_UINT8(SGT_EV_ENDED, n.event);
    TEST_ASSERT_EQUAL_INT32(STATION_SGT_OUT_INTERRUPTED, n.b);
}

TEST_CASE("sg test: the status summary rounds its countdowns", "[sg_test]")
{
    fx_t *x = fx_new();
    station_sgt_brief_t b;
    sgt_brief(&x->t, x->now, &b);
    TEST_ASSERT_EQUAL(STATION_SGT_IDLE, b.state);
    TEST_ASSERT_EQUAL(-1, b.phase);
    TEST_ASSERT_FALSE(b.have_running);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, b.ended_age_s);

    TEST_ASSERT_EQUAL(STATION_SGT_OK, fx_start(x, G(3, 6), G(3, 6), G(7, 0), 1800));
    fx_run(x, 61 * SEC);
    sgt_brief(&x->t, x->now, &b);
    TEST_ASSERT_EQUAL(STATION_SGT_MEASURING, b.state);
    TEST_ASSERT_EQUAL(0, b.phase);
    TEST_ASSERT_EQUAL_UINT32(1800, b.left_s);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, b.decide_left_s);
    fx_run(x, 15 * SEC);   /* one sample: 1790 left */
    sgt_brief(&x->t, x->now, &b);
    TEST_ASSERT_EQUAL_UINT32(1790, b.left_s);
}
