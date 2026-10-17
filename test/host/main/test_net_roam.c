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

TEST_CASE("roaming: one access point and a weak signal scan once per 6 h, and never move", "[net_roam]")
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

TEST_CASE("roaming: a candidate below the gain does not count; mesh seen means a scan every 10 min", "[net_roam]")
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
    TEST_ASSERT_EQUAL_HEX32(0x00010601u, (uint32_t)net_roam_rec_a(1, 6, NET_ROAM_BY_AP, false));
    TEST_ASSERT_EQUAL_HEX32(0x01020B0Bu, (uint32_t)net_roam_rec_a(11, 11, NET_ROAM_BY_STATION, true));
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
