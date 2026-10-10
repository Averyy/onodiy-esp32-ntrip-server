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
