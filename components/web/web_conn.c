/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_conn.h"

#include <string.h>

static web_conn_t *find(web_conns_t *t, int fd)
{
    if (fd < 0) {
        return NULL;
    }
    for (size_t i = 0; i < WEB_CONN_MAX; i++) {
        if (t->c[i].fd == fd) {
            return &t->c[i];
        }
    }
    return NULL;
}

static const web_conn_t *find_c(const web_conns_t *t, int fd)
{
    return find((web_conns_t *)t, fd);
}

static web_conn_t *find_free(web_conns_t *t)
{
    for (size_t i = 0; i < WEB_CONN_MAX; i++) {
        if (t->c[i].fd < 0) {
            return &t->c[i];
        }
    }
    return NULL;
}

void web_conns_init(web_conns_t *t, uint32_t deadline_ms)
{
    memset(t, 0, sizeof(*t));
    for (size_t i = 0; i < WEB_CONN_MAX; i++) {
        t->c[i].fd = -1;
    }
    t->deadline_ms = deadline_ms;
}

bool web_conn_open(web_conns_t *t, int fd, uint32_t now_ms)
{
    if (fd < 0) {
        return false;
    }
    web_conn_t *e = find(t, fd);   /* a reused number whose close we missed */
    if (e == NULL) {
        e = find_free(t);
    }
    if (e == NULL) {
        return false;
    }
    memset(e, 0, sizeof(*e));
    e->fd = fd;
    e->phase = WEB_CONN_IDLE;
    e->active_ms = now_ms;
    return true;
}

void web_conn_close(web_conns_t *t, int fd)
{
    web_conn_t *e = find(t, fd);
    if (e != NULL) {
        memset(e, 0, sizeof(*e));
        e->fd = -1;
    }
}

size_t web_conn_count(const web_conns_t *t)
{
    size_t n = 0;
    for (size_t i = 0; i < WEB_CONN_MAX; i++) {
        n += (t->c[i].fd >= 0 && !t->c[i].closing) ? 1u : 0u;
    }
    return n;
}

int32_t web_conn_recv_budget(const web_conns_t *t, int fd, uint32_t now_ms)
{
    const web_conn_t *e = find_c(t, fd);
    if (e == NULL || e->phase != WEB_CONN_RECEIVING) {
        return -1;
    }
    uint32_t used = now_ms - e->armed_ms;
    if (used >= t->deadline_ms) {
        return 0;
    }
    uint32_t left = t->deadline_ms - used;
    return left > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)left;
}

void web_conn_received(web_conns_t *t, int fd, uint32_t now_ms)
{
    web_conn_t *e = find(t, fd);
    if (e != NULL && e->phase == WEB_CONN_IDLE) {
        e->phase = WEB_CONN_RECEIVING;
        e->armed_ms = now_ms;
    }
}

/* ---- request-line scanner ------------------------------------------------------------------ */

static char scan_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* The current header line starts with `name` (lower case, with the colon), case-insensitive. */
static bool hdr_is(const web_scan_t *s, const char *name, size_t *value_at)
{
    size_t n = strlen(name);
    size_t have = s->hdr_len < sizeof(s->hdr) ? s->hdr_len : sizeof(s->hdr);
    if (have < n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (scan_lower(s->hdr[i]) != name[i]) {
            return false;
        }
    }
    *value_at = n;
    return true;
}

/* A header line ended (without its CR LF). */
static void scan_header_line(web_scan_t *s)
{
    size_t at = 0;
    size_t have = s->hdr_len < sizeof(s->hdr) ? s->hdr_len : sizeof(s->hdr);
    if (hdr_is(s, "content-length:", &at)) {
        while (at < have && (s->hdr[at] == ' ' || s->hdr[at] == '\t')) {
            at++;
        }
        uint32_t v = 0;
        bool digits = false;
        bool ok = true;
        while (at < have && s->hdr[at] >= '0' && s->hdr[at] <= '9') {
            uint32_t d = (uint32_t)(s->hdr[at] - '0');
            if (v > (UINT32_MAX - d) / 10u) {
                ok = false;
                break;
            }
            v = v * 10u + d;
            digits = true;
            at++;
        }
        if (!ok || !digits || s->hdr_len > sizeof(s->hdr)) {
            s->st = WEB_SCAN_OFF;   /* cannot tell where the next request starts */
            return;
        }
        s->content_len = v;
    } else if (hdr_is(s, "transfer-encoding:", &at)) {
        s->st = WEB_SCAN_OFF;       /* a chunked body: not followed */
    }
}

void web_scan_feed(web_scan_t *s, const char *buf, size_t len)
{
    size_t i = 0;
    while (i < len && s->st != WEB_SCAN_OFF) {
        if (s->st == WEB_SCAN_BODY) {
            size_t take = len - i < s->body_left ? len - i : s->body_left;
            s->body_left -= (uint32_t)take;
            i += take;
            if (s->body_left == 0) {
                s->st = WEB_SCAN_LINE;
                s->tail_len = 0;
            }
            continue;
        }
        char c = buf[i++];
        if (s->st == WEB_SCAN_LINE) {
            if (c == '\n') {
                if (s->tail_len == 0) {
                    continue;   /* an empty line before a request line (allowed) */
                }
                bool h10 = s->tail_len == sizeof(s->tail) && memcmp(s->tail, "HTTP/1.0", sizeof(s->tail)) == 0;
                if (s->queued >= WEB_SCAN_QUEUE) {
                    s->st = WEB_SCAN_OFF;   /* this many unanswered requests: give up on the order */
                    continue;
                }
                s->queue |= (uint8_t)((h10 ? 1u : 0u) << s->queued);
                s->queued++;
                s->st = WEB_SCAN_HEADERS;
                s->hdr_len = 0;
                s->content_len = 0;
                continue;
            }
            if (c == '\r') {
                continue;
            }
            if (s->tail_len == sizeof(s->tail)) {
                memmove(s->tail, s->tail + 1, sizeof(s->tail) - 1);
                s->tail_len--;
            }
            s->tail[s->tail_len++] = c;
            continue;
        }
        /* WEB_SCAN_HEADERS */
        if (c == '\n') {
            if (s->hdr_len == 0) {
                if (s->content_len > 0) {
                    s->st = WEB_SCAN_BODY;
                    s->body_left = s->content_len;
                } else {
                    s->st = WEB_SCAN_LINE;
                    s->tail_len = 0;
                }
            } else {
                scan_header_line(s);
                s->hdr_len = 0;
            }
            continue;
        }
        if (c == '\r') {
            continue;
        }
        if (s->hdr_len < sizeof(s->hdr)) {
            s->hdr[s->hdr_len] = c;
        }
        if (s->hdr_len < UINT8_MAX) {
            s->hdr_len++;
        }
    }
}

bool web_scan_take(web_scan_t *s)
{
    if (s->queued == 0) {
        return false;
    }
    bool h10 = (s->queue & 1u) != 0;
    s->queue >>= 1;
    s->queued--;
    return h10;
}

void web_conn_scan(web_conns_t *t, int fd, const char *buf, size_t len)
{
    web_conn_t *e = find(t, fd);
    if (e != NULL && !e->ws && buf != NULL) {
        web_scan_feed(&e->scan, buf, len);
    }
}

bool web_conn_begin_request(web_conns_t *t, int fd)
{
    web_conn_t *e = find(t, fd);
    if (e == NULL) {
        return false;
    }
    e->close_after = false;
    e->close_hdr = false;
    e->http10 = web_scan_take(&e->scan);
    return e->http10;
}

bool web_conn_http10(const web_conns_t *t, int fd)
{
    const web_conn_t *e = find_c(t, fd);
    return e != NULL && e->http10;
}

bool web_conn_on_worker(const web_conns_t *t, int fd)
{
    const web_conn_t *e = find_c(t, fd);
    return e != NULL && e->phase == WEB_CONN_ASYNC;
}

void web_conn_set_close_after(web_conns_t *t, int fd, bool on)
{
    web_conn_t *e = find(t, fd);
    if (e != NULL) {
        e->close_after = on;
    }
}

bool web_conn_close_after(const web_conns_t *t, int fd)
{
    const web_conn_t *e = find_c(t, fd);
    return e != NULL && e->close_after;
}

bool web_conn_note_close_hdr(web_conns_t *t, int fd)
{
    web_conn_t *e = find(t, fd);
    if (e == NULL) {
        return false;
    }
    bool had = e->close_hdr;
    e->close_hdr = true;
    return had;
}

void web_conn_done(web_conns_t *t, int fd, uint32_t now_ms, bool admin)
{
    web_conn_t *e = find(t, fd);
    if (e == NULL || e->phase == WEB_CONN_ASYNC) {
        return;
    }
    e->phase = WEB_CONN_IDLE;
    e->active_ms = now_ms;
    if (!e->ws) {
        e->admin = admin;
    }
}

void web_conn_async(web_conns_t *t, int fd, bool on, uint32_t now_ms)
{
    web_conn_t *e = find(t, fd);
    if (e == NULL) {
        return;
    }
    e->phase = on ? WEB_CONN_ASYNC : WEB_CONN_IDLE;
    e->active_ms = now_ms;
}

void web_conn_websocket(web_conns_t *t, int fd, bool admin, uint32_t now_ms)
{
    web_conn_t *e = find(t, fd);
    if (e == NULL) {
        return;
    }
    e->ws = true;
    e->admin = admin;
    e->phase = WEB_CONN_IDLE;
    e->active_ms = now_ms;
    memset(&e->scan, 0, sizeof(e->scan));
    e->scan.st = WEB_SCAN_OFF;   /* frames from here on, no request lines */
}

void web_conn_closing(web_conns_t *t, int fd, bool closing)
{
    web_conn_t *e = find(t, fd);
    if (e != NULL) {
        e->closing = closing;
    }
}

void web_conn_between_requests(web_conns_t *t)
{
    for (size_t i = 0; i < WEB_CONN_MAX; i++) {
        web_conn_t *e = &t->c[i];
        if (e->fd >= 0 && e->phase == WEB_CONN_RECEIVING) {
            e->phase = WEB_CONN_IDLE;   /* armed after its handler: no request runs now */
        }
    }
}

/* Eviction rank (lower goes first); -1 = never. */
static int rank(const web_conn_t *e)
{
    if (e->closing || e->phase != WEB_CONN_IDLE) {
        return -1;   /* already going, or its request runs on the worker */
    }
    if (!e->ws) {
        return e->admin ? 1 : 0;   /* a browser reopens an idle keep-alive connection by itself */
    }
    return e->admin ? -1 : 2;      /* an admin's live page is never cut for someone else's socket */
}

int web_conn_victim(web_conns_t *t, int new_fd, size_t max_open, uint32_t now_ms)
{
    web_conn_between_requests(t);
    if (web_conn_count(t) < max_open) {
        return -1;
    }
    web_conn_t *best = NULL;
    int best_rank = 0;
    uint32_t best_idle = 0;
    for (size_t i = 0; i < WEB_CONN_MAX; i++) {
        web_conn_t *e = &t->c[i];
        if (e->fd < 0 || e->fd == new_fd) {
            continue;
        }
        int r = rank(e);
        if (r < 0) {
            continue;
        }
        uint32_t idle = now_ms - e->active_ms;
        if (best == NULL || r < best_rank || (r == best_rank && idle > best_idle)) {
            best = e;
            best_rank = r;
            best_idle = idle;
        }
    }
    if (best == NULL) {
        best = find(t, new_fd);
        if (best == NULL) {
            return new_fd;   /* not tracked (table full): close it all the same */
        }
    }
    best->closing = true;
    return best->fd;
}
