/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "monitor_core.h"
#include "unity.h"

static monitor_bucket_t s_ring[MONITOR_BUCKETS];
static monitor_bucket_t s_out[MONITOR_BUCKETS];
static mon_core_t m;

static monitor_sample_t sample(uint16_t router_ms, uint16_t server_ms, int8_t rssi, uint8_t cpu, uint32_t mem)
{
    monitor_sample_t s;
    memset(&s, 0, sizeof(s));
    s.router = MONITOR_PING_OK;
    s.router_ms = router_ms;
    s.server = MONITOR_PING_OK;
    s.server_ms = server_ms;
    s.rssi_known = true;
    s.rssi = rssi;
    s.cpu_known = true;
    s.cpu_pct = cpu;
    s.mem_free = mem;
    return s;
}

TEST_CASE("monitor: samples fill buckets of ten, oldest first", "[monitor]")
{
    mon_init(&m, s_ring, MONITOR_BUCKETS);
    monitor_status_t st;
    mon_summary(&m, &st);
    TEST_ASSERT_FALSE(st.have_last);
    TEST_ASSERT_EQUAL_UINT32(0, st.span_s);
    TEST_ASSERT_FALSE(st.rssi_known);
    TEST_ASSERT_FALSE(st.cpu_known);
    TEST_ASSERT_FALSE(st.mem_known);
    TEST_ASSERT_EQUAL_size_t(0, mon_series(&m, s_out, MONITOR_BUCKETS));

    for (uint16_t i = 0; i < MONITOR_BUCKET_SAMPLES + 3; i++) {
        monitor_sample_t s = sample((uint16_t)(i + 1), 100, -70, 10, 100000);
        mon_add(&m, &s);
    }
    size_t n = mon_series(&m, s_out, MONITOR_BUCKETS);
    TEST_ASSERT_EQUAL_size_t(2, n);
    TEST_ASSERT_EQUAL_UINT16(MONITOR_BUCKET_SAMPLES, s_out[0].samples);
    TEST_ASSERT_EQUAL_UINT16(10, s_out[0].router.max_ms);      /* samples 1..10 */
    TEST_ASSERT_EQUAL_UINT16(3, s_out[1].samples);              /* the current one */
    TEST_ASSERT_EQUAL_UINT16(13, s_out[1].router.max_ms);
    mon_summary(&m, &st);
    TEST_ASSERT_TRUE(st.have_last);
    TEST_ASSERT_EQUAL_UINT16(13, st.last.router_ms);
    TEST_ASSERT_EQUAL_UINT32(13 * MONITOR_SAMPLE_S, st.span_s);
    TEST_ASSERT_EQUAL_UINT32(7, st.router.avg_ms);             /* 1..13: 91 / 13 */
    TEST_ASSERT_EQUAL_UINT32(13, st.router.max_ms);

    /* At most `max`: the newest. */
    TEST_ASSERT_EQUAL_size_t(1, mon_series(&m, s_out, 1));
    TEST_ASSERT_EQUAL_UINT16(3, s_out[0].samples);
}

TEST_CASE("monitor: pings without a target are no sample, unanswered ones are lost", "[monitor]")
{
    mon_init(&m, s_ring, MONITOR_BUCKETS);
    monitor_sample_t s = sample(4, 120, -60, 5, 90000);
    mon_add(&m, &s);
    s.router_ms = 8;
    s.server = MONITOR_PING_LOST;
    mon_add(&m, &s);
    s.router = MONITOR_PING_NONE;   /* offline: neither sent nor lost */
    s.server = MONITOR_PING_NONE;
    mon_add(&m, &s);
    monitor_status_t st;
    mon_summary(&m, &st);
    TEST_ASSERT_EQUAL_UINT32(2, st.router.sent);
    TEST_ASSERT_EQUAL_UINT32(0, st.router.lost);
    TEST_ASSERT_EQUAL_UINT32(6, st.router.avg_ms);
    TEST_ASSERT_EQUAL_UINT32(8, st.router.max_ms);
    TEST_ASSERT_EQUAL_UINT32(2, st.server.sent);
    TEST_ASSERT_EQUAL_UINT32(1, st.server.lost);
    TEST_ASSERT_EQUAL_UINT32(120, st.server.avg_ms);
    TEST_ASSERT_EQUAL_UINT32(3 * MONITOR_SAMPLE_S, st.span_s);  /* the time still counts */

    /* Every one lost: no average, no maximum. */
    mon_init(&m, s_ring, MONITOR_BUCKETS);
    s.server = MONITOR_PING_LOST;
    mon_add(&m, &s);
    mon_summary(&m, &st);
    TEST_ASSERT_EQUAL_UINT32(1, st.server.sent);
    TEST_ASSERT_EQUAL_UINT32(1, st.server.lost);
    TEST_ASSERT_EQUAL_UINT32(0, st.server.avg_ms);
    TEST_ASSERT_EQUAL_UINT32(0, st.server.max_ms);
}

TEST_CASE("monitor: signal, CPU and memory over the 24 h", "[monitor]")
{
    mon_init(&m, s_ring, MONITOR_BUCKETS);
    monitor_sample_t s = sample(3, 100, -78, 20, 120000);
    mon_add(&m, &s);
    s.rssi = -79;                    /* average -78.5: -79 (half away from zero) */
    s.cpu_pct = 41;
    s.mem_free = 96000;
    mon_add(&m, &s);
    s.rssi_known = false;            /* not associated: no signal sample */
    s.cpu_known = false;
    s.mem_free = 0;                  /* none read */
    mon_add(&m, &s);
    monitor_status_t st;
    mon_summary(&m, &st);
    TEST_ASSERT_TRUE(st.rssi_known);
    TEST_ASSERT_EQUAL_INT8(-79, st.rssi_avg);
    TEST_ASSERT_EQUAL_INT8(-79, st.rssi_min);
    TEST_ASSERT_TRUE(st.cpu_known);
    TEST_ASSERT_EQUAL_UINT8(31, st.cpu_avg);                    /* (20 + 41) / 2, rounded */
    TEST_ASSERT_EQUAL_UINT8(41, st.cpu_max);
    TEST_ASSERT_TRUE(st.mem_known);
    TEST_ASSERT_EQUAL_UINT32(96000, st.mem_min);
    TEST_ASSERT_FALSE(st.last.rssi_known);                      /* the latest is the latest, unknowns included */
}

TEST_CASE("monitor: past 24 h the oldest bucket gives way", "[monitor]")
{
    mon_init(&m, s_ring, MONITOR_BUCKETS);
    /* A full day plus 25 samples: bucket k's samples ping k + 1 ms. */
    uint32_t total = MONITOR_BUCKETS * MONITOR_BUCKET_SAMPLES + 25;
    for (uint32_t i = 0; i < total; i++) {
        monitor_sample_t s = sample((uint16_t)(i / MONITOR_BUCKET_SAMPLES + 1), 100, -70, 10, 100000);
        mon_add(&m, &s);
    }
    size_t n = mon_series(&m, s_out, MONITOR_BUCKETS);
    TEST_ASSERT_EQUAL_size_t(MONITOR_BUCKETS, n);
    /* Buckets 0..2 gave way: the oldest kept is bucket 3, the newest bucket 290 (5 samples so far). */
    TEST_ASSERT_EQUAL_UINT16(4, s_out[0].router.max_ms);
    TEST_ASSERT_EQUAL_UINT16(MONITOR_BUCKET_SAMPLES, s_out[0].samples);
    TEST_ASSERT_EQUAL_UINT16(291, s_out[n - 1].router.max_ms);
    TEST_ASSERT_EQUAL_UINT16(5, s_out[n - 1].samples);
    for (size_t i = 1; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT16(s_out[i - 1].router.max_ms + 1, s_out[i].router.max_ms);
    }
    monitor_status_t st;
    mon_summary(&m, &st);
    /* 287 full buckets and the current one's 5 samples: just under 24 h. */
    uint32_t kept = (MONITOR_BUCKETS - 1) * MONITOR_BUCKET_SAMPLES + 5;
    TEST_ASSERT_EQUAL_UINT32(kept, st.router.sent);
    TEST_ASSERT_EQUAL_UINT32(kept * MONITOR_SAMPLE_S, st.span_s);
}
