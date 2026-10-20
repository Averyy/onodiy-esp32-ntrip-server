/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The monitor's sums (pure, no IDF calls; host-tested): samples into
 * MONITOR_BUCKET_SAMPLES-sample buckets in a ring of the caller's storage,
 * the oldest bucket giving way once the ring is full, and the summary over
 * the whole ring. The caller locks.
 */
#pragma once

#include "monitor.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    monitor_bucket_t *ring;
    size_t cap;
    size_t n;                /**< buckets in use */
    size_t head;             /**< the current bucket (the newest) */
    bool have_last;
    monitor_sample_t last;
} mon_core_t;

void mon_init(mon_core_t *m, monitor_bucket_t *ring, size_t cap);

/** Add a sample to the current bucket; a full one gives way to a new one first. */
void mon_add(mon_core_t *m, const monitor_sample_t *s);

/** Another server is pinged: the server figures of every bucket go (the router's and the rest stay). */
void mon_forget_server(mon_core_t *m);

/** The summary over the ring (the server's name and `running` are the caller's). */
void mon_summary(const mon_core_t *m, monitor_status_t *out);

/** The buckets oldest first, at most `max` (the newest); returns how many. */
size_t mon_series(const mon_core_t *m, monitor_bucket_t *out, size_t max);

#ifdef __cplusplus
}
#endif
