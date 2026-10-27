/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <string.h>
#include "monitor_diag_core.h"
#include "web_build.h"
#include "cJSON.h"
#include "unity.h"

static monitor_diag_sample_t ring[MONITOR_DIAG_CAPACITY], page[MONITOR_DIAG_PAGE];
static const uint32_t addresses[3] = {1, 2, 0};
static mon_diag_t d;

TEST_CASE("diagnostic probes: overlapping cycles and deadline preserve loss evidence", "[monitor]")
{
    mon_diag_init(&d, ring, MONITOR_DIAG_CAPACITY);
    mon_diag_start(&d, 1000000);
    TEST_ASSERT_EQUAL_UINT32(1, mon_diag_begin(&d, 1000000, 0, addresses, -71));
    TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 1, 0, 1000000));
    TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 1, 1, 1000000));
    TEST_ASSERT_FALSE(mon_diag_reply(&d, d.generation + 1, 1, 0, 1, 1100000));
    TEST_ASSERT_FALSE(mon_diag_reply(&d, d.generation, 1, 0, 3, 1100000));
    TEST_ASSERT_TRUE(mon_diag_reply(&d, d.generation, 1, 0, 1, 1100000));
    TEST_ASSERT_FALSE(mon_diag_reply(&d, d.generation, 1, 0, 1, 1200000));
    TEST_ASSERT_EQUAL_UINT32(2, mon_diag_begin(&d, 2000000, 0, addresses, -71));
    TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 2, 0, 2000000));
    TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 2, 1, 2000000));
    TEST_ASSERT_EQUAL(MONITOR_DIAG_PENDING, ring[0].ping[1].result);
    mon_diag_expire(&d, 3000000);
    TEST_ASSERT_EQUAL(MONITOR_DIAG_TIMEOUT, ring[0].ping[1].result);
    TEST_ASSERT_FALSE(mon_diag_reply(&d, d.generation, 1, 1, 2, 3000000));
    TEST_ASSERT_EQUAL_UINT32(1, d.late);
    TEST_ASSERT_EQUAL_UINT32(1, d.duplicates);
    TEST_ASSERT_EQUAL_UINT16(100, ring[0].ping[0].rtt_ms);
    TEST_ASSERT_EQUAL(MONITOR_DIAG_NO_TARGET, ring[0].ping[2].result);
    mon_diag_error(&d, d.generation, 2, 0, ENOMEM);
    TEST_ASSERT_EQUAL(MONITOR_DIAG_LOCAL_ERROR, ring[1].ping[0].result);
    TEST_ASSERT_EQUAL_INT(ENOMEM, ring[1].ping[0].error);
    mon_diag_stop(&d);
    TEST_ASSERT_EQUAL(MONITOR_DIAG_CANCELLED, ring[1].ping[1].result);
    TEST_ASSERT_FALSE(mon_diag_reply(&d, d.generation, 2, 1, 2, 9000000));
    TEST_ASSERT_EQUAL_UINT32(1, d.late);
}

TEST_CASE("diagnostic probes: no catch-up burst, hard expiry, generation and bounded pagination", "[monitor]")
{
    mon_diag_init(&d, ring, MONITOR_DIAG_CAPACITY);
    mon_diag_start(&d, 1000000);
    for (uint32_t i = 0; i < MONITOR_DIAG_DURATION_S; i++) {
        TEST_ASSERT_EQUAL_UINT32(i + 1, mon_diag_begin(&d, 1000000ull + i * 1000000ull, 0, addresses, 0));
        TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, i + 1, 0, 1000000ull + i * 1000000ull));
        TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, i + 1, 1, 1000000ull + i * 1000000ull));
    }
    TEST_ASSERT_EQUAL_UINT32(0, mon_diag_begin(&d, 601000000, 0, addresses, 0));
    TEST_ASSERT_FALSE(d.active);
    mon_diag_expire(&d, 602000000);
    TEST_ASSERT_EQUAL(MONITOR_DIAG_TIMEOUT, ring[599].ping[0].result);
    TEST_ASSERT_EQUAL_size_t(32, mon_diag_copy(&d, 0, page, 32));
    TEST_ASSERT_EQUAL_UINT32(1, page[0].seq);
    TEST_ASSERT_EQUAL_UINT32(32, page[31].seq);
    TEST_ASSERT_EQUAL_size_t(8, mon_diag_copy(&d, 592, page, 32));
    TEST_ASSERT_EQUAL_UINT32(600, page[7].seq);
    TEST_ASSERT_EQUAL_size_t(0, mon_diag_copy(&d, UINT32_MAX, page, 32));
    uint32_t old = d.generation;
    mon_diag_start(&d, 700000000);
    TEST_ASSERT_EQUAL_UINT32(old + 1, d.generation);
    TEST_ASSERT_EQUAL_UINT32(1, mon_diag_begin(&d, 705000000, 0, addresses, 0));
    TEST_ASSERT_EQUAL_UINT32(5, d.skipped);
    TEST_ASSERT_FALSE(mon_diag_due(&d, 705500000));
    TEST_ASSERT_FALSE(mon_diag_reply(&d, old, 1, 0, 1, 705100000));
    TEST_ASSERT_EQUAL(MONITOR_DIAG_PENDING, ring[0].ping[0].result);
}

TEST_CASE("diagnostic probes: each target deadline starts at dispatch and unsent work is a local failure", "[monitor]")
{
    static const uint32_t all[3] = {1, 2, 3};
    mon_diag_init(&d, ring, MONITOR_DIAG_CAPACITY);
    mon_diag_start(&d, 1000000);
    TEST_ASSERT_EQUAL_UINT32(1, mon_diag_begin(&d, 1000000, 0, all, 0));
    TEST_ASSERT_FALSE(mon_diag_reply(&d, d.generation, 1, 2, 3, 1500000));   /* never dispatched */
    TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 1, 0, 1000000));
    TEST_ASSERT_EQUAL_UINT32(2, mon_diag_begin(&d, 2000000, 0, all, 0));
    TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 1, 2, 2900000));
    TEST_ASSERT_FALSE(mon_diag_sent(&d, d.generation, 1, 2, 2950000));   /* no second dispatch */
    TEST_ASSERT_EQUAL_UINT32(3, mon_diag_begin(&d, 3000000, 0, all, 0));
    TEST_ASSERT_EQUAL(MONITOR_DIAG_TIMEOUT, ring[0].ping[0].result);
    TEST_ASSERT_EQUAL(MONITOR_DIAG_LOCAL_ERROR, ring[0].ping[1].result);
    TEST_ASSERT_EQUAL_INT(ETIMEDOUT, ring[0].ping[1].error);
    TEST_ASSERT_EQUAL_UINT64(0, ring[0].ping[1].sent_us);
    TEST_ASSERT_EQUAL(MONITOR_DIAG_PENDING, ring[0].ping[2].result);
    TEST_ASSERT_EQUAL_UINT32(4, mon_diag_begin(&d, 4000000, 0, all, 0));
    TEST_ASSERT_TRUE(mon_diag_reply(&d, d.generation, 1, 2, 3, 4800000));
    TEST_ASSERT_EQUAL_UINT16(1900, ring[0].ping[2].rtt_ms);
    TEST_ASSERT_EQUAL_UINT64(2900000, ring[0].ping[2].sent_us);
    TEST_ASSERT_FALSE(mon_diag_sent(&d, d.generation, 2, 1, 4800000));   /* expired dispatch deadline */
}

TEST_CASE("diagnostic probes: fractional scheduler delay cannot trigger a catch-up pair", "[monitor]")
{
    mon_diag_init(&d, ring, MONITOR_DIAG_CAPACITY);
    mon_diag_start(&d, 1000000);
    TEST_ASSERT_EQUAL_UINT32(1, mon_diag_begin(&d, 1900000, 0, addresses, 0));
    TEST_ASSERT_EQUAL_UINT32(0, mon_diag_begin(&d, 2000000, 0, addresses, 0));
    TEST_ASSERT_FALSE(mon_diag_due(&d, 2899999));
    TEST_ASSERT_EQUAL_UINT32(2, mon_diag_begin(&d, 2900000, 0, addresses, 0));
    TEST_ASSERT_EQUAL_UINT32(3, mon_diag_begin(&d, 5600000, 0, addresses, 0));
    TEST_ASSERT_EQUAL_UINT32(1, d.skipped);
    TEST_ASSERT_EQUAL_UINT32(0, mon_diag_begin(&d, 6000000, 0, addresses, 0));
    TEST_ASSERT_EQUAL_UINT64(6600000, d.next_us);
}

TEST_CASE("diagnostic probes: stop and socket receive failure cover late-dispatched outstanding cycles", "[monitor]")
{
    for (unsigned stop = 0; stop < 2; stop++) {
        mon_diag_init(&d, ring, MONITOR_DIAG_CAPACITY);
        mon_diag_start(&d, 1000000);
        TEST_ASSERT_EQUAL_UINT32(1, mon_diag_begin(&d, 1000000, 0, addresses, 0));
        TEST_ASSERT_EQUAL_UINT32(2, mon_diag_begin(&d, 2000000, 0, addresses, 0));
        TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 1, 1, 2900000));
        TEST_ASSERT_EQUAL_UINT32(3, mon_diag_begin(&d, 3000000, 0, addresses, 0));
        TEST_ASSERT_EQUAL_UINT32(4, mon_diag_begin(&d, 4000000, 0, addresses, 0));
        TEST_ASSERT_TRUE(mon_diag_sent(&d, d.generation, 4, 0, 4000000));
        TEST_ASSERT_TRUE(mon_diag_reply(&d, d.generation, 4, 0, 1, 4050000));
        mon_diag_receive_error(&d, d.generation + 1, ENOMEM);
        TEST_ASSERT_EQUAL(MONITOR_DIAG_PENDING, ring[0].ping[1].result);
        if (stop) mon_diag_stop(&d);
        else mon_diag_receive_error(&d, d.generation, ENOMEM);
        TEST_ASSERT_EQUAL(stop ? MONITOR_DIAG_CANCELLED : MONITOR_DIAG_LOCAL_ERROR, ring[0].ping[1].result);
        TEST_ASSERT_EQUAL(stop ? MONITOR_DIAG_CANCELLED : MONITOR_DIAG_LOCAL_ERROR, ring[3].ping[1].result);
        TEST_ASSERT_EQUAL(MONITOR_DIAG_OK, ring[3].ping[0].result);
        TEST_ASSERT_EQUAL(MONITOR_DIAG_NO_TARGET, ring[3].ping[2].result);
        if (!stop) TEST_ASSERT_EQUAL_INT(ENOMEM, ring[0].ping[1].error);
        TEST_ASSERT_FALSE(mon_diag_reply(&d, d.generation, 1, 1, 2, 4500000));
    }
}

TEST_CASE("diagnostic JSON: unavailable evidence is null and TCP acceptance stays distinct", "[web][monitor]")
{
    static char buf[10000];
    monitor_diag_status_t st = {.generation = 1, .last_seq = 1};
    monitor_diag_sample_t s = {.seq = 1, .rssi = 0};
    s.ping[0].result = MONITOR_DIAG_TIMEOUT;
    s.ping[1].result = MONITOR_DIAG_OK;
    s.ping[1].rtt_ms = 0;
    ntrip_tx_diag_t tx = {.generation = 3};
    tx.tcp.sample_duration_us = 501;
    tx.tcp_sampling.samples = 2;
    tx.tcp_sampling.total_us = 551;
    tx.tcp_sampling.max_us = 501;
    tx.tcp_sampling.last_us = 50;
    tx.tcp_sampling.histogram[0] = 1;
    tx.tcp_sampling.histogram[2] = 1;
    tx.write.tcp_at_end.sample_duration_us = 50;
    tx.write.accepted_bytes = 14;
    tx.write.arrivals.live.frames = 2;
    tx.write.arrivals.live.timed_frames = 1;
    tx.write.arrivals.live.oldest_rx_ms = 42;
    ntrip_tx_write_diag_t hist[1][NTRIP_TX_DIAG_HISTORY] = {0};
    size_t counts[1] = {0};
    int history_error[1] = {ESP_ERR_TIMEOUT};
    wj_t w;
    wj_init(&w, buf, sizeof(buf), NULL, NULL);
    web_build_monitor_diag(&w, &st, &s, 1, &tx, hist, counts, history_error, 1);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *j = cJSON_Parse(wj_text(&w));
    TEST_ASSERT_NOT_NULL(j);
    cJSON *sample = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "samples"), 0);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(sample, "rssi")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(sample, "time_unix")));
    cJSON *ping = cJSON_GetObjectItem(sample, "ping");
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetArrayItem(ping, 0), "rtt_ms")));
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetObjectItem(cJSON_GetArrayItem(ping, 1), "rtt_ms")->valueint);
    cJSON *t = cJSON_GetArrayItem(cJSON_GetObjectItem(j, "transmit"), 0);
    cJSON *tcp = cJSON_GetObjectItem(t, "tcp");
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(t, "history_available")));
    TEST_ASSERT_EQUAL_INT(ESP_ERR_TIMEOUT, cJSON_GetObjectItem(t, "history_error")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(tcp, "state")));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(tcp, "acked_delta_bytes")));
    TEST_ASSERT_EQUAL_INT(501, cJSON_GetObjectItem(tcp, "sample_duration_us")->valueint);
    cJSON *cost = cJSON_GetObjectItem(t, "tcp_sampling");
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetObjectItem(cost, "samples")->valueint);
    TEST_ASSERT_EQUAL_INT(551, cJSON_GetObjectItem(cost, "total_us")->valueint);
    TEST_ASSERT_EQUAL_INT(501, cJSON_GetObjectItem(cost, "max_us")->valueint);
    TEST_ASSERT_EQUAL_INT(50, cJSON_GetObjectItem(cost, "last_us")->valueint);
    TEST_ASSERT_FALSE(cJSON_IsTrue(cJSON_GetObjectItem(cost, "saturated")));
    cJSON *buckets = cJSON_GetObjectItem(cost, "histogram");
    TEST_ASSERT_EQUAL_INT(8, cJSON_GetArraySize(buckets));
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArrayItem(buckets, 0)->valueint);
    TEST_ASSERT_EQUAL_INT(0, cJSON_GetArrayItem(buckets, 1)->valueint);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArrayItem(buckets, 2)->valueint);
    cJSON *wr = cJSON_GetObjectItem(t, "write");
    TEST_ASSERT_EQUAL_INT(14, cJSON_GetObjectItem(wr, "accepted_bytes")->valueint);
    TEST_ASSERT_EQUAL_INT(50, cJSON_GetObjectItem(cJSON_GetObjectItem(wr, "tcp_at_end"), "sample_duration_us")->valueint);
    TEST_ASSERT_EQUAL_INT(42, cJSON_GetObjectItem(cJSON_GetObjectItem(wr, "live"), "oldest_rx_ms")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(cJSON_GetObjectItem(wr, "sticky"), "oldest_rx_ms")));
    cJSON_Delete(j);
}
