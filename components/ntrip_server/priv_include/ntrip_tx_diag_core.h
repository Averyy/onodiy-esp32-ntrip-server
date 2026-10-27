/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "ntrip_tx_diag.h"

/** Pure numeric diagnostics; caller serializes access. No transport policy. */
typedef struct {
    ntrip_tx_diag_t status;
    ntrip_tx_write_diag_t history[NTRIP_TX_DIAG_HISTORY];
    uint32_t history_version;
    size_t history_next;
    uint32_t next_sequence;
} ntrip_tx_diag_core_t;

void ntrip_tx_diag_connection(ntrip_tx_diag_core_t *c, uint32_t generation);
void ntrip_tx_diag_begin(ntrip_tx_diag_core_t *c, uint64_t now_us, int64_t utc_s, ntrip_tx_phase_t phase,
                         bool tls, size_t bytes, const rtcm_hub_read_meta_t *arrivals);
void ntrip_tx_diag_progress(ntrip_tx_diag_core_t *c, uint64_t call_started_us, uint64_t now_us, size_t accepted);
void ntrip_tx_diag_end(ntrip_tx_diag_core_t *c, uint64_t now_us, ntrip_tx_outcome_t outcome);
void ntrip_tx_diag_snapshot(const ntrip_tx_diag_core_t *c, uint64_t now_us, int64_t utc_s, ntrip_tx_diag_t *out);
void ntrip_tx_diag_tcp_sample(ntrip_tx_diag_core_t *c, uint64_t now_us, uint64_t sample_duration_us,
                              const net_tcp_snapshot_t *sample, int error);
