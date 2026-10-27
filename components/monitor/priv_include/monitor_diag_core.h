/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include "monitor_diag.h"

#define MON_DIAG_INTERVAL_US 1000000ULL
#define MON_DIAG_TIMEOUT_US 2000000ULL
/* Dispatch can lag cycle start by <2 s; a dispatched probe has another 2 s
 * for its reply. Five newest cycles cover every still-pending probe. */
#define MON_DIAG_PENDING_CYCLES 5u

typedef struct {
    monitor_diag_sample_t *ring;
    size_t capacity;
    uint32_t generation, seq, skipped, late, duplicates;
    bool active;
    uint64_t next_us, until_us;
} mon_diag_t;

void mon_diag_init(mon_diag_t *d, monitor_diag_sample_t *ring, size_t capacity);
void mon_diag_start(mon_diag_t *d, uint64_t now_us);
void mon_diag_stop(mon_diag_t *d);
void mon_diag_expire(mon_diag_t *d, uint64_t now_us);
bool mon_diag_due(mon_diag_t *d, uint64_t now_us);
uint32_t mon_diag_begin(mon_diag_t *d, uint64_t now_us, int64_t unix_s,
                        const uint32_t address[MONITOR_DIAG_TARGETS], int8_t rssi);
/** Record dispatch just before sendto. No send after its cycle's 2 s
 * dispatch deadline; the reply timeout starts at this individual stamp. */
bool mon_diag_sent(mon_diag_t *d, uint32_t generation, uint32_t seq, unsigned target, uint64_t now_us);
void mon_diag_error(mon_diag_t *d, uint32_t generation, uint32_t seq, unsigned target, int error);
/** Socket receive/select failed locally: outstanding probes cannot receive
 * replies on that socket. Never convert this local failure into packet loss. */
void mon_diag_receive_error(mon_diag_t *d, uint32_t generation, int error);
bool mon_diag_reply(mon_diag_t *d, uint32_t generation, uint32_t seq, unsigned target,
                    uint32_t address, uint64_t now_us);
size_t mon_diag_copy(const mon_diag_t *d, uint32_t after, monitor_diag_sample_t *out, size_t max);
void mon_diag_status(const mon_diag_t *d, uint64_t now_us, monitor_diag_status_t *out);
