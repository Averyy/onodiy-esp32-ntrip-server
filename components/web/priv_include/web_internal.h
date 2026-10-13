/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Internal interface between the web server's files:
 *   web.c         server, request checks, dispatcher, assets, captive portal,
 *                 events and the periodic tick
 *   web_auth.c    admin password, sessions, login throttling
 *   web_api.c     REST handlers and the status snapshot
 *   web_ws.c      WebSocket clients, status deltas, events, progress, log
 *   web_worker.c  uploads and downloads on the worker task, the uploaded
 *                 receiver package, deferred system actions
 *
 * Threading: everything except the worker runs on the server task (handlers
 * and work queued with httpd_queue_work), so the session table and the
 * WebSocket clients need no lock. Event handlers only set flags and queue
 * work. The worker task touches only its own state, published to the
 * server task through atomics.
 */
#pragma once

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "web_build.h"
#include "web_core.h"
#include "web_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Receiver flashing is built in (the station knows: CONFIG_UM98X_FLASH_ENABLE,
 * off until the bench has confirmed the receiver side). Without it the
 * update flow checks the package and stops before the flash, and says so.
 */
#define WEB_FLASH_AVAILABLE station_flash_available()

/** Onocoy's TLS port for source connections (public Onocoy documentation). */
#define WEB_ONOCOY_TLS_PORT 2121u

/**
 * The one other origin the page may load from, for images only: the
 * OpenStreetMap tile server of the live view's station map (its tile usage
 * policy: this exact host, HTTPS, attribution shown, a Referer sent).
 */
#define WEB_MAP_TILE_ORIGIN "https://tile.openstreetmap.org"

/** This project, as the app description of an image names it. */
#define WEB_PROJECT_NAME "onocoydiy-esp-um982"

/** Largest JSON request body; the CA certificate route takes more. */
#define WEB_JSON_MAX       2048u
#define WEB_JSON_MAX_CA    6144u

/** Uploads and downloads move in pieces of this size (internal RAM buffer). */
#define WEB_IO_CHUNK       4096u

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

/** Queue the periodic work now (events, progress) instead of waiting for the next tick. */
void web_kick(void);

/* ---- requests and responses ------------------------------------------------------------- */

typedef enum {
    WEB_ACC_PUB = 0,     /**< always */
    WEB_ACC_PUBSTAR,     /**< without a session only while public status is on */
    WEB_ACC_ADMIN,       /**< admin session */
    WEB_ACC_CLAIM,       /**< the handler applies the claim rules */
} web_access_t;

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
} web_route_t;

/** The REST routes (web_api.c). */
extern const web_route_t web_api_routes[];
extern const size_t web_api_route_count;

/**
 * URI handlers the server registers room for: every REST route plus the
 * WebSocket and the catch-all (web_api.c checks the table fits at compile
 * time, so a new route can never make the server fail to start).
 */
#define WEB_MAX_URI_HANDLERS 72
#define WEB_EXTRA_URI_HANDLERS 2   /* /ws and the catch-all */

/** True if `path` (no query) is one of the API paths (for 405 versus 404). */
bool web_api_path_known(const char *path);

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
 * at most 64 KB or 2 s, so it reads the answer instead of a reset.
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

/* ---- connections (web.c, around web_conn.h) ---------------------------------------------- */

/** A handler on the server task finished its request (`admin`: it had an admin session). */
void web_conn_request_done(httpd_req_t *req, bool admin);

/** The handshake made `fd` a WebSocket client (of an admin session, or a public viewer). */
void web_conn_ws_open(int fd, bool admin);

/** A WebSocket client's close is under way. */
void web_conn_ws_closing(int fd);

/**
 * Close `fd`'s connection now (server task only, or the worker for the
 * request it holds before completing it): its socket is shut down, and the
 * server deletes exactly that session at its next select(). Used instead of
 * httpd_sess_trigger_close(), whose queued close names a session slot that
 * a new connection may have taken by the time it runs.
 */
void web_conn_shut(int fd);

/** The worker took over `fd`'s request (`on`), or finished it (any task). */
void web_conn_worker(int fd, bool on);

/* ---- authentication (web_auth.c) --------------------------------------------------------------- */

/** Load the admin password record; reset sessions and throttling. */
void web_auth_init(void);

typedef enum {
    WEB_ADMIN_NONE = 0,      /**< no password: claim over the AP */
    WEB_ADMIN_SET,           /**< a record is loaded (stored, or RAM-only this boot) */
    WEB_ADMIN_UNREADABLE,    /**< something is stored but it cannot be read */
} web_admin_state_t;

web_admin_state_t web_auth_admin_state(void);

/** Session of the request's cookie (extends its idle timeout when `touch`). -1 if none. */
int web_auth_session(httpd_req_t *req, bool touch);

/** Generation of a live session (links WebSocket clients to it). */
uint32_t web_auth_session_gen(int idx);
bool web_auth_session_alive(int idx, uint32_t gen);
uint32_t web_auth_session_left_s(int idx);
void web_auth_session_touch(int idx);

/**
 * Seconds this request's client must still wait before a password attempt
 * (0 = allowed now). Throttling is per client (peer address, and whether it
 * came over the AP; web_core.h).
 */
uint32_t web_auth_throttle_wait_s(httpd_req_t *req, bool on_ap);

/**
 * Check the password for this request's client (counts its failure, resets
 * its count on success). Call only when the throttle allows it.
 */
bool web_auth_check(httpd_req_t *req, bool on_ap, const char *pw);

/** New session; sets the cookie header on the response (`cookie_buf` must outlive the response). */
int web_auth_start_session(httpd_req_t *req, char cookie_buf[96]);

/** End the request's session (and its WebSocket clients); sets the clearing cookie. */
void web_auth_logout(httpd_req_t *req, int idx);

/**
 * Store a new password (first claim or change). `ram_only` when the
 * settings are unavailable (claim only). @return ESP_OK or the settings error.
 */
esp_err_t web_auth_set_password(const char *pw, bool *persisted);

/** End every session but `keep` (-1 = all); their WebSocket clients close with 4001. */
void web_auth_end_sessions(int keep);

/** End one session (its WebSocket clients close with 4001). */
void web_auth_end_session(int idx);

/** End expired sessions (once a second). */
void web_auth_expire(void);

/** SETTINGS_EVENT for sys/admin_hash or a factory reset: reload, end sessions if it changed behind our back. */
void web_auth_reload(void);

/**
 * The BOOT button cleared the admin password (health_status_t.admin_resets
 * went up): forget a password held only in RAM (settings unavailable), reload
 * the stored state and end every session.
 */
void web_auth_admin_reset(void);

/* ---- REST helpers (web_api.c) ------------------------------------------------------------------------ */

/** Gather the status snapshot (into PSRAM storage the caller owns). */
void web_snap_status(web_status_snap_t *s);

/** Record the outcome of an update job for GET /api/update. */
void web_note_result(const char *job, esp_err_t err, const char *result);

/** Note that the server started an update job (attribution of the next end). */
void web_note_job_started(const char *job);

/**
 * Watch fw_update's state: progress of a running job, its result when it
 * ends. Called on FW_UPDATE_EVENT (`event`) and twice a second.
 */
void web_track_fw_state(bool event);

/**
 * Receiver maintenance is on (gnss_link refuses every write and lease): the
 * receiver-update refusals answer receiver_maintenance then.
 */
bool web_rx_hands_off(void);

/** Protected-slot budget for the update dialogs (any task); UINT32_MAX = unknown. */
uint32_t web_budget_now(void);

/** The network reported a finished scan. */
void web_note_scan_done(void);

/* ---- WebSocket (web_ws.c) ---------------------------------------------------------------------------------- */

esp_err_t web_ws_init(void);

/** /ws route: pre-handshake check, post-handshake setup and the frame handler. */
esp_err_t web_ws_pre_handshake(httpd_req_t *req);
esp_err_t web_ws_post_handshake(httpd_req_t *req);
esp_err_t web_ws_handler(httpd_req_t *req);

/** A session closed (httpd close callback). */
void web_ws_on_close(int fd);

/** Close the clients of these sessions (bit mask of indexes) with 4001. */
void web_ws_close_sessions(uint32_t mask);

/** Close every client without a session (1008): public status was turned off. */
void web_ws_close_public(void);

/**
 * The sockets ran out and `fd` was chosen to make room: if it is a WebSocket
 * client, close it with 1013 (try again later). @return false if it is not one.
 */
bool web_ws_evict(int fd);

/** Periodic work on the server task: status at 1 Hz, progress at 2 Hz, log every 500 ms, events. */
void web_ws_tick(bool second, bool half_second);

/** Queue an event message for admin clients (server task or any task: copied). */
void web_ws_event(const char *json);

/** Send a progress message to the progress subscribers (server task). */
void web_ws_progress(const web_progress_t *p);

/**
 * Receiver flash progress from the station's events (any task: copied).
 * final: STATION_EVENT_FLASH_ENDED, the job's last message (`done`); kept
 * until it went out even when progress of a next job follows.
 */
void web_ws_note_flash_progress(const um98x_flash_progress_t *p, bool final);

/** Reboot under way: send `rebooting` and close every client with 1012. */
void web_ws_rebooting(int reason);

/** Clients connected now. */
size_t web_ws_client_count(void);

/* ---- worker (web_worker.c) ---------------------------------------------------------------------------------- */

esp_err_t web_worker_init(void);

/** POST /api/upload/app and /api/upload/gnss, after the request checks. */
esp_err_t web_upload_app(web_req_t *r);
esp_err_t web_upload_gnss(web_req_t *r);

/** GET /api/coredump and GET /api/logs?format=text. */
esp_err_t web_download_coredump(web_req_t *r);
esp_err_t web_download_log_text(web_req_t *r, bool prev_boot);
/** GET /api/diag/flash-capture (bench builds; 404 otherwise). */
esp_err_t web_download_flash_capture(web_req_t *r);
/** GET /api/diag/frame-times (bench builds with the link's bench hooks; 404 otherwise). */
esp_err_t web_download_frame_times(web_req_t *r);

/** An upload is being received or refused (one at a time). */
bool web_upload_busy(void);

/** Uploaded receiver package waiting for confirmation (matches the staged one). */
bool web_staged_get(um98x_flash_pkg_facts_t *facts, uint32_t *expires_in_s);
/** Forget it (released, confirmed or replaced). */
void web_staged_forget(void);

/** Run lifecycle_clear_safe_mode() on the worker after the response went out. */
esp_err_t web_worker_clear_safe_mode(void);

/** Progress of an upload for the WebSocket (job NULL = none). */
typedef struct {
    const char *job;     /**< "app_upload" / "gnss_upload" */
    const char *phase;
    uint32_t bytes;
    uint32_t total;
    bool done;
    const char *result;
    esp_err_t err;
    uint32_t seq;        /**< changes with every update */
} web_upload_progress_t;

void web_upload_progress(web_upload_progress_t *out);

/** Responses still being written by the worker (the reboot hook waits for them). */
int web_worker_pending_responses(void);

#ifdef __cplusplus
}
#endif
