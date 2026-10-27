/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include "web_build.h"

static void arrivals(wj_t *w, const char *key, const rtcm_hub_arrivals_t *a)
{
    wj_obj_open(w, key);
    wj_uint(w, "frames", a->frames);
    wj_uint(w, "timed_frames", a->timed_frames);
    wj_uint(w, "exact_frames", a->exact_frames);
    if (a->timed_frames) {
        wj_uint(w, "oldest_rx_ms", a->oldest_rx_ms);
        wj_uint(w, "newest_rx_ms", a->newest_rx_ms);
    } else {
        wj_null(w, "oldest_rx_ms");
        wj_null(w, "newest_rx_ms");
    }
    wj_obj_close(w);
}

static void tx_tcp(wj_t *w, const char *key, const ntrip_tx_tcp_diag_t *d);

static void tx_write(wj_t *w, const char *key, const ntrip_tx_write_diag_t *d)
{
    static const char *const phases[] = {"idle", "request", "rtcm", "terminator"};
    static const char *const outcomes[] = {"pending", "done", "stall", "error", "abort"};
    wj_obj_open(w, key);
    wj_uint(w, "generation", d->generation);
    wj_uint(w, "sequence", d->sequence);
    wj_str(w, "phase", (unsigned)d->phase < 4 ? phases[d->phase] : "unknown");
    wj_str(w, "outcome", (unsigned)d->outcome < 5 ? outcomes[d->outcome] : "unknown");
    wj_bool(w, "active", d->active);
    wj_bool(w, "tls", d->tls);
    wj_bool(w, "slow", d->slow);
    wj_uint(w, "started_us", d->started_us);
    wj_uint(w, "updated_us", d->updated_us);
    wj_uint(w, "last_accept_us", d->last_accept_us);
    wj_unix(w, "start_unix", d->utc_s);
    wj_uint(w, "total_bytes", d->total_bytes);
    wj_uint(w, "accepted_bytes", d->accepted_bytes);
    wj_uint(w, "remaining_bytes", d->remaining_bytes);
    wj_uint(w, "duration_ms", d->duration_ms);
    wj_uint(w, "max_no_progress_ms", d->max_no_progress_ms);
    wj_uint(w, "max_send_call_ms", d->max_send_call_ms);
    arrivals(w, "live", &d->arrivals.live);
    arrivals(w, "observations", &d->arrivals.observations);
    arrivals(w, "sticky", &d->arrivals.sticky);
    tx_tcp(w, "tcp_at_end", &d->tcp_at_end);
    wj_obj_close(w);
}

static void tx_tcp(wj_t *w, const char *key, const ntrip_tx_tcp_diag_t *d)
{
    wj_obj_open(w, key);
    wj_bool(w, "available", d->available);
    wj_uint(w, "generation", d->generation);
    wj_uint(w, "sampled_us", d->sampled_us);
    wj_uint(w, "sample_duration_us", d->sample_duration_us);
    wj_uint(w, "previous_sample_us", d->previous_sample_us);
    wj_uint(w, "last_ack_progress_us", d->last_ack_progress_us);
    wj_int(w, "error", d->error);
    wj_bool(w, "delta_valid", d->delta_valid);
    if (d->delta_valid) wj_uint(w, "acked_delta_bytes", d->acked_delta_bytes);
    else wj_null(w, "acked_delta_bytes");
    if (d->available) {
        wj_obj_open(w, "state");
        const net_tcp_snapshot_t *s = &d->state;
        wj_uint(w, "ack_seq", s->ack_seq);
        wj_uint(w, "outstanding_bytes", s->outstanding_bytes);
        wj_uint(w, "send_buffer_used_bytes", s->send_buffer_used_bytes);
        wj_uint(w, "send_window_bytes", s->send_window_bytes);
        wj_uint(w, "congestion_window_bytes", s->congestion_window_bytes);
        wj_uint(w, "rto_ms", s->rto_ms);
        wj_uint(w, "queued_pbufs", s->queued_pbufs);
        wj_uint(w, "retransmit_run", s->retransmit_run);
        wj_bool(w, "unsent", s->flags & NET_TCP_SNAPSHOT_UNSENT);
        wj_bool(w, "unacked", s->flags & NET_TCP_SNAPSHOT_UNACKED);
        wj_bool(w, "rto", s->flags & NET_TCP_SNAPSHOT_RTO);
        wj_obj_close(w);
    } else wj_null(w, "state");
    wj_obj_close(w);
}

void web_build_monitor_diag(wj_t *w, const monitor_diag_status_t *s,
                           const monitor_diag_sample_t *samples, size_t count,
                           const ntrip_tx_diag_t *tx,
                           const ntrip_tx_write_diag_t history[][NTRIP_TX_DIAG_HISTORY],
                           const size_t *history_count, const int *history_error, size_t slots)
{
    static const char *const results[] = {"no_target", "pending", "ok", "timeout", "local_error", "cancelled"};
    wj_obj_open(w, NULL);
    wj_uint(w, "generation", s->generation);
    wj_bool(w, "available", s->available);
    wj_bool(w, "active", s->active);
    wj_uint(w, "now_us", s->now_us);
    wj_uint(w, "expires_in_s", s->expires_in_s);
    wj_uint(w, "first_seq", s->first_seq);
    wj_uint(w, "last_seq", s->last_seq);
    wj_uint(w, "skipped_cycles", s->skipped_cycles);
    wj_uint(w, "late_replies", s->late_replies);
    wj_uint(w, "duplicate_replies", s->duplicate_replies);
    wj_arr_open(w, "targets");
    wj_str(w, NULL, "router"); wj_str(w, NULL, "internet"); wj_str(w, NULL, "caster");
    wj_arr_close(w);
    wj_arr_open(w, "samples");
    for (size_t i = 0; i < count; i++) {
        const monitor_diag_sample_t *p = &samples[i];
        wj_obj_open(w, NULL);
        wj_uint(w, "seq", p->seq);
        wj_uint(w, "started_us", p->started_us);
        wj_unix(w, "time_unix", p->unix_s);
        if (p->rssi) wj_int(w, "rssi", p->rssi); else wj_null(w, "rssi");
        wj_arr_open(w, "ping");
        for (unsigned j = 0; j < MONITOR_DIAG_TARGETS; j++) {
            const monitor_diag_ping_t *t = &p->ping[j];
            wj_obj_open(w, NULL);
            /* Address is stored in network byte order; bytes also work on host tests. */
            const uint8_t *b = (const uint8_t *)&t->address;
            char ip[16];
            snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
            if (t->address) wj_str(w, "address", ip); else wj_null(w, "address");
            if (t->sent_us) wj_uint(w, "sent_us", t->sent_us); else wj_null(w, "sent_us");
            wj_str(w, "result", t->result < 6 ? results[t->result] : "unknown");
            if (t->result == MONITOR_DIAG_OK) wj_uint(w, "rtt_ms", t->rtt_ms);
            else wj_null(w, "rtt_ms");
            wj_int(w, "error", t->error);
            wj_obj_close(w);
        }
        wj_arr_close(w);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_arr_open(w, "transmit");
    for (size_t i = 0; i < slots; i++) {
        const ntrip_tx_diag_t *d = &tx[i];
        wj_obj_open(w, NULL);
        wj_uint(w, "slot", i);
        wj_uint(w, "captured_us", d->captured_us);
        wj_unix(w, "time_unix", d->utc_s);
        wj_uint(w, "generation", d->generation);
        wj_uint(w, "slow_writes", d->slow_writes);
        wj_uint(w, "history_overwritten", d->history_overwritten);
        wj_bool(w, "history_available", history_error[i] == 0);
        wj_int(w, "history_error", history_error[i]);
        tx_write(w, "write", &d->write);
        tx_tcp(w, "tcp", &d->tcp);
        wj_obj_open(w, "tcp_sampling");
        wj_uint(w, "samples", d->tcp_sampling.samples);
        wj_uint(w, "total_us", d->tcp_sampling.total_us);
        wj_uint(w, "max_us", d->tcp_sampling.max_us);
        wj_uint(w, "last_us", d->tcp_sampling.last_us);
        wj_bool(w, "saturated", d->tcp_sampling.saturated);
        wj_arr_open(w, "histogram");
        for (size_t j = 0; j < NTRIP_TX_DIAG_COST_BUCKETS; j++) {
            wj_uint(w, NULL, d->tcp_sampling.histogram[j]);
        }
        wj_arr_close(w);
        wj_obj_close(w);
        wj_arr_open(w, "history");
        for (size_t j = 0; j < history_count[i] && j < NTRIP_TX_DIAG_HISTORY; j++) tx_write(w, NULL, &history[i][j]);
        wj_arr_close(w);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}
