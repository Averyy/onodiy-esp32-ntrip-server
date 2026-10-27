/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MONITOR_DIAG_TARGETS 3u
#define MONITOR_DIAG_CAPACITY 600u
#define MONITOR_DIAG_DURATION_S 600u
#define MONITOR_DIAG_PAGE 32u

typedef enum {
    MONITOR_DIAG_NO_TARGET = 0,
    MONITOR_DIAG_PENDING,
    MONITOR_DIAG_OK,
    MONITOR_DIAG_TIMEOUT,
    MONITOR_DIAG_LOCAL_ERROR,
    MONITOR_DIAG_CANCELLED,
} monitor_diag_result_t;

typedef struct {
    uint64_t sent_us;             /* monotonic dispatch time; 0 = never dispatched */
    uint32_t address;             /* IPv4, network byte order; zero = no target */
    uint16_t rtt_ms;              /* dispatch to reply processing, includes local scheduling */
    int16_t error;                /* errno only for LOCAL_ERROR */
    uint8_t result;
} monitor_diag_ping_t;

typedef struct {
    uint32_t seq;
    int8_t rssi;                  /* zero when unknown */
    uint64_t started_us;
    int64_t unix_s;               /* trusted whole seconds at cycle start, else zero */
    monitor_diag_ping_t ping[MONITOR_DIAG_TARGETS]; /* router, internet, server */
} monitor_diag_sample_t;

typedef struct {
    bool available;
    bool active;
    uint32_t generation;
    uint32_t expires_in_s;
    uint32_t first_seq;
    uint32_t last_seq;
    uint32_t skipped_cycles;
    uint32_t late_replies;
    uint32_t duplicate_replies;
    uint64_t now_us;
} monitor_diag_status_t;

/* Explicitly requested, ten minutes at most. Repeated starts while active are
 * refused; stopping preserves the record until another run or restart. */
esp_err_t monitor_diag_start(void);
void monitor_diag_stop(void);
void monitor_diag_get(uint32_t after, monitor_diag_sample_t *out, size_t max,
                      size_t *n, monitor_diag_status_t *status);
