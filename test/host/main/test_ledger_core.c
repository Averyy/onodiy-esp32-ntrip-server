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
        lc_restore(&c, &s, cut ? LC_RESET_POWER_CUT : LC_RESET_SOFTWARE);
        TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(12) + S(2)), LC_CLOCK_SNTP));
        /* 2 h gap + 2 s since boot (slot 0 not reported yet: expected, down) */
        TEST_ASSERT_EQUAL_UINT32(5000u + (uint32_t)H(2) + 2000u, c.today.down_ms[0]);
        TEST_ASSERT_EQUAL_UINT32(cut ? (uint32_t)H(2) : 0u, c.today.power_off_ms);
        TEST_ASSERT_EQUAL_UINT32(0, c.today.down_ms[1]);
        TEST_ASSERT_EQUAL_INT64(at(DAY0, H(12) + S(2)), c.alive_ms);
    }
}

TEST_CASE("ledger: a power-on reset that left the receiver running is no power-off time", "[ledger]")
{
    /* On the ESP32-S3 the reset pin reads as power-on: the station's
     * verdict comes before the first clock and turns the gap into a plain
     * (unplanned) reset gap. */
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(12));
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    s.planned = true;   /* receiver work at the reset: still never planned after a power-on class reset */
    fresh(T0, 2000);
    lc_restore(&c, &s, LC_RESET_POWER_CUT);
    TEST_ASSERT_TRUE(lc_power_kept(&c));
    TEST_ASSERT_FALSE(lc_power_kept(&c));   /* once */
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(12) + S(3)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32(0, c.today.power_off_ms);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)S(3), c.today.down_ms[0]);   /* 1 s gap + 2 s since boot: downtime */
    TEST_ASSERT_EQUAL_UINT32(0, c.today.planned_ms[0]);

    /* The verdict after the first clock: the gap stays as counted. */
    s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(12));
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    fresh(T0, 2000);
    lc_restore(&c, &s, LC_RESET_POWER_CUT);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(12) + S(3)), LC_CLOCK_SNTP));
    TEST_ASSERT_FALSE(lc_power_kept(&c));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)S(1), c.today.power_off_ms);

    /* Not a power-on class reset: nothing to change. */
    fresh(T0, 2000);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    TEST_ASSERT_FALSE(lc_power_kept(&c));
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
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
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
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
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
    lc_restore(&c, &s, LC_RESET_POWER_CUT);
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
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
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
    lc_rtc_seal(&blk, &s, 7u);
    TEST_ASSERT_TRUE(lc_rtc_valid(&blk));

    fresh(5000u, 1000);                                          /* new boot, 1 s up */
    lc_restore(&c, &blk.snap, LC_RESET_SOFTWARE);
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
    /* The stamp (lifecycle's image generation) is kept and covered by the CRC. */
    TEST_ASSERT_EQUAL_UINT32(7u, blk.stamp);
    bad = blk;
    bad.stamp = 8u;
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
        s.today.up_ms[i] = 500u * (uint32_t)i + 3u;
    }
    s.today.power_off_ms = 99;
    s.today.rx_overflows = 4;
    s.today.lost_epochs = 17;
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

    TEST_ASSERT_FALSE(d.reboot_planned);
    TEST_ASSERT_EQUAL_INT64(0, d.alive_ms);                      /* no mark */

    buf[len] = 0xAB;                                             /* a newer writer's extra field */
    TEST_ASSERT_TRUE(lc_decode_today(buf, len + 1u, &d));
    TEST_ASSERT_FALSE(lc_decode_today(buf, len - 2u, &d));       /* cut inside the lost epochs */
    TEST_ASSERT_FALSE(lc_decode_today(buf, len - 6u, &d));       /* cut inside the online times */
    /* Today carries the mark it runs up to (one unit), then each slot's online time. */
    s.reboot_planned = true;
    s.alive_ms = at(DAY0, H(7));
    len = lc_encode_today(&s, buf, sizeof(buf));
    TEST_ASSERT_EQUAL_size_t(126 + 4 * LC_SLOTS, len);
    TEST_ASSERT_TRUE(len <= 160);                                /* what the previous build's buffer takes */
    TEST_ASSERT_TRUE(lc_decode_today(buf, len, &d));
    TEST_ASSERT_TRUE(d.reboot_planned);
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(7)), d.alive_ms);
    TEST_ASSERT_EQUAL_MEMORY(&s.today, &d.today, sizeof(d.today));
    /* A blob from before the lost epochs existed ends 4 bytes earlier: none
     * lost, everything else as stored. */
    TEST_ASSERT_TRUE(lc_decode_today(buf, len - 4u, &d));
    TEST_ASSERT_EQUAL_UINT32(0, d.today.lost_epochs);
    TEST_ASSERT_EQUAL_UINT32(s.today.up_ms[3], d.today.up_ms[3]);
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(7)), d.alive_ms);
    /* One from before the online times existed ends 4 bytes a slot earlier
     * than that: no online time. */
    size_t old = len - 4u - 4u * LC_SLOTS;
    TEST_ASSERT_TRUE(lc_decode_today(buf, old, &d));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(7)), d.alive_ms);
    for (size_t i = 0; i < LC_SLOTS; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, d.today.up_ms[i]);
        TEST_ASSERT_EQUAL_UINT32(s.today.down_ms[i], d.today.down_ms[i]);
    }
    TEST_ASSERT_FALSE(lc_decode_today(buf, old - 2u, &d));       /* cut inside the mark */
    /* One from before the mark existed ends 8 bytes earlier, one from
     * before the reboot flag one more. */
    TEST_ASSERT_TRUE(lc_decode_today(buf, old - 8u, &d));
    TEST_ASSERT_TRUE(d.reboot_planned);
    TEST_ASSERT_EQUAL_INT64(0, d.alive_ms);
    TEST_ASSERT_TRUE(lc_decode_today(buf, old - 9u, &d));
    TEST_ASSERT_FALSE(d.reboot_planned);
    TEST_ASSERT_TRUE(d.planned);
    buf[0] = 0;
    TEST_ASSERT_FALSE(lc_decode_today(buf, len, &d));
    TEST_ASSERT_EQUAL_size_t(0, lc_encode_today(&s, buf, 20));

    static lc_day_t days[HIST];
    static lc_day_t back[HIST];
    for (size_t i = 0; i < HIST; i++) {
        memset(&days[i], 0, sizeof(days[i]));
        days[i].utc_day = DAY0 - 1 - (int32_t)i;
        days[i].down_ms[i % LC_SLOTS] = 1000u + (uint32_t)i;
        days[i].up_ms[(i + 1u) % LC_SLOTS] = 86000000u - (uint32_t)i;
        days[i].power_off_ms = (uint32_t)i;
    }
    static uint8_t hb[5000];
    TEST_ASSERT_TRUE(lc_hist_blob_max(HIST) <= sizeof(hb));
    size_t hlen = lc_encode_hist(days, HIST, hb, sizeof(hb));
    TEST_ASSERT_EQUAL_size_t(lc_hist_blob_max(HIST), hlen);
    TEST_ASSERT_EQUAL_size_t(HIST, lc_decode_hist(hb, hlen, back, HIST));
    TEST_ASSERT_EQUAL_MEMORY(days, back, sizeof(days));
    TEST_ASSERT_EQUAL_size_t(5, lc_decode_hist(hb, hlen, back, 5));
    TEST_ASSERT_EQUAL_MEMORY(days, back, 5 * sizeof(days[0]));
    TEST_ASSERT_EQUAL_size_t(0, lc_decode_hist(hb, hlen - 3u, back, HIST));
    /* A history from before the online times (the days alone): no online time. */
    size_t hold = hlen - HIST * 4u * LC_SLOTS;
    TEST_ASSERT_EQUAL_size_t(HIST, lc_decode_hist(hb, hold, back, HIST));
    TEST_ASSERT_EQUAL_UINT32(days[3].down_ms[3], back[3].down_ms[3]);
    TEST_ASSERT_EQUAL_UINT32(0, back[3].up_ms[4]);
    /* A newer writer's extra section after the online times is ignored. */
    hb[hlen] = 0x5A;
    TEST_ASSERT_EQUAL_size_t(HIST, lc_decode_hist(hb, hlen + 1u, back, HIST));
    TEST_ASSERT_EQUAL_MEMORY(days, back, sizeof(days));

    /* history loaded at boot: kept newest first, nothing at or after today */
    fresh(T0, 0);
    lc_snapshot_t t = snap_base();
    t.today.utc_day = DAY0 - 3;
    lc_restore(&c, &t, LC_RESET_SOFTWARE);
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

TEST_CASE("ledger: a reset just after UTC midnight keeps the day that ended", "[ledger]")
{
    /* The protected slot is down from 23:00:30; the stores run once a
     * minute at :30 as ledger.c does; the RTC copy is taken every second. */
    static uint8_t hist_blob[4000];
    static uint8_t today_blob[200];
    size_t hist_len = 0, today_len = 0;
    fresh(T0, 0);
    uint32_t now = T0;
    lc_set_utc(&c, now, at(DAY0, H(23) + S(30)), LC_CLOCK_SNTP);
    lc_set_slot(&c, now, 0, true, true, false);
    lc_rtc_t rtc;
    for (int sec = 0; sec <= 3600 - 30 + 20; sec++) {      /* to 00:00:20 */
        lc_tick(&c, now);
        lc_snapshot_t snap;
        lc_snapshot(&c, &snap);
        lc_rtc_seal(&rtc, &snap, 7u);
        int64_t alive;
        bool today, hist;
        if (lc_persist_due(&c, now, 60000u, &alive, &today, &hist)) {
            if (hist) {
                hist_len = lc_encode_hist(c.hist, c.hist_n, hist_blob, sizeof(hist_blob));
            }
            if (today) {
                today_len = lc_encode_today(&snap, today_blob, sizeof(today_blob));
            }
        }
        now += 1000u;
    }
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.hist[0].utc_day);
    uint32_t down = c.hist[0].down_ms[0];
    TEST_ASSERT_EQUAL_UINT32(S(3570), down);

    /* A watchdog reset: the RTC copy (today = the new day) and what NVS holds. */
    fresh(5000u, 1000);
    lc_restore(&c, &rtc.snap, LC_RESET_OTHER);
    static lc_day_t back[HIST];
    size_t n = lc_decode_hist(hist_blob, hist_len, back, HIST);
    lc_set_history(&c, back, n);
    lc_snapshot_t stored = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(today_blob, today_len, &stored));
    lc_merge_stored_day(&c, &stored.today);
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
    TEST_ASSERT_EQUAL_size_t(1, c.hist_n);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32(down, c.hist[0].down_ms[0]);   /* stored at midnight, not a minute before */
}

TEST_CASE("ledger: the stored today fills a day the stored history missed", "[ledger]")
{
    /* The history write at midnight did not happen (reset in between, or the
     * write failed): the day comes from the stored today, as of its last
     * store; a day the history has is never added twice. */
    lc_snapshot_t rtc = snap_base();
    rtc.today.utc_day = DAY0 + 1;
    rtc.today.down_ms[0] = 5000;
    lc_day_t stored;
    memset(&stored, 0, sizeof(stored));
    stored.utc_day = DAY0;
    stored.down_ms[0] = S(3540);
    lc_day_t old;
    memset(&old, 0, sizeof(old));
    old.utc_day = DAY0 - 1;
    old.down_ms[0] = 42;

    fresh(5000u, 1000);
    lc_restore(&c, &rtc, LC_RESET_SOFTWARE);
    lc_set_history(&c, &old, 1);
    lc_merge_stored_day(&c, &stored);
    TEST_ASSERT_EQUAL_size_t(2, c.hist_n);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32(S(3540), c.hist[0].down_ms[0]);
    TEST_ASSERT_EQUAL_INT32(DAY0 - 1, c.hist[1].utc_day);
    lc_merge_stored_day(&c, &stored);
    TEST_ASSERT_EQUAL_size_t(2, c.hist_n);
    TEST_ASSERT_EQUAL_UINT32(S(3540), c.hist[0].down_ms[0]);
    /* the restored today itself, or an older day the history has: nothing */
    stored.utc_day = DAY0 + 1;
    lc_merge_stored_day(&c, &stored);
    stored.utc_day = DAY0 - 1;
    lc_merge_stored_day(&c, &stored);
    TEST_ASSERT_EQUAL_size_t(2, c.hist_n);
    TEST_ASSERT_EQUAL_UINT32(42, c.hist[1].down_ms[0]);
    /* the merged day is stored with the first clock */
    int64_t alive;
    bool today, hist;
    lc_set_utc(&c, 5000u, at(DAY0 + 1, S(30)), LC_CLOCK_SNTP);
    TEST_ASSERT_TRUE(lc_persist_due(&c, 5000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_TRUE(hist);
}

/* GPS time of week (ms) of a UTC time. */
static uint32_t tow_of(int64_t unix_ms)
{
    int64_t g = unix_ms - LC_GPS_EPOCH_UNIX_MS + (int64_t)LC_GPS_UTC_LEAP_S * 1000;
    return (uint32_t)(g % LC_GPS_WEEK_MS);
}

TEST_CASE("ledger: GNSS time keeps the clock across a reset without SNTP", "[ledger]")
{
    /* A session with a clock; a watchdog reset; the router is down, so no
     * SNTP: the receiver's GPS time of week dates itself against the RTC
     * copy's last-alive mark, and the reset gap is counted. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(10)), LC_CLOCK_SNTP);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    lc_tick(&c, T0 + 5000u);
    lc_snapshot_t s;
    lc_snapshot(&c, &s);

    fresh(9000u, 4000);                                          /* 4 s after boot; 3 s of reset */
    lc_restore(&c, &s, LC_RESET_OTHER);
    int64_t truth = at(DAY0, H(10) + S(5) + S(3) + S(4));
    TEST_ASSERT_TRUE(lc_set_gps_tow(&c, 9000u, tow_of(truth - 300), 300));
    TEST_ASSERT_EQUAL(LC_CLOCK_GNSS, c.clock);
    TEST_ASSERT_EQUAL_INT64(truth, lc_unix_now(&c, 9000u));
    TEST_ASSERT_EQUAL_UINT32(S(7), c.today.down_ms[0]);          /* 3 s gap + 4 s since boot */
    /* SNTP is ignored while GNSS time keeps coming. */
    TEST_ASSERT_FALSE(lc_set_utc(&c, 10000u, at(DAY0, H(11)), LC_CLOCK_SNTP));
    /* A GNSS time that is far from the running clock is not taken. */
    TEST_ASSERT_FALSE(lc_set_gps_tow(&c, 10000u, tow_of(truth + 1000 + H(1)), 0));
    TEST_ASSERT_TRUE(lc_set_gps_tow(&c, 10000u, tow_of(truth + 1000 + 200), 0));
    TEST_ASSERT_EQUAL_INT64(truth + 1200, lc_unix_now(&c, 10000u));
}

TEST_CASE("ledger: GNSS time without a datable week waits for SNTP", "[ledger]")
{
    /* A power cut: the gap is unknown, so the week is too. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(10)), LC_CLOCK_SNTP);
    lc_tick(&c, T0 + 5000u);
    lc_snapshot_t s;
    lc_snapshot(&c, &s);
    fresh(9000u, 4000);
    lc_restore(&c, &s, LC_RESET_POWER_CUT);
    int64_t truth = at(DAY0, H(12));
    TEST_ASSERT_FALSE(lc_set_gps_tow(&c, 9000u, tow_of(truth), 0));
    TEST_ASSERT_EQUAL(LC_CLOCK_NONE, c.clock);
    TEST_ASSERT_TRUE(lc_set_utc(&c, 9000u, truth, LC_CLOCK_SNTP));
    TEST_ASSERT_TRUE(lc_set_gps_tow(&c, 10000u, tow_of(truth + 1000), 0));
    TEST_ASSERT_EQUAL(LC_CLOCK_GNSS, c.clock);

    /* A new station: no mark at all. */
    fresh(T0, 0);
    TEST_ASSERT_FALSE(lc_set_gps_tow(&c, T0, tow_of(truth), 0));
    /* A previous session that itself had no clock at its end: its mark is old. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(1)), LC_CLOCK_SNTP);
    lc_snapshot(&c, &s);
    fresh(9000u, 4000);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    lc_tick(&c, 20000u);
    lc_snapshot(&c, &s);                                         /* no clock this session */
    fresh(9000u, 4000);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    TEST_ASSERT_FALSE(lc_set_gps_tow(&c, 9000u, tow_of(at(DAY0, H(1) + S(20))), 0));
    /* garbage time of week */
    TEST_ASSERT_FALSE(lc_set_gps_tow(&c, 9000u, (uint32_t)LC_GPS_WEEK_MS, 0));
}

TEST_CASE("ledger: GNSS time is dated across the GPS week boundary", "[ledger]")
{
    /* GPS weeks start Sunday 00:00:00 GPS time = Saturday 23:59:42 UTC. */
    int64_t week_start = LC_GPS_EPOCH_UNIX_MS - (int64_t)LC_GPS_UTC_LEAP_S * 1000 + 2437 * LC_GPS_WEEK_MS;
    fresh(T0, 0);
    lc_set_utc(&c, T0, week_start - S(2), LC_CLOCK_SNTP);       /* the old week's end */
    TEST_ASSERT_TRUE(lc_set_gps_tow(&c, T0 + 3000u, 1000, 0));    /* 1 s into the new week */
    TEST_ASSERT_EQUAL_INT64(week_start + 1000, lc_unix_now(&c, T0 + 3000u));
    TEST_ASSERT_TRUE(lc_set_gps_tow(&c, T0 + 3000u, (uint32_t)(LC_GPS_WEEK_MS - 500), 0));   /* late epoch */
    TEST_ASSERT_EQUAL_INT64(week_start - 500, lc_unix_now(&c, T0 + 3000u));
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

/* ---- planned reboots --------------------------------------------------------------------------- */

TEST_CASE("ledger: after a planned reboot the gap and each slot's reconnect are planned, within the window", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(10));                               /* stored as the reboot began */
    s.expected_mask = 0x03;
    s.protected_mask = 0x01;
    s.reboot_planned = true;
    fresh(T0, 2000);                                            /* booted 2 s ago */
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    TEST_ASSERT_TRUE(c.today_dirty);                            /* the stored flag gets replaced */
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(8)), LC_CLOCK_SNTP));
    /* 6 s of gap until the boot, 2 s since: all planned */
    TEST_ASSERT_EQUAL_UINT32(8000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(8000u, c.today.planned_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(8000u, c.today.planned_ms[1]);

    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 1, true, false, false);
    lc_set_slot(&c, T0 + 28000u, 0, true, true, true);          /* Onocoy back 30 s after boot */
    lc_tick(&c, T0 + 198000u);                                  /* 200 s after boot, past the window */
    TEST_ASSERT_EQUAL_UINT32(36000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(36000u, c.today.planned_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(206000u, c.today.down_ms[1]);
    TEST_ASSERT_EQUAL_UINT32(126000u, c.today.planned_ms[1]);   /* 6 s gap + the 120 s window */

    lc_set_slot(&c, T0 + 198000u, 0, true, true, false);        /* a later outage is not planned */
    lc_tick(&c, T0 + 208000u);
    TEST_ASSERT_EQUAL_UINT32(46000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(36000u, c.today.planned_ms[0]);
    lc_snapshot_t now;
    lc_snapshot(&c, &now);
    TEST_ASSERT_FALSE(now.reboot_planned);                      /* this session is not ending */
}

TEST_CASE("ledger: a slot back up during the window is done; one dropping within it is not planned again", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(10));
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    s.reboot_planned = true;
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(5)), LC_CLOCK_SNTP));
    lc_set_slot(&c, T0, 0, true, true, true);                   /* streaming, but no fresh RTCM yet */
    lc_tick(&c, T0 + 10000u);
    TEST_ASSERT_EQUAL_UINT32(15000u, c.today.planned_ms[0]);    /* not up without RTCM behind it */
    lc_set_rtcm_ok(&c, T0 + 10000u, true);                      /* up: its planned reconnect is over */
    lc_set_rtcm_ok(&c, T0 + 20000u, false);                     /* a receiver stall inside the window */
    lc_tick(&c, T0 + 30000u);
    TEST_ASSERT_EQUAL_UINT32(25000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(15000u, c.today.planned_ms[0]);
    lc_planned_boot(&c);                                        /* lifecycle's record arrives late: no change */
    lc_tick(&c, T0 + 40000u);
    TEST_ASSERT_EQUAL_UINT32(15000u, c.today.planned_ms[0]);
}

TEST_CASE("ledger: a power cut is never planned, whatever the books or lifecycle say", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(10));
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    s.planned = true;                                           /* receiver work when the power went */
    s.reboot_planned = true;                                    /* and a reboot had begun */
    fresh(T0, 2000);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_POWER_CUT);
    lc_planned_boot(&c);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(11)), LC_CLOCK_SNTP));
    lc_tick(&c, T0 + 30000u);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H(1) + 30000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, c.today.planned_ms[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)H(1) - 2000u, c.today.power_off_ms);
}

TEST_CASE("ledger: lifecycle's record plans the boot when the books could not; time without a clock too", "[ledger]")
{
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(10));
    s.expected_mask = 0x03;
    s.protected_mask = 0x01;
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);                      /* stale books: no reboot flag */
    TEST_ASSERT_FALSE(c.prev_reboot_planned);
    lc_planned_boot(&c);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0 + 20000u, 0, true, true, true);          /* up after 20 s, before any clock */
    lc_tick(&c, T0 + 50000u);
    TEST_ASSERT_EQUAL_UINT64(50000u, c.pending_ms);
    TEST_ASSERT_EQUAL_UINT32(20000u, c.pending.planned_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(50000u, c.pending.planned_ms[1]);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0 + 50000u, at(DAY0, H(10) + S(80)), LC_CLOCK_SNTP));
    /* 30 s of gap (planned for both) + what the boot counted */
    TEST_ASSERT_EQUAL_UINT32(50000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(50000u, c.today.planned_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(80000u, c.today.planned_ms[1]);

    /* Window 0: only the gap is planned. */
    fresh(T0, 0);
    lc_set_boot_window(&c, 0);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    lc_planned_boot(&c);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(30)), LC_CLOCK_SNTP));
    lc_tick(&c, T0 + 10000u);
    TEST_ASSERT_EQUAL_UINT32(40000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(30000u, c.today.planned_ms[0]);

    /* Without either, the boot is an ordinary one. */
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(30)), LC_CLOCK_SNTP));
    lc_tick(&c, T0 + 10000u);
    TEST_ASSERT_EQUAL_UINT32(40000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, c.today.planned_ms[0]);
}

TEST_CASE("ledger: a planned reboot counts as planned from its start and stores the books at once", "[ledger]")
{
    int64_t alive = 0;
    bool today = false;
    bool hist = false;
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(12)), LC_CLOCK_GNSS);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 1000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_FALSE(lc_persist_due(&c, T0 + 2000u, 60000u, &alive, &today, &hist));

    lc_shutdown(&c, T0 + 2000u, true);                          /* the user's restart begins */
    lc_set_slot(&c, T0 + 2500u, 0, true, true, false);          /* the casters close */
    lc_tick(&c, T0 + 3000u);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 3000u, 0, &alive, &today, &hist));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(12) + 3000), alive);
    TEST_ASSERT_TRUE(today);                                    /* it carries the flag */
    TEST_ASSERT_EQUAL_UINT32(500u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(500u, c.today.planned_ms[0]);
    lc_snapshot_t s;
    lc_snapshot(&c, &s);
    TEST_ASSERT_TRUE(s.reboot_planned);
    lc_rtc_t blk;
    lc_rtc_seal(&blk, &s, 7u);
    TEST_ASSERT_TRUE(lc_rtc_valid(&blk));
    TEST_ASSERT_TRUE(blk.snap.reboot_planned);

    /* Any other reboot stores the books too, without planning anything. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(12)), LC_CLOCK_GNSS);
    lc_set_slot(&c, T0, 0, true, true, false);
    lc_shutdown(&c, T0 + 1000u, false);
    lc_tick(&c, T0 + 2000u);
    TEST_ASSERT_EQUAL_UINT32(2000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, c.today.planned_ms[0]);
    lc_snapshot(&c, &s);
    TEST_ASSERT_FALSE(s.reboot_planned);
}

TEST_CASE("ledger: a crash after a planned reboot is not planned; one boot uses the flag", "[ledger]")
{
    /* An app update's shutdown hook stored the books with the flag. */
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(10));
    s.expected_mask = 0x01;
    s.protected_mask = 0x01;
    s.reboot_planned = true;

    /* The new image's boot (a software restart) is planned. Its snapshot
     * right after the restore, which ledger.c stores at once, has no flag:
     * a panic in a later init or start step hands nothing on. */
    fresh(T0, 2000);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    TEST_ASSERT_TRUE(c.prev_reboot_planned);
    lc_snapshot_t resealed;
    lc_snapshot(&c, &resealed);
    TEST_ASSERT_FALSE(resealed.reboot_planned);
    TEST_ASSERT_EQUAL_INT64(s.alive_ms, resealed.alive_ms);
    TEST_ASSERT_EQUAL_HEX8(0x01, resealed.expected_mask);
    lc_rtc_t rtc;
    lc_rtc_seal(&rtc, &resealed, 7u);

    /* The boot after the panic: 60 s of gap, then 25 s of boot. */
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &rtc.snap, LC_RESET_OTHER);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(60)), LC_CLOCK_SNTP));
    lc_tick(&c, T0 + 25000u);
    TEST_ASSERT_EQUAL_UINT32(85000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, c.today.planned_ms[0]);

    /* Still the flagged books (the panic came before the reseal, or a
     * bootloader rollback reads them): a crash reset is never planned, and
     * lifecycle's record cannot make it so. */
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_OTHER);
    lc_planned_boot(&c);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(60)), LC_CLOCK_SNTP));
    lc_tick(&c, T0 + 25000u);
    TEST_ASSERT_EQUAL_UINT32(85000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, c.today.planned_ms[0]);

    /* Planned receiver work at the crash still makes the gap planned. */
    s.planned = true;
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_OTHER);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(60)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32(60000u, c.today.planned_ms[0]);
}

TEST_CASE("ledger: a failed app trial after an update is not planned, whatever the books say", "[ledger]")
{
    /* The update stored the flag; the new image crashed until its trial was
     * reverted from init (no shutdown hook of ours ran): the old image's
     * restart is a software one, but lifecycle knows it was APP_REVERT. */
    lc_snapshot_t s = snap_base();
    s.today.utc_day = DAY0;
    s.alive_ms = at(DAY0, H(10));
    s.expected_mask = 0x03;
    s.protected_mask = 0x01;
    s.reboot_planned = true;
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    lc_unplanned_boot(&c);
    TEST_ASSERT_FALSE(c.prev_reboot_planned);
    lc_planned_boot(&c);                                        /* nothing plans it again */
    TEST_ASSERT_FALSE(c.prev_reboot_planned);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(40)), LC_CLOCK_SNTP));
    lc_tick(&c, T0 + 30000u);
    TEST_ASSERT_EQUAL_UINT32(70000u, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, c.today.planned_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, c.today.planned_ms[1]);

    /* Without a record either way (another image), the books decide. */
    fresh(T0, 0);
    lc_set_boot_window(&c, 120000u);
    lc_restore(&c, &s, LC_RESET_SOFTWARE);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(40)), LC_CLOCK_SNTP));
    lc_tick(&c, T0 + 30000u);
    TEST_ASSERT_EQUAL_UINT32(70000u, c.today.planned_ms[0]);
}

TEST_CASE("ledger: a reboot without a clock stores today with the mark it runs up to", "[ledger]")
{
    static uint8_t blob[200];
    size_t blob_len = 0;
    int64_t nvs_alive = 0;
    int64_t alive = 0;
    bool today = false;
    bool hist = false;

    /* S1 has a clock and slot 0 is down; stored at its first tick, then 50 s
     * more reach only the RTC copy before a panic. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(10)), LC_CLOCK_SNTP);
    lc_set_slot(&c, T0, 0, true, true, false);
    lc_snapshot_t snap;
    lc_snapshot(&c, &snap);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0, 60000u, &alive, &today, &hist));
    blob_len = lc_encode_today(&snap, blob, sizeof(blob));
    nvs_alive = alive;
    lc_tick(&c, T0 + 50000u);
    lc_snapshot(&c, &snap);

    /* S2 restores the RTC copy and gets no clock (no fix, no internet); a
     * ladder reboot after 100 s stores today with the mark it runs up to. */
    fresh(T0, 0);
    lc_restore(&c, &snap, LC_RESET_OTHER);
    TEST_ASSERT_FALSE(lc_persist_due(&c, T0 + 60000u, 60000u, &alive, &today, &hist));   /* the tick: no clock */
    lc_shutdown(&c, T0 + 100000u, false);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 100000u, 0, &alive, &today, &hist));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(10) + S(50)), alive);
    TEST_ASSERT_TRUE(today);
    lc_snapshot(&c, &snap);
    blob_len = lc_encode_today(&snap, blob, sizeof(blob));
    nvs_alive = alive;

    /* S3 after a power cut: restored from NVS, SNTP 200 s after S1 began. */
    lc_snapshot_t st = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(blob, blob_len, &st));
    TEST_ASSERT_EQUAL_INT64(nvs_alive, st.alive_ms);            /* the blob carries its own mark */
    st.alive_ms = lc_stored_mark(st.alive_ms, 0);
    fresh(T0, 0);
    lc_restore(&c, &st, LC_RESET_POWER_CUT);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + S(200)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32(200000u, c.today.down_ms[0]);      /* not 250 s: nothing counted twice */

    /* A station that never had a clock has no mark: nothing is stored. */
    fresh(T0, 0);
    lc_shutdown(&c, T0 + 1000u, true);
    TEST_ASSERT_FALSE(lc_persist_due(&c, T0 + 1000u, 0, &alive, &today, &hist));
}

TEST_CASE("ledger: today and its mark are stored as one unit; a stale alive key never counts time twice",
          "[ledger]")
{
    static uint8_t blob[200];
    size_t blob_len = 0;
    int64_t alive_key_s = 0;   /* the alive key as NVS holds it */
    int64_t alive = 0;
    bool today = false;
    bool hist = false;

    /* Slot 0 is down from 10:00; stores once a minute, as ledger.c does:
     * today changed, so today with its mark goes as one blob. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(10)), LC_CLOCK_SNTP);
    lc_set_slot(&c, T0, 0, true, true, false);
    uint32_t now = T0;
    for (int min = 0; min <= 2; min++) {
        lc_tick(&c, now);
        lc_snapshot_t snap;
        lc_snapshot(&c, &snap);
        TEST_ASSERT_TRUE(lc_persist_due(&c, now, 60000u, &alive, &today, &hist));
        TEST_ASSERT_TRUE(today);
        snap.alive_ms = alive;
        blob_len = lc_encode_today(&snap, blob, sizeof(blob));
        if (min == 0) {
            alive_key_s = alive / 1000;   /* an older alive key, from before */
        }
        now += 60000u;
    }
    /* A power cut 30 s after the last store; back at 10:03:00 by SNTP. The
     * alive key is two minutes behind the blob: with two keys this is what a
     * failed second write left, and the restore counted those two minutes
     * twice. The blob's own mark wins. */
    lc_snapshot_t st = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(blob, blob_len, &st));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(10) + M(2)), st.alive_ms);
    st.alive_ms = lc_stored_mark(st.alive_ms, alive_key_s);
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(10) + M(2)), st.alive_ms);
    fresh(T0, 0);
    lc_restore(&c, &st, LC_RESET_POWER_CUT);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + M(3)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(3), c.today.down_ms[0]);   /* not 5 min */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(1), c.today.power_off_ms);
}

TEST_CASE("ledger: while today is unchanged the alive key alone moves the mark on", "[ledger]")
{
    static uint8_t blob[200];
    size_t blob_len = 0;
    int64_t alive_key_s = 0;
    int64_t alive = 0;
    bool today = false;
    bool hist = false;

    /* Slot 0 streams all along: after the first store today never changes,
     * so only the mark is written (one small write, not the blob). */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(10)), LC_CLOCK_SNTP);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    uint32_t now = T0;
    int blobs = 0;
    for (int min = 0; min <= 10; min++) {
        lc_tick(&c, now);
        lc_snapshot_t snap;
        lc_snapshot(&c, &snap);
        TEST_ASSERT_TRUE(lc_persist_due(&c, now, 60000u, &alive, &today, &hist));
        if (today) {
            snap.alive_ms = alive;
            blob_len = lc_encode_today(&snap, blob, sizeof(blob));
            blobs++;
        } else {
            alive_key_s = alive / 1000;
        }
        now += 60000u;
    }
    TEST_ASSERT_EQUAL(1, blobs);
    /* A power cut; back at 10:12. The gap runs from the later mark (10:10),
     * not from the blob's (10:00): ten minutes of streaming are not down. */
    lc_snapshot_t st = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(blob, blob_len, &st));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(10)), st.alive_ms);
    TEST_ASSERT_EQUAL_UINT32(0, st.today.up_ms[0]);              /* the blob is from 10:00 */
    int64_t blob_mark = st.alive_ms;
    st.alive_ms = lc_stored_mark(st.alive_ms, alive_key_s);
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(10) + M(10)), st.alive_ms);
    /* While the mark alone moved on, slot 0 streamed: ten minutes online. */
    lc_stored_online(&st, blob_mark, st.alive_ms);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(10), st.today.up_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0, st.today.up_ms[1]);              /* not expected */
    fresh(T0, 0);
    lc_restore(&c, &st, LC_RESET_POWER_CUT);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + M(12)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(2), c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(10), c.today.up_ms[0]);   /* the cut itself is down, not online */

    /* No mark of its own (an older layout): nothing to rebuild. Never past
     * the end of the blob's day. */
    lc_snapshot_t o = snap_base();
    o.today.utc_day = DAY0;
    o.expected_mask = 0x01;
    lc_stored_online(&o, 0, at(DAY0, H(11)));
    TEST_ASSERT_EQUAL_UINT32(0, o.today.up_ms[0]);
    lc_stored_online(&o, at(DAY0, H(24) - M(1)), at(DAY0 + 1, H(2)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(1), o.today.up_ms[0]);
    lc_stored_online(&o, at(DAY0, H(5)), at(DAY0, H(4)));          /* a mark behind the blob's */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(1), o.today.up_ms[0]);

    /* The older two-key layout: a today blob without a mark, the alive key alone. */
    TEST_ASSERT_EQUAL_INT64(S(1234), lc_stored_mark(0, 1234));
    TEST_ASSERT_EQUAL_INT64(S(1234), lc_stored_mark(S(1000), 1234));
    TEST_ASSERT_EQUAL_INT64(S(2000), lc_stored_mark(S(2000), 1234));
    TEST_ASSERT_EQUAL_INT64(S(2000), lc_stored_mark(S(2000), 0));
    TEST_ASSERT_EQUAL_INT64(0, lc_stored_mark(0, -5));
}

TEST_CASE("ledger: after the clock went back, today is stored again with the corrected mark", "[ledger]")
{
    static uint8_t blob[200];
    size_t blob_len = 0;
    int64_t alive_key_s = 0;
    int64_t alive = 0;
    bool today = false;
    bool hist = false;

    /* SNTP set the clock 8 min fast; today is stored at "12:00". Half a
     * minute later GNSS time corrects it; the slot streams all along, so
     * today does not change. Stores once a minute, as ledger.c does. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(12)), LC_CLOCK_SNTP);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    uint32_t now = T0;
    int blobs = 0;
    for (int min = 0; min <= 5; min++) {
        if (min == 1) {
            TEST_ASSERT_TRUE(lc_set_utc(&c, now - 30000u, at(DAY0, H(11) + M(52) + S(30)), LC_CLOCK_GNSS));
        }
        lc_tick(&c, now);
        lc_snapshot_t snap;
        lc_snapshot(&c, &snap);
        TEST_ASSERT_TRUE(lc_persist_due(&c, now, 60000u, &alive, &today, &hist));
        if (today) {
            snap.alive_ms = alive;
            blob_len = lc_encode_today(&snap, blob, sizeof(blob));
            blobs++;
            if (alive_key_s > alive / 1000) {
                alive_key_s = alive / 1000;   /* lowered, as ledger.c does */
            }
        } else {
            alive_key_s = alive / 1000;
        }
        now += 60000u;
    }
    /* The first store and the one right after the correction (11:53). */
    TEST_ASSERT_EQUAL(2, blobs);

    /* A power cut after the 11:57 store; back at 12:30 by SNTP. The gap runs
     * from 11:57, not from the blob's 12:00 of the fast clock. */
    lc_snapshot_t st = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(blob, blob_len, &st));
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(11) + M(53)), st.alive_ms);
    st.alive_ms = lc_stored_mark(st.alive_ms, alive_key_s);
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(11) + M(57)), st.alive_ms);
    fresh(T0, 0);
    lc_restore(&c, &st, LC_RESET_POWER_CUT);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(12) + M(30)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(33), c.today.power_off_ms);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(33), c.today.down_ms[0]);

    /* GNSS time comes every second, a few ms off the running clock: a
     * sample behind it is no correction behind the stored mark, and the
     * mark alone moves on (no blob per store). */
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0, 60000u, &alive, &today, &hist));
    TEST_ASSERT_TRUE(today);                                    /* restored: stored again */
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0 + 1000u, lc_unix_now(&c, T0 + 1000u) - 40, LC_CLOCK_GNSS));
    lc_tick(&c, T0 + 60000u);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 60000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_FALSE(today);
}

TEST_CASE("ledger: the time a slot streamed is counted next to its downtime", "[ledger]")
{
    fresh(T0, 0);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(24) - S(20)), LC_CLOCK_SNTP));
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);                   /* streaming */
    lc_set_slot(&c, T0, 1, true, false, false);                 /* expected, not streaming */
    lc_tick(&c, T0 + 5000u);
    TEST_ASSERT_EQUAL_UINT32(5000, c.today.up_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0, c.today.down_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0, c.today.up_ms[1]);
    TEST_ASSERT_EQUAL_UINT32(5000, c.today.down_ms[1]);

    /* a receiver stall with the session still open: down, not online */
    lc_set_rtcm_ok(&c, T0 + 5000u, false);
    lc_tick(&c, T0 + 8000u);
    TEST_ASSERT_EQUAL_UINT32(5000, c.today.up_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(3000, c.today.down_ms[0]);
    lc_set_rtcm_ok(&c, T0 + 8000u, true);

    /* UTC midnight splits it like the downtime */
    lc_tick(&c, T0 + 30000u);
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.up_ms[0]);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32(17000, c.hist[0].up_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(3000, c.hist[0].down_ms[0]);

    /* a reset keeps it (the RTC copy) */
    lc_snapshot_t snap;
    lc_snapshot(&c, &snap);
    lc_rtc_t blk;
    lc_rtc_seal(&blk, &snap, 1u);
    fresh(1000u, 0);
    lc_restore(&c, &blk.snap, LC_RESET_SOFTWARE);
    TEST_ASSERT_EQUAL_UINT32(10000, c.today.up_ms[0]);

    /* a slot turned off counts neither */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(3)), LC_CLOCK_SNTP);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, false, true, false);
    lc_tick(&c, T0 + 10000u);
    TEST_ASSERT_EQUAL_UINT32(0, c.today.up_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0, c.today.down_ms[0]);
}

TEST_CASE("ledger: online time before the first clock is spread like the downtime", "[ledger]")
{
    fresh(T0, 0);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    lc_tick(&c, T0 + 3600000u);                                 /* one hour streaming, no clock */
    lc_day_t t;
    lc_today(&c, &t);
    TEST_ASSERT_EQUAL_UINT32(3600000u, t.up_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(0, t.down_ms[0]);

    TEST_ASSERT_TRUE(lc_set_utc(&c, T0 + 3600000u, at(DAY0, M(30)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32(1800000u, c.today.up_ms[0]);
    TEST_ASSERT_EQUAL_INT32(DAY0 - 1, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32(1800000u, c.hist[0].up_ms[0]);
}

/* ---- Lost epochs (the protected slot's data rate across restarts) ---------------------------- */

TEST_CASE("ledger: today's lost epochs add up, go with today and start again at midnight", "[ledger]")
{
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(23) + M(59)), LC_CLOCK_SNTP);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    lc_count_lost_epochs(&c, T0, 0);                             /* nothing */
    TEST_ASSERT_EQUAL_UINT32(0, c.today.lost_epochs);
    lc_count_lost_epochs(&c, T0 + 1000u, 2);
    lc_count_lost_epochs(&c, T0 + 2000u, 3);
    TEST_ASSERT_EQUAL_UINT32(5, c.today.lost_epochs);
    TEST_ASSERT_TRUE(c.today_dirty);                             /* stored with today at the next store */
    lc_day_t t;
    lc_today(&c, &t);
    TEST_ASSERT_EQUAL_UINT32(5, t.lost_epochs);
    /* UTC midnight: the new day starts with none, and the finished day does
     * not keep them (the history blob has no room: an older build reads it
     * into a buffer of exactly its size). */
    lc_tick(&c, T0 + 61000u);
    TEST_ASSERT_EQUAL_INT32(DAY0 + 1, c.today.utc_day);
    TEST_ASSERT_EQUAL_UINT32(0, c.today.lost_epochs);
    TEST_ASSERT_EQUAL_INT32(DAY0, c.hist[0].utc_day);
    TEST_ASSERT_EQUAL_UINT32(0, c.hist[0].lost_epochs);
    lc_count_lost_epochs(&c, T0 + 62000u, 1);
    TEST_ASSERT_EQUAL_UINT32(1, c.today.lost_epochs);
    /* A clock set back behind today keeps counting into today, never into a history day. */
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0 + 63000u, at(DAY0, H(23) + M(30)), LC_CLOCK_GNSS));
    lc_count_lost_epochs(&c, T0 + 64000u, 4);
    TEST_ASSERT_EQUAL_UINT32(5, c.today.lost_epochs);
    TEST_ASSERT_EQUAL_UINT32(0, c.hist[0].lost_epochs);
}

TEST_CASE("ledger: lost epochs before the first clock go to today with it", "[ledger]")
{
    fresh(T0, 0);
    lc_count_lost_epochs(&c, T0 + 1000u, 7);
    TEST_ASSERT_EQUAL_UINT32(7, c.pending.lost_epochs);
    lc_day_t t;
    lc_today(&c, &t);
    TEST_ASSERT_EQUAL_UINT32(7, t.lost_epochs);                  /* shown while there is no clock */
    /* The pending span reaches back into yesterday: the time is spread, the lost epochs are today's. */
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0 + 10000u, at(DAY0, S(2)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_INT32(DAY0, c.today.utc_day);
    TEST_ASSERT_EQUAL_UINT32(7, c.today.lost_epochs);
    TEST_ASSERT_EQUAL_UINT32(0, c.pending.lost_epochs);
    for (size_t i = 0; i < c.hist_n; i++) {
        TEST_ASSERT_EQUAL_UINT32(0, c.hist[i].lost_epochs);
    }
}

TEST_CASE("ledger: lost epochs survive a reset (RTC copy) and a power cut (as last stored)", "[ledger]")
{
    static uint8_t blob[200];
    size_t blob_len = 0;
    int64_t alive = 0;
    bool today = false;
    bool hist = false;

    /* 10:00, Onocoy streams; 5 epochs lost, stored at the next store. */
    fresh(T0, 0);
    lc_set_utc(&c, T0, at(DAY0, H(10)), LC_CLOCK_SNTP);
    lc_set_rtcm_ok(&c, T0, true);
    lc_set_slot(&c, T0, 0, true, true, true);
    lc_count_lost_epochs(&c, T0 + 1000u, 5);
    lc_tick(&c, T0 + 60000u);
    lc_snapshot_t snap;
    lc_snapshot(&c, &snap);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 60000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_TRUE(today);
    snap.alive_ms = alive;
    blob_len = lc_encode_today(&snap, blob, sizeof(blob));
    TEST_ASSERT_TRUE(blob_len > 0 && blob_len <= 160);

    /* 3 more, then the store a minute later is cut by a power cut while
     * NVS writes it: NVS replaces a key as a unit, so the stored blob stays
     * the one from the first store, with its own mark. */
    lc_count_lost_epochs(&c, T0 + 70000u, 3);
    lc_tick(&c, T0 + 120000u);
    TEST_ASSERT_TRUE(lc_persist_due(&c, T0 + 120000u, 60000u, &alive, &today, &hist));
    TEST_ASSERT_TRUE(today);
    lc_snapshot(&c, &snap);
    TEST_ASSERT_EQUAL_UINT32(8, snap.today.lost_epochs);

    /* A software reset first: the RTC copy has all 8. */
    lc_rtc_t blk;
    lc_rtc_seal(&blk, &snap, 3u);
    fresh(5000u, 1000);
    TEST_ASSERT_TRUE(lc_rtc_valid(&blk));
    lc_restore(&c, &blk.snap, LC_RESET_SOFTWARE);
    TEST_ASSERT_EQUAL_UINT32(8, c.today.lost_epochs);
    lc_set_utc(&c, 5000u, at(DAY0, H(10) + M(2) + S(3)), LC_CLOCK_SNTP);
    TEST_ASSERT_EQUAL_UINT32(8, c.today.lost_epochs);

    /* The power cut instead: restored from the stored blob, the count as of
     * its store (5), never more than was stored, and the gap from its mark. */
    lc_snapshot_t st = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(blob, blob_len, &st));
    TEST_ASSERT_EQUAL_UINT32(5, st.today.lost_epochs);
    st.alive_ms = lc_stored_mark(st.alive_ms, 0);
    fresh(T0, 0);
    lc_restore(&c, &st, LC_RESET_POWER_CUT);
    TEST_ASSERT_TRUE(lc_set_utc(&c, T0, at(DAY0, H(10) + M(5)), LC_CLOCK_SNTP));
    TEST_ASSERT_EQUAL_UINT32(5, c.today.lost_epochs);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)M(4), c.today.down_ms[0]);   /* the gap from the 10:01 mark */

    /* An older build's blob (no lost epochs) after an update: none lost, nothing else changed. */
    st = snap_base();
    TEST_ASSERT_TRUE(lc_decode_today(blob, blob_len - 4u, &st));
    TEST_ASSERT_EQUAL_UINT32(0, st.today.lost_epochs);
    TEST_ASSERT_EQUAL_INT64(at(DAY0, H(10) + M(1)), st.alive_ms);

    /* The RTC block of an older build (version 4) is refused: the stored blob decides. */
    lc_rtc_seal(&blk, &snap, 3u);
    blk.version = 4u;
    TEST_ASSERT_FALSE(lc_rtc_valid(&blk));
}
