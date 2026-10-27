/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "monitor_diag_core.h"
#include <errno.h>
#include <limits.h>
#include <string.h>

static monitor_diag_sample_t *sample(mon_diag_t *d, uint32_t seq)
{
    if (!seq || !d->capacity || seq > d->seq || d->seq - seq >= d->capacity) return NULL;
    monitor_diag_sample_t *s = &d->ring[(seq - 1u) % d->capacity];
    return s->seq == seq ? s : NULL;
}

void mon_diag_init(mon_diag_t *d, monitor_diag_sample_t *ring, size_t capacity)
{
    memset(d, 0, sizeof(*d));
    d->ring = ring;
    d->capacity = capacity;
}

void mon_diag_start(mon_diag_t *d, uint64_t now_us)
{
    uint32_t generation = d->generation + 1u;
    mon_diag_init(d, d->ring, d->capacity);
    d->generation = generation ? generation : 1u;
    d->next_us = now_us;
    d->until_us = now_us + MONITOR_DIAG_DURATION_S * MON_DIAG_INTERVAL_US;
    d->active = d->capacity != 0;
}

void mon_diag_stop(mon_diag_t *d)
{
    d->active = false;
    /* Includes probes dispatched late in their cycle. */
    for (uint32_t i = 0; i < MON_DIAG_PENDING_CYCLES && i < d->seq; i++) {
        monitor_diag_sample_t *s = sample(d, d->seq - i);
        if (!s) continue;
        for (unsigned j = 0; j < MONITOR_DIAG_TARGETS; j++)
            if (s->ping[j].result == MONITOR_DIAG_PENDING) s->ping[j].result = MONITOR_DIAG_CANCELLED;
    }
}

void mon_diag_expire(mon_diag_t *d, uint64_t now_us)
{
    if (now_us >= d->until_us) d->active = false;
    for (uint32_t i = 0; i < MON_DIAG_PENDING_CYCLES && i < d->seq; i++) {
        monitor_diag_sample_t *s = sample(d, d->seq - i);
        if (!s) continue;
        for (unsigned j = 0; j < MONITOR_DIAG_TARGETS; j++) {
            monitor_diag_ping_t *p = &s->ping[j];
            uint64_t from = p->sent_us ? p->sent_us : s->started_us;
            if (p->result != MONITOR_DIAG_PENDING || now_us < from || now_us - from < MON_DIAG_TIMEOUT_US)
                continue;
            if (p->sent_us) {
                p->result = MONITOR_DIAG_TIMEOUT;
            } else {
                p->result = MONITOR_DIAG_LOCAL_ERROR;
                p->error = ETIMEDOUT;   /* no dispatched echo: not evidence of network loss */
            }
        }
    }
}

bool mon_diag_due(mon_diag_t *d, uint64_t now_us)
{
    mon_diag_expire(d, now_us);
    return d->active && now_us >= d->next_us;
}

uint32_t mon_diag_begin(mon_diag_t *d, uint64_t now_us, int64_t unix_s,
                        const uint32_t address[MONITOR_DIAG_TARGETS], int8_t rssi)
{
    if (!mon_diag_due(d, now_us)) return 0;
    uint64_t missed = (now_us - d->next_us) / MON_DIAG_INTERVAL_US;
    d->skipped += (uint32_t)missed;
    d->next_us = now_us + MON_DIAG_INTERVAL_US;   /* a late cycle never schedules a catch-up pair */
    monitor_diag_sample_t *s = &d->ring[d->seq % d->capacity];
    memset(s, 0, sizeof(*s));
    s->seq = ++d->seq;
    s->started_us = now_us;
    s->unix_s = unix_s;
    s->rssi = rssi;
    for (unsigned j = 0; j < MONITOR_DIAG_TARGETS; j++) {
        s->ping[j].address = address[j];
        s->ping[j].result = address[j] ? MONITOR_DIAG_PENDING : MONITOR_DIAG_NO_TARGET;
    }
    return s->seq;
}

bool mon_diag_sent(mon_diag_t *d, uint32_t generation, uint32_t seq, unsigned target, uint64_t now_us)
{
    monitor_diag_sample_t *s = sample(d, seq);
    if (!d->active || now_us >= d->until_us || generation != d->generation || !s ||
        target >= MONITOR_DIAG_TARGETS || now_us == 0 || now_us < s->started_us ||
        now_us - s->started_us >= MON_DIAG_TIMEOUT_US || s->ping[target].result != MONITOR_DIAG_PENDING ||
        s->ping[target].sent_us != 0) return false;
    s->ping[target].sent_us = now_us;
    return true;
}

void mon_diag_error(mon_diag_t *d, uint32_t generation, uint32_t seq, unsigned target, int error)
{
    monitor_diag_sample_t *s = sample(d, seq);
    if (generation != d->generation || !s || target >= MONITOR_DIAG_TARGETS ||
        s->ping[target].result != MONITOR_DIAG_PENDING) return;
    s->ping[target].result = MONITOR_DIAG_LOCAL_ERROR;
    s->ping[target].error = (int16_t)(error > INT16_MAX ? INT16_MAX : error);
}

void mon_diag_receive_error(mon_diag_t *d, uint32_t generation, int error)
{
    if (generation != d->generation) return;
    for (uint32_t i = 0; i < MON_DIAG_PENDING_CYCLES && i < d->seq; i++) {
        uint32_t seq = d->seq - i;
        for (unsigned j = 0; j < MONITOR_DIAG_TARGETS; j++)
            mon_diag_error(d, generation, seq, j, error);
    }
}

bool mon_diag_reply(mon_diag_t *d, uint32_t generation, uint32_t seq, unsigned target,
                    uint32_t address, uint64_t now_us)
{
    if (generation != d->generation || target >= MONITOR_DIAG_TARGETS) return false;
    monitor_diag_sample_t *s = sample(d, seq);
    if (!s || s->ping[target].address != address) return false;
    monitor_diag_ping_t *p = &s->ping[target];
    if (!p->sent_us || now_us < p->sent_us) return false;
    if (p->result == MONITOR_DIAG_OK) { d->duplicates++; return false; }
    if (p->result != MONITOR_DIAG_PENDING && p->result != MONITOR_DIAG_TIMEOUT) return false;
    if (p->result == MONITOR_DIAG_TIMEOUT || now_us - p->sent_us >= MON_DIAG_TIMEOUT_US) {
        if (p->result == MONITOR_DIAG_PENDING) p->result = MONITOR_DIAG_TIMEOUT;
        d->late++;
        return false;
    }
    if (p->result != MONITOR_DIAG_PENDING) return false;
    p->result = MONITOR_DIAG_OK;
    p->rtt_ms = (uint16_t)((now_us - p->sent_us + 500u) / 1000u);
    return true;
}

size_t mon_diag_copy(const mon_diag_t *d, uint32_t after, monitor_diag_sample_t *out, size_t max)
{
    if (!d->seq || !max || !d->capacity) return 0;
    uint32_t first = d->seq > d->capacity ? d->seq - (uint32_t)d->capacity + 1u : 1u;
    if (after >= d->seq) return 0;
    if (after >= first) first = after + 1u;
    size_t n = 0;
    for (uint32_t seq = first; seq <= d->seq && n < max; seq++)
        out[n++] = d->ring[(seq - 1u) % d->capacity];
    return n;
}

void mon_diag_status(const mon_diag_t *d, uint64_t now_us, monitor_diag_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->available = d->ring != NULL;
    out->active = d->active && now_us < d->until_us;
    out->generation = d->generation;
    out->now_us = now_us;
    out->expires_in_s = out->active ? (uint32_t)((d->until_us - now_us + 999999u) / 1000000u) : 0;
    out->last_seq = d->seq;
    out->first_seq = d->seq > d->capacity ? d->seq - (uint32_t)d->capacity + 1u : (d->seq ? 1u : 0u);
    out->skipped_cycles = d->skipped;
    out->late_replies = d->late;
    out->duplicate_replies = d->duplicates;
}
