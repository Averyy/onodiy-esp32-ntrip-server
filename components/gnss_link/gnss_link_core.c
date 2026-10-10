/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_link_core.h"

#include <string.h>

void gnss_link_core_init(gnss_link_core_t *c, uint32_t baud)
{
    memset(c, 0, sizeof(*c));
    c->mode = GNSS_LINK_MODE_STREAM;
    c->baud = baud;
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
    if (c->mode != GNSS_LINK_MODE_STREAM || c->lease != 0) {
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

esp_err_t gnss_link_core_check(const gnss_link_core_t *c, gnss_link_op_t op, uint32_t token)
{
    if (c->mode == GNSS_LINK_MODE_OFF) {
        return ESP_ERR_INVALID_STATE;
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
        return true;
    case GNSS_LINK_EV_BUFFER_FULL:
        c->stats.rx_buffer_full++;
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
