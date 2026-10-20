/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "monitor_core.h"

#include <string.h>

static void bucket_clear(monitor_bucket_t *b)
{
    memset(b, 0, sizeof(*b));
    b->mem_min = UINT32_MAX;
}

void mon_init(mon_core_t *m, monitor_bucket_t *ring, size_t cap)
{
    memset(m, 0, sizeof(*m));
    m->ring = ring;
    m->cap = cap;
}

static void ping_add(monitor_ping_acc_t *a, monitor_ping_t p, uint16_t ms)
{
    if (p == MONITOR_PING_NONE) {
        return;
    }
    a->sent++;
    if (p == MONITOR_PING_OK) {
        a->ok++;
        a->sum_ms += ms;
        if (ms > a->max_ms) {
            a->max_ms = ms;
        }
    }
}

void mon_add(mon_core_t *m, const monitor_sample_t *s)
{
    if (m->cap == 0) {
        return;
    }
    if (m->n == 0) {
        m->head = 0;
        m->n = 1;
        bucket_clear(&m->ring[0]);
    } else if (m->ring[m->head].samples >= MONITOR_BUCKET_SAMPLES) {
        m->head = (m->head + 1) % m->cap;
        if (m->n < m->cap) {
            m->n++;
        }
        bucket_clear(&m->ring[m->head]);   /* the oldest gives way once the ring is full */
    }
    monitor_bucket_t *b = &m->ring[m->head];
    b->samples++;
    ping_add(&b->router, s->router, s->router_ms);
    ping_add(&b->server, s->server, s->server_ms);
    if (s->rssi_known) {
        if (b->rssi_n == 0 || s->rssi < b->rssi_min) {
            b->rssi_min = s->rssi;
        }
        b->rssi_n++;
        b->rssi_sum += s->rssi;
    }
    if (s->cpu_known) {
        b->cpu_n++;
        b->cpu_sum += s->cpu_pct;
        if (s->cpu_pct > b->cpu_max) {
            b->cpu_max = s->cpu_pct;
        }
    }
    if (s->mem_free != 0 && s->mem_free < b->mem_min) {
        b->mem_min = s->mem_free;
    }
    m->last = *s;
    m->have_last = true;
}

void mon_forget_server(mon_core_t *m)
{
    for (size_t i = 0; i < m->n; i++) {
        memset(&m->ring[i].server, 0, sizeof(m->ring[i].server));
    }
}

typedef struct {
    uint32_t sent, ok;
    uint64_t sum_ms;
    uint32_t max_ms;
} ping_tot_t;

static void ping_tot(ping_tot_t *t, const monitor_ping_acc_t *a)
{
    t->sent += a->sent;
    t->ok += a->ok;
    t->sum_ms += a->sum_ms;
    if (a->max_ms > t->max_ms) {
        t->max_ms = a->max_ms;
    }
}

static void ping_out(monitor_ping_sum_t *o, const ping_tot_t *t)
{
    o->sent = t->sent;
    o->lost = t->sent - t->ok;
    o->avg_ms = t->ok ? (uint32_t)((t->sum_ms + t->ok / 2u) / t->ok) : 0;
    o->max_ms = t->max_ms;
}

void mon_summary(const mon_core_t *m, monitor_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->have_last = m->have_last;
    out->last = m->last;
    ping_tot_t router = { 0 }, server = { 0 };
    uint32_t samples = 0, rssi_n = 0, cpu_n = 0, cpu_sum = 0;
    int64_t rssi_sum = 0;
    int8_t rssi_min = 0;
    uint8_t cpu_max = 0;
    uint32_t mem_min = UINT32_MAX;
    for (size_t i = 0; i < m->n; i++) {
        const monitor_bucket_t *b = &m->ring[i];
        samples += b->samples;
        ping_tot(&router, &b->router);
        ping_tot(&server, &b->server);
        if (b->rssi_n) {
            if (rssi_n == 0 || b->rssi_min < rssi_min) {
                rssi_min = b->rssi_min;
            }
            rssi_n += b->rssi_n;
            rssi_sum += b->rssi_sum;
        }
        if (b->cpu_n) {
            cpu_n += b->cpu_n;
            cpu_sum += b->cpu_sum;
            if (b->cpu_max > cpu_max) {
                cpu_max = b->cpu_max;
            }
        }
        if (b->mem_min < mem_min) {
            mem_min = b->mem_min;
        }
    }
    uint64_t span = (uint64_t)samples * MONITOR_SAMPLE_S;
    uint64_t day = (uint64_t)MONITOR_BUCKETS * MONITOR_BUCKET_S;
    out->span_s = (uint32_t)(span < day ? span : day);
    ping_out(&out->router, &router);
    ping_out(&out->server, &server);
    if (rssi_n) {
        /* Rounded half away from zero: -78.5 dBm is -79. */
        int64_t q = (rssi_sum * 2 - (int64_t)rssi_n) / (2 * (int64_t)rssi_n);
        out->rssi_known = true;
        out->rssi_avg = (int8_t)q;
        out->rssi_min = rssi_min;
    }
    if (cpu_n) {
        out->cpu_known = true;
        out->cpu_avg = (uint8_t)((cpu_sum + cpu_n / 2u) / cpu_n);
        out->cpu_max = cpu_max;
    }
    if (mem_min != UINT32_MAX) {
        out->mem_known = true;
        out->mem_min = mem_min;
    }
}

size_t mon_series(const mon_core_t *m, monitor_bucket_t *out, size_t max)
{
    size_t k = m->n < max ? m->n : max;
    /* The oldest of the k newest: head - (k - 1), around the ring. */
    size_t start = (m->head + m->cap - (k ? k - 1 : 0)) % (m->cap ? m->cap : 1);
    for (size_t i = 0; i < k; i++) {
        out[i] = m->ring[(start + i) % m->cap];
    }
    return k;
}
