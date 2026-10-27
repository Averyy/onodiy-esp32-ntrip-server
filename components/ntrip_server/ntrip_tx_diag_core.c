/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "ntrip_tx_diag_core.h"

#include <string.h>

static uint32_t elapsed_ms(uint64_t now, uint64_t before)
{
    uint64_t ms = now > before ? (now - before) / 1000u : 0;
    return ms > UINT32_MAX ? UINT32_MAX : (uint32_t)ms;
}

static void update_time(ntrip_tx_write_diag_t *w, uint64_t now_us)
{
    w->updated_us = now_us;
    w->duration_ms = elapsed_ms(now_us, w->started_us);
    uint32_t gap = elapsed_ms(now_us, w->last_accept_us);
    if (gap > w->max_no_progress_ms) {
        w->max_no_progress_ms = gap;
    }
}

static void count_slow(ntrip_tx_diag_core_t *c)
{
    if (!c->status.write.slow && c->status.write.duration_ms >= NTRIP_TX_DIAG_SLOW_MS) {
        c->status.write.slow = true;
        c->status.slow_writes++;
    }
}

void ntrip_tx_diag_connection(ntrip_tx_diag_core_t *c, uint32_t generation)
{
    c->status.generation = generation;
    memset(&c->status.tcp, 0, sizeof(c->status.tcp));
    c->status.tcp.generation = generation;
}

void ntrip_tx_diag_begin(ntrip_tx_diag_core_t *c, uint64_t now_us, int64_t utc_s, ntrip_tx_phase_t phase,
                         bool tls, size_t bytes, const rtcm_hub_read_meta_t *arrivals)
{
    ntrip_tx_write_diag_t *w = &c->status.write;
    memset(w, 0, sizeof(*w));
    w->started_us = now_us;
    w->updated_us = now_us;
    w->last_accept_us = now_us;   /* baseline before the first accepted byte */
    w->utc_s = utc_s;
    w->generation = c->status.generation;
    w->sequence = ++c->next_sequence;
    w->total_bytes = bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes;
    w->remaining_bytes = w->total_bytes;
    w->phase = phase;
    w->tls = tls;
    w->active = true;
    if (arrivals != NULL) {
        w->arrivals = *arrivals;
    }
}

void ntrip_tx_diag_progress(ntrip_tx_diag_core_t *c, uint64_t call_started_us, uint64_t now_us, size_t accepted)
{
    ntrip_tx_write_diag_t *w = &c->status.write;
    update_time(w, now_us);
    uint32_t call_ms = elapsed_ms(now_us, call_started_us);
    if (call_ms > w->max_send_call_ms) {
        w->max_send_call_ms = call_ms;
    }
    if (accepted > 0) {
        uint32_t used = accepted > w->remaining_bytes ? w->remaining_bytes : (uint32_t)accepted;
        w->accepted_bytes += used;
        w->remaining_bytes -= used;
        w->last_accept_us = now_us;
    }
    count_slow(c);
}

void ntrip_tx_diag_end(ntrip_tx_diag_core_t *c, uint64_t now_us, ntrip_tx_outcome_t outcome)
{
    ntrip_tx_write_diag_t *w = &c->status.write;
    update_time(w, now_us);
    count_slow(c);
    w->active = false;
    w->outcome = outcome;
    w->tcp_at_end = c->status.tcp;
    if (w->slow || outcome != NTRIP_TX_OUTCOME_DONE) {
        c->history[c->history_next] = *w;
        c->history_next = (c->history_next + 1u) % NTRIP_TX_DIAG_HISTORY;
        c->history_version++;
        if (c->status.history_count < NTRIP_TX_DIAG_HISTORY) {
            c->status.history_count++;
        } else {
            c->status.history_overwritten++;
        }
    }
}

void ntrip_tx_diag_snapshot(const ntrip_tx_diag_core_t *c, uint64_t now_us, int64_t utc_s, ntrip_tx_diag_t *out)
{
    *out = c->status;
    out->captured_us = now_us;
    out->utc_s = utc_s;
    if (out->write.active) {
        uint64_t updated_us = out->write.updated_us;
        update_time(&out->write, now_us);
        out->write.updated_us = updated_us;   /* last real update, not the export time */
        if (!out->write.slow && out->write.duration_ms >= NTRIP_TX_DIAG_SLOW_MS) {
            out->write.slow = true;
            out->slow_writes++;
        }
    }
}

static void saturated_add(uint64_t *value, uint64_t add, bool *saturated)
{
    if (UINT64_MAX - *value < add) {
        *value = UINT64_MAX;
        *saturated = true;
    } else {
        *value += add;
    }
}

static void sample_cost(ntrip_tx_sampling_diag_t *cost, uint64_t duration_us)
{
    static const uint32_t upper_us[] = {100, 500, 1000, 5000, 10000, 50000, 100000};
    _Static_assert(sizeof(upper_us) / sizeof(upper_us[0]) + 1u == NTRIP_TX_DIAG_COST_BUCKETS,
                   "every snapshot duration has one cost bucket");
    size_t bucket = 0;
    while (bucket < NTRIP_TX_DIAG_COST_BUCKETS - 1u && duration_us > upper_us[bucket]) {
        bucket++;
    }
    saturated_add(&cost->samples, 1, &cost->saturated);
    saturated_add(&cost->total_us, duration_us, &cost->saturated);
    saturated_add(&cost->histogram[bucket], 1, &cost->saturated);
    cost->last_us = duration_us;
    if (duration_us > cost->max_us) {
        cost->max_us = duration_us;
    }
}

void ntrip_tx_diag_tcp_sample(ntrip_tx_diag_core_t *c, uint64_t now_us, uint64_t sample_duration_us,
                              const net_tcp_snapshot_t *sample, int error)
{
    sample_cost(&c->status.tcp_sampling, sample_duration_us);
    ntrip_tx_tcp_diag_t *t = &c->status.tcp;
    bool previous = t->available && t->generation == c->status.generation;
    uint32_t old_ack = t->state.ack_seq;
    t->previous_sample_us = t->sampled_us;
    t->sampled_us = now_us;
    t->sample_duration_us = sample_duration_us;
    t->generation = c->status.generation;
    t->available = sample != NULL;
    t->error = sample == NULL ? error : 0;
    t->delta_valid = sample != NULL && previous;
    t->acked_delta_bytes = t->delta_valid ? sample->ack_seq - old_ack : 0;
    if (sample != NULL) {
        t->state = *sample;
        if (t->delta_valid && t->acked_delta_bytes > 0) {
            t->last_ack_progress_us = now_us;
        }
    } else {
        memset(&t->state, 0, sizeof(t->state));
        t->last_ack_progress_us = 0;   /* gap in evidence: no continuous ACK-age claim */
    }
}
