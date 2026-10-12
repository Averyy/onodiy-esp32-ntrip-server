/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_link_core.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gnss_rtcm3.h"

void gnss_link_core_init(gnss_link_core_t *c, uint32_t baud)
{
    memset(c, 0, sizeof(*c));
    c->mode = GNSS_LINK_MODE_STREAM;
    c->baud = baud;
    c->link_open = true;
}

void gnss_link_core_set_open(gnss_link_core_t *c, bool open)
{
    c->link_open = open;
}

void gnss_link_core_on_open_failed(gnss_link_core_t *c)
{
    c->link_open = false;
    if (c->stats.open_failures < UINT32_MAX) {
        c->stats.open_failures++;
    }
}

uint32_t gnss_link_core_reopen_delay_ms(uint32_t failures)
{
    static const uint32_t k[] = { 1000u, 5000u, 30000u };
    return failures < sizeof(k) / sizeof(k[0]) ? k[failures] : 60000u;
}

uint32_t gnss_link_core_reopen_next_ms(uint32_t failed_before)
{
    return gnss_link_core_reopen_delay_ms(failed_before < UINT32_MAX ? failed_before + 1u : UINT32_MAX);
}

bool gnss_link_core_reopen_log_due(bool logged, int64_t last_log_us, int64_t now_us)
{
    return !logged || now_us - last_log_us >= (int64_t)GNSS_LINK_REOPEN_LOG_MS * 1000;
}

static bool is_lease_mode(gnss_link_mode_t mode)
{
    return mode == GNSS_LINK_MODE_COMMAND || mode == GNSS_LINK_MODE_RAW;
}

static bool holds(const gnss_link_core_t *c, uint32_t token)
{
    return token != 0 && token == c->lease && is_lease_mode(c->mode);
}

esp_err_t gnss_link_core_acquire(gnss_link_core_t *c, gnss_link_mode_t mode, const char *owner, uint32_t *token)
{
    if (token == NULL || !is_lease_mode(mode)) {
        return ESP_ERR_INVALID_ARG;
    }
    *token = 0;
    if (c->mode != GNSS_LINK_MODE_STREAM || c->lease != 0 || c->hands_off) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t next = c->last_token + 1;
    if (next == 0) {
        next = 1;   /* 0 is "no lease"; a wrap takes 2^32 acquisitions */
    }
    c->last_token = next;
    c->lease = next;
    c->mode = mode;
    c->owner = owner;
    c->prompt_armed = false;
    *token = next;
    return ESP_OK;
}

esp_err_t gnss_link_core_set_hands_off(gnss_link_core_t *c, bool on)
{
    if (on && c->lease != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    c->hands_off = on;
    return ESP_OK;
}

esp_err_t gnss_link_core_check(const gnss_link_core_t *c, gnss_link_op_t op, uint32_t token)
{
    if (c->mode == GNSS_LINK_MODE_OFF) {
        return ESP_ERR_INVALID_STATE;
    }
    if (c->hands_off && (op == GNSS_LINK_OP_WRITE || op == GNSS_LINK_OP_BREAK)) {
        return ESP_ERR_INVALID_STATE;   /* nothing may reach the receiver */
    }
    bool ok = false;
    switch (op) {
    case GNSS_LINK_OP_WRITE:
        ok = token == 0 ? c->mode == GNSS_LINK_MODE_STREAM : holds(c, token);
        break;
    case GNSS_LINK_OP_SET_BAUD:
    case GNSS_LINK_OP_BREAK:
    case GNSS_LINK_OP_RELEASE:
        ok = holds(c, token);
        break;
    case GNSS_LINK_OP_PROMPT:
        ok = holds(c, token) && c->mode == GNSS_LINK_MODE_COMMAND;
        break;
    case GNSS_LINK_OP_RESYNC:
    case GNSS_LINK_OP_RESTART:
        ok = token == 0 ? c->mode == GNSS_LINK_MODE_STREAM
                        : holds(c, token) && c->mode == GNSS_LINK_MODE_COMMAND;
        break;
    case GNSS_LINK_OP_RAW_IO:
        ok = holds(c, token) && c->mode == GNSS_LINK_MODE_RAW;
        break;
    }
    return ok ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t gnss_link_core_release(gnss_link_core_t *c, uint32_t token, gnss_link_mode_t *was)
{
    if (!holds(c, token)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (was != NULL) {
        *was = c->mode;
    }
    c->mode = GNSS_LINK_MODE_STREAM;
    c->lease = 0;
    c->owner = NULL;
    c->prompt_armed = false;
    return ESP_OK;
}

esp_err_t gnss_link_core_set_prompt(gnss_link_core_t *c, uint32_t token, bool armed)
{
    esp_err_t err = gnss_link_core_check(c, GNSS_LINK_OP_PROMPT, token);
    if (err == ESP_OK) {
        c->prompt_armed = armed;
    }
    return err;
}

bool gnss_link_core_on_event(gnss_link_core_t *c, gnss_link_ev_t ev)
{
    switch (ev) {
    case GNSS_LINK_EV_FIFO_OVF:
        c->stats.hw_fifo_overflows++;
        c->overflow_seen = true;
        return true;
    case GNSS_LINK_EV_BUFFER_FULL:
        if (c->ring_full_seen) {
            return false;   /* this episode was counted already (a full queue found it first) */
        }
        c->stats.rx_buffer_full++;
        c->ring_full_seen = true;
        return true;
    case GNSS_LINK_EV_FRAME_ERR:
    case GNSS_LINK_EV_PARITY_ERR:
    case GNSS_LINK_EV_BREAK:
        c->stats.line_errors++;
        return false;
    case GNSS_LINK_EV_DATA:
    case GNSS_LINK_EV_OTHER:
        break;
    }
    return false;
}

bool gnss_link_core_ring_full(size_t buffered, size_t ring_size)
{
    return ring_size <= GNSS_LINK_HW_FIFO_LEN || buffered >= ring_size - GNSS_LINK_HW_FIFO_LEN;
}

bool gnss_link_core_on_queue_full(gnss_link_core_t *c, size_t buffered, size_t ring_size, bool count_drop)
{
    bool reset = false;
    if (count_drop && !c->queue_full_seen) {
        c->queue_full_seen = true;
        c->stats.rx_events_dropped++;
        reset = true;
    }
    if (gnss_link_core_ring_full(buffered, ring_size)) {
        if (!c->ring_full_seen) {
            c->ring_full_seen = true;
            c->stats.rx_buffer_full++;
            reset = true;
        }
        if (!c->overflow_seen) {
            c->overflow_seen = true;
            c->stats.hw_fifo_overflows++;
            reset = true;
        }
    }
    return reset;
}

void gnss_link_core_on_queue_ok(gnss_link_core_t *c)
{
    c->queue_full_seen = false;
}

void gnss_link_core_on_level(gnss_link_core_t *c, size_t buffered, size_t ring_size)
{
    if (!gnss_link_core_ring_full(buffered, ring_size)) {
        c->ring_full_seen = false;
        c->overflow_seen = false;
    }
}

void gnss_link_core_on_rx(gnss_link_core_t *c, size_t n, int64_t now_us)
{
    if (n == 0) {
        return;
    }
    c->stats.rx_bytes += n;
    c->rx_seen = true;
    c->last_rx_us = now_us;
}

gnss_link_route_t gnss_link_core_route(gnss_link_core_t *c, gnss_frame_kind_t kind, int64_t now_us)
{
    switch (kind) {
    case GNSS_FRAME_RTCM3:
        c->rtcm_seen = true;
        c->last_rtcm_us = now_us;
        if (c->mode == GNSS_LINK_MODE_STREAM) {
            return GNSS_LINK_ROUTE_PUBLISH;
        }
        if (is_lease_mode(c->mode)) {
            c->stats.rtcm_held++;
            return GNSS_LINK_ROUTE_HOLD;
        }
        return GNSS_LINK_ROUTE_DROP;
    case GNSS_FRAME_TEXT:
        if (c->mode == GNSS_LINK_MODE_STREAM || c->mode == GNSS_LINK_MODE_COMMAND) {
            return GNSS_LINK_ROUTE_LINE;
        }
        return GNSS_LINK_ROUTE_DROP;
    case GNSS_FRAME_PROMPT:
        if (c->mode == GNSS_LINK_MODE_COMMAND && c->prompt_armed) {
            return GNSS_LINK_ROUTE_PROMPT;
        }
        if (c->observe && (c->mode == GNSS_LINK_MODE_STREAM || c->mode == GNSS_LINK_MODE_COMMAND)) {
            /* Report-only: counted, never to the prompt sink. */
            if (c->obs_hits < UINT32_MAX) {
                c->obs_hits++;
            }
            c->obs_hit_seen = true;
            c->obs_hit_us = now_us;
            c->obs_ctx_len = 0;
            return GNSS_LINK_ROUTE_OBSERVE;
        }
        return GNSS_LINK_ROUTE_DROP;
    }
    return GNSS_LINK_ROUTE_DROP;
}

void gnss_link_core_on_tx(gnss_link_core_t *c, size_t n)
{
    c->stats.tx_bytes += n;
}

void gnss_link_core_on_baud(gnss_link_core_t *c, uint32_t baud, size_t discarded)
{
    c->baud = baud;
    c->stats.baud_switch_bytes += discarded;
}

void gnss_link_core_on_flush(gnss_link_core_t *c, size_t n)
{
    c->stats.flushed_bytes += n;
}

void gnss_link_core_set_framing(gnss_link_core_t *c, const gnss_framing_stats_t *f)
{
    c->stats.framing = *f;
}

uint32_t gnss_link_core_age_ms(bool seen, int64_t stamp_us, int64_t now_us)
{
    if (!seen) {
        return UINT32_MAX;
    }
    if (now_us <= stamp_us) {
        return 0;
    }
    int64_t age_ms = (now_us - stamp_us) / 1000;
    if (age_ms >= (int64_t)UINT32_MAX) {
        return UINT32_MAX - 1;   /* UINT32_MAX means "never" */
    }
    return (uint32_t)age_ms;
}

void gnss_link_core_snapshot(const gnss_link_core_t *c, int64_t now_us, gnss_link_stats_t *out)
{
    *out = c->stats;
    out->mode = c->mode;
    out->baud = c->baud;
    out->last_rx_age_ms = gnss_link_core_age_ms(c->rx_seen, c->last_rx_us, now_us);
    out->last_rtcm_age_ms = gnss_link_core_age_ms(c->rtcm_seen, c->last_rtcm_us, now_us);
    out->hands_off = c->hands_off;
    out->link_open = c->mode != GNSS_LINK_MODE_OFF && c->link_open;
}

bool gnss_link_core_baud_supported(uint32_t baud)
{
    static const uint32_t rates[] = { 9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 };
    for (size_t i = 0; i < sizeof(rates) / sizeof(rates[0]); i++) {
        if (rates[i] == baud) {
            return true;
        }
    }
    return false;
}

uint32_t gnss_link_core_open_baud(esp_err_t stored_err, uint32_t stored, uint32_t fallback)
{
    if (stored_err == ESP_OK && gnss_link_core_baud_supported(stored)) {
        return stored;
    }
    if (gnss_link_core_baud_supported(fallback)) {
        return fallback;
    }
    return 115200;
}

int gnss_link_core_rx_full_threshold(uint32_t baud)
{
    /* PROVISIONAL, to be confirmed on hardware: the driver default at every
     * baud. If 460800 is adopted for streaming and the bench then shows FIFO
     * overflows, this becomes 64 B above 115200, here and nowhere else. */
    (void)baud;
    return GNSS_LINK_RX_FULL_THRESH_DEFAULT;
}

uint8_t gnss_link_core_rx_timeout(uint32_t baud)
{
    (void)baud;
    return GNSS_LINK_RX_TIMEOUT_DEFAULT;
}

bool gnss_link_core_break_ok(uint32_t duration_ms)
{
    return duration_ms >= 1 && duration_ms <= GNSS_LINK_BREAK_MAX_MS;
}

void gnss_link_pass_begin(gnss_link_pass_t *p)
{
    p->done = 0;
    p->capped = false;
}

size_t gnss_link_pass_next(gnss_link_pass_t *p, size_t buffered)
{
    if (buffered == 0) {
        return 0;
    }
    if (p->done >= GNSS_LINK_DRAIN_CAP) {
        p->capped = true;
        return 0;
    }
    size_t n = buffered;
    if (n > GNSS_LINK_READ_CHUNK) {
        n = GNSS_LINK_READ_CHUNK;
    }
    if (n > GNSS_LINK_DRAIN_CAP - p->done) {
        n = GNSS_LINK_DRAIN_CAP - p->done;
    }
    return n;
}

void gnss_link_pass_add(gnss_link_pass_t *p, size_t n)
{
    p->done += n;
}

uint32_t gnss_link_pass_wait_ms(const gnss_link_pass_t *p)
{
    return p->capped ? 0 : GNSS_LINK_EVENT_WAIT_MS;
}

/* ---- report-only prompt observer --------------------------------------------- */

void gnss_link_core_set_observe(gnss_link_core_t *c, bool on)
{
    c->observe = on;
    c->obs_chunk = NULL;
    c->obs_chunk_len = 0;
    if (!on) {
        c->obs_hist_len = 0;   /* a later start keeps no stale bytes */
    }
}

bool gnss_link_core_detector_on(const gnss_link_core_t *c)
{
    return (c->mode == GNSS_LINK_MODE_COMMAND && c->prompt_armed) || c->observe;
}

void gnss_link_core_observe_feed(gnss_link_core_t *c, const uint8_t *data, size_t len, uint64_t at)
{
    if (!c->observe) {
        return;
    }
    if (c->obs_hist_end != at) {
        c->obs_hist_len = 0;   /* not right before this chunk: of no use for its context */
    }
    c->obs_chunk = data;
    c->obs_chunk_len = len;
    c->obs_chunk_at = at;
}

void gnss_link_core_observe_fed(gnss_link_core_t *c)
{
    if (c->obs_chunk == NULL) {
        return;
    }
    const size_t cap = GNSS_LINK_PROMPT_CTX;
    size_t len = c->obs_chunk_len;
    if (len >= cap) {
        memcpy(c->obs_hist, c->obs_chunk + len - cap, cap);
        c->obs_hist_len = (uint8_t)cap;
    } else {
        size_t keep = c->obs_hist_len;
        if (keep + len > cap) {
            keep = cap - len;
        }
        memmove(c->obs_hist, c->obs_hist + c->obs_hist_len - keep, keep);
        memcpy(c->obs_hist + keep, c->obs_chunk, len);
        c->obs_hist_len = (uint8_t)(keep + len);
    }
    c->obs_hist_end = c->obs_chunk_at + len;
    c->obs_chunk = NULL;
    c->obs_chunk_len = 0;
}

/* The byte at stream offset o, if the observer still holds it (-1 otherwise). */
static int obs_byte(const gnss_link_core_t *c, uint64_t o)
{
    if (c->obs_chunk != NULL && o >= c->obs_chunk_at && o - c->obs_chunk_at < c->obs_chunk_len) {
        return c->obs_chunk[o - c->obs_chunk_at];
    }
    if (o < c->obs_hist_end && c->obs_hist_end - o <= c->obs_hist_len) {
        return c->obs_hist[c->obs_hist_len - (size_t)(c->obs_hist_end - o)];
    }
    return -1;
}

void gnss_link_core_observe_hit(gnss_link_core_t *c, uint64_t at)
{
    uint8_t tmp[GNSS_LINK_PROMPT_CTX];
    size_t n = 0;
    while (n < GNSS_LINK_PROMPT_CTX && n <= at) {
        int b = obs_byte(c, at - n);
        if (b < 0) {
            break;
        }
        tmp[GNSS_LINK_PROMPT_CTX - 1 - n] = (uint8_t)b;
        n++;
    }
    memcpy(c->obs_ctx, tmp + GNSS_LINK_PROMPT_CTX - n, n);
    c->obs_ctx_len = (uint8_t)n;
}

void gnss_link_core_observe_get(const gnss_link_core_t *c, gnss_link_prompt_obs_t *out)
{
    memset(out, 0, sizeof(*out));
    out->on = c->observe;
    out->hits = c->obs_hits;
    out->last_hit_ms = c->obs_hit_seen ? c->obs_hit_us / 1000 : -1;
    memcpy(out->context, c->obs_ctx, c->obs_ctx_len);
    out->context_len = c->obs_ctx_len;
}

/* ---- timing recorder: summary and CSV ------------------------------------------ */

static int cmp_i32(const void *a, const void *b)
{
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

/* Nearest rank: the smallest value with at least permille / 1000 of the values at or below it. */
static int32_t rank(const int32_t *v, size_t n, unsigned permille)
{
    size_t k = (size_t)(((uint64_t)n * permille + 999u) / 1000u);
    return v[k > 0 ? k - 1 : 0];
}

void gnss_link_pct(int32_t *v, size_t n, gnss_link_pct_t *out)
{
    memset(out, 0, sizeof(*out));
    if (n == 0) {
        return;
    }
    qsort(v, n, sizeof(v[0]), cmp_i32);
    out->n = (uint32_t)n;
    out->p50 = rank(v, n, 500);
    out->p95 = rank(v, n, 950);
    out->p99 = rank(v, n, 990);
    out->p999 = rank(v, n, 999);
    out->max = v[n - 1];
}

static int32_t clamp_i32(int64_t v)
{
    return v > INT32_MAX ? INT32_MAX : (v < INT32_MIN ? INT32_MIN : (int32_t)v);
}

static uint32_t clamp_u32(int64_t v)
{
    return v <= 0 ? 0 : (v > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)v);
}

static bool published_frame(const gnss_link_rec_t *r)
{
    return r->kind == GNSS_LINK_REC_FRAME && (r->flags & GNSS_LINK_REC_F_HELD) == 0;
}

/* After the RX stall at rec[i]: the zero-wait capped passes until a pass emptied the ring, how long that took
 * from the stall's end, and the first frame published after it (the most over the stalls). */
static void after_stall(const gnss_link_rec_t *rec, size_t n, size_t i, gnss_link_timing_summary_t *out)
{
    int64_t end = rec[i].t0_us + (int64_t)rec[i].dt_us;
    uint32_t capped = 0;
    bool drained = false;
    bool frame = false;
    for (size_t k = i + 1; k < n && (!drained || !frame); k++) {
        const gnss_link_rec_t *r = &rec[k];
        if (r->kind == GNSS_LINK_REC_STALL) {
            break;
        }
        if (!drained && r->kind == GNSS_LINK_REC_PASS) {
            if (r->flags & GNSS_LINK_REC_F_CAPPED) {
                capped++;
            } else {
                drained = true;
                uint32_t us = clamp_u32(r->t0_us + (int64_t)r->dt_us - end);
                if (us > out->stall_drain_us) {
                    out->stall_drain_us = us;
                }
            }
        }
        if (!frame && published_frame(r)) {
            frame = true;
            uint32_t us = clamp_u32(r->t0_us + (int64_t)r->dt_us - end);
            if (!out->stall_first_frame_valid || us > out->stall_first_frame_us) {
                out->stall_first_frame_us = us;
            }
            out->stall_first_frame_valid = true;
        }
    }
    if (capped > out->stall_capped_passes) {
        out->stall_capped_passes = capped;
    }
}

void gnss_link_timing_summary(const gnss_link_rec_t *rec, size_t n, bool full, bool clock_ok,
                              int64_t unix_minus_timer_us, int32_t *scratch, size_t scratch_cap,
                              gnss_link_timing_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->records = (uint32_t)n;
    out->full = full;
    out->clock = clock_ok;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        const gnss_link_rec_t *r = &rec[i];
        switch (r->kind) {
        case GNSS_LINK_REC_FRAME:
            out->frames++;
            if (published_frame(r) && m < scratch_cap) {
                scratch[m++] = clamp_i32(r->dt_us);
            }
            break;
        case GNSS_LINK_REC_PASS:
            if (r->bytes > 0) {
                out->passes++;
                if (r->gap_us > out->longest_gap_us) {
                    out->longest_gap_us = r->gap_us;
                }
            }
            break;
        case GNSS_LINK_REC_STALL:
            out->stalls++;
            if ((r->flags & GNSS_LINK_REC_F_RAW) == 0) {
                after_stall(rec, n, i, out);
            }
            break;
        default:
            break;
        }
    }
    gnss_link_pct(scratch, m, &out->read_to_pub_us);

    if (!clock_ok) {
        return;   /* no epoch delays without the SNTP-set clock */
    }
    /* Each epoch's first frame per constellation: its read time against its MSM time. */
    for (unsigned sys = 0; sys < GNSS_SYS_COUNT; sys++) {
        bool have = false;
        uint32_t last = 0;
        m = 0;
        for (size_t i = 0; i < n; i++) {
            const gnss_link_rec_t *r = &rec[i];
            if (r->kind != GNSS_LINK_REC_FRAME || (r->flags & GNSS_LINK_REC_F_MSM) == 0 || r->sys != sys) {
                continue;
            }
            if (have && r->epoch == last) {
                continue;   /* a later MSM of the same epoch */
            }
            have = true;
            last = r->epoch;
            int64_t unix_ms = (r->t0_us + unix_minus_timer_us) / 1000;
            int64_t gps_now = unix_ms + (int64_t)GNSS_RTCM3_GPS_UTC_LEAP_S * 1000 - GNSS_RTCM3_GPS_EPOCH_UNIX_MS;
            int64_t gps_epoch = 0;
            if (gnss_rtcm3_epoch_gps_ms((gnss_sys_t)sys, r->epoch, gps_now, &gps_epoch) && m < scratch_cap) {
                scratch[m++] = clamp_i32(gps_now - gps_epoch);
            }
        }
        gnss_link_pct(scratch, m, &out->epoch_delay_ms[sys]);
    }
}

static const char *const k_sys_csv[GNSS_SYS_COUNT] = { "gps", "glonass", "galileo", "sbas", "qzss", "beidou", "navic" };

size_t gnss_link_rec_csv(const gnss_link_rec_t *r, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    int64_t end = r->t0_us + (int64_t)r->dt_us;
    int n;
    switch (r->kind) {
    case GNSS_LINK_REC_FRAME: {
        bool msm = (r->flags & GNSS_LINK_REC_F_MSM) != 0 && r->sys < GNSS_SYS_COUNT;
        char epoch[12] = "";
        if (msm) {
            snprintf(epoch, sizeof(epoch), "%" PRIu32, r->epoch);
        }
        n = snprintf(buf, cap, "F,%" PRId64 ",%" PRId64 ",%u,%u,%s,%s,%u,%u\n", r->t0_us, end, (unsigned)r->msg,
                     (unsigned)r->bytes, msm ? k_sys_csv[r->sys] : "", epoch,
                     (r->flags & GNSS_LINK_REC_F_MMB) ? 1u : 0u, (r->flags & GNSS_LINK_REC_F_HELD) ? 1u : 0u);
        break;
    }
    case GNSS_LINK_REC_PASS:
        n = snprintf(buf, cap, "P,%" PRId64 ",%" PRId64 ",%u,%u,%u,%" PRIu32 "\n", r->t0_us, end, (unsigned)r->bytes,
                     (r->flags & GNSS_LINK_REC_F_CAPPED) ? 1u : 0u, (unsigned)r->wait_ms, r->gap_us);
        break;
    case GNSS_LINK_REC_STALL:
        n = snprintf(buf, cap, "S,%" PRId64 ",%" PRId64 ",%s\n", r->t0_us, end,
                     (r->flags & GNSS_LINK_REC_F_RAW) ? "raw" : "rx");
        break;
    default:
        return 0;
    }
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}
