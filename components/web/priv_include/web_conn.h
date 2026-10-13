/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Per-connection bookkeeping of the HTTP server (pure logic, no IDF or RTOS
 * includes, host-testable). web.c keeps one table under a spinlock and feeds
 * it from the server's open and close callbacks, its receive override, the
 * handlers and the worker.
 *
 * Request deadline. esp_http_server reads a request (request line, headers,
 * a JSON body, a WebSocket frame) on its one task with only a per-receive
 * timeout, so a client sending a byte every few seconds would hold that task,
 * and everything queued behind it, for as long as it likes. Here the first
 * byte of a request arms a deadline (`deadline_ms` after it) and every
 * receive until the request is done may wait only for what is left of it;
 * then the receive reports a timeout and the server closes the connection.
 * The handler's end, or the WebSocket handshake, disarms it. Nothing should
 * read after that: every handler reads its request's body, hands it to the
 * worker, or answers and closes the connection, since the server would read
 * a body left over (and re-arm the deadline) only after the handler. Should
 * a receive still come after it, the arm it causes is stale, and the server
 * task drops it the next time it is certainly between requests
 * (web_conn_between_requests: a new connection, the web tick), so it never
 * keeps a connection from being closed and its next request starts afresh.
 * A request handed to the worker (uploads, downloads) is not bounded: the
 * worker reads large bodies at the client's pace with its own limits, off
 * the server task.
 *
 * Eviction. With lru_purge_enable, esp_http_server closes the connection
 * whose last request is oldest when a new one arrives with every socket in
 * use; an admin's WebSocket that receives nothing would be that one, so any
 * client opening sockets could cut it off. Instead the server keeps one
 * socket free: when a new connection takes the last one, another is closed
 * right away, chosen here in this order: an idle connection that did not
 * carry an admin session, an idle one that did, a public WebSocket viewer;
 * oldest activity first within each. An admin's WebSocket, a connection
 * whose request runs on the worker and one already closing are never chosen;
 * when nothing else is left, the new connection itself is closed. The choice
 * is made on the server task between requests, so no connection is in the
 * middle of one then (an armed deadline there is stale and dropped first).
 *
 * HTTP version. esp_http_server parses each request's version but does not
 * keep it, so an HTTP/1.0 request could not be told from an HTTP/1.1 one,
 * and HTTP/1.0 connections are not persistent unless the client asks. A
 * small scanner per connection reads the bytes the receive override sees
 * (request line, header lines, Content-Length bytes of body) and notes for
 * every request line whether it ends in "HTTP/1.0", in order, so pipelined
 * requests keep their own flag. The handler of a request takes the oldest
 * flag (web_conn_begin_request) and decides whether to close after its
 * answer; the worker finds that decision in the table. After a WebSocket
 * handshake it stops (frames follow, no request lines); after a request the
 * scanner cannot follow (a chunked body) it stops too, and that request's
 * handler answers with "Connection: close" and ends the connection, since
 * nothing reads such a body (web_req_body_of). Every HTTP/1.0 request ends
 * its connection (web_conn_should_close), and its flag stays with the
 * connection while the request is answered (web_conn_http10), so an answer
 * that is streamed (a large JSON body, a download) goes without chunked
 * framing, which HTTP/1.0 does not know: its body ends where the
 * connection does, or at its Content-Length when the size is known.
 *
 * Times are milliseconds of an injected monotonic clock, compared only as
 * (uint32_t)(now - then), so they survive the clock wrap.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Connections tracked (at least the server's max_open_sockets). */
#define WEB_CONN_MAX 8

typedef enum {
    WEB_CONN_IDLE = 0,       /**< between requests: the next byte arms the deadline */
    WEB_CONN_RECEIVING,      /**< a request is being received on the server task (stale between requests) */
    WEB_CONN_ASYNC,          /**< its request runs on the worker: not bounded, never evicted */
} web_conn_phase_t;

/** Request lines noted and not taken yet, per connection. */
#define WEB_SCAN_QUEUE 8u

typedef enum {
    WEB_SCAN_LINE = 0,       /**< before or in a request line */
    WEB_SCAN_HEADERS,        /**< in the header lines */
    WEB_SCAN_BODY,           /**< in a Content-Length body */
    WEB_SCAN_OFF,            /**< not followed (WebSocket, chunked body, overflow) */
} web_scan_state_t;

typedef struct {
    web_scan_state_t st;
    uint8_t tail_len;
    char tail[8];            /**< the last bytes of the request line ("HTTP/1.0") */
    uint8_t hdr_len;         /**< bytes of the current header line (saturating) */
    char hdr[32];            /**< its first bytes */
    uint32_t content_len;    /**< of the current request */
    uint32_t body_left;
    uint8_t queue;           /**< HTTP/1.0 flags of noted request lines, oldest in bit 0 */
    uint8_t queued;
} web_scan_t;

/**
 * Feed bytes a connection received. Pure: also used on its own (tests).
 */
void web_scan_feed(web_scan_t *s, const char *buf, size_t len);

/** Take the oldest noted request line: true when it was HTTP/1.0 (false when none is noted). */
bool web_scan_take(web_scan_t *s);

typedef struct {
    int fd;                  /**< -1 = free entry */
    web_conn_phase_t phase;
    uint32_t armed_ms;       /**< RECEIVING: when the request's first byte arrived */
    uint32_t active_ms;      /**< opened, or its last request ended */
    bool ws;                 /**< a WebSocket client */
    bool admin;              /**< its last request (or the WebSocket) had an admin session */
    bool closing;            /**< its close is under way */
    web_scan_t scan;         /**< request lines received (HTTP version) */
    bool close_after;        /**< the request being answered ends the connection */
    bool close_hdr;          /**< its answer carries "Connection: close" already */
    bool http10;             /**< the request being answered is HTTP/1.0 (its streamed answer is not chunked) */
} web_conn_t;

typedef struct {
    web_conn_t c[WEB_CONN_MAX];
    uint32_t deadline_ms;
} web_conns_t;

/** Empty table; every request must be in within `deadline_ms` of its first byte. */
void web_conns_init(web_conns_t *t, uint32_t deadline_ms);

/** A connection opened. @return false when the table is full (it is not tracked). */
bool web_conn_open(web_conns_t *t, int fd, uint32_t now_ms);

/** A connection closed (unknown fds are ignored). */
void web_conn_close(web_conns_t *t, int fd);

/** Connections tracked that are not closing. */
size_t web_conn_count(const web_conns_t *t);

/**
 * Before a receive: how long it may wait, in ms. -1 = no limit of ours (not
 * tracked, idle, or on the worker; the socket's own timeout applies), 0 =
 * the request's time is up (report a timeout), > 0 = at most this long.
 */
int32_t web_conn_recv_budget(const web_conns_t *t, int fd, uint32_t now_ms);

/** Bytes arrived: the first of a request arms its deadline. */
void web_conn_received(web_conns_t *t, int fd, uint32_t now_ms);

/** The bytes themselves, for the request-line scanner (WebSocket connections are not scanned). */
void web_conn_scan(web_conns_t *t, int fd, const char *buf, size_t len);

/**
 * A handler starts on the request (server task): takes its request line's
 * flag, keeps it for the request's lifetime (web_conn_http10) and resets
 * the close decision. @return true for HTTP/1.0.
 */
bool web_conn_begin_request(web_conns_t *t, int fd);

/**
 * The request being answered on `fd` is HTTP/1.0 (as its handler found it,
 * until the next request's handler starts; false for an unknown fd). A
 * streamed answer to it goes without chunked framing.
 */
bool web_conn_http10(const web_conns_t *t, int fd);

/** Its request runs on the worker (web_conn_async on). */
bool web_conn_on_worker(const web_conns_t *t, int fd);

/** The request being answered ends the connection after its answer (`on`). */
void web_conn_set_close_after(web_conns_t *t, int fd, bool on);
bool web_conn_close_after(const web_conns_t *t, int fd);

/**
 * Note that the answer carries "Connection: close". @return true when it
 * was noted before (the header is set already); false for an unknown fd.
 */
bool web_conn_note_close_hdr(web_conns_t *t, int fd);

/**
 * The request's handler finished on the server task (its body was read or
 * the connection is closing): the deadline is disarmed. `admin`: the
 * request had an admin session (ignored for a WebSocket, whose owner is
 * fixed at the handshake). A connection on the worker stays there.
 */
void web_conn_done(web_conns_t *t, int fd, uint32_t now_ms, bool admin);

/** The request moved to the worker (`on`), or the worker finished it. */
void web_conn_async(web_conns_t *t, int fd, bool on, uint32_t now_ms);

/** The handshake made it a WebSocket client (of an admin session or a public viewer); disarms. */
void web_conn_websocket(web_conns_t *t, int fd, bool admin, uint32_t now_ms);

/** Its close is under way (it no longer counts, and is not chosen again), or could not be started after all. */
void web_conn_closing(web_conns_t *t, int fd, bool closing);

/**
 * The server task is between requests (its open callback, work queued to
 * it): no request is being received now, so a deadline still armed belongs
 * to one that is over (a receive after its handler) and is dropped.
 * Connections on the worker keep their state.
 */
void web_conn_between_requests(web_conns_t *t);

/**
 * After `new_fd` opened (server task, between requests: stale deadlines are
 * dropped first, as web_conn_between_requests does): the connection to close
 * so that one of `max_open` sockets stays free, marked closing; -1 when none
 * needs to go. It is `new_fd` itself when nothing else may be closed.
 */
int web_conn_victim(web_conns_t *t, int new_fd, size_t max_open, uint32_t now_ms);

#ifdef __cplusplus
}
#endif
