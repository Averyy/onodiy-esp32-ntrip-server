/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "net_roam.h"
#include "unity.h"

static const uint8_t AP_A[6] = { 0x02, 0, 0, 0, 0, 0x0A };   /* locally administered: never a real device */
static const uint8_t AP_B[6] = { 0x02, 0, 0, 0, 0, 0x0B };
static const uint8_t AP_C[6] = { 0x02, 0, 0, 0, 0, 0x0C };

static const net_roam_cfg_t CFG = { .weak_dbm = -70, .gain_db = 12, .min_interval_s = 1800 };

static net_roam_bss_t bss(const uint8_t id[6], int8_t rssi, uint8_t ch)
{
    net_roam_bss_t b = { .rssi = rssi, .channel = ch };
    memcpy(b.bssid, id, 6);
    return b;
}

/* Associated with AP_A at t0, a steady reading every 5 s from then on (as network.c feeds it). */
static void start(net_roam_t *r, uint32_t t0, int8_t rssi)
{
    net_roam_init(r, &CFG);
    net_roam_associated(r, AP_A, t0);
    net_roam_rssi(r, rssi);
}

/* Ticks from `from` to `to` (inclusive) with a reading every 5 s; the first second with `want`, or 0 if none. */
static uint32_t run_until(net_roam_t *r, uint32_t from, uint32_t to, int8_t rssi, bool held, net_roam_act_t want)
{
    for (uint32_t t = from; t != to + 1u; t++) {
        if ((t - from) % 5u == 0) {
            net_roam_rssi(r, rssi);
        }
        if (net_roam_tick(r, held, t) == want) {
            return t;
        }
    }
    return 0;
}

TEST_CASE("roaming: a strong signal never scans, a weak one only after the settle time and a minute of weakness",
          "[net_roam]")
{
    net_roam_t r;
    start(&r, 1000, -55);
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, 1000, 1000 + 86400, -55, false, NET_ROAM_SCAN));
    /* At -70 dBm (the threshold itself): weak. First scan once the association is 120 s old... */
    start(&r, 1000, -70);
    TEST_ASSERT_EQUAL_UINT32(1000 + NET_ROAM_SETTLE_S, run_until(&r, 1000, 5000, -70, false, NET_ROAM_SCAN));
    /* ...and only after 60 s of weakness: weak from 1500 on. */
    start(&r, 1000, -60);
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, 1000, 1499, -60, false, NET_ROAM_SCAN));
    for (int i = 0; i < 40; i++) {
        net_roam_rssi(&r, -80);   /* the average follows within about 20 s */
    }
    TEST_ASSERT_EQUAL_UINT32(1500 + NET_ROAM_WEAK_HOLD_S, run_until(&r, 1500, 3000, -80, false, NET_ROAM_SCAN));
    /* -69 dBm is not weak. */
    start(&r, 1000, -69);
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, 1000, 1000 + 86400, -69, false, NET_ROAM_SCAN));
}

TEST_CASE("roaming: one access point and a weak signal wait at least 6 h, and never move", "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -78);
    uint32_t t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SETTLE_S, t);
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 1));   /* no second scan while one runs */
    net_roam_bss_t only = bss(AP_A, -77, 6);
    net_roam_scan_done(&r, &only, 1, t + 2);
    uint32_t next = run_until(&r, t + 3, t + 2 + NET_ROAM_SCAN_SOLO_S + 10, -78, false, NET_ROAM_SCAN);
    TEST_ASSERT_EQUAL_UINT32(t + 2 + NET_ROAM_SCAN_SOLO_S, next);
    net_roam_scan_done(&r, NULL, 0, next);   /* nothing found at all */
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, next + 1, next + 3600, -78, false, NET_ROAM_GO));
}

TEST_CASE("roaming: a clearly stronger access point found twice 30 s apart makes the station reconnect, once per "
          "min interval",
          "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -78);
    uint32_t t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_bss_t seen[] = { bss(AP_A, -79, 1), bss(AP_B, -60, 6), bss(AP_C, -85, 11) };
    net_roam_scan_done(&r, seen, 3, t);
    TEST_ASSERT_EQUAL_UINT8(1, r.hits);
    TEST_ASSERT_EQUAL_MEMORY(AP_B, r.cand.bssid, 6);
    /* The confirming scan 30 s later. */
    uint32_t t2 = run_until(&r, t + 1, t + 100, -78, false, NET_ROAM_SCAN);
    TEST_ASSERT_EQUAL_UINT32(t + NET_ROAM_CONFIRM_S, t2);
    net_roam_scan_done(&r, seen, 3, t2);
    TEST_ASSERT_EQUAL(NET_ROAM_GO, net_roam_tick(&r, false, t2 + 1));
    TEST_ASSERT_EQUAL_INT8(-60, r.cand.rssi);
    TEST_ASSERT_EQUAL_UINT8(6, r.cand.channel);
    /* The reconnect joined B; still weak there (say), but no scan for 30 min after the move. */
    net_roam_disassociated(&r);
    net_roam_associated(&r, AP_B, t2 + 5);
    uint32_t t3 = run_until(&r, t2 + 5, t2 + 1 + 4000, -75, false, NET_ROAM_SCAN);
    TEST_ASSERT_EQUAL_UINT32(t2 + 1 + CFG.min_interval_s, t3);
}

TEST_CASE("roaming: a candidate below the gain does not count; mesh backoff starts at 10 min", "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -78);
    uint32_t t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_bss_t seen[] = { bss(AP_B, -67, 6) };   /* 11 dB over the average */
    net_roam_scan_done(&r, seen, 1, t);
    TEST_ASSERT_EQUAL_UINT8(0, r.hits);
    TEST_ASSERT_TRUE(r.other_seen);
    TEST_ASSERT_EQUAL_UINT32(t + NET_ROAM_SCAN_MESH_S, run_until(&r, t + 1, t + 2000, -78, false, NET_ROAM_SCAN));
}

TEST_CASE("roaming: the current access point's scan reading raises the baseline", "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -78);
    uint32_t t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    /* B is 12 dB over the average (-78) but only 9 dB over A's own reading in the same scan. */
    net_roam_bss_t seen[] = { bss(AP_A, -75, 1), bss(AP_B, -66, 6) };
    net_roam_scan_done(&r, seen, 2, t);
    TEST_ASSERT_EQUAL_UINT8(0, r.hits);
    /* A weaker own reading in the scan does not lower it: the average stands. */
    net_roam_t q;
    start(&q, 0, -78);
    t = run_until(&q, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_bss_t low[] = { bss(AP_A, -90, 1), bss(AP_B, -70, 6) };
    net_roam_scan_done(&q, low, 2, t);
    TEST_ASSERT_EQUAL_UINT8(0, q.hits);
}

/* Complete ordinary scans without a worthwhile alternative, stopping just
 * after the result which selects the daily interval. */
static uint32_t reach_daily(net_roam_t *r, uint32_t start_s, int8_t rssi)
{
    static const uint32_t waits[] = { 600u, 1800u, 3600u, 10800u, 21600u, 43200u, 86400u };
    start(r, start_s, rssi);
    uint32_t t = run_until(r, start_s, start_s + 1000u, rssi, false, NET_ROAM_SCAN);
    net_roam_bss_t other = bss(AP_B, rssi, 6);
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
        net_roam_scan_done(r, &other, 1, t);
        TEST_ASSERT_EQUAL_UINT32(waits[i], r->scan_wait_s);
        if (i + 1u < sizeof(waits) / sizeof(waits[0])) {
            TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(r, false, t + waits[i] - 1u));
            t += waits[i];
            TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(r, false, t));
        }
    }
    return t;
}

TEST_CASE("roaming: completed searches back off to a day and saturate across the clock wrap", "[net_roam]")
{
    net_roam_t r;
    uint32_t t = reach_daily(&r, UINT32_MAX - 1000u, -71);
    net_roam_bss_t other = bss(AP_B, -74, 6);
    for (unsigned i = 0; i < 3; i++) {
        TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + NET_ROAM_SCAN_MAX_S - 1u));
        t += NET_ROAM_SCAN_MAX_S;
        TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(&r, false, t));
        net_roam_scan_done(&r, &other, 1, t);
        TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SCAN_MAX_S, r.scan_wait_s);
    }
}

TEST_CASE("roaming: a singleton starts at six hours then twelve and twenty four", "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -78);
    uint32_t t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_bss_t only = bss(AP_A, -77, 6);
    const uint32_t waits[] = { 21600u, 43200u, 86400u, 86400u };
    for (size_t i = 0; i < sizeof(waits) / sizeof(waits[0]); i++) {
        net_roam_scan_done(&r, &only, 1, t);
        TEST_ASSERT_EQUAL_UINT32(waits[i], r.scan_wait_s);
        t += waits[i];
        TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(&r, false, t));
    }
}

TEST_CASE("roaming: threshold oscillations and holds preserve daily backoff and never catch up in a burst", "[net_roam]")
{
    net_roam_t r;
    uint32_t t = reach_daily(&r, 0, -71);
    uint8_t step = r.negative_step;
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, t + 1, t + 120, -69, false, NET_ROAM_SCAN));
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, t + 121, t + 300, -71, false, NET_ROAM_SCAN));
    TEST_ASSERT_EQUAL_UINT8(step, r.negative_step);
    TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SCAN_MAX_S, r.scan_wait_s);
    t += NET_ROAM_SCAN_MAX_S + 100;
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, true, t));
    TEST_ASSERT_EQUAL_UINT8(step, r.negative_step);
    TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(&r, false, t + 1));
    net_roam_bss_t other = bss(AP_B, -74, 6);
    net_roam_scan_done(&r, &other, 1, t + 2);
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 3));
    TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SCAN_MAX_S, r.scan_wait_s);
}

TEST_CASE("roaming: deterioration is sustained, shortens the deadline once and respects the ten minute floor", "[net_roam]")
{
    net_roam_t r;
    uint32_t t = reach_daily(&r, 0, -71);
    for (unsigned i = 0; i < 40; i++) {
        net_roam_rssi(&r, -80);
    }
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 100));
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 100 + NET_ROAM_CHANGE_HOLD_S - 1));
    TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SCAN_MAX_S, r.scan_wait_s);
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 100 + NET_ROAM_CHANGE_HOLD_S));
    TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SCAN_MESH_S, r.scan_wait_s);
    TEST_ASSERT_EQUAL_UINT8(0, r.negative_step);
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + NET_ROAM_SCAN_MESH_S - 1));
    TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(&r, false, t + NET_ROAM_SCAN_MESH_S));
    net_roam_bss_t other = bss(AP_B, -80, 6);
    net_roam_scan_done(&r, &other, 1, t + NET_ROAM_SCAN_MESH_S);
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, t + 601, t + 1100, -80, false, NET_ROAM_SCAN));
    TEST_ASSERT_EQUAL_UINT8(1, r.negative_step);   /* the same poor signal never resets again */
}

TEST_CASE("roaming: sustained recovery re-arms deterioration without itself resetting backoff", "[net_roam]")
{
    net_roam_t r;
    uint32_t t = reach_daily(&r, 0, -80);
    for (unsigned i = 0; i < 40; i++) {
        net_roam_rssi(&r, -65);
    }
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 1));
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 1 + NET_ROAM_CHANGE_HOLD_S));
    TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SCAN_MAX_S, r.scan_wait_s);
    for (unsigned i = 0; i < 40; i++) {
        net_roam_rssi(&r, -75);
    }
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 100));
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 100 + NET_ROAM_CHANGE_HOLD_S));
    TEST_ASSERT_EQUAL_UINT32(NET_ROAM_SCAN_MESH_S, r.scan_wait_s);
}

TEST_CASE("roaming: failed scans preserve negative backoff and invalidate confirmation hits", "[net_roam]")
{
    net_roam_t r;
    uint32_t t = reach_daily(&r, 0, -78);
    uint8_t step = r.negative_step;
    net_roam_bss_t better = bss(AP_B, -55, 6);
    t += NET_ROAM_SCAN_MAX_S;
    TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(&r, false, t));
    net_roam_scan_done(&r, &better, 1, t);
    TEST_ASSERT_EQUAL_UINT8(1, r.hits);
    t += NET_ROAM_CONFIRM_S;
    TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(&r, false, t));
    net_roam_scan_failed(&r, t);
    TEST_ASSERT_EQUAL_UINT8(0, r.hits);
    TEST_ASSERT_EQUAL_UINT8(step, r.negative_step);
    t += NET_ROAM_RETRY_S;
    TEST_ASSERT_EQUAL(NET_ROAM_SCAN, net_roam_tick(&r, false, t));
    net_roam_scan_done(&r, &better, 1, t);
    TEST_ASSERT_EQUAL_UINT8(1, r.hits);
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 1));
    uint32_t gen = r.generation;
    net_roam_associated(&r, AP_B, t + 2);
    TEST_ASSERT_EQUAL_UINT32(gen + 1, r.generation);
    TEST_ASSERT_EQUAL_UINT8(0, r.negative_step);
    TEST_ASSERT_FALSE(r.ref_ok);
}

TEST_CASE("roaming: a hold, a recovered signal or a lost association forgets the candidate", "[net_roam]")
{
    net_roam_bss_t seen[] = { bss(AP_B, -55, 6) };
    /* Held at the confirming scan (an update starts): no scan, no move while held, and the run starts over. */
    net_roam_t r;
    start(&r, 0, -78);
    uint32_t t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_scan_done(&r, seen, 1, t);
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, t + 1, t + 3600, -78, true, NET_ROAM_SCAN));
    TEST_ASSERT_EQUAL_UINT8(0, r.hits);
    uint32_t t2 = run_until(&r, t + 3601, t + 4000, -78, false, NET_ROAM_SCAN);
    TEST_ASSERT_NOT_EQUAL(0, t2);
    net_roam_scan_done(&r, seen, 1, t2);
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t2 + 1));   /* one hit only: confirm first */
    /* A hit, then the signal recovers before the confirming scan. */
    start(&r, 0, -78);
    t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_scan_done(&r, seen, 1, t);
    TEST_ASSERT_EQUAL_UINT32(0, run_until(&r, t + 1, t + 600, -50, false, NET_ROAM_SCAN));
    TEST_ASSERT_EQUAL_UINT8(0, r.hits);
    /* The association ends while a scan runs: its result is ignored. */
    start(&r, 0, -78);
    t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_disassociated(&r);
    net_roam_scan_done(&r, seen, 1, t + 1);
    TEST_ASSERT_EQUAL_UINT8(0, r.hits);
    TEST_ASSERT_EQUAL(NET_ROAM_IDLE, net_roam_tick(&r, false, t + 2));
}

TEST_CASE("roaming: two passing scans in a row move even when the strongest access point changed", "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -80);
    uint32_t t = run_until(&r, 0, 1000, -80, false, NET_ROAM_SCAN);
    net_roam_bss_t s1[] = { bss(AP_B, -60, 6), bss(AP_C, -62, 11) };
    net_roam_bss_t s2[] = { bss(AP_B, -63, 6), bss(AP_C, -59, 11) };
    net_roam_scan_done(&r, s1, 2, t);
    t = run_until(&r, t + 1, t + 100, -80, false, NET_ROAM_SCAN);
    net_roam_scan_done(&r, s2, 2, t);
    TEST_ASSERT_EQUAL(NET_ROAM_GO, net_roam_tick(&r, false, t + 1));
    TEST_ASSERT_EQUAL_MEMORY(AP_C, r.cand.bssid, 6);
}

TEST_CASE("roaming: a scan that could not run is tried again a minute later", "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -78);
    uint32_t t = run_until(&r, 0, 1000, -78, false, NET_ROAM_SCAN);
    net_roam_scan_failed(&r, t);
    TEST_ASSERT_EQUAL_UINT32(t + NET_ROAM_RETRY_S, run_until(&r, t + 1, t + 1000, -78, false, NET_ROAM_SCAN));
}

TEST_CASE("roaming: a move the access point asked for holds the station's own moves and further requests back",
          "[net_roam]")
{
    net_roam_t r;
    start(&r, 0, -78);
    TEST_ASSERT_TRUE(net_roam_btm_open(&r, false, 10));
    TEST_ASSERT_FALSE(net_roam_btm_open(&r, true, 10));   /* an update, a flash, the AP up, a trial */
    net_roam_moved(&r, 100);
    net_roam_disassociated(&r);
    net_roam_associated(&r, AP_B, 103);
    TEST_ASSERT_FALSE(net_roam_btm_open(&r, false, 100 + NET_ROAM_BTM_GAP_S - 1));
    TEST_ASSERT_TRUE(net_roam_btm_open(&r, false, 100 + NET_ROAM_BTM_GAP_S));
    /* Weak at the new access point: no own scan before 30 min after that move. */
    TEST_ASSERT_EQUAL_UINT32(100 + CFG.min_interval_s, run_until(&r, 103, 5000, -78, false, NET_ROAM_SCAN));
}

TEST_CASE("roaming: a closed gate passes only requests that disconnect the station anyway", "[net_roam]")
{
    TEST_ASSERT_TRUE(net_roam_btm_pass(true, 0x00));
    TEST_ASSERT_TRUE(net_roam_btm_pass(true, 0x01));   /* candidate list only */
    TEST_ASSERT_FALSE(net_roam_btm_pass(false, 0x00));
    TEST_ASSERT_FALSE(net_roam_btm_pass(false, 0x01));
    TEST_ASSERT_FALSE(net_roam_btm_pass(false, 0x03));   /* candidate list, abridged */
    TEST_ASSERT_TRUE(net_roam_btm_pass(false, 0x05));    /* disassociation imminent */
    TEST_ASSERT_TRUE(net_roam_btm_pass(false, 0x08));    /* BSS termination */
    TEST_ASSERT_TRUE(net_roam_btm_pass(false, 0x10));    /* ESS disassociation imminent */
}

TEST_CASE("roaming: the average ignores unknown readings and rounds toward minus infinity", "[net_roam]")
{
    net_roam_t r;
    net_roam_init(&r, &CFG);
    net_roam_rssi(&r, -60);   /* not associated: ignored */
    TEST_ASSERT_FALSE(r.avg_ok);
    TEST_ASSERT_EQUAL_INT8(0, net_roam_avg_dbm(&r));
    net_roam_associated(&r, AP_A, 0);
    net_roam_rssi(&r, -128);
    net_roam_rssi(&r, 0);
    TEST_ASSERT_FALSE(r.avg_ok);
    net_roam_rssi(&r, -60);
    TEST_ASSERT_EQUAL_INT8(-60, net_roam_avg_dbm(&r));
    net_roam_rssi(&r, -80);   /* a quarter of the way: -65 */
    TEST_ASSERT_EQUAL_INT8(-65, net_roam_avg_dbm(&r));
    net_roam_rssi(&r, -66);   /* -65.25 */
    TEST_ASSERT_EQUAL_INT8(-66, net_roam_avg_dbm(&r));
    /* A new association starts the average again. */
    net_roam_associated(&r, AP_B, 10);
    TEST_ASSERT_FALSE(r.avg_ok);
}

TEST_CASE("roaming: the times survive the clock wrap", "[net_roam]")
{
    net_roam_t r;
    uint32_t t0 = UINT32_MAX - 100u;
    start(&r, t0, -78);
    uint32_t t = run_until(&r, t0, t0 + 1000u, -78, false, NET_ROAM_SCAN);
    TEST_ASSERT_EQUAL_UINT32(t0 + NET_ROAM_SETTLE_S, t);
    net_roam_bss_t seen[] = { bss(AP_B, -50, 6) };
    net_roam_scan_done(&r, seen, 1, t);
    t = run_until(&r, t + 1u, t + 100u, -78, false, NET_ROAM_SCAN);
    net_roam_scan_done(&r, seen, 1, t);
    TEST_ASSERT_EQUAL(NET_ROAM_GO, net_roam_tick(&r, false, t + 1u));
    TEST_ASSERT_FALSE(net_roam_btm_open(&r, false, t + 2u));
    TEST_ASSERT_TRUE(net_roam_btm_open(&r, false, t + 1u + NET_ROAM_BTM_GAP_S));
}

TEST_CASE("roaming: journal fields pack channels, who, the same access point, signals and seconds", "[net_roam]")
{
    TEST_ASSERT_EQUAL_HEX32(0x00010601u, (uint32_t)net_roam_rec_a(1, 6, NET_ROAM_BY_AP, false, false));
    TEST_ASSERT_EQUAL_HEX32(0x01020B0Bu, (uint32_t)net_roam_rec_a(11, 11, NET_ROAM_BY_STATION, true, false));
    /* Bit 25: the address and the connections were kept. */
    TEST_ASSERT_EQUAL_HEX32(0x02010B06u, (uint32_t)net_roam_rec_a(6, 11, NET_ROAM_BY_AP, false, true));
    TEST_ASSERT_EQUAL_HEX32(0x03030101u, (uint32_t)net_roam_rec_a(1, 1, NET_ROAM_BY_REQUEST, true, true));
    int32_t b = net_roam_rec_b(-78, -52, 4);
    TEST_ASSERT_EQUAL_INT8(-78, (int8_t)(b & 0xFF));
    TEST_ASSERT_EQUAL_INT8(-52, (int8_t)((b >> 8) & 0xFF));
    TEST_ASSERT_EQUAL_INT32(4, b >> 16);
    /* Unknown readings are 0; the seconds stop at 32767 (b stays positive). */
    b = net_roam_rec_b(-128, 5, 100000);
    TEST_ASSERT_EQUAL_INT32(0, b & 0xFFFF);
    TEST_ASSERT_EQUAL_INT32(32767, b >> 16);
    TEST_ASSERT_TRUE(b > 0);
}

/* ---- keeping the address across a move ------------------------------------------------- */

#define IP_A 0x3201A8C0u   /* 192.168.1.50 as lwIP holds it */
#define IP_B 0x1405A8C0u   /* 192.168.5.20 */
#define REASON_LEAVE 8u    /* the station's own esp_wifi_disconnect() */
#define REASON_BEACON 200u /* beacon timeout: an ordinary drop */

/* A hold with its gate open for IP_A. */
static void hold_open(net_hold_t *h)
{
    net_hold_init(h);
    net_hold_gate(h, true, IP_A);
}

TEST_CASE("keeping the address: a disconnect the supplicant reports as roaming is kept only with the gate open",
          "[net_roam]")
{
    net_hold_t h;
    /* Gate closed (credentials on trial, too little lease, no address): the interface goes down as always. */
    net_hold_init(&h);
    net_hold_gate(&h, false, IP_A);
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, NET_REASON_ROAMING, 1000));
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, h.state);
    /* Open, but without an address: closed. */
    net_hold_gate(&h, true, 0);
    TEST_ASSERT_FALSE(h.open);
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, NET_REASON_ROAMING, 1000));
    /* Open with an address: kept. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, NET_REASON_ROAMING, 1000));
    TEST_ASSERT_EQUAL(NET_HOLD_AWAY, h.state);
    TEST_ASSERT_EQUAL_HEX32(IP_A, h.kept_ip);
    /* An ordinary drop with the gate open is not a move: not kept. */
    hold_open(&h);
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, REASON_BEACON, 1000));
    hold_open(&h);
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, REASON_LEAVE, 1000));   /* a deliberate disconnect that was not armed */
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, h.state);
}

TEST_CASE("keeping the address: the station's own move is armed right before its disconnect, once, briefly",
          "[net_roam]")
{
    net_hold_t h;
    /* Armed: its disconnect is kept whatever the reason. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_arm(&h, 5000));
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, REASON_LEAVE, 5000 + NET_HOLD_ARM_MS));
    TEST_ASSERT_EQUAL(NET_HOLD_AWAY, h.state);
    /* Not within NET_HOLD_ARM_MS: an unrelated drop later is not kept. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_arm(&h, 5000));
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, REASON_LEAVE, 5000 + NET_HOLD_ARM_MS + 1u));
    /* The arming is used up by the first disconnect, kept or not. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_arm(&h, 5000));
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, REASON_LEAVE, 5100));
    TEST_ASSERT_FALSE(h.armed);
    /* Closed gate: nothing armed, and closing the gate disarms. */
    net_hold_init(&h);
    TEST_ASSERT_FALSE(net_hold_arm(&h, 5000));
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, REASON_LEAVE, 5100));
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_arm(&h, 5000));
    net_hold_gate(&h, false, IP_A);   /* e.g. changed credentials queued meanwhile */
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, REASON_LEAVE, 5100));
    /* Not while a hold runs. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, NET_REASON_ROAMING, 5000));
    net_hold_gate(&h, true, IP_A);
    TEST_ASSERT_FALSE(net_hold_arm(&h, 5100));
}

TEST_CASE("keeping the address: every disconnect closes the gate until the net task opens it again", "[net_roam]")
{
    net_hold_t h;
    hold_open(&h);
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, REASON_BEACON, 1000));
    TEST_ASSERT_FALSE(h.open);
    /* A roaming disconnect right after, before the gate was opened again (no association, no address): not kept. */
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, NET_REASON_ROAMING, 1100));
    /* A kept one closes it too: the next disconnect ends the hold instead of starting another. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, NET_REASON_ROAMING, 2000));
    TEST_ASSERT_FALSE(h.open);
}

TEST_CASE("keeping the address: the new association within NET_HOLD_AWAY_S is held, later the address is given up",
          "[net_roam]")
{
    net_hold_t h;
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, NET_REASON_ROAMING, 10000));
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, false, IP_A, 10000 + NET_HOLD_AWAY_S * 1000u - 1u));
    TEST_ASSERT_TRUE(net_hold_connected(&h, 10000 + NET_HOLD_AWAY_S * 1000u - 1u));
    TEST_ASSERT_EQUAL(NET_HOLD_BACK, h.state);
    TEST_ASSERT_EQUAL_UINT32(NET_HOLD_AWAY_S * 1000u - 1u, h.away_ms);
    /* Too long without a link: given up; the association that comes later is an ordinary one. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, NET_REASON_ROAMING, 10000));
    TEST_ASSERT_EQUAL(NET_HOLD_EXPIRED, net_hold_check(&h, false, IP_A, 10000 + NET_HOLD_AWAY_S * 1000u));
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, h.state);
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, false, IP_A, 30000));
    TEST_ASSERT_FALSE(net_hold_connected(&h, 30000));
    /* Without a hold an association is never held. */
    net_hold_init(&h);
    TEST_ASSERT_FALSE(net_hold_connected(&h, 1000));
}

/* A hold back on a new link at t (kept at t - 1500 ms). */
static void hold_back(net_hold_t *h, uint32_t t)
{
    hold_open(h);
    TEST_ASSERT_TRUE(net_hold_disconnect(h, NET_REASON_ROAMING, t - 1500u));
    TEST_ASSERT_TRUE(net_hold_connected(h, t));
}

TEST_CASE("keeping the address: DHCP on the new link confirms it, changes it, or does not answer", "[net_roam]")
{
    net_hold_t h;
    /* Until DHCP was asked its state is the one from before the move: a bound state confirms nothing. */
    hold_back(&h, 20000);
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, true, IP_A, 20100));
    net_hold_verify(&h, 20200);
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, false, IP_A, 21000));   /* asking (INIT-REBOOT) */
    TEST_ASSERT_EQUAL(NET_HOLD_CONFIRMED, net_hold_check(&h, true, IP_A, 21200));
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, h.state);
    /* A NAK clears the address; another network hands out another one: the address is lost. */
    hold_back(&h, 20000);
    net_hold_verify(&h, 20000);
    TEST_ASSERT_EQUAL(NET_HOLD_LOST, net_hold_check(&h, false, 0, 20500));
    hold_back(&h, 20000);
    net_hold_verify(&h, 20000);
    TEST_ASSERT_EQUAL(NET_HOLD_LOST, net_hold_check(&h, true, IP_B, 24000));
    /* Lost even before DHCP was asked. */
    hold_back(&h, 20000);
    TEST_ASSERT_EQUAL(NET_HOLD_LOST, net_hold_check(&h, true, 0, 20100));
    /* No answer: kept, unconfirmed, NET_HOLD_VERIFY_S after asking. */
    hold_back(&h, 20000);
    net_hold_verify(&h, 20300);
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, false, IP_A, 20300 + NET_HOLD_VERIFY_S * 1000u - 1u));
    TEST_ASSERT_EQUAL(NET_HOLD_UNCONFIRMED, net_hold_check(&h, false, IP_A, 20300 + NET_HOLD_VERIFY_S * 1000u));
    /* Never asked (the request could not be made): the same wait from the association. */
    hold_back(&h, 20000);
    TEST_ASSERT_EQUAL(NET_HOLD_UNCONFIRMED, net_hold_check(&h, true, IP_A, 20000 + NET_HOLD_VERIFY_S * 1000u));
    /* Asking is only for a hold back on a link. */
    hold_open(&h);
    net_hold_verify(&h, 1000);
    TEST_ASSERT_FALSE(h.verifying);
}

TEST_CASE("keeping the address: a disconnect during the move ends the hold and is not kept", "[net_roam]")
{
    net_hold_t h;
    /* The connect to the new access point failed. */
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, NET_REASON_ROAMING, 1000));
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, 15u, 2000));
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, h.state);
    TEST_ASSERT_FALSE(net_hold_connected(&h, 2500));
    /* The new link dropped while DHCP was asked, even with reason roaming and the gate opened again. */
    hold_back(&h, 5000);
    net_hold_verify(&h, 5000);
    net_hold_gate(&h, true, IP_A);
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, NET_REASON_ROAMING, 6000));
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, h.state);
    TEST_ASSERT_FALSE(h.verifying);
}

TEST_CASE("keeping the address: cut short by something else, and the lease it needs", "[net_roam]")
{
    net_hold_t h;
    hold_open(&h);
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, net_hold_end(&h));
    TEST_ASSERT_FALSE(h.open);
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, NET_REASON_ROAMING, 1000));
    TEST_ASSERT_EQUAL(NET_HOLD_AWAY, net_hold_end(&h));
    TEST_ASSERT_EQUAL(NET_HOLD_IDLE, h.state);
    TEST_ASSERT_FALSE(net_hold_connected(&h, 1200));   /* the association after it is an ordinary one */
    hold_back(&h, 3000);
    TEST_ASSERT_EQUAL(NET_HOLD_BACK, net_hold_end(&h));
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, true, IP_A, 4000));
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_arm(&h, 1000));
    (void)net_hold_end(&h);
    TEST_ASSERT_FALSE(net_hold_disconnect(&h, REASON_LEAVE, 1100));
    /* At least a minute of the lease left, bound or renewing (not past the rebind time). */
    TEST_ASSERT_TRUE(net_hold_lease_ok(true, NET_HOLD_LEASE_MIN_S));
    TEST_ASSERT_FALSE(net_hold_lease_ok(true, NET_HOLD_LEASE_MIN_S - 1u));
    TEST_ASSERT_FALSE(net_hold_lease_ok(false, 86400));
}

TEST_CASE("keeping the address: its times survive the clock wrap", "[net_roam]")
{
    net_hold_t h;
    uint32_t t0 = UINT32_MAX - 1000u;
    hold_open(&h);
    TEST_ASSERT_TRUE(net_hold_arm(&h, t0));
    TEST_ASSERT_TRUE(net_hold_disconnect(&h, REASON_LEAVE, t0 + 2000u));   /* across the wrap, within the arming */
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, false, IP_A, t0 + 2000u + NET_HOLD_AWAY_S * 1000u - 1u));
    TEST_ASSERT_EQUAL(NET_HOLD_EXPIRED, net_hold_check(&h, false, IP_A, t0 + 2000u + NET_HOLD_AWAY_S * 1000u));
    hold_back(&h, t0);
    net_hold_verify(&h, t0 + 500u);
    TEST_ASSERT_EQUAL(NET_HOLD_WAIT, net_hold_check(&h, false, IP_A, t0 + 2000u));
    TEST_ASSERT_EQUAL(NET_HOLD_UNCONFIRMED, net_hold_check(&h, false, IP_A, t0 + 500u + NET_HOLD_VERIFY_S * 1000u));
}
