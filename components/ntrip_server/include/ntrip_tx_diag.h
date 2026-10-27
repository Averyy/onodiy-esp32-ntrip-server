/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tcp_diag.h"
#include "rtcm_hub_meta.h"

/* Diagnostic thresholds only: never alter delivery or reconnect policy.
 * PROVISIONAL: validate overhead and usefulness with recorded-stream tests. */
#define NTRIP_TX_DIAG_SLOW_MS 500u
#define NTRIP_TX_DIAG_HISTORY 8u
#define NTRIP_TX_DIAG_TCP_MS 1000u
#define NTRIP_TX_DIAG_COST_BUCKETS 8u

typedef enum {
    NTRIP_TX_PHASE_IDLE = 0,
    NTRIP_TX_PHASE_REQUEST,
    NTRIP_TX_PHASE_RTCM,
    NTRIP_TX_PHASE_TERMINATOR,
} ntrip_tx_phase_t;

typedef enum {
    NTRIP_TX_OUTCOME_PENDING = 0,
    NTRIP_TX_OUTCOME_DONE,
    NTRIP_TX_OUTCOME_STALL,
    NTRIP_TX_OUTCOME_ERROR,
    NTRIP_TX_OUTCOME_ABORT,
} ntrip_tx_outcome_t;

typedef struct {
    uint64_t sampled_us;
    uint64_t sample_duration_us; /**< Time inside the synchronous snapshot call, including its handoff wait. */
    uint64_t previous_sample_us;
    uint64_t last_ack_progress_us; /**< Last observed ACK advance; 0 = no continuous evidence. */
    uint32_t generation;
    uint32_t acked_delta_bytes;  /**< TCP ACK sequence progress, including TLS/HTTP overhead. */
    int error;                  /**< errno if latest attempt failed; no zero-backlog inference. */
    bool available;
    bool delta_valid;           /**< False for first sample, failed sample or new connection. */
    net_tcp_snapshot_t state;
} ntrip_tx_tcp_diag_t;

/** Snapshot-call cost this boot, including failed calls. Connections do not
 * reset these counters. Histogram buckets are noncumulative with inclusive
 * upper bounds 100, 500, 1000, 5000, 10000, 50000, 100000 us, then >100000.
 * Counts and total_us saturate at UINT64_MAX; saturated means a sum/count
 * lost precision, so total_us/samples no longer gives a trustworthy mean.
 */
typedef struct {
    uint64_t samples;
    uint64_t total_us;
    uint64_t max_us;
    uint64_t last_us;
    uint64_t histogram[NTRIP_TX_DIAG_COST_BUCKETS];
    bool saturated;
} ntrip_tx_sampling_diag_t;

/** One current or completed application write. Accepted bytes mean local
 * send/TLS acceptance, never remote receipt. Includes HTTP chunk framing,
 * excludes TLS encryption overhead. No request or RTCM contents are retained.
 */
typedef struct {
    uint64_t started_us;
    uint64_t updated_us;         /**< Last actual writer update; getters do not advance it. */
    uint64_t last_accept_us;     /**< Last local acceptance; start baseline until any byte is accepted. */
    int64_t utc_s;               /**< Trusted UTC at started_us, whole seconds; 0 = unavailable. */
    uint32_t generation;         /**< Connection attempt; never compare ACKs across generations. */
    uint32_t sequence;           /**< Write sequence this boot, wraps modulo 2^32. */
    uint32_t total_bytes;
    uint32_t accepted_bytes;
    uint32_t remaining_bytes;
    uint32_t duration_ms;        /**< Frozen on completion; derived live by getter while pending. */
    uint32_t max_no_progress_ms; /**< Longest interval without local byte acceptance. */
    uint32_t max_send_call_ms;   /**< Longest synchronous send/TLS call. */
    ntrip_tx_phase_t phase;
    ntrip_tx_outcome_t outcome;
    bool tls;
    bool active;
    bool slow;
    rtcm_hub_read_meta_t arrivals;
    ntrip_tx_tcp_diag_t tcp_at_end; /**< Last existing sample when completed; no completion-time socket call. */
} ntrip_tx_write_diag_t;

typedef struct {
    uint64_t captured_us;
    int64_t utc_s;               /**< Trusted UTC at captured_us, whole seconds; 0 = unavailable. */
    uint32_t generation;
    uint32_t slow_writes;        /**< Writes that crossed SLOW_MS, including an active one. */
    uint32_t history_overwritten;
    uint32_t history_count;
    ntrip_tx_write_diag_t write; /**< Current write or last completed write. */
    ntrip_tx_tcp_diag_t tcp;
    ntrip_tx_sampling_diag_t tcp_sampling;
} ntrip_tx_diag_t;
