/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS recovery ladder core, run against a small simulated world: a receiver
 * that sends RTCM, text lines or garbage (or nothing) every second, and a
 * station that takes the ladder's requests, runs each step for a set time
 * (taking a lease for steps 3-5) and reports its state, its transitions and
 * its lease releases the way station.c and its events do. The clock starts
 * near the 32-bit millisecond wrap.
 */
#include <stdint.h>
#include <string.h>

#include "health_ladder.h"
#include "unity.h"

#define T0 ((int64_t)UINT32_MAX - 2000)
#define SEC 1000
#define MIN (60 * SEC)
#define HOUR (60 * MIN)
#define NEVER INT64_MIN

typedef struct {
    uint8_t code;
    uint8_t outcome;
    uint8_t detail;
    int64_t at;
} rec_t;

typedef struct {
    hl_t l;
    hl_in_t in;
    int64_t now;

    /* the receiver: what arrives each second */
    bool rtcm;
    bool lines;
    bool bytes;
    int64_t last_rtcm;
    int64_t last_rx;
    uint8_t silence_after;      /* this finished step silences the receiver */
    uint8_t answers_during;     /* the receiver answers the station's commands while this step runs */

    /* the station */
    uint8_t pending;
    uint8_t running;
    int64_t run_end;
    uint32_t dur[6];            /* step durations, ms */
    uint8_t fix_step;           /* this finished step brings RTCM back */
    bool rescan_finds;
    bool events;                /* transitions are counted (events delivered) */
    bool refuse4;               /* the station refuses RECONFIGURE */
    bool reject;                /* the station rejects every request */
    bool lease;
    int64_t release_at;
    uint8_t apply_after;        /* this finished step makes the station run an apply of its own */
    int64_t apply_at;           /* when it starts (NEVER = none scheduled) */
    int64_t apply_end;

    /* reboot cap model (optional) */
    bool refuse_reboot;         /* the reboot request is blocked after all */
    unsigned refused_reboots;
    bool cap_model;
    int64_t reboots_at[16];
    unsigned nreboots_cap;

    /* recorded */
    uint8_t req[256];
    int64_t req_at[256];
    unsigned nreq;
    unsigned nreq_step[6];      /* requests per step (not bounded by req[]) */
    rec_t rec[512];
    unsigned nrec;
    unsigned reboots;
    int64_t reboot_at;
    unsigned started_steps;     /* journalled starts of steps 1-5 (not bounded by rec[]) */
} sim_t;

static hl_req_t cb_request(void *ctx, uint8_t step)
{
    sim_t *w = (sim_t *)ctx;
    TEST_ASSERT_TRUE(step >= 1 && step <= 5);
    w->nreq_step[step]++;
    if (w->nreq < 256) {
        w->req[w->nreq] = step;
        w->req_at[w->nreq] = w->now;
        w->nreq++;
    }
    if (w->reject) {
        return HL_REQ_REJECTED;
    }
    if (step == 4 && w->refuse4) {
        return HL_REQ_REFUSED;
    }
    if (w->pending < step) {
        w->pending = step;   /* an equal or stronger one already queued wins */
    }
    return HL_REQ_ACCEPTED;
}

static bool cb_reboot(void *ctx)
{
    sim_t *w = (sim_t *)ctx;
    if (w->refuse_reboot) {
        w->refused_reboots++;
        return false;   /* blocked after all: a race with the cap or an inhibit */
    }
    w->reboots++;
    w->reboot_at = w->now;
    if (w->cap_model && w->nreboots_cap < 16) {
        w->reboots_at[w->nreboots_cap++] = w->now;
    }
    return true;
}

static void cb_journal(void *ctx, uint8_t code, uint8_t outcome, uint8_t detail)
{
    sim_t *w = (sim_t *)ctx;
    if (outcome == HL_OUT_STARTED && code >= 1 && code <= 5) {
        w->started_steps++;
    }
    if (code == HL_CODE_REBOOT_BLOCKED && outcome == HL_OUT_STARTED) {
        /* a blocked restart names what blocked it */
        TEST_ASSERT_TRUE_MESSAGE(detail >= HL_CAP_HOUR && detail <= HL_CAP_LOCK, "a blocked restart's why");
    } else if (outcome != HL_OUT_HELD) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0, detail, "only a held or blocked restart carries a detail");
    }
    if (w->nrec < 512) {
        w->rec[w->nrec].code = code;
        w->rec[w->nrec].outcome = outcome;
        w->rec[w->nrec].detail = detail;
        w->rec[w->nrec].at = w->now;
        w->nrec++;
    }
}

static const hl_ops_t k_ops = { cb_request, cb_reboot, cb_journal, NULL };

static void sim_init(sim_t *w)
{
    memset(w, 0, sizeof(*w));
    w->now = T0;
    hl_ops_t ops = k_ops;
    ops.ctx = w;
    hl_init(&w->l, &ops, T0);
    w->last_rtcm = NEVER;
    w->last_rx = NEVER;
    w->release_at = NEVER;
    w->apply_at = NEVER;
    w->apply_end = NEVER;
    w->in.stn_state = HL_STN_STREAMING;
    w->in.reboot_allowed = true;
    w->in.reboot_cap_why = HL_CAP_HOUR;   /* when a test makes the cap block: the hourly limit */
    w->events = true;
    w->rescan_finds = true;
    w->dur[1] = 0;
    w->dur[2] = 0;
    w->dur[3] = 45 * SEC;
    w->dur[4] = 20 * SEC;
    w->dur[5] = 3 * SEC;
    w->reboot_at = NEVER;
}

/* A reboot cap like lifecycle's: 60 min apart, fewer than 3 in 24 h. */
static bool cap_allows(const sim_t *w)
{
    unsigned in_day = 0;
    for (unsigned i = 0; i < w->nreboots_cap; i++) {
        int64_t age = w->now - w->reboots_at[i];
        if (age < HOUR) {
            return false;
        }
        if (age < 24 * HOUR) {
            in_day++;
        }
    }
    return in_day < 3;
}

static void sim_station(sim_t *w)
{
    /* The station's own apply (CONFIGURING under its lease). */
    if (w->apply_at != NEVER && w->running == 0 && w->now >= w->apply_at &&
        w->in.stn_state == HL_STN_STREAMING) {
        w->apply_at = NEVER;
        w->in.stn_state = HL_STN_CONFIGURING;
        w->lease = true;
    }
    if (w->in.stn_state == HL_STN_CONFIGURING && w->apply_end != NEVER && w->now >= w->apply_end) {
        w->apply_end = NEVER;
        w->in.stn_state = HL_STN_STREAMING;
        w->lease = false;
        w->release_at = w->now;
    }
    if (w->pending != 0 && w->running == 0 && w->in.stn_state == HL_STN_STREAMING) {
        w->running = w->pending;
        w->pending = 0;
        w->in.stn_state = HL_STN_RECOVERING;
        if (w->events) {
            w->in.rec_enters++;
        }
        w->run_end = w->now + (int64_t)w->dur[w->running];
        if (w->running >= 3) {
            w->lease = true;
        }
    }
    if (w->running != 0 && w->now >= w->run_end) {
        uint8_t st = w->running;
        w->running = 0;
        if (st >= 3) {
            w->lease = false;
            w->release_at = w->now;
        }
        if (w->events) {
            w->in.rec_exits++;
        }
        w->in.stn_state = (st == 3 && !w->rescan_finds) ? HL_STN_NO_RECEIVER : HL_STN_STREAMING;
        if (st == w->fix_step) {
            w->rtcm = true;
        }
        if (st == w->silence_after) {
            w->rtcm = false;
            w->lines = false;
            w->bytes = false;
        }
        if (st == w->apply_after) {
            w->apply_at = w->now + 60 * SEC;
            w->apply_end = w->apply_at + 20 * SEC;
        }
    }
}

static void sim_world(sim_t *w)
{
    if (w->rtcm) {
        w->last_rtcm = w->now;
    }
    if (w->rtcm || w->lines || w->bytes) {
        w->last_rx = w->now;
        w->in.rx_bytes += 120;
    }
    if (w->lines) {
        w->in.text_lines++;
    }
    if (w->running != 0 && w->running == w->answers_during) {
        w->in.answers++;   /* its replies to the step's commands */
    }
}

static uint32_t age(int64_t now, int64_t then)
{
    if (then == NEVER) {
        return UINT32_MAX;
    }
    int64_t a = now - then;
    return a >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)a;
}

static void sim_fill(sim_t *w)
{
    w->in.now_ms = w->now;
    w->in.stall_ms = 10 * SEC;
    w->in.rtcm_age_ms = age(w->now, w->last_rtcm);
    w->in.rx_age_ms = age(w->now, w->last_rx);
    w->in.lease_held = w->lease;
    w->in.since_release_ms = age(w->now, w->release_at);
    if (w->cap_model) {
        w->in.reboot_allowed = cap_allows(w);
        w->in.reboot_cap_why = HL_CAP_HOUR;
    }
}

static void sim_step(sim_t *w)
{
    sim_station(w);
    sim_world(w);
    sim_fill(w);
    hl_tick(&w->l, &w->in);
    w->now += SEC;
}

static void sim_run(sim_t *w, int64_t ms)
{
    for (int64_t t = 0; t < ms; t += SEC) {
        sim_step(w);
    }
}

/* Run until the journal has `n` records (or `limit` passes). */
static bool sim_until_rec(sim_t *w, unsigned n, int64_t limit)
{
    for (int64_t t = 0; t < limit; t += SEC) {
        if (w->nrec >= n) {
            return true;
        }
        sim_step(w);
    }
    return w->nrec >= n;
}

static int find_rec(const sim_t *w, uint8_t code, uint8_t outcome, unsigned from)
{
    for (unsigned i = from; i < w->nrec; i++) {
        if (w->rec[i].code == code && w->rec[i].outcome == outcome) {
            return (int)i;
        }
    }
    return -1;
}

static unsigned count_rec(const sim_t *w, uint8_t code, uint8_t outcome)
{
    unsigned n = 0;
    for (unsigned i = 0; i < w->nrec; i++) {
        if (w->rec[i].code == code && w->rec[i].outcome == outcome) {
            n++;
        }
    }
    return n;
}

static void expect_recs(const sim_t *w, const uint8_t (*want)[2], unsigned n)
{
    TEST_ASSERT_TRUE_MESSAGE(w->nrec >= n, "fewer journal records than expected");
    for (unsigned i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want[i][0], w->rec[i].code, "journal step code");
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(want[i][1], w->rec[i].outcome, "journal outcome");
    }
}

static hl_status_t st_of(const sim_t *w)
{
    hl_status_t s;
    hl_get_status(&w->l, &w->in, &s);
    return s;
}

/* Receiver streams for a while, then goes completely silent. */
static void stream_then_silence(sim_t *w)
{
    w->rtcm = true;
    sim_run(w, 20 * SEC);
    w->rtcm = false;
    w->lines = false;
    w->bytes = false;
}

/* ---- basics --------------------------------------------------------------- */

TEST_CASE("ladder: a healthy stream never starts a step", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 24 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_EQUAL_UINT32(0, w.nrec);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, s.level);
    TEST_ASSERT_EQUAL_UINT32(0, s.stalls);
    TEST_ASSERT_FALSE(s.suspended);
}

TEST_CASE("ladder: classes and entry steps", "[health_ladder]")
{
    TEST_ASSERT_EQUAL_UINT8(1, hl_entry_step(HL_CLASS_NO_BYTES));
    TEST_ASSERT_EQUAL_UINT8(3, hl_entry_step(HL_CLASS_GARBAGE));
    TEST_ASSERT_EQUAL_UINT8(4, hl_entry_step(HL_CLASS_TEXT_ONLY));

    const struct {
        bool lines, bytes;
        uint8_t first;
        hl_class_t cls;
    } cases[] = {
        { false, false, 1, HL_CLASS_NO_BYTES },
        { false, true, 3, HL_CLASS_GARBAGE },
        { true, false, 4, HL_CLASS_TEXT_ONLY },
        { true, true, 4, HL_CLASS_TEXT_ONLY },
    };
    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        sim_t w;
        sim_init(&w);
        w.rtcm = true;
        sim_run(&w, 30 * SEC);
        w.rtcm = false;
        w.lines = cases[i].lines;
        w.bytes = cases[i].bytes;
        sim_run(&w, 15 * SEC);
        TEST_ASSERT_EQUAL_INT(cases[i].cls, hl_classify(&w.l, &w.in));
        TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
        TEST_ASSERT_EQUAL_UINT8(cases[i].first, w.req[0]);
        TEST_ASSERT_EQUAL_UINT32(1, st_of(&w).stalls);
    }
}

TEST_CASE("ladder: the stall is declared only past the threshold", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 30 * SEC);
    int64_t last = w.last_rtcm;
    w.rtcm = false;
    while (w.nreq == 0 && w.now < last + MIN) {
        sim_step(&w);
    }
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
    TEST_ASSERT_TRUE(w.req_at[0] - last > 10 * SEC);
    TEST_ASSERT_TRUE(w.req_at[0] - last <= 12 * SEC);

    /* Right after start, before any RTCM: the stall timer counts from the start. */
    sim_init(&w);
    sim_run(&w, 10 * SEC);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    sim_run(&w, 3 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
}

/* ---- the full ladder -------------------------------------------------------- */

TEST_CASE("ladder: a silent receiver runs 1, 2, 3 and the byte rule reboots the ESP32", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 7, 10 * MIN));
    static const uint8_t want[][2] = {
        { 1, HL_OUT_STARTED }, { 1, HL_OUT_FAILED }, { 2, HL_OUT_STARTED }, { 2, HL_OUT_FAILED },
        { 3, HL_OUT_STARTED }, { 3, HL_OUT_FAILED }, { 6, HL_OUT_STARTED },
    };
    expect_recs(&w, want, 7);
    /* graces from the end of each step (steps 1 and 2 end within a tick) */
    int64_t g1 = w.rec[1].at - w.rec[0].at;
    int64_t g2 = w.rec[3].at - w.rec[2].at;
    int64_t g3 = w.rec[5].at - w.rec[4].at;
    TEST_ASSERT_TRUE(g1 >= 5 * SEC && g1 <= 7 * SEC);
    TEST_ASSERT_TRUE(g2 >= 15 * SEC && g2 <= 17 * SEC);
    TEST_ASSERT_TRUE(g3 >= 45 * SEC + 30 * SEC && g3 <= 45 * SEC + 33 * SEC);   /* 30 s from the end of the scan */
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_REBOOTING, s.phase);
    TEST_ASSERT_EQUAL_UINT8(6, s.last_step);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT32(4, s.recoveries);   /* 1, 2, 3 and the reboot */
    TEST_ASSERT_EQUAL_UINT32(1, s.stalls);
    TEST_ASSERT_FALSE(s.power_alert);
}

TEST_CASE("ladder: the reboot cap blocks 6a: FAILED without the alert, held in NO_RECEIVER", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    w.in.reboot_allowed = false;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 10 * MIN));
    TEST_ASSERT_EQUAL_UINT8(9, w.rec[6].code);
    TEST_ASSERT_EQUAL_UINT8(8, w.rec[7].code);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    TEST_ASSERT_FALSE(s.power_alert);
    TEST_ASSERT_EQUAL_UINT8(HL_CODE_FAILED, s.rung);
    TEST_ASSERT_EQUAL_UINT32(0, s.retry_in_s);   /* the station's own schedule retries */

    /* While the station stays NO_RECEIVER: no retries, no second byte rule. */
    unsigned nreq = w.nreq;
    unsigned nrec = w.nrec;
    sim_run(&w, 2 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(nreq, w.nreq);
    TEST_ASSERT_EQUAL_UINT32(nrec, w.nrec);
}

TEST_CASE("ladder: a held reboot inhibit blocks 6a like the cap", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    w.in.reboot_inhibited = true;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 10 * MIN));
    TEST_ASSERT_EQUAL_UINT8(9, w.rec[6].code);
    TEST_ASSERT_EQUAL_UINT8(HL_CAP_LOCK, w.rec[6].detail);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
}

TEST_CASE("ladder: a blocked 6a journals the limit that blocked it", "[health_ladder]")
{
    /* The cap's hourly limit, its daily limit, a restart lock; the cap wins
     * when both block (it is the one with a deadline). */
    static const struct {
        bool allowed;
        uint8_t why;
        bool inhibited;
        uint8_t want;
    } k[] = {
        { false, HL_CAP_HOUR, false, HL_CAP_HOUR },
        { false, HL_CAP_DAY, false, HL_CAP_DAY },
        { true, HL_CAP_UNKNOWN, true, HL_CAP_LOCK },
        { false, HL_CAP_DAY, true, HL_CAP_DAY },
    };
    for (size_t i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        sim_t w;
        sim_init(&w);
        w.rescan_finds = false;
        w.in.reboot_allowed = k[i].allowed;
        w.in.reboot_cap_why = k[i].why;
        w.in.reboot_inhibited = k[i].inhibited;
        stream_then_silence(&w);
        TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 10 * MIN));
        TEST_ASSERT_EQUAL_UINT8(HL_CODE_REBOOT_BLOCKED, w.rec[6].code);
        TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[6].outcome);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(k[i].want, w.rec[6].detail, "why");
        TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    }

    /* Blocked after all (the request found an inhibit taken since the
     * tick's sample): a restart lock. */
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    w.refuse_reboot = true;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 10 * MIN));
    TEST_ASSERT_EQUAL_UINT8(HL_CODE_REBOOT, w.rec[6].code);
    TEST_ASSERT_EQUAL_UINT8(HL_CODE_REBOOT_BLOCKED, w.rec[7].code);
    TEST_ASSERT_EQUAL_UINT8(HL_CAP_LOCK, w.rec[7].detail);
}

TEST_CASE("ladder: a rescan that finds nothing while bytes arrive raises the power-cycle alert", "[health_ladder]")
{
    /* Bytes during the scan window. */
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;   /* GARBAGE: entry at step 3 */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 4, 10 * MIN));
    static const uint8_t want[][2] = {
        { 3, HL_OUT_STARTED }, { 3, HL_OUT_FAILED }, { 7, HL_OUT_STARTED }, { 8, HL_OUT_STARTED },
    };
    expect_recs(&w, want, 4);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_TRUE(s.power_alert);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_CODE_POWER_ALERT, s.rung);

    /* No bytes on the link, but the station's pass saw some (a loader prompt). */
    sim_init(&w);
    w.rescan_finds = false;
    w.in.detect_saw_bytes = true;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 10 * MIN));
    TEST_ASSERT_EQUAL_UINT8(7, w.rec[6].code);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
}

TEST_CASE("ladder: a found receiver without RTCM goes on to reconfigure and reset", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;               /* GARBAGE: 3 */
    w.silence_after = 0;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 4, 10 * MIN));
    /* found -> 4 (even if the class still says GARBAGE) */
    TEST_ASSERT_EQUAL_UINT8(3, w.rec[1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_FAILED, w.rec[1].outcome);
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[2].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[2].outcome);
    int64_t scan_grace = w.rec[1].at - w.rec[0].at;
    TEST_ASSERT_TRUE(scan_grace >= 75 * SEC && scan_grace <= 78 * SEC);

    /* 4 fails after its 90 s grace -> 5; 5 fails after 120 s with bytes -> 6b */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 20 * MIN));
    static const uint8_t want[][2] = {
        { 3, 0 }, { 3, 2 }, { 4, 0 }, { 4, 2 }, { 5, 0 }, { 5, 2 }, { 7, 0 }, { 8, 0 },
    };
    expect_recs(&w, want, 8);
    int64_t g4 = w.rec[3].at - w.rec[2].at;
    int64_t g5 = w.rec[5].at - w.rec[4].at;
    TEST_ASSERT_TRUE(g4 >= 20 * SEC + 90 * SEC && g4 <= 20 * SEC + 93 * SEC);
    TEST_ASSERT_TRUE(g5 >= 3 * SEC + 120 * SEC && g5 <= 3 * SEC + 124 * SEC);
    TEST_ASSERT_TRUE(st_of(&w).power_alert);
    TEST_ASSERT_EQUAL_UINT8(5, st_of(&w).last_step);   /* 6b starts no action */
}

TEST_CASE("ladder: step 5 byte rule looks only at its grace", "[health_ladder]")
{
    /* The receiver answers text until the reset, then nothing: 6a. */
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;              /* TEXT_ONLY: 4 */
    w.silence_after = 5;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 5, 20 * MIN));
    static const uint8_t want[][2] = {
        { 4, 0 }, { 4, 2 }, { 5, 0 }, { 5, 2 }, { 6, 0 },
    };
    expect_recs(&w, want, 5);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
}

TEST_CASE("ladder: a refused reconfigure fails at once and the ladder goes on to step 5", "[health_ladder]")
{
    /* Refused by the status flag: the station is not even asked. */
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    w.in.reconfigure_refused = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, 2 * MIN));
    static const uint8_t want[][2] = { { 4, HL_OUT_FAILED }, { 5, HL_OUT_STARTED } };
    expect_recs(&w, want, 2);
    TEST_ASSERT_EQUAL_INT64(w.rec[0].at, w.rec[1].at);   /* same tick, no grace */
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
    TEST_ASSERT_EQUAL_UINT8(5, w.req[0]);
    TEST_ASSERT_EQUAL_UINT32(1, st_of(&w).recoveries);   /* a refused step is not an action */

    /* Refused by the station's answer (it changed between sample and call). */
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    w.refuse4 = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, 2 * MIN));
    expect_recs(&w, want, 2);
    TEST_ASSERT_EQUAL_UINT32(2, w.nreq);
    TEST_ASSERT_EQUAL_UINT8(4, w.req[0]);
    TEST_ASSERT_EQUAL_UINT8(5, w.req[1]);
}

/* ---- success, reset and flapping ------------------------------------------- */

TEST_CASE("ladder: success, 60 s of health resets, a shorter spell escalates", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.fix_step = 1;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, MIN));
    TEST_ASSERT_EQUAL_UINT8(1, w.rec[1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_HEALTHY, w.rec[1].outcome);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, st_of(&w).level);

    /* healthy for 30 s only, then silent again: the next step is 2, not 1 */
    sim_run(&w, 30 * SEC);
    w.rtcm = false;
    w.fix_step = 2;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 4, MIN));
    TEST_ASSERT_EQUAL_UINT8(2, w.rec[2].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[2].outcome);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_HEALTHY, w.rec[3].outcome);
    TEST_ASSERT_EQUAL_UINT32(2, st_of(&w).stalls);

    /* healthy for 70 s: reset; the next stall starts at step 1 again */
    sim_run(&w, 70 * SEC);
    w.rtcm = false;
    w.fix_step = 1;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 5, MIN));
    TEST_ASSERT_EQUAL_UINT8(1, w.rec[4].code);
}

TEST_CASE("ladder: a re-stall after a successful step 5 goes to the byte rule", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    w.in.reconfigure_refused = true;   /* 4 refused -> 5 */
    w.fix_step = 5;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 3, 3 * MIN));
    TEST_ASSERT_EQUAL_UINT8(5, w.rec[2].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_HEALTHY, w.rec[2].outcome);
    sim_run(&w, 20 * SEC);
    w.rtcm = false;      /* text lines keep coming: bytes -> 6b */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 5, MIN));
    TEST_ASSERT_EQUAL_UINT8(7, w.rec[3].code);
    TEST_ASSERT_EQUAL_UINT8(8, w.rec[4].code);
}

TEST_CASE("ladder: FAILED retries every 10 min at max(2, class entry)", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;      /* GARBAGE: 3 (found), 4, 5, 6b (bytes, no answer) */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 20 * MIN));
    static const uint8_t want[][2] = {
        { 3, 0 }, { 3, 2 }, { 4, 0 }, { 4, 2 }, { 5, 0 }, { 5, 2 }, { 7, 0 }, { 8, 0 },
    };
    expect_recs(&w, want, 8);
    int64_t failed_at = w.rec[7].at;
    hl_status_t s = st_of(&w);
    TEST_ASSERT_TRUE(s.retry_in_s >= 599 && s.retry_in_s <= 600);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_ANSWER, s.why);
    unsigned nreq = w.nreq;
    sim_run(&w, 9 * MIN);
    TEST_ASSERT_EQUAL_UINT32(nreq, w.nreq);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 9, 2 * MIN));
    TEST_ASSERT_EQUAL_UINT8(3, w.rec[8].code);       /* GARBAGE re-enters at 3 */
    int64_t gap = w.rec[8].at - failed_at;
    TEST_ASSERT_TRUE(gap >= 10 * MIN && gap <= 10 * MIN + 2 * SEC);
    TEST_ASSERT_TRUE(st_of(&w).power_alert);        /* kept through the retry */
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, st_of(&w).level);

    /* back in FAILED, now silent: the next pass enters at step 2 */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 16, 20 * MIN));
    TEST_ASSERT_EQUAL_UINT8(8, w.rec[15].code);
    w.bytes = false;
    unsigned n = w.nrec;
    TEST_ASSERT_TRUE(sim_until_rec(&w, n + 1, 11 * MIN));
    TEST_ASSERT_EQUAL_UINT8(2, w.rec[n].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[n].outcome);
}

TEST_CASE("ladder: FAILED does not re-enter a TEXT_ONLY stall while reconfigure is refused", "[health_ladder]")
{
    /* The receiver goes quiet with the reset (the reboot cap blocks 6a),
     * then sends text again: a TEXT_ONLY stall in FAILED. */
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    w.silence_after = 5;
    w.in.reboot_allowed = false;
    w.in.reconfigure_refused = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 5, 10 * MIN));   /* 4 refused, 5, 5 failed, 6a blocked, 8 */
    TEST_ASSERT_EQUAL_UINT8(9, w.rec[w.nrec - 2].code);
    TEST_ASSERT_EQUAL_UINT8(8, w.rec[w.nrec - 1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_SILENT, st_of(&w).why);
    w.lines = true;
    w.bytes = true;
    unsigned nreq = w.nreq;
    unsigned nrec = w.nrec;
    sim_run(&w, 3 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(nreq, w.nreq);   /* no reset every 10 min */
    TEST_ASSERT_EQUAL_UINT32(nrec, w.nrec);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, st_of(&w).level);

    /* refusal lifted: the next retry runs */
    w.in.reconfigure_refused = false;
    TEST_ASSERT_TRUE(sim_until_rec(&w, nrec + 1, 11 * MIN));
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[nrec].code);
}

TEST_CASE("ladder: 60 s of health in FAILED resets and clears the power-cycle alert", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;   /* bytes but no answer: 3, 4, 5, 6b */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 20 * MIN));
    TEST_ASSERT_TRUE(st_of(&w).power_alert);
    w.rtcm = true;    /* power-cycled by the user */
    sim_run(&w, 2 * SEC);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);   /* RTCM back, ladder not reset yet */
    TEST_ASSERT_TRUE(s.power_alert);
    sim_run(&w, 60 * SEC);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, s.level);
    TEST_ASSERT_FALSE(s.power_alert);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_IDLE, s.phase);
    TEST_ASSERT_TRUE(find_rec(&w, 8, HL_OUT_HEALTHY, 0) >= 0);

    /* a short healthy spell in FAILED keeps FAILED and its retry timer */
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 20 * MIN));
    w.rtcm = true;
    sim_run(&w, 30 * SEC);
    w.rtcm = false;
    sim_run(&w, 30 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, st_of(&w).phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, st_of(&w).level);
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, 8, HL_OUT_HEALTHY, 0));
}

/* ---- suspension ------------------------------------------------------------- */

TEST_CASE("ladder: station states that suspend it", "[health_ladder]")
{
    const uint8_t states[] = { HL_STN_IDLE, HL_STN_DETECTING, HL_STN_CONFIGURING, HL_STN_FLASHING,
                               HL_STN_FLASH_RECOVERY };
    for (unsigned i = 0; i < sizeof(states); i++) {
        sim_t w;
        sim_init(&w);
        w.in.stn_state = states[i];
        sim_run(&w, 30 * MIN);
        TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
        TEST_ASSERT_EQUAL_UINT32(0, w.nrec);
        hl_status_t s = st_of(&w);
        TEST_ASSERT_TRUE(s.suspended);
        TEST_ASSERT_TRUE((s.susp & HL_SUSP_STATION) != 0);
        TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);   /* no RTCM, but never FAILED */
    }
}

TEST_CASE("ladder: an open flash journal suspends it in every state, and the stall timer restarts", "[health_ladder]")
{
    /* Silent receiver with a journal waiting for the user: nothing at all. */
    sim_t w;
    sim_init(&w);
    w.in.journal_open = true;
    stream_then_silence(&w);
    sim_run(&w, 2 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_TRUE(s.suspended);
    TEST_ASSERT_TRUE((s.susp & HL_SUSP_JOURNAL) != 0);

    /* Journal state unknown (settings unavailable): the same. */
    sim_init(&w);
    w.in.journal_unknown = true;
    stream_then_silence(&w);
    sim_run(&w, HOUR);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_TRUE(st_of(&w).suspended);

    /* A journal opening during a step's grace abandons the step; when it
     * closes the stall timer counts from there and the step runs again. */
    sim_init(&w);
    w.dur[1] = 0;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    sim_run(&w, 2 * SEC);
    w.in.journal_open = true;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT8(1, w.rec[1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_ABANDONED, w.rec[1].outcome);
    sim_run(&w, 20 * MIN);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
    w.in.journal_open = false;
    int64_t closed = w.now;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 3, MIN));
    TEST_ASSERT_EQUAL_UINT8(1, w.rec[2].code);   /* the abandoned step again */
    TEST_ASSERT_TRUE(w.rec[2].at - closed > 10 * SEC);
    TEST_ASSERT_TRUE(w.rec[2].at - closed <= 12 * SEC);
}

TEST_CASE("ladder: safe mode suspends it and is never FAILED", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.in.safe_mode = true;
    w.in.stn_state = HL_STN_IDLE;   /* the station does not run in safe mode */
    sim_run(&w, 4 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_TRUE((s.susp & HL_SUSP_SAFE_MODE) != 0);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);
}

TEST_CASE("ladder: another owner's lease suspends it; the stall counts from the release", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lease = true;              /* e.g. a configuration session (RTCM held back) */
    sim_run(&w, 2 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_TRUE((st_of(&w).susp & HL_SUSP_LEASE) != 0);
    w.lease = false;
    w.release_at = w.now;
    sim_run(&w, 9 * SEC);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_TRUE((st_of(&w).susp & HL_SUSP_POST_LEASE) != 0);
    while (w.nreq == 0 && w.now < w.release_at + MIN) {
        sim_step(&w);
    }
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
    TEST_ASSERT_TRUE(w.req_at[0] - w.release_at > 10 * SEC);
    TEST_ASSERT_TRUE(w.req_at[0] - w.release_at <= 12 * SEC);   /* from the release, not 10 s after it */

    /* RTCM resuming within the 10 s: no stall at all */
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lease = true;
    sim_run(&w, MIN);
    w.lease = false;
    w.release_at = w.now;
    sim_run(&w, 6 * SEC);
    w.rtcm = true;
    sim_run(&w, 10 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
}

TEST_CASE("ladder: the ladder's own lease release does not cut its grace short", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;               /* step 4: a lease, released at its end */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, 5 * MIN));
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_FAILED, w.rec[1].outcome);   /* not abandoned */
    TEST_ASSERT_EQUAL_UINT32(0, count_rec(&w, 4, HL_OUT_ABANDONED));
}

TEST_CASE("ladder: a station job that is not the ladder's suspends it", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    stream_then_silence(&w);
    w.in.stn_state = HL_STN_RECOVERING;   /* a leftover request still running */
    sim_run(&w, 2 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_TRUE((st_of(&w).susp & HL_SUSP_LEASE) != 0);
}

TEST_CASE("ladder: a configuration session during a grace abandons the step, which runs again", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));   /* step 4 started */
    sim_run(&w, 30 * SEC);                         /* run 20 s, then in grace */
    w.in.stn_state = HL_STN_CONFIGURING;
    w.lease = true;
    sim_run(&w, 30 * SEC);
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_ABANDONED, w.rec[1].outcome);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, st_of(&w).level);
    w.in.stn_state = HL_STN_STREAMING;
    w.lease = false;
    w.release_at = w.now;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 3, MIN));
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[2].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[2].outcome);
    TEST_ASSERT_TRUE(w.rec[2].at - w.release_at > 10 * SEC);
}

TEST_CASE("ladder: a step abandoned twice in an episode counts as failed", "[health_ladder]")
{
    /* The receiver sends garbage, no RTCM and no answer. About 60 s after
     * each receiver reset the station runs an apply of its own (its check
     * after the restart), which suspends the ladder inside step 5's grace.
     * Step 5 runs again once; abandoned a second time it counts as failed
     * and the byte rule raises the power-cycle alert. */
    sim_t w;
    sim_init(&w);
    w.apply_after = 5;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;
    for (int64_t t = 0; t < 30 * MIN && !w.l.power_alert; t += SEC) {
        sim_step(&w);
    }
    TEST_ASSERT_EQUAL_UINT32(2, w.nreq_step[5]);
    TEST_ASSERT_EQUAL_UINT32(2, count_rec(&w, 5, HL_OUT_ABANDONED));
    int alert = find_rec(&w, HL_CODE_POWER_ALERT, HL_OUT_STARTED, 0);
    TEST_ASSERT_TRUE(alert >= 0);
    TEST_ASSERT_TRUE(find_rec(&w, HL_CODE_FAILED, HL_OUT_STARTED, (unsigned)alert) > alert);
    TEST_ASSERT_TRUE(w.l.power_alert);

    /* Over a day the FAILED passes keep it bounded: one reset per pass,
     * never a reset every couple of minutes. */
    sim_run(&w, 24 * HOUR);
    unsigned passes = w.l.phase == HL_PH_FAILED ? 0u : 1u;
    passes += (unsigned)(24 * HOUR / HL_FAILED_RETRY_MS) + 2u;
    TEST_ASSERT_TRUE(w.nreq_step[5] <= passes);
    TEST_ASSERT_TRUE(w.nreq_step[5] < 24 * HOUR / (5 * MIN));
    TEST_ASSERT_TRUE(w.l.power_alert);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);

    /* 60 s of health resets the ladder: a new episode may run step 5 twice
     * again. */
    w.apply_after = 0;
    w.apply_at = NEVER;
    w.rtcm = true;
    sim_run(&w, 2 * MIN);
    TEST_ASSERT_EQUAL_UINT8(0, w.l.abandoned);
    TEST_ASSERT_FALSE(w.l.power_alert);
}

TEST_CASE("ladder: a reset abandoned twice on a receiver that answers: 7 with the reason, no restart", "[health_ladder]")
{
    /* Text lines (TEXT_ONLY), no RTCM: as after a failed step 5, no
     * power-cycle advice, and no reset on every retry pass. */
    sim_t w;
    sim_init(&w);
    w.apply_after = 5;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    for (int64_t t = 0; t < 30 * MIN && w.l.phase != HL_PH_FAILED; t += SEC) {
        sim_step(&w);
    }
    TEST_ASSERT_EQUAL_UINT32(2, w.nreq_step[5]);
    TEST_ASSERT_EQUAL_UINT32(2, count_rec(&w, 5, HL_OUT_ABANDONED));
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_FALSE(s.power_alert);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, HL_CODE_POWER_ALERT, HL_OUT_STARTED, 0));
    sim_run(&w, 3 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(2, w.nreq_step[5]);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);

    /* Nothing unasked (NO_BYTES), but the reset's banner and replies (the
     * board with no antenna), full recovery and nothing holding a restart
     * back: no ESP32 restart for a receiver that answers. */
    sim_init(&w);
    w.apply_after = 5;
    w.in.answers = 40;
    w.answers_during = 5;
    for (int64_t t = 0; t < 30 * MIN && w.l.phase != HL_PH_FAILED; t += SEC) {
        sim_step(&w);
        if (w.nreq_step[5] == 1 && w.in.stn_state == HL_STN_STREAMING && w.running == 0 && w.apply_end == NEVER &&
            w.l.phase == HL_PH_IDLE && w.l.abandoned != 0) {
            /* Between the abandoned reset and its second run: still recovering, with the reason. */
            s = st_of(&w);
            TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
            TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);
            TEST_ASSERT_EQUAL_UINT8(0, s.doing);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(2, w.nreq_step[5]);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, HL_CODE_REBOOT, HL_OUT_STARTED, 0));
}

TEST_CASE("ladder: a rescan abandoned twice goes on to reconfigure", "[health_ladder]")
{
    /* Garbage on the line: entry step 3. Each rescan's grace is cut short
     * by a station apply; the second time the ladder goes on to step 4. */
    sim_t w;
    sim_init(&w);
    w.apply_after = 3;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;
    for (int64_t t = 0; t < 30 * MIN && w.nreq_step[4] == 0; t += SEC) {
        /* the apply lands 10 s into the rescan's 30 s grace */
        if (w.apply_at != NEVER && w.apply_at - w.now > 10 * SEC) {
            w.apply_at = w.now + 10 * SEC;
            w.apply_end = w.apply_at + 20 * SEC;
        }
        sim_step(&w);
    }
    TEST_ASSERT_EQUAL_UINT32(2, w.nreq_step[3]);
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq_step[4]);
    TEST_ASSERT_EQUAL_UINT32(2, count_rec(&w, 3, HL_OUT_ABANDONED));
}

TEST_CASE("ladder: the receiver lost during reconfigure: abandoned, then the NO_RECEIVER rule", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    sim_run(&w, 5 * SEC);
    /* the station gives up the apply and looks for the receiver */
    w.running = 0;
    w.lease = false;
    w.release_at = w.now;
    w.in.rec_exits++;
    w.in.stn_state = HL_STN_DETECTING;
    w.lines = false;
    sim_run(&w, 45 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_ABANDONED, w.rec[1].outcome);
    w.in.stn_state = HL_STN_NO_RECEIVER;
    w.in.detect_saw_bytes = false;
    sim_run(&w, 2 * SEC);
    static const uint8_t want[][2] = { { 4, 0 }, { 4, 3 }, { 6, 0 } };
    expect_recs(&w, want, 3);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
}

/* ---- NO_RECEIVER ----------------------------------------------------------------- */

TEST_CASE("ladder: NO_RECEIVER at boot: the byte rule once per episode, then held", "[health_ladder]")
{
    /* No bytes in the station's pass: 6a. */
    sim_t w;
    sim_init(&w);
    w.in.stn_state = HL_STN_IDLE;
    sim_run(&w, 3 * SEC);
    w.in.stn_state = HL_STN_DETECTING;
    sim_run(&w, 45 * SEC);
    TEST_ASSERT_EQUAL_UINT32(0, w.nrec);
    w.in.stn_state = HL_STN_NO_RECEIVER;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_EQUAL_UINT32(1, st_of(&w).stalls);

    /* Bytes in the pass (a receiver at its loader): 6b, held without retries. */
    sim_init(&w);
    w.in.stn_state = HL_STN_DETECTING;
    sim_run(&w, 45 * SEC);
    w.in.stn_state = HL_STN_NO_RECEIVER;
    w.in.detect_saw_bytes = true;
    w.lease = true;               /* listen-only / detection passes hold leases */
    sim_run(&w, 5 * SEC);
    static const uint8_t want[][2] = { { 7, 0 }, { 8, 0 } };
    expect_recs(&w, want, 2);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_TRUE(s.power_alert);
    TEST_ASSERT_FALSE(s.suspended);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    for (int i = 0; i < 12; i++) {            /* passes: lease taken and released */
        w.lease = false;
        w.release_at = w.now;
        sim_run(&w, 4 * MIN);
        w.lease = true;
        sim_run(&w, MIN);
    }
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);

    /* the station finds it streaming text but no RTCM: retries resume 10 min later */
    w.lease = false;
    w.release_at = w.now;
    w.in.stn_state = HL_STN_STREAMING;
    w.lines = true;
    int64_t left = w.now;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 3, 12 * MIN));
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[2].code);
    TEST_ASSERT_TRUE(w.rec[2].at - left >= 10 * MIN);
    TEST_ASSERT_TRUE(w.rec[2].at - left <= 10 * MIN + 12 * SEC);

    /* a new episode applies the rule again */
    sim_init(&w);
    w.in.stn_state = HL_STN_NO_RECEIVER;
    w.in.detect_saw_bytes = true;
    sim_run(&w, 2 * SEC);
    w.in.stn_state = HL_STN_STREAMING;
    w.rtcm = true;
    sim_run(&w, 30 * SEC);
    w.rtcm = false;
    w.in.stn_state = HL_STN_DETECTING;
    sim_run(&w, 45 * SEC);
    w.in.stn_state = HL_STN_NO_RECEIVER;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(2, count_rec(&w, 7, 0));
}

TEST_CASE("ladder: a failed rescan's NO_RECEIVER episode is not handled twice", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    w.in.reboot_allowed = false;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 10 * MIN));
    sim_run(&w, 3 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(1, count_rec(&w, 9, 0));
    TEST_ASSERT_EQUAL_UINT32(1, count_rec(&w, 8, 0));
    TEST_ASSERT_EQUAL_UINT32(3, w.nreq);
}

TEST_CASE("ladder: the receiver comes back during the rescan's grace", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 5, 5 * MIN));   /* 3 started */
    while (w.in.stn_state != HL_STN_NO_RECEIVER) {
        sim_step(&w);
    }
    sim_run(&w, 10 * SEC);
    w.in.stn_state = HL_STN_STREAMING;   /* found between passes, data flowing */
    w.rtcm = true;
    sim_run(&w, 3 * SEC);
    TEST_ASSERT_EQUAL_UINT8(3, w.rec[5].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_HEALTHY, w.rec[5].outcome);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
}

/* ---- following the station's steps ----------------------------------------------- */

TEST_CASE("ladder: step ends are found without events, by sampling or by the pickup timeout", "[health_ladder]")
{
    /* Long steps: seen RECOVERING in the samples. */
    sim_t w;
    sim_init(&w);
    w.events = false;
    w.rescan_finds = false;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, 5 * MIN));
    int64_t g = w.rec[1].at - w.rec[0].at;
    TEST_ASSERT_TRUE(g >= 75 * SEC && g <= 78 * SEC);

    /* Instant steps never sampled: the pickup timeout ends them. */
    sim_init(&w);
    w.events = false;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, 5 * MIN));
    g = w.rec[1].at - w.rec[0].at;
    TEST_ASSERT_TRUE(g >= HL_PICKUP_TIMEOUT_MS + 5 * SEC && g <= HL_PICKUP_TIMEOUT_MS + 7 * SEC);
}

TEST_CASE("ladder: a step the station is slow to pick up is waited for", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    stream_then_silence(&w);
    w.in.stn_state = HL_STN_STREAMING;
    /* hold the station busy (still STREAMING) for 20 s after the request */
    w.dur[1] = 0;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    uint8_t keep = w.pending;
    w.pending = 0;
    sim_run(&w, 20 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_RUN, st_of(&w).phase);
    w.pending = keep;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_GRACE, st_of(&w).phase);
}

TEST_CASE("ladder: rejected requests are asked again without journal records", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.reject = true;
    stream_then_silence(&w);
    sim_run(&w, 30 * SEC);
    TEST_ASSERT_TRUE(w.nreq >= 15);
    TEST_ASSERT_EQUAL_UINT32(0, w.nrec);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_WANT, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT32(1, s.stalls);
    TEST_ASSERT_EQUAL_UINT32(0, s.recoveries);
    w.reject = false;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT8(1, w.rec[0].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[0].outcome);
}

TEST_CASE("ladder: a requested reboot that does not happen ends in FAILED", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 7, 10 * MIN));
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    sim_run(&w, HL_REBOOT_WAIT_MS + 2 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, st_of(&w).phase);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);   /* never asked twice */
}

/* ---- survey window ------------------------------------------------------------------- */

TEST_CASE("ladder: step 4's grace lasts while the survey window is open", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    w.in.surveying = true;           /* the apply started a survey */
    sim_run(&w, 10 * MIN);
    TEST_ASSERT_EQUAL_UINT32(1, w.nrec);
    w.in.surveying = false;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_FAILED, w.rec[1].outcome);

    /* other steps keep their grace while surveying */
    sim_init(&w);
    w.in.surveying = true;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, MIN));
    TEST_ASSERT_EQUAL_UINT8(1, w.rec[1].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_FAILED, w.rec[1].outcome);
}

TEST_CASE("ladder: with RTCM held back while surveying, no stall inside the window", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.in.survey_holds_rtcm = true;
    w.in.surveying = true;
    stream_then_silence(&w);
    w.lines = true;
    sim_run(&w, 12 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_FALSE(st_of(&w).suspended);
    w.in.surveying = false;
    int64_t end = w.now;
    while (w.nreq == 0 && w.now < end + MIN) {
        sim_step(&w);
    }
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
    TEST_ASSERT_TRUE(w.req_at[0] - end >= 10 * SEC && w.req_at[0] - end <= 12 * SEC);

    /* without the option the survey window does not hold the stall */
    sim_init(&w);
    w.in.surveying = true;
    stream_then_silence(&w);
    sim_run(&w, 15 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
}

/* ---- status --------------------------------------------------------------------- */

TEST_CASE("ladder: status levels follow the phase", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, st_of(&w).level);
    w.rtcm = false;
    sim_run(&w, 10 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, st_of(&w).level);   /* 10 s old is still healthy */
    sim_run(&w, 2 * SEC);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(1, s.rung);
    TEST_ASSERT_EQUAL_UINT8(1, s.last_step);
}

/* ---- long runs -------------------------------------------------------------------- */

TEST_CASE("ladder: three days of a dead receiver: one capped reboot, never a loop", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.cap_model = true;
    w.rescan_finds = false;
    stream_then_silence(&w);
    int64_t end = w.now + 72 * HOUR;
    unsigned seen = 0;
    while (w.now < end) {
        sim_step(&w);
        if (w.reboots != seen) {
            /* the ESP32 restarts: a new ladder, the station boots and finds nothing */
            seen = w.reboots;
            w.now += 5 * SEC;
            hl_ops_t ops = k_ops;
            ops.ctx = &w;
            hl_init(&w.l, &ops, w.now);
            w.pending = 0;
            w.running = 0;
            w.lease = false;
            w.release_at = NEVER;
            w.in.stn_state = HL_STN_IDLE;
            sim_run(&w, 3 * SEC);
            w.in.stn_state = HL_STN_DETECTING;
            sim_run(&w, 45 * SEC);
            w.in.stn_state = HL_STN_NO_RECEIVER;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, st_of(&w).phase);

    /* The receiver is found again every few hours but never sends RTCM:
     * the ladder runs its passes; reboots stay within the cap. */
    sim_init(&w);
    w.cap_model = true;
    w.rescan_finds = false;
    stream_then_silence(&w);
    end = w.now + 72 * HOUR;
    seen = 0;
    int64_t next_found = w.now + 3 * HOUR;
    while (w.now < end) {
        sim_step(&w);
        if (w.reboots != seen) {
            seen = w.reboots;
            w.now += 5 * SEC;
            hl_ops_t ops = k_ops;
            ops.ctx = &w;
            hl_init(&w.l, &ops, w.now);
            w.pending = 0;
            w.running = 0;
            w.lease = false;
            w.release_at = NEVER;
            w.in.stn_state = HL_STN_DETECTING;
            sim_run(&w, 45 * SEC);
            w.in.stn_state = HL_STN_NO_RECEIVER;
        }
        if (w.now >= next_found && w.in.stn_state == HL_STN_NO_RECEIVER) {
            w.in.stn_state = HL_STN_STREAMING;   /* found, silent again */
            next_found = w.now + 3 * HOUR;
        }
    }
    TEST_ASSERT_TRUE(w.nreboots_cap >= 2);
    for (unsigned i = 1; i < w.nreboots_cap; i++) {
        TEST_ASSERT_TRUE(w.reboots_at[i] - w.reboots_at[i - 1] >= HOUR);
    }
    for (unsigned i = 0; i < w.nreboots_cap; i++) {
        unsigned in_day = 0;
        for (unsigned k = 0; k < w.nreboots_cap; k++) {
            int64_t d = w.reboots_at[k] - w.reboots_at[i];
            if (d >= 0 && d < 24 * HOUR) {
                in_day++;
            }
        }
        TEST_ASSERT_TRUE(in_day <= 3);
    }
}

TEST_CASE("ladder: a flapping receiver over a day escalates but stays bounded", "[health_ladder]")
{
    /* RTCM for 40 s, silence for 30 s, forever: each stall escalates one
     * step since the link is never healthy for 60 s. */
    sim_t w;
    sim_init(&w);
    w.cap_model = true;
    w.rescan_finds = true;
    w.rtcm = true;
    int64_t end = w.now + 24 * HOUR;
    while (w.now < end) {
        int64_t t = (w.now - T0) % (70 * SEC);
        w.rtcm = t < 40 * SEC;
        sim_step(&w);
    }
    TEST_ASSERT_TRUE(w.nreq > 0);
    TEST_ASSERT_TRUE(w.reboots <= 3);
    TEST_ASSERT_EQUAL_UINT32(w.started_steps, st_of(&w).recoveries - w.reboots);
}

/* ---- a blocked 6a is looked at again ------------------------------------- */

/* Boot detection fails silently while the reboot cap is closed: 6a blocked. */
static void blocked_in_no_receiver(sim_t *w)
{
    sim_init(w);
    w->in.reboot_allowed = false;
    w->in.stn_state = HL_STN_DETECTING;
    sim_run(w, 45 * SEC);
    w->in.stn_state = HL_STN_NO_RECEIVER;
    w->in.detect_passes = 1;
    sim_run(w, 2 * SEC);
    static const uint8_t want[][2] = { { 9, HL_OUT_STARTED }, { 8, HL_OUT_STARTED } };
    expect_recs(w, want, 2);
    TEST_ASSERT_EQUAL_UINT32(2, w->nrec);
    TEST_ASSERT_TRUE(st_of(w).reboot_recheck);
}

TEST_CASE("ladder: a blocked 6a in NO_RECEIVER runs once the cap allows it and a newer pass is silent", "[health_ladder]")
{
    sim_t w;
    blocked_in_no_receiver(&w);

    /* the cap opens, but no pass completed since the block: nothing */
    w.in.reboot_allowed = true;
    sim_run(&w, 10 * MIN);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);

    /* a newer pass without a byte: 6a, journalled as a new start */
    w.in.detect_passes = 2;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    TEST_ASSERT_EQUAL_UINT8(6, w.rec[2].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[2].outcome);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_REBOOTING, s.phase);
    TEST_ASSERT_FALSE(s.reboot_recheck);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
}

TEST_CASE("ladder: a blocked 6a looked at again with bytes in the newer pass raises the power-cycle alert", "[health_ladder]")
{
    sim_t w;
    blocked_in_no_receiver(&w);
    w.in.detect_passes = 2;
    w.in.detect_saw_bytes = true;
    sim_run(&w, 5 * MIN);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);   /* the cap is still closed */
    w.in.reboot_allowed = true;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    TEST_ASSERT_EQUAL_UINT8(7, w.rec[2].code);
    TEST_ASSERT_EQUAL_UINT8(8, w.rec[3].code);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_TRUE(s.power_alert);
    TEST_ASSERT_FALSE(s.reboot_recheck);
    /* once decided, nothing more in this episode */
    w.in.detect_passes = 9;
    sim_run(&w, 2 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(4, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
}

TEST_CASE("ladder: a blocked 6a waits for a released inhibit and never queues behind it", "[health_ladder]")
{
    sim_t w;
    blocked_in_no_receiver(&w);
    w.in.reboot_allowed = true;
    w.in.reboot_inhibited = true;
    w.in.detect_passes = 2;
    sim_run(&w, 30 * MIN);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    w.in.reboot_inhibited = false;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
}

TEST_CASE("ladder: a blocked 6a is not looked at again outside full mode or after NO_RECEIVER", "[health_ladder]")
{
    /* "no ESP32 reboot": nothing; back to full: the next tick decides */
    sim_t w;
    blocked_in_no_receiver(&w);
    w.in.reboot_allowed = true;
    w.in.detect_passes = 2;
    w.in.recovery_mode = HL_MODE_NO_REBOOT;
    sim_run(&w, 30 * MIN);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    w.in.recovery_mode = HL_MODE_FULL;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);

    /* "alerts only": nothing */
    blocked_in_no_receiver(&w);
    w.in.reboot_allowed = true;
    w.in.detect_passes = 2;
    w.in.recovery_mode = HL_MODE_ALERTS;
    sim_run(&w, 30 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);

    /* the station left NO_RECEIVER: the block is forgotten */
    blocked_in_no_receiver(&w);
    w.in.stn_state = HL_STN_STREAMING;   /* found, still silent */
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_FALSE(st_of(&w).reboot_recheck);
    w.in.reboot_allowed = true;
    w.in.detect_passes = 2;
    sim_run(&w, 5 * SEC);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
}

TEST_CASE("ladder: a reboot blocked again by a race waits for the next pass", "[health_ladder]")
{
    sim_t w;
    blocked_in_no_receiver(&w);
    w.in.reboot_allowed = true;
    w.in.detect_passes = 2;
    w.refuse_reboot = true;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.refused_reboots);
    static const uint8_t want[][2] = { { 9, 0 }, { 8, 0 }, { 6, 0 }, { 9, 0 }, { 8, 0 } };
    expect_recs(&w, want, 5);
    TEST_ASSERT_TRUE(st_of(&w).reboot_recheck);
    w.refuse_reboot = false;
    sim_run(&w, 10 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);   /* same pass: not again */
    w.in.detect_passes = 3;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
}

/* ---- user recovery mode ----------------------------------------------------- */

TEST_CASE("ladder: mode no ESP32 reboot journals 6a as not run and goes to FAILED", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    w.in.recovery_mode = HL_MODE_NO_REBOOT;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 8, 10 * MIN));
    static const uint8_t want[][2] = {
        { 1, HL_OUT_STARTED }, { 1, HL_OUT_FAILED }, { 2, HL_OUT_STARTED }, { 2, HL_OUT_FAILED },
        { 3, HL_OUT_STARTED }, { 3, HL_OUT_FAILED }, { 9, HL_OUT_NOT_RUN }, { 8, HL_OUT_STARTED },
    };
    expect_recs(&w, want, 8);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    TEST_ASSERT_EQUAL_UINT32(3, w.nreq);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_MODE_NO_REBOOT, s.mode);

    /* NO_RECEIVER at boot: the same */
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_NO_REBOOT;
    w.in.stn_state = HL_STN_NO_RECEIVER;
    sim_run(&w, 3 * SEC);
    static const uint8_t want2[][2] = { { 9, HL_OUT_NOT_RUN }, { 8, HL_OUT_STARTED } };
    expect_recs(&w, want2, 2);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);

    /* 6b is not affected */
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_NO_REBOOT;
    w.in.stn_state = HL_STN_NO_RECEIVER;
    w.in.detect_saw_bytes = true;
    sim_run(&w, 3 * SEC);
    TEST_ASSERT_EQUAL_UINT8(7, w.rec[0].code);
    TEST_ASSERT_TRUE(st_of(&w).power_alert);
}

/* Alerts only: the stream stops (then garbage, lines or nothing) until FAILED;
 * nothing may be requested or rebooted on the way. */
static void alerts_only_run(sim_t *w, bool bytes, bool lines)
{
    sim_init(w);
    w->in.recovery_mode = HL_MODE_ALERTS;
    w->rescan_finds = false;
    w->rtcm = true;
    sim_run(w, 20 * SEC);
    w->rtcm = false;
    w->bytes = bytes;
    w->lines = lines;
    for (int64_t t = 0; t < 10 * MIN && find_rec(w, 8, HL_OUT_STARTED, 0) < 0; t += SEC) {
        sim_step(w);
        if (w->nrec == 1) {
            hl_status_t s = st_of(w);
            TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);   /* stalled before 7: degraded only */
        }
    }
    TEST_ASSERT_EQUAL_UINT32(0, w->nreq);
    TEST_ASSERT_EQUAL_UINT32(0, w->reboots);
    hl_status_t s = st_of(w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    TEST_ASSERT_FALSE(s.power_alert);
    TEST_ASSERT_EQUAL_UINT32(0, s.recoveries);
    TEST_ASSERT_EQUAL_UINT8(0, s.last_step);
}

TEST_CASE("ladder: alerts only journals each would-be step once and sends nothing", "[health_ladder]")
{
    sim_t w;
    /* NO_BYTES: 1, 2, 3, then where the byte rule would run, 7 */
    alerts_only_run(&w, false, false);
    static const uint8_t nb[][2] = { { 1, HL_OUT_NOT_RUN }, { 2, HL_OUT_NOT_RUN }, { 3, HL_OUT_NOT_RUN }, { 8, 0 } };
    expect_recs(&w, nb, 4);
    TEST_ASSERT_EQUAL_UINT32(4, w.nrec);
    /* each grace still elapses */
    TEST_ASSERT_TRUE(w.rec[1].at - w.rec[0].at >= 5 * SEC);
    TEST_ASSERT_TRUE(w.rec[2].at - w.rec[1].at >= 15 * SEC);
    TEST_ASSERT_TRUE(w.rec[3].at - w.rec[2].at >= 30 * SEC);
    /* the FAILED retry pass journals the step it would enter, every 10 min, and sends nothing */
    sim_run(&w, 21 * MIN);
    TEST_ASSERT_EQUAL_UINT32(3, count_rec(&w, 2, HL_OUT_NOT_RUN));
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);

    /* GARBAGE: 3, then 7 */
    alerts_only_run(&w, true, false);
    static const uint8_t gb[][2] = { { 3, HL_OUT_NOT_RUN }, { 8, 0 } };
    expect_recs(&w, gb, 2);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);

    /* TEXT_ONLY: 4, 5, then 7, no power-cycle alert */
    alerts_only_run(&w, false, true);
    static const uint8_t to[][2] = { { 4, HL_OUT_NOT_RUN }, { 5, HL_OUT_NOT_RUN }, { 8, 0 } };
    expect_recs(&w, to, 3);
    TEST_ASSERT_EQUAL_UINT32(3, w.nrec);

    /* NO_RECEIVER: no byte rule, straight to 7 */
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_ALERTS;
    w.in.stn_state = HL_STN_NO_RECEIVER;
    w.in.detect_saw_bytes = true;
    sim_run(&w, 3 * SEC);
    static const uint8_t nr[][2] = { { 8, 0 } };
    expect_recs(&w, nr, 1);
    TEST_ASSERT_FALSE(st_of(&w).power_alert);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
}

TEST_CASE("ladder: a mode change mid-step lets the step finish; leaving alerts only starts afresh", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = true;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.bytes = true;   /* GARBAGE: the ladder starts at 3 */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    TEST_ASSERT_EQUAL_UINT8(3, w.rec[0].code);
    sim_run(&w, 10 * SEC);   /* the rescan runs (45 s) */
    w.in.recovery_mode = HL_MODE_ALERTS;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 5, 10 * MIN));
    static const uint8_t want[][2] = {
        { 3, HL_OUT_STARTED }, { 3, HL_OUT_FAILED }, { 4, HL_OUT_NOT_RUN }, { 5, HL_OUT_NOT_RUN }, { 8, 0 },
    };
    expect_recs(&w, want, 5);
    TEST_ASSERT_EQUAL_UINT32(1, w.nreq);
    TEST_ASSERT_EQUAL_UINT8(3, w.req[0]);

    /* back to full while still stalled: a fresh pass at the class entry step (3), not max(2, ...) */
    sim_run(&w, MIN);
    unsigned nreq = w.nreq;
    w.in.recovery_mode = HL_MODE_FULL;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(nreq + 1, w.nreq);
    TEST_ASSERT_EQUAL_UINT8(3, w.req[w.nreq - 1]);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, st_of(&w).level);
}

TEST_CASE("ladder: a stored recovery mode out of range acts as full", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    w.in.recovery_mode = 7;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 7, 10 * MIN));
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    TEST_ASSERT_EQUAL_UINT8(HL_MODE_FULL, st_of(&w).mode);
}

/* ---- restart hold ---------------------------------------------------------------- */

/* NO_RECEIVER at boot after a silent detection pass, with `hold` set. */
static void held_in_no_receiver(sim_t *w, uint8_t hold)
{
    sim_init(w);
    w->in.reboot_hold = hold;
    w->in.stn_state = HL_STN_DETECTING;
    sim_run(w, 45 * SEC);
    w->in.stn_state = HL_STN_NO_RECEIVER;
    w->in.detect_passes = 1;
    sim_run(w, 2 * SEC);
}

TEST_CASE("ladder: first setup holds 6a: code 9 held with the mask, no restart", "[health_ladder]")
{
    sim_t w;
    held_in_no_receiver(&w, HL_HOLD_SETUP);
    static const uint8_t want[][2] = { { 9, HL_OUT_HELD }, { 8, HL_OUT_STARTED } };
    expect_recs(&w, want, 2);
    TEST_ASSERT_EQUAL_UINT8(HL_HOLD_SETUP, w.rec[0].detail);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_TRUE(s.reboot_recheck);
    TEST_ASSERT_TRUE(s.episode);

    /* Journalled once: newer passes while the hold lasts add nothing. */
    for (uint32_t pass = 2; pass < 20; pass++) {
        w.in.detect_passes = pass;
        sim_run(&w, 5 * MIN);
    }
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
}

TEST_CASE("ladder: the hold cleared and a new detection pass: 6a runs", "[health_ladder]")
{
    sim_t w;
    held_in_no_receiver(&w, HL_HOLD_SETUP | HL_HOLD_POWER_ON);
    TEST_ASSERT_EQUAL_UINT8(HL_HOLD_SETUP | HL_HOLD_POWER_ON, w.rec[0].detail);
    /* the hold ends, but no pass completed since: nothing yet */
    w.in.reboot_hold = 0;
    sim_run(&w, 10 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    /* a newer silent pass: 6a, journalled as a new start */
    w.in.detect_passes = 2;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    TEST_ASSERT_EQUAL_UINT8(6, w.rec[2].code);
    TEST_ASSERT_EQUAL_UINT8(HL_OUT_STARTED, w.rec[2].outcome);
}

TEST_CASE("ladder: without a hold, a receiver that streamed and went silent gets 6a at once", "[health_ladder]")
{
    /* health gives no power_on bit once something was heard this boot
     * (test_health_mon.c): the ladder then restarts within the cap. */
    sim_t w;
    sim_init(&w);
    w.rescan_finds = false;
    stream_then_silence(&w);
    w.in.reboot_hold = 0;
    sim_run(&w, 5 * MIN);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    TEST_ASSERT_EQUAL_UINT32(0, count_rec(&w, 9, HL_OUT_HELD));
}

TEST_CASE("ladder: the cap and a hold together: the cap is journalled, the hold is not", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.in.reboot_allowed = false;
    w.in.reboot_hold = HL_HOLD_ADMIN;
    w.in.stn_state = HL_STN_DETECTING;
    sim_run(&w, 45 * SEC);
    w.in.stn_state = HL_STN_NO_RECEIVER;
    w.in.detect_passes = 1;
    sim_run(&w, 2 * SEC);
    static const uint8_t want[][2] = { { 9, HL_OUT_STARTED }, { 8, HL_OUT_STARTED } };
    expect_recs(&w, want, 2);
    TEST_ASSERT_EQUAL_UINT8(HL_CAP_HOUR, w.rec[0].detail);   /* the cap's limit, not the hold bits */
    /* the cap opens while the hold lasts: still nothing */
    w.in.reboot_allowed = true;
    w.in.detect_passes = 2;
    sim_run(&w, 20 * MIN);
    TEST_ASSERT_EQUAL_UINT32(2, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    TEST_ASSERT_TRUE(st_of(&w).reboot_recheck);
    /* the hold ends: the next pass decides */
    w.in.reboot_hold = 0;
    w.in.detect_passes = 3;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
}

TEST_CASE("ladder: a held 6a in a stall outside NO_RECEIVER comes back with the FAILED pass", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.dur[5] = 3 * SEC;
    w.in.reboot_hold = HL_HOLD_AP_CLIENT;
    stream_then_silence(&w);
    /* steps 1, 2, then the rescan finds the (silent) receiver: 4, 5, byte rule */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 12, 20 * MIN));
    int held = find_rec(&w, 9, HL_OUT_HELD, 0);
    TEST_ASSERT_TRUE(held >= 0);
    TEST_ASSERT_EQUAL_UINT8(HL_HOLD_AP_CLIENT, w.rec[held].detail);
    TEST_ASSERT_EQUAL_UINT8(8, w.rec[held + 1].code);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    /* the client leaves: the FAILED retry pass (10 min) reaches 6a again and runs it */
    w.in.reboot_hold = 0;
    sim_run(&w, 20 * MIN);
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
}

TEST_CASE("ladder: no-reboot and alerts-only modes never reach the hold", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_NO_REBOOT;
    w.in.reboot_hold = HL_HOLD_SETUP;
    w.in.stn_state = HL_STN_NO_RECEIVER;
    sim_run(&w, 3 * SEC);
    static const uint8_t want[][2] = { { 9, HL_OUT_NOT_RUN }, { 8, HL_OUT_STARTED } };
    expect_recs(&w, want, 2);

    sim_init(&w);
    w.in.recovery_mode = HL_MODE_ALERTS;
    w.in.reboot_hold = HL_HOLD_SETUP;
    w.in.stn_state = HL_STN_NO_RECEIVER;
    sim_run(&w, 3 * SEC);
    TEST_ASSERT_EQUAL_UINT32(0, count_rec(&w, 9, HL_OUT_HELD));
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
}

TEST_CASE("ladder: the episode flag follows a stall until the ladder resets", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 30 * SEC);
    TEST_ASSERT_FALSE(st_of(&w).episode);
    w.rtcm = false;
    sim_run(&w, 15 * SEC);
    TEST_ASSERT_TRUE(st_of(&w).episode);
    w.rtcm = true;
    sim_run(&w, 2 * MIN);
    TEST_ASSERT_FALSE(st_of(&w).episode);
}

/* ---- the receiver answers but sends no RTCM; the held first configuration ---- */

static unsigned count_req(const sim_t *w, uint8_t step)
{
    return w->nreq_step[step];
}

TEST_CASE("ladder: a receiver that sends text after the reset gets no power-cycle advice, and no reset every pass", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;      /* it answers (text lines), but no RTCM: 4, 5, then 7 with the reason */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 5, 20 * MIN));
    static const uint8_t want[][2] = { { 4, 0 }, { 4, 2 }, { 5, 0 }, { 5, 2 }, { 8, 0 } };
    expect_recs(&w, want, 5);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_FALSE(s.power_alert);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, 7, HL_OUT_STARTED, 0));

    /* It keeps answering without RTCM: no configuration and no reset again. */
    unsigned nreq = w.nreq;
    sim_run(&w, 3 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(nreq, w.nreq);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, st_of(&w).why);

    /* Then it goes silent: the retry pass asks it again (steps 2 and 3),
     * finds it, and goes back to 7 without steps 4 and 5. */
    w.lines = false;
    unsigned n4 = count_req(&w, 4), n5 = count_req(&w, 5);
    sim_run(&w, 12 * MIN);
    TEST_ASSERT_EQUAL_UINT(1, count_req(&w, 2));
    TEST_ASSERT_EQUAL_UINT(1, count_req(&w, 3));
    TEST_ASSERT_EQUAL_UINT(n4, count_req(&w, 4));
    TEST_ASSERT_EQUAL_UINT(n5, count_req(&w, 5));
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, st_of(&w).phase);

    /* RTCM back for a minute: reset, and the reason is gone. */
    w.rtcm = true;
    sim_run(&w, 70 * SEC);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NONE, s.why);
}

TEST_CASE("ladder: replies after the receiver reset count as answers", "[health_ladder]")
{
    /* A configured receiver without satellites: nothing on the line, but the
     * rescan finds it and it answers the reset's queries. */
    sim_t w;
    sim_init(&w);
    stream_then_silence(&w);
    w.answers_during = 5;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 11, 20 * MIN));
    static const uint8_t want[][2] = {
        { 1, 0 }, { 1, 2 }, { 2, 0 }, { 2, 2 }, { 3, 0 }, { 3, 2 }, { 4, 0 }, { 4, 2 }, { 5, 0 }, { 5, 2 }, { 8, 0 },
    };
    expect_recs(&w, want, 11);
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_FALSE(s.power_alert);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);

    /* The retry pass: 2 and 3 again (the rescan finds it), never 4 or 5. */
    sim_run(&w, 25 * MIN);
    TEST_ASSERT_EQUAL_UINT(1, count_req(&w, 4));
    TEST_ASSERT_EQUAL_UINT(1, count_req(&w, 5));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(2, count_req(&w, 3));

    /* The rescan finds nothing any more: the byte rule as usual (no bytes: 6a). */
    w.rescan_finds = false;
    sim_run(&w, 12 * MIN);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(1, w.reboots);
}

TEST_CASE("ladder: a receiver that answers and uses no satellites gets no reset", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    w.in.no_sky = true;   /* its position sentence: no fix, no satellites */
    w.in.reconfigure_refused = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 2, 2 * MIN));
    static const uint8_t want[][2] = { { 4, HL_OUT_FAILED }, { 8, HL_OUT_STARTED } };
    expect_recs(&w, want, 2);
    TEST_ASSERT_EQUAL_UINT(0, count_req(&w, 5));
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_SKY, s.why);
    sim_run(&w, 2 * HOUR);
    TEST_ASSERT_EQUAL_UINT(0, count_req(&w, 5));

    /* The same in alerts-only mode: nothing sent, the reason shown. */
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_ALERTS;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    w.in.no_sky = true;
    sim_run(&w, 5 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, 5, HL_OUT_NOT_RUN, 0));
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_SKY, s.why);
}

TEST_CASE("ladder: alerts-only mode and NO_RECEIVER carry their reason", "[health_ladder]")
{
    /* Silent, nothing answered for a long time: SILENT, already while the
     * steps are only journalled (DEGRADED), and in 7 (FAILED). */
    sim_t w;
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_ALERTS;
    sim_run(&w, 15 * MIN);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_FAILED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_SILENT, s.why);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);

    sim_init(&w);
    w.in.recovery_mode = HL_MODE_ALERTS;
    sim_run(&w, 12 * SEC);   /* the stall is declared; step 1 is only journalled */
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_SILENT, s.why);

    /* It answered the station a minute before the RTCM stopped: alive. */
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_ALERTS;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.in.answers++;
    sim_run(&w, 60 * SEC);
    w.rtcm = false;
    sim_run(&w, 15 * MIN);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, st_of(&w).why);

    /* NO_RECEIVER: the station's own alert says it; no reason here. */
    sim_init(&w);
    w.in.stn_state = HL_STN_NO_RECEIVER;
    w.in.reboot_allowed = false;
    sim_run(&w, 2 * MIN);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NONE, s.why);
}

TEST_CASE("ladder: a held first configuration with a receiver that answers is no stall", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.in.rx_held = true;
    w.in.recovery_mode = HL_MODE_ALERTS;   /* what a hold build sets */
    w.lines = true;                        /* the receiver as delivered: text, no RTCM */
    sim_run(&w, 2 * HOUR);
    TEST_ASSERT_EQUAL_UINT32(0, w.nrec);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_HELD, s.why);
    TEST_ASSERT_EQUAL_UINT32(0, s.stalls);
    TEST_ASSERT_FALSE(s.episode);

    /* Silent while held: a stall like any other (the receiver may be off). */
    w.lines = false;
    sim_run(&w, 15 * MIN);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_SILENT, s.why);
    TEST_ASSERT_EQUAL_UINT32(1, s.stalls);

    /* It answers again: back to waiting for the user, quietly. */
    unsigned nrec = w.nrec;
    w.lines = true;
    sim_run(&w, 5 * SEC);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_IDLE, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_HELD, s.why);
    TEST_ASSERT_EQUAL_UINT32(nrec, w.nrec);   /* no "healthy again" */

    /* The user applies: the hold ends; RTCM flows. */
    w.in.rx_held = false;
    w.rtcm = true;
    sim_run(&w, 10 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NONE, st_of(&w).why);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, st_of(&w).level);
}

/* ---- what the recovery does now; the receiver that answers without RTCM ---------------------- */

TEST_CASE("ladder: the step in progress and where it is", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    stream_then_silence(&w);   /* NO_BYTES: 1, 2, 3 (found), 4, 5, then 6a */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(1, s.doing);
    TEST_ASSERT_FALSE(s.waiting);   /* requested: the station runs it */
    sim_step(&w);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(1, s.doing);
    TEST_ASSERT_TRUE(s.waiting);    /* it ran: its grace */
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NONE, s.why);   /* nothing answered since the last frame */

    /* The rescan runs 45 s: running all along, then waiting. */
    while (find_rec(&w, 3, HL_OUT_STARTED, 0) < 0 && w.now < T0 + 5 * MIN) {
        sim_step(&w);
    }
    sim_run(&w, 30 * SEC);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(3, s.doing);
    TEST_ASSERT_FALSE(s.waiting);
    sim_run(&w, 20 * SEC);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(3, s.doing);
    TEST_ASSERT_TRUE(s.waiting);

    /* 6a: the restart is what it does; then FAILED: nothing in progress. */
    while (w.reboots == 0 && w.now < T0 + 20 * MIN) {
        sim_step(&w);
    }
    TEST_ASSERT_EQUAL_UINT32(1, w.reboots);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_CODE_REBOOT, s.doing);
    sim_run(&w, 2 * MIN);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(0, s.doing);
    TEST_ASSERT_FALSE(s.waiting);

    /* Alerts only: a step only journalled is nothing in progress. */
    sim_init(&w);
    w.in.recovery_mode = HL_MODE_ALERTS;
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);
    TEST_ASSERT_EQUAL_UINT8(0, s.doing);
}

TEST_CASE("ladder: a receiver that answers but never streamed: the reason at once, step 4 not held by the survey", "[health_ladder]")
{
    /* No antenna: the station's boot session got answers (counted before
     * the ladder's first sample) and opened the survey window; nothing
     * comes unasked and no RTCM frame ever. */
    sim_t w;
    sim_init(&w);
    w.in.answers = 40;
    w.in.surveying = true;
    w.in.recovery_mode = HL_MODE_NO_REBOOT;
    w.answers_during = 4;   /* the reconfigure's session gets replies */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    TEST_ASSERT_EQUAL_UINT8(1, w.rec[0].code);   /* NO_BYTES: the ladder starts at 1 */
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(1, s.doing);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);   /* the antenna hint with the first step, not after the ladder */

    /* Its own position sentence says no satellites: that reason instead. */
    w.in.no_sky = true;
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_SKY, st_of(&w).why);
    w.in.no_sky = false;

    /* Step 4: the plain 90 s grace although the survey window stays open. */
    while (find_rec(&w, 4, HL_OUT_STARTED, 0) < 0 && w.now < T0 + 10 * MIN) {
        sim_step(&w);
    }
    int i4 = find_rec(&w, 4, HL_OUT_STARTED, 0);
    TEST_ASSERT_TRUE(i4 >= 0);
    int64_t t4 = w.rec[i4].at;
    sim_run(&w, 40 * SEC);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(4, s.doing);
    TEST_ASSERT_TRUE(s.waiting);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);
    while (find_rec(&w, 4, HL_OUT_FAILED, 0) < 0 && w.now < t4 + 10 * MIN) {
        sim_step(&w);
    }
    int f4 = find_rec(&w, 4, HL_OUT_FAILED, 0);
    TEST_ASSERT_TRUE_MESSAGE(f4 >= 0, "step 4 judged inside the survey window");
    TEST_ASSERT_TRUE(w.in.surveying);
    TEST_ASSERT_TRUE(w.rec[f4].at - t4 <= (int64_t)w.dur[4] + HL_GRACE_RECONFIGURE_MS + 2 * SEC);
    TEST_ASSERT_EQUAL_UINT(1, count_req(&w, 5));   /* then the receiver reset, as for any receiver that answers */

    /* The same receiver with RTCM held back while surveying (the option): no stall inside the window at all. */
    sim_init(&w);
    w.in.answers = 40;
    w.in.surveying = true;
    w.in.survey_holds_rtcm = true;
    sim_run(&w, 15 * MIN);
    TEST_ASSERT_EQUAL_UINT32(0, w.nreq);
}

TEST_CASE("ladder: after a stall the reason waits for an answer since the last frame; step 4 waits for the survey", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.in.answers = 40;          /* it answered at boot */
    w.in.surveying = true;
    w.rtcm = true;
    sim_run(&w, 60 * SEC);
    w.rtcm = false;             /* then nothing: it may be gone */
    w.answers_during = 3;       /* the rescan asks it, and it answers */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NONE, s.why);   /* its answers came before the last frame */
    while (find_rec(&w, 3, HL_OUT_STARTED, 0) < 0 && w.now < T0 + 5 * MIN) {
        sim_step(&w);
    }
    sim_run(&w, 5 * SEC);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, st_of(&w).why);   /* it answered the rescan */

    /* It streamed this boot: step 4 keeps waiting while the survey window is open. */
    while (find_rec(&w, 4, HL_OUT_STARTED, 0) < 0 && w.now < T0 + 10 * MIN) {
        sim_step(&w);
    }
    TEST_ASSERT_TRUE(find_rec(&w, 4, HL_OUT_STARTED, 0) >= 0);
    sim_run(&w, 8 * MIN);
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, 4, HL_OUT_FAILED, 0));
    TEST_ASSERT_EQUAL_UINT8(4, st_of(&w).doing);
    w.in.surveying = false;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_TRUE(find_rec(&w, 4, HL_OUT_FAILED, 0) >= 0);
}

TEST_CASE("ladder: alerts only chosen while a step's grace runs: only watched, still judged", "[health_ladder]")
{
    sim_t w;
    sim_init(&w);
    w.in.surveying = true;      /* the grace of step 4 lasts (it streamed this boot) */
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;             /* TEXT_ONLY: step 4 */
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    TEST_ASSERT_EQUAL_UINT8(4, w.rec[0].code);

    /* The user chooses alerts only while the station runs it: it completes, and is said so. */
    sim_run(&w, 5 * SEC);
    w.in.recovery_mode = HL_MODE_ALERTS;
    sim_step(&w);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_RUN, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(4, s.doing);
    TEST_ASSERT_FALSE(s.waiting);

    /* Once it ran, its grace is only watched: no step in progress, the stall reported with its reason. */
    sim_run(&w, 30 * SEC);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_GRACE, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);
    TEST_ASSERT_EQUAL_UINT8(0, s.doing);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, s.why);

    /* Back to full inside that grace: the station's step is in progress again. */
    w.in.recovery_mode = HL_MODE_FULL;
    sim_step(&w);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_RECOVERING, s.level);
    TEST_ASSERT_EQUAL_UINT8(4, s.doing);
    TEST_ASSERT_TRUE(s.waiting);

    /* Alerts only again; the window ends: the step that ran is judged, the next one only journalled. */
    w.in.recovery_mode = HL_MODE_ALERTS;
    sim_run(&w, 2 * MIN);
    unsigned nreq = w.nreq;
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, 4, HL_OUT_FAILED, 0));
    w.in.surveying = false;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_TRUE(find_rec(&w, 4, HL_OUT_FAILED, 0) >= 0);
    TEST_ASSERT_TRUE(find_rec(&w, 5, HL_OUT_NOT_RUN, 0) >= 0);
    TEST_ASSERT_EQUAL_UINT32(nreq, w.nreq);
    s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_DEGRADED, s.level);
    TEST_ASSERT_EQUAL_UINT8(0, s.doing);

    /* RTCM back during a watched grace: the step that ran gets its result. */
    sim_init(&w);
    w.in.surveying = true;
    w.rtcm = true;
    sim_run(&w, 20 * SEC);
    w.rtcm = false;
    w.lines = true;
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    sim_run(&w, 30 * SEC);
    w.in.recovery_mode = HL_MODE_ALERTS;
    sim_run(&w, 10 * SEC);
    w.rtcm = true;
    sim_run(&w, 2 * SEC);
    TEST_ASSERT_TRUE(find_rec(&w, 4, HL_OUT_HEALTHY, 0) >= 0);
    TEST_ASSERT_EQUAL_UINT8(HL_LEVEL_OK, st_of(&w).level);
}

TEST_CASE("ladder: no restart is pending while the receiver answers, and again once a step gets no answer", "[health_ladder]")
{
    /* A silent receiver: pending from the stall on. */
    sim_t w;
    sim_init(&w);
    stream_then_silence(&w);
    TEST_ASSERT_TRUE(sim_until_rec(&w, 1, MIN));
    TEST_ASSERT_TRUE(st_of(&w).restart_possible);

    /* No receiver found: pending (the station's own alert says why). */
    sim_t n;
    held_in_no_receiver(&n, HL_HOLD_ADMIN);
    TEST_ASSERT_TRUE(st_of(&n).restart_possible);

    /* No antenna: it answers, no RTCM this boot, and the receiver reset is
     * answered too: never pending, through steps 1-5, the station's own
     * session after the reset (the ladder suspended), 7 and the retry
     * passes (which ask it again with the rescan). */
    sim_init(&w);
    w.in.answers = 40;
    w.answers_during = 5;
    w.apply_after = 5;
    w.in.reboot_hold = HL_HOLD_ADMIN;
    unsigned in_episode = 0;
    while (w.now < T0 + 35 * MIN) {
        sim_step(&w);
        hl_status_t s = st_of(&w);
        if (s.episode) {
            in_episode++;
            TEST_ASSERT_FALSE_MESSAGE(s.restart_possible, "no restart pending for a receiver that answers");
        }
    }
    TEST_ASSERT_TRUE(in_episode > 30 * 60);
    TEST_ASSERT_TRUE(count_rec(&w, HL_CODE_RECEIVER_RESET, HL_OUT_ABANDONED) >= 1);   /* the session cut it short */
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, st_of(&w).phase);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_NO_RTCM, st_of(&w).why);
    TEST_ASSERT_TRUE(count_req(&w, 3) >= 2);   /* the retry passes ran */
    TEST_ASSERT_EQUAL_UINT32(0, w.reboots);
    TEST_ASSERT_EQUAL_INT(-1, find_rec(&w, HL_CODE_REBOOT_BLOCKED, HL_OUT_HELD, 0));

    /* The same receiver, but nothing answers the reset and no byte comes:
     * the byte rule's restart (held by the admin page here). Not pending
     * before that, pending from then on. */
    sim_init(&w);
    w.in.answers = 40;
    w.answers_during = 4;
    w.in.reboot_hold = HL_HOLD_ADMIN;
    bool early = false;
    while (find_rec(&w, HL_CODE_REBOOT_BLOCKED, HL_OUT_HELD, 0) < 0 && w.now < T0 + 20 * MIN) {
        sim_step(&w);
        hl_status_t s = st_of(&w);
        early = early || (s.episode && s.restart_possible && w.nrec > 0 &&
                          find_rec(&w, HL_CODE_RECEIVER_RESET, HL_OUT_FAILED, 0) < 0);
    }
    TEST_ASSERT_FALSE_MESSAGE(early, "a restart said pending before the reset went unanswered");
    TEST_ASSERT_TRUE(find_rec(&w, HL_CODE_REBOOT_BLOCKED, HL_OUT_HELD, 0) >= 0);
    hl_status_t s = st_of(&w);
    TEST_ASSERT_EQUAL_UINT8(HL_PH_FAILED, s.phase);
    TEST_ASSERT_EQUAL_UINT8(HL_WHY_SILENT, s.why);
    TEST_ASSERT_TRUE(s.restart_possible);
}
