/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 * HTTP callbacks execute on the HTTP task. Request, body, query and writer
 * pointers are valid only until the handler returns. Submit bounded copied
 * jobs to the worker for blocking I/O; never retain these pointers yourself.
 * Handler errors must answer or return an HTTP-server error to close the
 * connection. Every registered route passes through shared prechecks.
 */
#pragma once
#include "cJSON.h"
#include "esp_http_server.h"
#include "web_shell_errors.h"
#include "web_shell_error_build.h"
#include "web_json.h"
#include "web_shell_provider.h"

#ifdef __cplusplus
extern "C" {
#endif
#define WEB_JSON_MAX 2048u
#define WEB_JSON_MAX_CA 6144u
#define WEB_JSON_MAX_LCAST 512u

/* ---- server ---------------------------------------------------------------------------- */

/** Running server, NULL when stopped. */
httpd_handle_t web_server(void);

/** Monotonic seconds (esp_timer). */
uint32_t web_now_s(void);

/** Public status setting (cached; refreshed on change). */
bool web_public_status(void);
void web_public_status_set_cached(bool on);

/** Hostname in use (DHCP / mDNS name of the station interface). */
const char *web_hostname_active(void);

/**
 * The device's name as the pages show it (header, tab title): the configured
 * backend hostname, also before the restart that puts a new one in use, else
 * the name in use. Any task.
 */
void web_shell_name(char *out, size_t size);

/** A name just stored (PUT /api/system): shown from now on. */
void web_shell_name_set(const char *name);

/** HTTP task only: immediately copy/enqueue progress to admin subscribers.
 * JSON must be a complete bounded progress message; used by product ticks. */
void web_shell_http_progress(const char *json);

/** Queue the periodic work now (events, progress) instead of waiting for the next tick. */
void web_kick(void);

/* ---- requests and responses ------------------------------------------------------------- */

typedef enum {
    WEB_BODY_NONE = 0,   /**< GET, DELETE */
    WEB_BODY_JSON,       /**< POST, PUT: application/json, parsed */
    WEB_BODY_UPLOAD,     /**< raw octet stream, read by the handler */
} web_body_t;

/** One request after the checks. */
typedef struct {
    httpd_req_t *req;
    int sess;            /**< session index, -1 = none */
    bool admin;          /**< an admin session (WEB_ACC_PUBSTAR: false = public form) */
    bool on_ap;          /**< arrived over the device's own AP */
    cJSON *body;         /**< WEB_BODY_JSON: the parsed object */
    void *ctx;          /**< descriptor handler context; never owned by the request */
    char query[96];      /**< query string ("" = none) */
} web_req_t;

typedef esp_err_t (*web_handler_t)(web_req_t *r);

typedef struct {
    const char *uri;
    httpd_method_t method;
    web_access_t access;
    web_body_t body;
    size_t max_body;     /**< JSON bodies; 0 = WEB_JSON_MAX */
    web_handler_t fn;
    void *ctx;
    bool (*path_known)(void *ctx, const char *path); /**< optional wildcard concrete-path test */
} web_route_t;

/**
 * URI handlers the server registers room for: every REST route plus the
 * shared REST routes, WebSocket and catch-all. Registration validates the
 * configured product capacity before start.
 */
#define WEB_MAX_URI_HANDLERS 84
#define WEB_EXTRA_URI_HANDLERS 8   /* six shared REST routes, /ws and catch-all */

/** True if `path` (no query) is one of the API paths (for 405 versus 404). */
bool web_shell_path_known(const char *path);

/** Response headers every API and page response carries (no-store for the API). */
void web_set_common_headers(httpd_req_t *req, bool api);

/** Error response with the common body. Returns ESP_OK (the request is answered). */
esp_err_t web_send_error(httpd_req_t *req, web_err_t err, const char *message, const char *field, uint32_t retry_in_s,
                         const char *by, const char *reason);
esp_err_t web_send_err(httpd_req_t *req, web_err_t err);
/**
 * Answer with `err` and close the connection instead of reading a body the
 * handler refuses (Connection: close). Returns ESP_FAIL: the handler returns
 * it, so the server closes the connection rather than read the body away.
 */
esp_err_t web_refuse_and_close(httpd_req_t *req, web_err_t err);
/** web_refuse_and_close() with a retry_in_s (UINT32_MAX = null). */
esp_err_t web_refuse_and_close_retry(httpd_req_t *req, web_err_t err, uint32_t retry_in_s);
/**
 * After an answer that closes the connection (Connection: close): end our
 * side (shutdown SHUT_WR) and read and drop what the client still sends,
 * at most 64 KB or 2 s, so it reads the answer instead of a reset; never
 * the client's FIN (web_conn_drain).
 *
 * Nothing may read the socket after it: the caller ends the request with
 * ESP_FAIL, and the server then deletes the session without reading
 * (httpd_sess_process stops before httpd_req_delete) and its close() is a
 * full close; on the worker, the caller shuts both sides (web_conn_shut)
 * before it hands the request back. A caller that returned ESP_OK instead
 * would have the server read away what is left of the announced body
 * (httpd_req_delete). The drain took those bytes off the socket itself, so
 * that read would find the client's FIN and take it, the very read the
 * drain avoids: with our FIN out, it lets lwIP free the connection behind
 * the socket (web_shell.c, web_linger_close).
 */
void web_linger_close(httpd_req_t *req);
/** 500 internal with the esp_err_t name as message. */
esp_err_t web_send_internal(httpd_req_t *req, esp_err_t err);

/**
 * The request's body (web_req_body): a Content-Length, or any
 * Transfer-Encoding header, which esp_http_server reports as no body at all
 * (content_len 0) while the chunks wait on the socket. Every handler treats
 * anything but WEB_REQ_BODY_NONE as a body: read it, or answer with
 * Connection: close and close; never answer as if there were none.
 */
web_req_body_t web_req_body_of(httpd_req_t *req);

/** Send a short literal JSON body ("{\"ok\": true}"). */
esp_err_t web_send_json_literal(httpd_req_t *req, int status, const char *json);

/**
 * A streamed answer: a JSON answer larger than its buffer (web_resp_t), an
 * upload's result, a download. To an HTTP/1.1 request it goes chunked, as
 * httpd_resp_send_chunk() sends it. HTTP/1.0 knows no chunked framing, so
 * an answer to an HTTP/1.0 request gets a head of our own (web_stream_head:
 * the status line, Content-Type, the common headers, Content-Disposition of
 * a download, "Connection: close", Content-Length when `content_length` >= 0)
 * and the body as it is: it ends at Content-Length, or where the connection
 * ends (an HTTP/1.0 connection always ends after its answer: the
 * close-after logic of the server task and of the worker closes it).
 *
 * Invariant: a streamed answer carries no header besides these. The
 * HTTP/1.0 head is written here, not by the server, so headers set on the
 * request (httpd_resp_set_hdr) would go out on HTTP/1.1 only. Cookies are
 * therefore only ever set on literal answers (web_send_json_literal), which
 * the server sends with every header it was given.
 */
typedef struct {
    httpd_req_t *req;
    bool raw;            /**< HTTP/1.0: our own head, the body unframed */
    bool failed;         /**< a send failed: the rest is not sent */
} web_stream_t;

/** Start a streamed answer (an API answer: Cache-Control: no-store; `disposition` NULL = none). */
esp_err_t web_stream_begin(web_stream_t *s, httpd_req_t *req, int status, const char *type, const char *disposition,
                           int64_t content_length);
/** Send part of its body (nothing for len 0). */
esp_err_t web_stream_write(web_stream_t *s, const void *data, size_t len);
/** End it: the last chunk (HTTP/1.1), nothing (HTTP/1.0: the connection's end ends the body). */
esp_err_t web_stream_end(web_stream_t *s);

/** The request being answered is HTTP/1.0 (web_conn_http10). Any task. */
bool web_conn_req_http10(httpd_req_t *req);

/**
 * JSON response through the streaming writer: a streamed answer
 * (web_stream_t) once the buffer fills, a literal Content-Length response
 * otherwise. `buf` is the caller's (the server task uses web_http_buf()).
 */
typedef struct {
    wj_t w;
    httpd_req_t *req;
    int status;
    bool headers_sent;   /**< the stream started */
    bool send_failed;
    web_stream_t stream;
} web_resp_t;

void web_resp_begin(web_resp_t *r, httpd_req_t *req, int status, char *buf, size_t cap);
esp_err_t web_resp_end(web_resp_t *r);

/** The server task's 2 KB PSRAM response buffer (server task only). */
char *web_http_buf(size_t *cap);

/**
 * Interface of the request: true when its local address is the AP's (its
 * IPv4 address, also v4-mapped, or its IPv6 link-local address).
 */
bool web_req_on_ap(httpd_req_t *req);

/** The request's peer address. @return false when the socket cannot tell. */
bool web_req_peer(httpd_req_t *req, web_addr_t *out);

/** The peer address as text for the log (web_addr_text; "?" when unknown). */
void web_peer_text(httpd_req_t *req, char *out, size_t cap);

/**
 * "Connection: close" on this request's answer, once (a refusal and the
 * client's own "Connection: close" can both ask for it). Any task.
 */
void web_resp_close_hdr(httpd_req_t *req);

/**
 * The request being answered ends its connection after the answer: the
 * client asked ("Connection: close"), or it is HTTP/1.0 (never kept,
 * web_conn_should_close). The worker closes such a connection when it has
 * answered. Any task.
 */
bool web_conn_req_close(httpd_req_t *req);

/** Request checks of a WebSocket handshake (Host, Origin); false = answered already. */
bool web_ws_precheck(httpd_req_t *req);

/* ---- connections (web_shell.c, around web_conn.h) ---------------------------------------------- */

/** A handler on the server task finished its request (`admin`: it had an admin session). */
void web_conn_request_done(httpd_req_t *req, bool admin);

/** The handshake made `fd` a WebSocket client (of an admin session, or a public viewer). */
void web_conn_ws_open(int fd, bool admin);

/** A WebSocket client's close is under way. */
void web_conn_ws_closing(int fd);

/**
 * Close `fd`'s connection now (server task only, or the worker for the
 * request it holds before completing it, or stop under the worker ownership
 * lock): its socket is shut down, and the
 * server deletes exactly that session at its next select(). Used instead of
 * httpd_sess_trigger_close(), whose queued close names a session slot that
 * a new connection may have taken by the time it runs.
 */
void web_conn_shut(int fd);

/** The worker took over `fd`'s request (`on`), or finished it (any task). */
void web_conn_worker(int fd, bool on);

#ifdef __cplusplus
}
#endif
