/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * CPU share per task from two snapshots of the run-time counters (pure
 * logic, no IDF or RTOS includes, host-testable).
 *
 * A task's share is its run time since the previous snapshot divided by
 * the time all cores had in that interval (elapsed time x cores), so the
 * shares of every task, the idle tasks included, add up to about 100 %. A
 * task that busies one core of two fully shows 50 %. Tasks are matched by
 * their unique task number, so a task created between the snapshots counts
 * from its creation and a reused handle is never confused with a deleted
 * task. The first snapshot has no interval: it reports each task's share
 * since boot.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DTS_MAX_TASKS 48u

typedef struct {
    uint32_t id;           /**< unique task number */
    uint64_t runtime;      /**< run-time counter (same unit as `now`) */
} dts_sample_t;

typedef struct {
    dts_sample_t prev[DTS_MAX_TASKS];
    size_t prev_n;
    uint64_t prev_now;
    bool have_prev;
} dts_state_t;

/**
 * Compute the share (0..100 %) of each sample since the previous call and
 * remember this snapshot. `now` = the run-time clock now (it started at
 * boot); `cores` = number of cores.
 */
void dts_update(dts_state_t *st, const dts_sample_t *samples, size_t n, uint64_t now, uint32_t cores,
                uint8_t *pct_out);

#ifdef __cplusplus
}
#endif
