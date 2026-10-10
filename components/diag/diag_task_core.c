/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "diag_task_core.h"

static const dts_sample_t *find_prev(const dts_state_t *st, uint32_t id)
{
    for (size_t i = 0; i < st->prev_n; i++) {
        if (st->prev[i].id == id) {
            return &st->prev[i];
        }
    }
    return NULL;
}

void dts_update(dts_state_t *st, const dts_sample_t *samples, size_t n, uint64_t now, uint32_t cores,
                uint8_t *pct_out)
{
    if (cores == 0) {
        cores = 1;
    }
    uint64_t elapsed = (st->have_prev && now > st->prev_now) ? now - st->prev_now : now;
    uint64_t capacity = elapsed * cores;
    for (size_t i = 0; i < n; i++) {
        uint64_t used = samples[i].runtime;
        if (st->have_prev) {
            const dts_sample_t *p = find_prev(st, samples[i].id);
            if (p != NULL) {
                used = (samples[i].runtime >= p->runtime) ? samples[i].runtime - p->runtime : 0;
            }
        }
        uint64_t pct = (capacity == 0) ? 0 : (used * 100u + capacity / 2u) / capacity;
        if (pct_out != NULL) {
            pct_out[i] = (uint8_t)(pct > 100u ? 100u : pct);
        }
    }
    st->prev_n = (n > DTS_MAX_TASKS) ? DTS_MAX_TASKS : n;
    for (size_t i = 0; i < st->prev_n; i++) {
        st->prev[i] = samples[i];
    }
    st->prev_now = now;
    st->have_prev = true;
}
