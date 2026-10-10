/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "ledger_core.h"
#include "unity.h"

#define HIST   30u
#define DAY0     20736                       /* 2026-10-10 */
#define H(x)   ((int64_t)(x) * 3600000LL)
#define M(x)   ((int64_t)(x) * 60000LL)
#define S(x)   ((int64_t)(x) * 1000LL)
#define T0     (UINT32_MAX - 5000u)        /* monotonic clock about to wrap */

static lc_day_t s_hist[HIST];
static lc_t c;

static int64_t at(int32_t day, int64_t ms_of_day)
{
    return (int64_t)day * LC_DAY_MS + ms_of_day;
}

static void fresh(uint32_t now, uint64_t uptime_ms)
{
    memset(s_hist, 0, sizeof(s_hist));
    lc_init(&c, s_hist, HIST, now, uptime_ms);
}

static lc_snapshot_t snap_base(void)
{
    lc_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.today.utc_day = -1;
    s.pending.utc_day = -1;
    return s;
}

TEST_CASE("ledger counts what an expected slot did not stream, planned apart", "[ledger]")
{
    fresh(T0, 0);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(12)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_INT32(DAY0, c.today.utc_day);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, false);
    lc_set_slot(&c, T0, 1, true, false, false);
    lc_tick(&c, T0 + 10000u);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.down_ms[1]);

    lc_set_slot(&c, T0 + 10000u, 0, true, true, true);         /* connected */
    TEST_ASSERT_EQUAL_UINT32(1, c.today.reconnects[0]);
    lc_tick(&c, T0 + 70000u);                                   /* across the wrap */
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.down_ms[0]);

    lc_set_rtcm_ok(&c, T0 + 70000u, false);                     /* open session, no data */
    lc_tick(&c, T0 + 75000u);
    TEST_ASSERT_EQUAL_UINT32(15000, c.today.down_ms[0]);

    lc_set_planned(&c, T0 + 75000u, true);                      /* paused for receiver work */
    lc_set_slot(&c, T0 + 75000u, 0, true, true, false);
    lc_tick(&c, T0 + 85000u);
    TEST_ASSERT_EQUAL_UINT32(25000, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.planned_ms[0]);
    TEST_ASSERT_EQUAL_UINT64(25000, lc_protected_down_ms(&c));  /* slot 1 is not protected */

    lc_set_slot(&c, T0 + 85000u, 1, false, false, false);       /* disabled: not counted any more */
    lc_tick(&c, T0 + 95000u);
    TEST_ASSERT_EQUAL_UINT32(85000, c.today.down_ms[1]);
    lc_count_overflow(&c, T0 + 95000u);
    TEST_ASSERT_EQUAL_UINT32(1, c.today.rx_overflows);
}

TEST_CASE("ledger: UTC midnight splits the time and moves today into the history", "[ledger]")
{
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(24) - S(10)), LC_CLOCK_GNSS);
    lc_set_slot(&c, T0, 0, true, true, false);
    lc_tick(&c, T0 + 25000u);
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
    TEST_ASSERT_EQUAL_UINT32(15000, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_size_t(1, c.hist_n);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32(10000, c.hist[0].down_ms[0]);
    TEST_ASSERT_TRUE(c.hist_dirty);

    /* 40 more days, ticking once a minute: 30 kept, newest first, all full days */
    uint32_t now = T0 + 25000u;
    for (uint32_t i = 0; i < 40u * 1440u; i++) {
        now += 60000u;
        lc_tick(&c, now);
    }
    TEST_ASSERT_EQUAL_size_t(HIST, c.hist_n);
    for (size_t i = 0; i < c.hist_n; i++) {
        TEST_ASSERT_EQUAL_INT32(c.today.utc_day - 1 - (int32_t)i, c.hist[i].utc_day);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)LC_DAY_MS, c.hist[i].down_ms[0]);
    }
    lc_day_t t;
    lc_today(&c, &t);
    TEST_ASSERT_EQUAL_INT32(c.today.utc_day, t.utc_day);
}

TEST_CASE("ledger: seconds before the first clock are spread over the days they covered", "[ledger]")
{
    fresh(T0, 0);
    lc_set_slot(&c, T0, 0, true, true, false);
    lc_count_overflow(&c, T0 + 1000u);
    lc_tick(&c, T0 + 3600000u);                                 /* one hour, no clock */
    TEST_ASSERT_EQUAL_UINT64(3600000u, c.pending_ms);
    TEST_ASSERT_EQUAL_UINT64(3600000u, lc_protected_down_ms(&c));

    TEST_ASSERT_TRUE(lc_set_utc(&c, T0 + 3600000u, at(DAY0, M(30)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_INT32(DAY0, c.today.utc_day);
    TEST_ASSERT_EQUAL_UINT32(1800000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(1, c.today.rx_overflows);
    TEST_ASSERT_EQUAL_size_t(1, c.hist_n);
    TEST_ASSERT_EQUAL_INT32(DAY0 - 1, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32(1800000u, c.hist[0].down_ms[0]);
    TEST_ASSERT_EQUAL_UINT64(0, c.pending_ms);
}

TEST_CASE("ledger: a power cut's gap is downtime and power-off time, a reset's only downtime", "[ledger]")
{
    for (int cut = 0; cut <= 1; cut++) {
        lc_snapshot_t s = snap_base();
        s.today.utc_day = DAY0;
        s.today.down_ms[0] = 5000;
        s.alive_ms = at(DAY0, H(10));
        s.expected_mask = 0x01;
        s.protected_mask = 0x01;
        fresh(T0, 2000);                                        /* booted 2 s ago */
        lc_restore(&c, &s, cut != 0);
        TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(12) + S(2)), LC_CLOCK_SNTP));
        /* 2 h gap + 2 s since boot (slot 0 not reported yet: expected, down) */
        TEST_ASSERT_EQUAL_UINT32(5000u + (uint32_t)H(2) + 2000u, c.today.down_ms[0]);
        TEST_ASSERT_EQUAL_UINT32(cut ? (uint32_t)H(2) : 0u, c.today.power_off_ms);
        TEST_ASSERT_EQUAL_UINT32(0, c.today.down_ms[1]);
        TEST_ASSERT_EQUAL_INT64(at(DAY0, H(12) + S(2)), c.alive_ms);
    }
}

TEST_CASE("ledger: a gap that spans midnight and planned work at the reset", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(23));
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    s.planned = true;
    fresh(T0, 0);
    lc_restore(&c, &s, false);
    lc_set_utc(&c, T0, at(DAY0 + 1, H(1)), LC_CLOCK_GNSS);
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H(1), c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H(1), c.today.planned_ms[0]);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H(1), c.hist[0].down_ms[0]);
    TEST_ASSERT_FALSE(c.planned);                                /* planned work does not carry over */
}

TEST_CASE("ledger: time an earlier boot counted without a clock is taken out of the gap", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(10));
    s.pending.down_ms[0] = (uint32_t)M(10);
    s.pending_ms = (uint64_t)M(10);
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    fresh(T0, 0);
    lc_restore(&c, &s, false);
    lc_set_utc(&c, T0, at(DAY0, H(11)), LC_CLOCK_SNTP);
    /* 50 min of gap + 10 min carried over = the hour, not 70 min */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H(1), c.today.down_ms[0]);
}

TEST_CASE("ledger: a gap longer than the history is capped", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0 - 100;
    s.alive_ms = at(DAY0 - 100, H(12));
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    fresh(T0, 0);
    lc_restore(&c, &s, true);
    lc_set_utc(&c, T0, at(DAY0, H(6)), LC_CLOCK_SNTP);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.today.utc_day);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H(6), c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_size_t(HIST, c.hist_n);
    for (size_t i = 0; i < HIST; i++) {
        TEST_ASSERT_EQUAL_INT32(DAY0 - 1 - (int32_t)i, c.hist[i].utc_day);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)LC_DAY_MS, c.hist[i].down_ms[0]);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)LC_DAY_MS, c.hist[i].power_off_ms);
    }
}

TEST_CASE("ledger: GNSS time first, SNTP only as a fallback; no time before the floor", "[ledger]")
{
    fresh(T0, 0);
    TEST_ASSERT_FALSE(lc_set_utc(&c, T0, S(LC_UTC_FLOOR_S) - 1, LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_INT(LC_CLOCK_NONE, c.clock);
    TEST_ASSERT_EQUAL_INT64(-1, lc_unix_now(&c, T0));

    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(8)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_INT(LC_CLOCK_SNTP, c.clock);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0 + 1000u, at(DAY0, H(8) + 1500), LC_CLOCK_GNSS));
    TEST_ASSERT_EQUAL_INT(LC_CLOCK_GNSS, c.clock);
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(8) + 2500), lc_unix_now(&c, T0 + 2000u));
    TEST_ASSERT_FALSE(lc_set_utc(&c, T0 + 2000u, at(DAY0, H(8) + 99000), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(8) + 2500), lc_unix_now(&c, T0 + 2000u));
    uint32_t later = T0 + 1000u + LC_GNSS_HOLD_MS;
    TEST_ASSERT_TRUE(lc_set_utc(&c, later, at(DAY0, H(9)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_INT(LC_CLOCK_SNTP, c.clock);

    /* a correction backwards across midnight keeps counting into today */
    lc_set_utc(&c, later, at(DAY0 + 1, S(5)), LC_CLOCK_GNSS);
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
    lc_set_utc(&c, later + 1000u, at(DAY0, H(24) - S(3)), LC_CLOCK_GNSS);
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
}

TEST_CASE("ledger: slots not reported yet follow the previous session", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.expected_mask = 0x03;
    s.protected_mask = 0x01;
    fresh(T0, 0);
    lc_restore(&c, &s, false);
    lc_set_utc(&c, T0, at(DAY0, H(3)), LC_CLOCK_GNSS);
    lc_set_rtcm_ok(&c, T0, true);
    lc_tick(&c, T0 + 10000u);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.down_ms[1]);
    TEST_ASSERT_EQUAL_UINT64(10000, lc_protected_down_ms(&c));

    lc_set_slot(&c, T0 + 10000u, 1, false, false, false);
    lc_set_slot(&c, T0 + 10000u, 0, true, true, true);          /* back after the reboot */
    TEST_ASSERT_EQUAL_UINT32(1, c.today.reconnects[0]);
    lc_tick(&c, T0 + 20000u);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.down_ms[1]);

    lc_snapshot_t now;
    lc_snapshot(&c, &now);
    TEST_ASSERT_EQUAL_HEX8(0x01, now.expected_mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, now.protected_mask);
}

TEST_CASE("ledger: the RTC copy carries a reset across; damage is caught", "[ledger]")
{
    /* a session with a clock, then a software reset 3 s after its last tick */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(5)), LC_CLOCK_SNTP);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, false);
    lc_tick(&c, T0 + 4000u);
    lc_snapshot_t s;
    lc_snapshot(&c, &s);
    lc_rtc_t blk;
    lc_rtc_seal(&blk, &s);
    TEST_ASSERT_TRUE(lc_rtc_valid(&blk));

    fresh(5000u, 1000);                                          /* new boot, 1 s up */
    lc_restore(&c, &blk.snap, false);
    TEST_ASSERT_EQUAL_UINT32(4000, c.today.down_ms[0]);
    lc_set_utc(&c, 5000u, at(DAY0, H(5) + S(4) + S(3)), LC_CLOCK_SNTP);
    /* 2 s of reset gap + 1 s since boot */
    TEST_ASSERT_EQUAL_UINT32(7000, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0, c.today.power_off_ms);

    lc_rtc_t bad = blk;
    ((uint8_t *)&bad.snap)[17] ^= 0x04;
    TEST_ASSERT_FALSE(lc_rtc_valid(&bad));
    bad = blk;
    bad.version++;
    TEST_ASSERT_FALSE(lc_rtc_valid(&bad));
    bad = blk;
    bad.magic = 0;
    TEST_ASSERT_FALSE(lc_rtc_valid(&bad));
}

TEST_CASE("ledger blobs: round trip, newer writers may append, damage is refused", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    for (size_t i = 0; i < LC_SLOTS; i++) {
        s.today.down_ms[i] = 1000u * (uint32_t)i + 7u;
        s.today.planned_ms[i] = (uint32_t)i;
        s.today.reconnects[i] = 3u * (uint32_t)i;
    }
    s.today.power_off_ms = 99;
    s.today.rx_overflows = 4;
    s.expected_mask = 0x81;
    s.protected_mask = 0x01;
    s.planned = true;
    uint8_t buf[200];
    size_t len = lc_encode_today(&s, buf, sizeof(buf));
    TEST_ASSERT_TRUE(len > 0);
    lc_snapshot_t d = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(buf, len, &d));
    TEST_ASSERT_EQUAL_MEMORY(&s.today, &d.today, sizeof(d.today));
    TEST_ASSERT_EQUAL_HEX8(0x81, d.expected_mask);
    TEST_ASSERT_EQUAL_HEX8(0x01, d.protected_mask);
    TEST_ASSERT_TRUE(d.planned);

    buf[len] = 0xAB;                                             /* a newer writer's extra field */
    TEST_ASSERT_TRUE(lc_decode_today(buf, len + 1u, &d));
    TEST_ASSERT_FALSE(lc_decode_today(buf, len - 1u, &d));
    buf[0] = 0;
    TEST_ASSERT_FALSE(lc_decode_today(buf, len, &d));
    TEST_ASSERT_EQUAL_size_t(0, lc_encode_today(&s, buf, 20));

    static lc_day_t days[HIST];
    static lc_day_t back[HIST];
    for (size_t i = 0; i < HIST; i++) {
        memset(&days[i], 0, sizeof(days[i]));
        days[i].utc_day = DAY0 - 1 - (int32_t)i;
        days[i].down_ms[i % LC_SLOTS] = 1000u + (uint32_t)i;
        days[i].power_off_ms = (uint32_t)i;
    }
    static uint8_t hb[4000];
    TEST_ASSERT_TRUE(lc_hist_blob_max(HIST) <= sizeof(hb));
    size_t hlen = lc_encode_hist(days, HIST, hb, sizeof(hb));
    TEST_ASSERT_EQUAL_size_t(lc_hist_blob_max(HIST), hlen);
    TEST_ASSERT_EQUAL_size_t(HIST, lc_decode_hist(hb, hlen, back, HIST));
    TEST_ASSERT_EQUAL_MEMORY(days, back, sizeof(days));
    TEST_ASSERT_EQUAL_size_t(5, lc_decode_hist(hb, hlen, back, 5));
    TEST_ASSERT_EQUAL_size_t(0, lc_decode_hist(hb, hlen - 3u, back, HIST));

    /* history loaded at boot: kept newest first, nothing at or after today */
    fresh(T0, 0);
    lc_snapshot_t t = snap_base();
    t.today.utc_day = DAY0 - 3;
    lc_restore(&c, &t, false);
    lc_set_history(&c, days, HIST);
    TEST_ASSERT_EQUAL_size_t(HIST - 3u, c.hist_n);
    TEST_ASSERT_EQUAL_INT32(DAY0 - 4, c.hist[0].utc_day);
}

TEST_CASE("ledger: the last-alive mark is due once a minute with a clock; dirty flags", "[ledger]")
{
    int64_t alive = 0;
    bool today = false;
    bool hist = false;
    fresh(T0, 0);
    TEST_ASSERT_FALSE(lc_persist_due(&c, T0, 60000u, &alive, &today, &hist));   /* no clock */
    lc_set_utc(&c, T0, at(DAY0, H(24) - S(30)), LC_CLOCK_SNTP);
    lc_set_slot(&c, T0, 0, true, true, false);
    lc_tick(&c, T0 + 1000u);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 1000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(24) - S(29)), alive);
    TEST_ASSERT_TRUE(today);
    TEST_ASSERT_FALSE(lc_persist_due(&c, T0 + 30000u, 60000u, &alive, &today, &hist));
    lc_tick(&c, T0 + 61000u);                                   /* midnight passed */
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 61000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_TRUE(today);
    TEST_ASSERT_TRUE(hist);
    lc_mark_dirty(&c, false, true);                              /* the history write failed */
    lc_tick(&c, T0 + 121000u);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 121000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_TRUE(hist);
}

TEST_CASE("ledger: three days of outages across the clock wrap add up", "[ledger]")
{
    fresh(UINT32_MAX - 100000u, 0);
    uint32_t now = UINT32_MAX - 100000u;
    lc_set_utc(&c, now, at(DAY0, 0), LC_CLOCK_GNSS);
    lc_set_rtcm_ok(&c, now, true);
    lc_set_slot(&c, now, 0, true, true, true);
    uint64_t outage_total = 0;
    uint32_t reconnects = 0;
    /* every 2 h: 37 s down; ticks every second */
    for (uint32_t sec = 1; sec <= 3u * 86400u; sec++) {
        now += 1000u;
        uint32_t phase = sec % 7200u;
        if (phase == 100u) {
            lc_set_slot(&c, now, 0, true, true, false);
        } else if (phase == 137u) {
            lc_set_slot(&c, now, 0, true, true, true);
            outage_total += 37000u;
            reconnects++;
        }
        lc_tick(&c, now);
    }
    TEST_ASSERT_EQUAL_INT32(DAY0 + 3, c.today.utc_day);
    uint64_t sum = c.today.down_ms[0];
    uint32_t rec = c.today.reconnects[0];
    for (size_t i = 0; i < c.hist_n; i++) {
        sum += c.hist[i].down_ms[0];
        rec += c.hist[i].reconnects[0];
    }
    TEST_ASSERT_EQUAL_UINT64(outage_total, sum);
    TEST_ASSERT_EQUAL_UINT32(reconnects, rec);
    TEST_ASSERT_EQUAL_UINT32(12u * 37000u, c.hist[0].down_ms[0]);
}
