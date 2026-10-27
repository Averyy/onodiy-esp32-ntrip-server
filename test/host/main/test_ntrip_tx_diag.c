/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>

#include "fixture_util.h"
#include "ntrip_tx_diag_core.h"
#include "unity.h"

TEST_CASE("sender diagnostics: pending partial write has its own clock and completed history", "[ntrip_diag]")
{
    size_t bytes = 0;
    uint8_t *frame = fx_read("rtcm/go-gnss_1077_frame.bin", &bytes);
    ntrip_tx_diag_core_t *c = calloc(1, sizeof(*c));
    TEST_ASSERT_NOT_NULL(c);
    ntrip_tx_diag_connection(c, 4);
    rtcm_hub_read_meta_t meta = { .observations = {
        .frames = 1, .timed_frames = 1, .exact_frames = 1, .oldest_rx_ms = 900, .newest_rx_ms = 900,
    } };
    ntrip_tx_diag_begin(c, 1000000, 1800000000, NTRIP_TX_PHASE_RTCM, false, bytes, &meta);
    size_t first = bytes / 2u;
    ntrip_tx_diag_progress(c, 1000000, 1005000, first);
    ntrip_tx_diag_t out;
    ntrip_tx_diag_snapshot(c, 1600000, 1800000001, &out);
    TEST_ASSERT_TRUE(out.write.active);
    TEST_ASSERT_TRUE(out.write.slow);
    TEST_ASSERT_EQUAL_UINT32(600, out.write.duration_ms);
    TEST_ASSERT_EQUAL_UINT32(595, out.write.max_no_progress_ms);
    TEST_ASSERT_EQUAL_UINT32(first, out.write.accepted_bytes);
    TEST_ASSERT_EQUAL_UINT32(bytes - first, out.write.remaining_bytes);
    TEST_ASSERT_EQUAL_UINT64(1005000, out.write.updated_us);
    TEST_ASSERT_EQUAL_UINT64(900, out.write.arrivals.observations.oldest_rx_ms);
    TEST_ASSERT_EQUAL_UINT32(1, out.slow_writes);
    TEST_ASSERT_EQUAL_UINT32(0, c->status.slow_writes);   /* a getter never changes the core */
    net_tcp_snapshot_t tcp = { .ack_seq = 42, .outstanding_bytes = 700 };
    ntrip_tx_diag_tcp_sample(c, 1600000, 173, &tcp, 0);
    ntrip_tx_diag_progress(c, 1600000, 1650000, bytes - first);
    ntrip_tx_diag_end(c, 1650000, NTRIP_TX_OUTCOME_DONE);
    ntrip_tx_diag_snapshot(c, 9000000, 1800000008, &out);
    TEST_ASSERT_FALSE(out.write.active);
    TEST_ASSERT_EQUAL_UINT32(650, out.write.duration_ms);   /* completed age freezes */
    TEST_ASSERT_EQUAL_UINT32(645, out.write.max_no_progress_ms);
    TEST_ASSERT_EQUAL_UINT32(50, out.write.max_send_call_ms);
    TEST_ASSERT_EQUAL_UINT32(1, out.slow_writes);
    TEST_ASSERT_EQUAL_UINT32(1, out.history_count);
    TEST_ASSERT_EQUAL_UINT32(4, c->history[0].generation);
    TEST_ASSERT_EQUAL_UINT32(0, c->history[0].remaining_bytes);
    ntrip_tx_diag_connection(c, 5);
    TEST_ASSERT_FALSE(c->status.tcp.available);
    TEST_ASSERT_TRUE(c->history[0].tcp_at_end.available);
    TEST_ASSERT_EQUAL_UINT64(1600000, c->history[0].tcp_at_end.sampled_us);
    TEST_ASSERT_EQUAL_UINT32(4, c->history[0].tcp_at_end.generation);
    TEST_ASSERT_EQUAL_UINT32(700, c->history[0].tcp_at_end.state.outstanding_bytes);
    TEST_ASSERT_EQUAL_UINT64(173, c->history[0].tcp_at_end.sample_duration_us);
    TEST_ASSERT_EQUAL_UINT64(1, c->status.tcp_sampling.samples);
    TEST_ASSERT_EQUAL_UINT64(173, c->status.tcp_sampling.total_us);
    free(c);
    free(frame);
}

TEST_CASE("sender diagnostics: bounded history retains slow and failed writes across connections", "[ntrip_diag]")
{
    ntrip_tx_diag_core_t *c = calloc(1, sizeof(*c));
    TEST_ASSERT_NOT_NULL(c);
    for (unsigned i = 0; i < NTRIP_TX_DIAG_HISTORY + 3u; i++) {
        ntrip_tx_diag_connection(c, i + 1u);
        uint64_t now = (uint64_t)i * 2000000u;
        ntrip_tx_diag_begin(c, now, 0, NTRIP_TX_PHASE_REQUEST, true, 100, NULL);
        ntrip_tx_diag_progress(c, now, now + 2000u, 40);
        ntrip_tx_diag_end(c, now + 4000u, NTRIP_TX_OUTCOME_ERROR);
    }
    TEST_ASSERT_EQUAL_UINT32(NTRIP_TX_DIAG_HISTORY, c->status.history_count);
    TEST_ASSERT_EQUAL_UINT32(3, c->status.history_overwritten);
    TEST_ASSERT_EQUAL_UINT32(0, c->status.slow_writes);
    TEST_ASSERT_EQUAL_UINT32(4, c->history[c->history_next].generation);
    TEST_ASSERT_EQUAL_UINT32(60, c->history[c->history_next].remaining_bytes);
    TEST_ASSERT_EQUAL_INT(NTRIP_TX_OUTCOME_ERROR, c->history[c->history_next].outcome);
    /* A fast success changes the live last-write view, not the event ring. */
    ntrip_tx_diag_begin(c, 30000000, 0, NTRIP_TX_PHASE_TERMINATOR, false, 5, NULL);
    ntrip_tx_diag_progress(c, 30000000, 30001000, 5);
    ntrip_tx_diag_end(c, 30001000, NTRIP_TX_OUTCOME_DONE);
    TEST_ASSERT_EQUAL_UINT32(3, c->status.history_overwritten);
    TEST_ASSERT_EQUAL_UINT32(0, c->status.write.arrivals.observations.frames);
    free(c);
}

TEST_CASE("sender diagnostics: ACK deltas wrap and unavailable/new connections reset the baseline", "[ntrip_diag]")
{
    ntrip_tx_diag_core_t *c = calloc(1, sizeof(*c));
    TEST_ASSERT_NOT_NULL(c);
    ntrip_tx_diag_connection(c, 1);
    net_tcp_snapshot_t tcp = { .ack_seq = UINT32_MAX - 9u, .outstanding_bytes = 2048 };
    ntrip_tx_diag_tcp_sample(c, 1000000, 60, &tcp, 0);
    TEST_ASSERT_TRUE(c->status.tcp.available);
    TEST_ASSERT_FALSE(c->status.tcp.delta_valid);
    tcp.ack_seq = 10;
    ntrip_tx_diag_tcp_sample(c, 2000000, 80, &tcp, 0);
    TEST_ASSERT_TRUE(c->status.tcp.delta_valid);
    TEST_ASSERT_EQUAL_UINT32(20, c->status.tcp.acked_delta_bytes);
    TEST_ASSERT_EQUAL_UINT64(2000000, c->status.tcp.last_ack_progress_us);
    ntrip_tx_diag_tcp_sample(c, 3000000, 90, NULL, 22);
    TEST_ASSERT_FALSE(c->status.tcp.available);
    TEST_ASSERT_FALSE(c->status.tcp.delta_valid);
    TEST_ASSERT_EQUAL_INT(22, c->status.tcp.error);
    TEST_ASSERT_EQUAL_UINT64(0, c->status.tcp.last_ack_progress_us);
    TEST_ASSERT_EQUAL_UINT64(90, c->status.tcp.sample_duration_us);
    tcp.ack_seq = 200;
    ntrip_tx_diag_tcp_sample(c, 4000000, 100, &tcp, 0);
    TEST_ASSERT_FALSE(c->status.tcp.delta_valid);
    ntrip_tx_diag_connection(c, 2);
    tcp.ack_seq = 90000;
    ntrip_tx_diag_tcp_sample(c, 5000000, 110, &tcp, 0);
    TEST_ASSERT_EQUAL_UINT32(2, c->status.tcp.generation);
    TEST_ASSERT_FALSE(c->status.tcp.delta_valid);
    TEST_ASSERT_EQUAL_UINT32(0, c->status.tcp.acked_delta_bytes);
    TEST_ASSERT_EQUAL_UINT64(5, c->status.tcp_sampling.samples);   /* failed samples and old connections count */
    TEST_ASSERT_EQUAL_UINT64(440, c->status.tcp_sampling.total_us);
    free(c);
}

TEST_CASE("sender diagnostics: snapshot call cost histogram has inclusive bounds and survives reconnect", "[ntrip_diag]")
{
    static const uint64_t durations[] = {
        0, 100, 101, 500, 501, 1000, 1001, 5000, 5001, 10000, 10001, 50000, 50001, 100000, 100001,
    };
    static const uint64_t expected[] = {2, 2, 2, 2, 2, 2, 2, 1};
    ntrip_tx_diag_core_t *c = calloc(1, sizeof(*c));
    TEST_ASSERT_NOT_NULL(c);
    ntrip_tx_diag_connection(c, 1);
    net_tcp_snapshot_t tcp = {0};
    uint64_t total = 0;
    for (size_t i = 0; i < sizeof(durations) / sizeof(durations[0]); i++) {
        total += durations[i];
        /* Both a successful and an unavailable sample consume caller time. */
        ntrip_tx_diag_tcp_sample(c, (i + 1u) * UINT64_C(1000000), durations[i], i & 1u ? NULL : &tcp, 22);
    }
    TEST_ASSERT_EQUAL_UINT64(15, c->status.tcp_sampling.samples);
    TEST_ASSERT_EQUAL_UINT64(total, c->status.tcp_sampling.total_us);
    TEST_ASSERT_EQUAL_UINT64(100001, c->status.tcp_sampling.last_us);
    TEST_ASSERT_EQUAL_UINT64(100001, c->status.tcp_sampling.max_us);
    TEST_ASSERT_FALSE(c->status.tcp_sampling.saturated);
    for (size_t i = 0; i < NTRIP_TX_DIAG_COST_BUCKETS; i++) {
        TEST_ASSERT_EQUAL_UINT64(expected[i], c->status.tcp_sampling.histogram[i]);
    }
    ntrip_tx_diag_connection(c, 2);
    TEST_ASSERT_EQUAL_UINT64(0, c->status.tcp.sample_duration_us);
    TEST_ASSERT_EQUAL_UINT64(15, c->status.tcp_sampling.samples);
    TEST_ASSERT_EQUAL_UINT64(total, c->status.tcp_sampling.total_us);
    for (size_t i = 0; i < NTRIP_TX_DIAG_COST_BUCKETS; i++) {
        TEST_ASSERT_EQUAL_UINT64(expected[i], c->status.tcp_sampling.histogram[i]);
    }
    ntrip_tx_diag_tcp_sample(c, UINT64_C(6000000000), UINT64_C(5000000000), NULL, 22);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(5000000000), c->status.tcp.sample_duration_us);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(5000000000), c->status.tcp_sampling.last_us);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(5000000000), c->status.tcp_sampling.max_us);
    TEST_ASSERT_EQUAL_UINT64(total + UINT64_C(5000000000), c->status.tcp_sampling.total_us);
    TEST_ASSERT_EQUAL_UINT64(2, c->status.tcp_sampling.histogram[7]);
    free(c);
}

TEST_CASE("sender diagnostics: snapshot cost overflow saturates without wrapping or losing the last duration", "[ntrip_diag]")
{
    ntrip_tx_diag_core_t *c = calloc(1, sizeof(*c));
    TEST_ASSERT_NOT_NULL(c);
    c->status.tcp_sampling.samples = UINT64_MAX - 1u;
    c->status.tcp_sampling.total_us = UINT64_MAX - 50u;
    c->status.tcp_sampling.histogram[0] = UINT64_MAX - 1u;
    ntrip_tx_diag_tcp_sample(c, 1000000, 100, NULL, 22);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c->status.tcp_sampling.samples);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c->status.tcp_sampling.total_us);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c->status.tcp_sampling.histogram[0]);
    TEST_ASSERT_TRUE(c->status.tcp_sampling.saturated);
    ntrip_tx_diag_tcp_sample(c, 2000000, 0, NULL, 22);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c->status.tcp_sampling.samples);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c->status.tcp_sampling.total_us);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c->status.tcp_sampling.histogram[0]);
    TEST_ASSERT_EQUAL_UINT64(0, c->status.tcp_sampling.last_us);
    TEST_ASSERT_EQUAL_UINT64(100, c->status.tcp_sampling.max_us);
    ntrip_tx_diag_connection(c, 9);
    TEST_ASSERT_TRUE(c->status.tcp_sampling.saturated);
    TEST_ASSERT_EQUAL_UINT64(UINT64_MAX, c->status.tcp_sampling.samples);
    free(c);
}
