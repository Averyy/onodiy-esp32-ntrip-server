/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * HTTP server: configuration, the request checks every request passes
 * before a handler runs, the dispatcher of the REST routes, the embedded UI
 * assets, the captive portal, component events and the periodic tick.
 */
#include "web.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "diag.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "fw_update.h"
#include "health.h"
#include "lifecycle.h"
#include "lwip/sockets.h"
#include "network.h"
#include "ntrip_server.h"
#include "sdkconfig.h"
#include "settings.h"
#include "station.h"
#include "web_assets.h"
#include "web_conn.h"
#include "web_enums.h"
#include "web_internal.h"
#include "web_parse.h"

static const char *TAG = "web";

/* The fixed offsets the upload check reads must match the IDF image types. */
_Static_assert(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) == WEB_IMAGE_DESC_OFS,
               "app description offset moved");
_Static_assert(offsetof(esp_image_header_t, chip_id) == WEB_IMAGE_CHIP_ID_OFS, "chip id offset moved");
_Static_assert(offsetof(esp_app_desc_t, version) == WEB_DESC_VERSION_OFS, "app version offset moved");
_Static_assert(offsetof(esp_app_desc_t, project_name) == WEB_DESC_PROJECT_OFS, "project name offset moved");
_Static_assert(offsetof(esp_app_desc_t, app_elf_sha256) == WEB_DESC_ELF_SHA_OFS, "app ELF SHA-256 offset moved");
_Static_assert(ESP_APP_DESC_MAGIC_WORD == WEB_DESC_MAGIC, "app description magic moved");
_Static_assert(ESP_IMAGE_HEADER_MAGIC == WEB_IMAGE_MAGIC, "image magic moved");
_Static_assert(ESP_ERR_OTA_VALIDATE_FAILED == 0x1503, "OTA validation error code moved (web_build.c)");

/* Server limits (socket budget: 7 here + 3 for the server itself, beside the
 * casters). One of the 7 is kept free for the next connection (web_conn.h). */
#define SERVER_MAX_SOCKETS   7
#define SERVER_MAX_URIS      WEB_MAX_URI_HANDLERS
#define SERVER_STACK         8192
#define SERVER_PRIO          5
#define SERVER_HDR_LEN       2048
/* One server task handles every request: a client that sends nothing for
 * this long is dropped (headers), or its body read gives up (after a few
 * tries); and the whole request must be in within CONFIG_WEB_REQUEST_DEADLINE_S
 * of its first byte, so a stalled or trickling client holds the server only
 * briefly. */
#define SERVER_RECV_TIMEOUT_S 4
#define SERVER_SEND_TIMEOUT_S 10
#define TICK_MS              250
#define DEADLINE_LOG_S       60   /* at most one log line about late requests per minute */
/* A refused request's body: after the answer, read and drop at most this
 * much, for at most this long, so the client reads the answer instead of a
 * reset (a close with unread data in the socket sends an RST, which makes
 * many clients drop the answer still in their receive buffer). */
#define LINGER_DRAIN_BYTES   (64u * 1024u)
#define LINGER_DRAIN_MS      2000
/* X-Client-Id of the request that changed settings: a settings event this
 * soon after that request was its own (the event comes from another task). */
#define CLIENT_ID_HOLD_MS    2000

_Static_assert(SERVER_MAX_SOCKETS <= WEB_CONN_MAX, "every connection must fit the connection table");
_Static_assert(CONFIG_WEB_REQUEST_DEADLINE_S > SERVER_RECV_TIMEOUT_S,
               "the request deadline must allow one full per-receive timeout");

static httpd_handle_t s_server;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static char s_hostname[33];
static char s_etag[64];   /* "<app version>-<UI digest>" with quotes */
static bool s_public;
static esp_timer_handle_t s_tick_timer;
static atomic_bool s_tick_queued;
static uint32_t s_ticks;
static SemaphoreHandle_t s_reboot_sem;
static atomic_int s_reboot_reason = -1;
static atomic_bool s_rebooting;

/* Flags set by event handlers, consumed by the tick on the server task. */
static atomic_uint s_caster_changed;     /* slot bit mask */
static atomic_bool s_auth_reload;
static atomic_bool s_end_all_sessions;
static atomic_bool s_public_reload;
static atomic_bool s_fw_changed;
static atomic_bool s_scan_done;

/* The X-Client-Id of the mutating request running now or just finished
 * (settings_changed events carry it, so the page that saved knows it was
 * its own change). Written on the server task, read by the event task. */
static portMUX_TYPE s_client_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_client_id[33];
static bool s_client_active;
static uint32_t s_client_done_ms;

/* Connections: request deadlines and which one to close when the sockets
 * run out (web_conn.h). Server task, and the worker for its requests. */
static portMUX_TYPE s_conn_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static web_conns_t s_conns;   /* PSRAM: about 1 KB with the request-line scanners */
static uint32_t s_late_logged_s;
static uint32_t s_late_count;

/* Server-task buffers in PSRAM. */
EXT_RAM_BSS_ATTR static char s_http_buf[2048];
EXT_RAM_BSS_ATTR static char s_body[WEB_JSON_MAX_CA + 1];
static char s_csp[384];

/* ---- small accessors ------------------------------------------------------------------- */

httpd_handle_t web_server(void)
{
    return s_server;
}

uint32_t web_now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

bool web_public_status(void)
{
    return s_public;
}

/* ---- connections ------------------------------------------------------------------------ */

void web_conn_request_done(httpd_req_t *req, bool admin)
{
    int fd = httpd_req_to_sockfd(req);
    uint32_t now = now_ms();
    taskENTER_CRITICAL(&s_conn_lock);
    web_conn_done(&s_conns, fd, now, admin);
    taskEXIT_CRITICAL(&s_conn_lock);
}

void web_conn_ws_open(int fd, bool admin)
{
    uint32_t now = now_ms();
    taskENTER_CRITICAL(&s_conn_lock);
    web_conn_websocket(&s_conns, fd, admin, now);
    taskEXIT_CRITICAL(&s_conn_lock);
}

void web_conn_ws_closing(int fd)
{
    taskENTER_CRITICAL(&s_conn_lock);
    web_conn_closing(&s_conns, fd, true);
    taskEXIT_CRITICAL(&s_conn_lock);
}

void web_conn_worker(int fd, bool on)
{
    uint32_t now = now_ms();
    taskENTER_CRITICAL(&s_conn_lock);
    web_conn_async(&s_conns, fd, on, now);
    taskEXIT_CRITICAL(&s_conn_lock);
}

void web_resp_close_hdr(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    taskENTER_CRITICAL(&s_conn_lock);
    bool had = web_conn_note_close_hdr(&s_conns, fd);
    taskEXIT_CRITICAL(&s_conn_lock);
    if (!had) {
        httpd_resp_set_hdr(req, "Connection", "close");
    }
}

bool web_conn_req_close(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    taskENTER_CRITICAL(&s_conn_lock);
    bool close = web_conn_close_after(&s_conns, fd);
    taskEXIT_CRITICAL(&s_conn_lock);
    return close;
}

/*
 * A handler starts on a request (server task): did the client ask for the
 * connection to end after the answer ("Connection: close", or HTTP/1.0
 * without keep-alive)? Then the answer carries "Connection: close" (set
 * now, so a request the worker takes over carries it too) and
 * request_end() closes the connection.
 */
static bool request_begin(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    taskENTER_CRITICAL(&s_conn_lock);
    bool http10 = web_conn_begin_request(&s_conns, fd);
    taskEXIT_CRITICAL(&s_conn_lock);
    char v[64];
    unsigned tokens = 0;
    esp_err_t err = httpd_req_get_hdr_value_str(req, "Connection", v, sizeof(v));
    if (err == ESP_OK || err == ESP_ERR_HTTPD_RESULT_TRUNC) {
        tokens = web_connection_tokens(v);
    }
    bool close = web_conn_should_close(tokens, http10);
    if (close) {
        taskENTER_CRITICAL(&s_conn_lock);
        web_conn_set_close_after(&s_conns, fd, true);
        taskEXIT_CRITICAL(&s_conn_lock);
        web_resp_close_hdr(req);
    }
    return close;
}

/*
 * After the handler: a request that asked for it ends its connection (our
 * side first, then what the client still sends is drained briefly, and the
 * server deletes the session on ESP_FAIL). Not when the handler closed it
 * already (ESP_FAIL), nor when the request runs on the worker, which closes
 * it when it is done.
 */
static esp_err_t request_end(httpd_req_t *req, bool close, esp_err_t ret)
{
    if (!close || ret != ESP_OK) {
        return ret;
    }
    int fd = httpd_req_to_sockfd(req);
    taskENTER_CRITICAL(&s_conn_lock);
    bool worker = web_conn_on_worker(&s_conns, fd);
    taskEXIT_CRITICAL(&s_conn_lock);
    if (worker) {
        return ret;
    }
    web_linger_close(req);
    return ESP_FAIL;
}

/* A request ran out of time: logged at most once a minute (a client could
 * otherwise fill the log). */
static void note_late(void)
{
    uint32_t now = web_now_s();
    taskENTER_CRITICAL(&s_conn_lock);
    s_late_count++;
    bool log = s_late_logged_s == 0 || (uint32_t)(now - s_late_logged_s) >= DEADLINE_LOG_S;
    uint32_t count = s_late_count;
    if (log) {
        s_late_logged_s = now == 0 ? 1 : now;
        s_late_count = 0;
    }
    taskEXIT_CRITICAL(&s_conn_lock);
    if (log) {
        ESP_LOGW(TAG, "%u request(s) not received within %d s: connection closed", (unsigned)count,
                 CONFIG_WEB_REQUEST_DEADLINE_S);
    }
}

/* True if the socket has something to read within `ms`. */
static bool readable_within(int fd, int32_t ms)
{
    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(fd, &rfds);
    struct timeval tv = { .tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000 };
    int n = select(fd + 1, &rfds, NULL, NULL, &tv);
    return n != 0;   /* an error is left to recv() to report */
}

/*
 * Receive override of every connection: the default receive, except that a
 * request being received on the server task waits only for what is left of
 * its deadline (web_conn.h), then reports a timeout, after which the server
 * answers 408 (headers) or gives up on the body or frame and closes the
 * connection.
 */
static int conn_recv(httpd_handle_t hd, int fd, char *buf, size_t len, int flags)
{
    (void)hd;
    if (buf == NULL) {
        return HTTPD_SOCK_ERR_INVALID;
    }
    uint32_t now = now_ms();
    taskENTER_CRITICAL(&s_conn_lock);
    int32_t budget = web_conn_recv_budget(&s_conns, fd, now);
    taskEXIT_CRITICAL(&s_conn_lock);
    if (budget == 0 || (budget > 0 && budget < SERVER_RECV_TIMEOUT_S * 1000 && !readable_within(fd, budget))) {
        note_late();
        return HTTPD_SOCK_ERR_TIMEOUT;
    }
    int n = recv(fd, buf, len, flags);
    if (n < 0) {
        switch (errno) {
        case EAGAIN:
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINTR:
            return HTTPD_SOCK_ERR_TIMEOUT;
        case EINVAL:
        case EBADF:
        case EFAULT:
        case ENOTSOCK:
            return HTTPD_SOCK_ERR_INVALID;
        default:
            return HTTPD_SOCK_ERR_FAIL;
        }
    }
    if (n > 0) {
        now = now_ms();
        taskENTER_CRITICAL(&s_conn_lock);
        web_conn_received(&s_conns, fd, now);
        web_conn_scan(&s_conns, fd, buf, (size_t)n);   /* request lines: their HTTP version */
        taskEXIT_CRITICAL(&s_conn_lock);
    }
    return n;
}

/*
 * Close a connection from the server task. Its socket is shut down here and
 * now: the server's next select() sees it end and deletes exactly that
 * session (its close callback then forgets it). A close queued to the
 * server instead names a session slot, which the connection may have left,
 * and a new one taken, by the time the queue reaches it.
 */
void web_conn_shut(int fd)
{
    if (shutdown(fd, SHUT_RDWR) != 0) {
        /* Already reset by the peer: the server sees that by itself. */
        ESP_LOGD(TAG, "connection %d: shutdown: errno %d", fd, errno);
    }
}

/* Close `fd` to keep a socket free (server task, from the open callback). */
static void evict(int fd)
{
    if (web_ws_evict(fd)) {
        return;   /* a public viewer: told to come back later, then closed */
    }
    web_conn_shut(fd);
}

static esp_err_t on_sess_open(httpd_handle_t hd, int fd)
{
    uint32_t now = now_ms();
    taskENTER_CRITICAL(&s_conn_lock);
    bool tracked = web_conn_open(&s_conns, fd, now);
    int victim = web_conn_victim(&s_conns, fd, SERVER_MAX_SOCKETS, now);
    taskEXIT_CRITICAL(&s_conn_lock);
    if (!tracked) {
        ESP_LOGW(TAG, "connection %d not tracked", fd);
    }
    /* Never fail here: esp_http_server would close the socket twice. */
    (void)httpd_sess_set_recv_override(hd, fd, conn_recv);
    if (victim >= 0) {
        ESP_LOGD(TAG, "sockets full: closing connection %d%s", victim, victim == fd ? " (the new one)" : "");
        evict(victim);
    }
    return ESP_OK;
}

void web_public_status_set_cached(bool on)
{
    s_public = on;
    if (!on) {
        web_ws_close_public();   /* viewers connected while it was on lose it now */
    }
}

const char *web_hostname_active(void)
{
    return s_hostname;
}

char *web_http_buf(size_t *cap)
{
    *cap = sizeof(s_http_buf);
    return s_http_buf;
}

static void load_public_status(void)
{
    uint8_t v = 0;
    web_public_status_set_cached(settings_get_u8(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_PUBLIC, &v) == ESP_OK && v != 0);
}

static void load_hostname(void)
{
    const char *name = NULL;
    if (s_sta != NULL && esp_netif_get_hostname(s_sta, &name) == ESP_OK && name != NULL && name[0] != '\0') {
        strlcpy(s_hostname, name, sizeof(s_hostname));
    } else {
        strlcpy(s_hostname, CONFIG_LWIP_LOCAL_HOSTNAME, sizeof(s_hostname));
    }
}

static void ip_of(esp_netif_t *netif, char *out, size_t out_size, const char *fallback)
{
    esp_netif_ip_info_t info;
    out[0] = '\0';
    if (netif != NULL && esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
        esp_ip4addr_ntoa(&info.ip, out, (int)out_size);
    } else if (fallback != NULL) {
        strlcpy(out, fallback, out_size);
    }
}

/* ---- responses -------------------------------------------------------------------------- */

void web_set_common_headers(httpd_req_t *req, bool api)
{
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    if (api) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
}

esp_err_t web_send_error(httpd_req_t *req, web_err_t err, const char *message, const char *field, uint32_t retry_in_s,
                         const char *by, const char *reason)
{
    char buf[384];
    wj_t w;
    wj_init(&w, buf, sizeof(buf), NULL, NULL);
    web_build_error(&w, err, message, field, retry_in_s, by, reason);
    if (!wj_ok(&w)) {
        wj_init(&w, buf, sizeof(buf), NULL, NULL);
        web_build_error(&w, err, NULL, NULL, UINT32_MAX, NULL, NULL);
    }
    httpd_resp_set_status(req, web_status_line(web_err_status(err)));
    httpd_resp_set_type(req, "application/json");
    web_set_common_headers(req, true);
    /* HEAD: the same status and headers (Content-Length included), no body. */
    return httpd_resp_send(req, req->method == HTTP_HEAD ? NULL : wj_text(&w), (ssize_t)strlen(wj_text(&w)));
}

esp_err_t web_send_err(httpd_req_t *req, web_err_t err)
{
    return web_send_error(req, err, NULL, NULL, UINT32_MAX, NULL, NULL);
}

esp_err_t web_send_internal(httpd_req_t *req, esp_err_t err)
{
    return web_send_error(req, WEB_ERR_INTERNAL, esp_err_to_name(err), NULL, UINT32_MAX, NULL, NULL);
}

esp_err_t web_send_json_literal(httpd_req_t *req, int status, const char *json)
{
    httpd_resp_set_status(req, web_status_line(status));
    httpd_resp_set_type(req, "application/json");
    web_set_common_headers(req, true);
    return httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
}

static void resp_headers(web_resp_t *r)
{
    if (!r->headers_sent) {
        httpd_resp_set_status(r->req, web_status_line(r->status));
        httpd_resp_set_type(r->req, "application/json");
        web_set_common_headers(r->req, true);
        r->headers_sent = true;
    }
}

static bool resp_flush(void *ctx, const char *data, size_t len)
{
    web_resp_t *r = ctx;
    resp_headers(r);
    if (httpd_resp_send_chunk(r->req, data, (ssize_t)len) != ESP_OK) {
        r->send_failed = true;
        return false;
    }
    return true;
}

void web_resp_begin(web_resp_t *r, httpd_req_t *req, int status, char *buf, size_t cap)
{
    memset(r, 0, sizeof(*r));
    r->req = req;
    r->status = status;
    wj_init(&r->w, buf, cap, resp_flush, r);
}

esp_err_t web_resp_end(web_resp_t *r)
{
    if (r->send_failed) {
        return ESP_FAIL;
    }
    if (!r->headers_sent) {
        if (!wj_ok(&r->w)) {
            return web_send_internal(r->req, ESP_ERR_INVALID_SIZE);
        }
        resp_headers(r);
        return httpd_resp_send(r->req, r->w.buf, (ssize_t)r->w.len);
    }
    if (!wj_finish(&r->w)) {
        return ESP_FAIL;   /* half a body went out: close the connection */
    }
    return httpd_resp_send_chunk(r->req, NULL, 0);
}

/* ---- request checks ---------------------------------------------------------------------- */

/* A socket address as web_addr_t (IPv4, or IPv6 with v4-mapped addresses). */
static bool sock_addr(const struct sockaddr_storage *ss, web_addr_t *out)
{
    memset(out, 0, sizeof(*out));
    if (ss->ss_family == AF_INET) {
        memcpy(out->b, &((const struct sockaddr_in *)ss)->sin_addr.s_addr, 4);
        return true;
    }
#if CONFIG_LWIP_IPV6
    if (ss->ss_family == AF_INET6) {
        out->v6 = true;
        memcpy(out->b, ((const struct sockaddr_in6 *)ss)->sin6_addr.s6_addr, 16);
        return true;
    }
#endif
    return false;
}

bool web_req_peer(httpd_req_t *req, web_addr_t *out)
{
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    if (fd < 0 || getpeername(fd, (struct sockaddr *)&addr, &len) != 0) {
        return false;
    }
    return sock_addr(&addr, out);
}

void web_peer_text(httpd_req_t *req, char *out, size_t cap)
{
    web_addr_t a;
    (void)web_addr_text(web_req_peer(req, &a) ? &a : NULL, out, cap);
}

bool web_req_on_ap(httpd_req_t *req)
{
#if CONFIG_NETWORK_QEMU_ETHERNET
    /* QEMU development build: no AP; the emulated Ethernet sits behind
     * QEMU's user-mode NAT that only the development machine reaches, so it
     * stands in for the setup AP (the first admin password can be set). */
    (void)req;
    return true;
#endif
    int fd = httpd_req_to_sockfd(req);
    struct sockaddr_storage addr;
    socklen_t len = sizeof(addr);
    web_addr_t local;
    if (fd < 0 || s_ap == NULL || getsockname(fd, (struct sockaddr *)&addr, &len) != 0 ||
        !sock_addr(&addr, &local)) {
        return false;
    }
    esp_netif_ip_info_t info;
    uint32_t ap4 = esp_netif_get_ip_info(s_ap, &info) == ESP_OK ? info.ip.addr : 0;
    const uint8_t *ap6 = NULL;
#if CONFIG_LWIP_IPV6
    /* A phone that reaches the AP over IPv6 talks to the AP's link-local address. */
    uint8_t ll[16];
    esp_ip6_addr_t ip6;
    if (local.v6 && esp_netif_get_ip6_linklocal(s_ap, &ip6) == ESP_OK) {
        memcpy(ll, ip6.addr, sizeof(ll));
        ap6 = ll;
    }
#endif
    return web_addr_on_ap(&local, ap4, ap6);
}

static esp_err_t captive_redirect(httpd_req_t *req)
{
    char ap_ip[16];
    ip_of(s_ap, ap_ip, sizeof(ap_ip), "192.168.4.1");
    static char location[40];
    snprintf(location, sizeof(location), "http://%s/", ap_ip);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    web_set_common_headers(req, true);
    return httpd_resp_send(req, NULL, 0);
}

typedef enum { PRE_OK = 0, PRE_ANSWERED, PRE_CLOSE } pre_t;

static bool mutating(int method)
{
    return method == HTTP_POST || method == HTTP_PUT || method == HTTP_DELETE;
}

/* Host header value (validated), kept for the Origin check and the CSP. */
static char s_host[80];
/* The mDNS name the network answers now (server task). */
static network_status_t s_pre_net;

/* Host (DNS rebinding; on the AP: the captive portal) and Origin. */
static pre_t precheck(httpd_req_t *req, bool *on_ap, bool websocket)
{
    *on_ap = web_req_on_ap(req);
    bool host_ok = false;
    if (httpd_req_get_hdr_value_str(req, "Host", s_host, sizeof(s_host)) == ESP_OK) {
        char sta_ip[16];
        char ap_ip[16];
        ip_of(s_sta, sta_ip, sizeof(sta_ip), NULL);
        ip_of(s_ap, ap_ip, sizeof(ap_ip), "192.168.4.1");
        if (network_get_status(&s_pre_net) != ESP_OK) {
            s_pre_net.mdns_host[0] = '\0';
        }
        web_host_names_t names = {
            .sta_ip = sta_ip, .ap_ip = ap_ip, .hostname = s_hostname, .mdns_host = s_pre_net.mdns_host,
        };
        host_ok = web_host_allowed(s_host, &names);
#if CONFIG_NETWORK_QEMU_ETHERNET
        if (!host_ok) {
            /* QEMU development build: the browser reaches the device through QEMU's port forward. */
            const web_host_names_t fwd = { .sta_ip = "127.0.0.1", .ap_ip = "localhost", .hostname = NULL,
                                           .mdns_host = NULL };
            host_ok = web_host_allowed(s_host, &fwd);
        }
#endif
    } else {
        s_host[0] = '\0';
    }
    /* A request answered here never has its body read: with a body, the
     * connection closes instead of the server reading and discarding it
     * (it could announce gigabytes). */
    bool body = req->content_len > 0;
    if (!host_ok) {
        if (body) {
            web_resp_close_hdr(req);
        }
        if (*on_ap && !websocket) {
            captive_redirect(req);
        } else {
            web_send_err(req, WEB_ERR_BAD_HOST);
        }
        if (body) {
            web_linger_close(req);
        }
        return body ? PRE_CLOSE : PRE_ANSWERED;
    }
    if (mutating(req->method) || websocket) {
        char origin[96];
        esp_err_t err = httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof(origin));
        if (err == ESP_ERR_HTTPD_RESULT_TRUNC || (err == ESP_OK && !web_origin_ok(origin, s_host))) {
            if (body) {
                web_resp_close_hdr(req);
            }
            web_send_error(req, WEB_ERR_FORBIDDEN, "Cross-origin request refused", NULL, UINT32_MAX, NULL, NULL);
            if (body) {
                web_linger_close(req);
            }
            return body ? PRE_CLOSE : PRE_ANSWERED;
        }
    }
    return PRE_OK;
}

bool web_ws_precheck(httpd_req_t *req)
{
    bool on_ap = false;
    return precheck(req, &on_ap, true) == PRE_OK;
}

static bool header_is(httpd_req_t *req, const char *field, const char *type, bool content_type)
{
    char v[96];
    if (httpd_req_get_hdr_value_str(req, field, v, sizeof(v)) != ESP_OK) {
        return false;
    }
    return content_type ? web_content_type_is(v, type) : strcmp(v, type) == 0;
}

void web_linger_close(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return;
    }
    /* The answer is complete: our side ends here (a FIN follows it). */
    if (shutdown(fd, SHUT_WR) != 0) {
        return;   /* reset already: nothing to wait for */
    }
    char buf[512];
    size_t dropped = 0;
    int64_t until = esp_timer_get_time() + (int64_t)LINGER_DRAIN_MS * 1000;
    while (dropped < LINGER_DRAIN_BYTES) {
        int64_t left_ms = (until - esp_timer_get_time()) / 1000;
        if (left_ms <= 0 || !readable_within(fd, (int32_t)left_ms)) {
            break;
        }
        int n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n <= 0) {
            break;   /* the client closed (it read the answer), or an error */
        }
        dropped += (size_t)n;
    }
}

/* Answer `err` (`message` NULL: its default text), and close the connection
 * instead of reading a body we refuse. */
static esp_err_t refuse_close_msg(httpd_req_t *req, web_err_t err, const char *message, uint32_t retry_in_s)
{
    web_resp_close_hdr(req);
    web_send_error(req, err, message, NULL, retry_in_s, NULL, NULL);
    web_linger_close(req);
    return ESP_FAIL;
}

/* Answer, and close the connection instead of reading a body we refuse. */
esp_err_t web_refuse_and_close(httpd_req_t *req, web_err_t err)
{
    return web_refuse_and_close_retry(req, err, UINT32_MAX);
}

esp_err_t web_refuse_and_close_retry(httpd_req_t *req, web_err_t err, uint32_t retry_in_s)
{
    return refuse_close_msg(req, err, NULL, retry_in_s);
}

static esp_err_t read_body(httpd_req_t *req, size_t len)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < len) {
        int n = httpd_req_recv(req, s_body + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3) {
            continue;
        }
        if (n <= 0) {
            return ESP_FAIL;
        }
        got += (size_t)n;
    }
    s_body[got] = '\0';
    return ESP_OK;
}

/*
 * The page's X-Client-Id (random per tab; [A-Za-z0-9_-], at most 32) of a
 * mutating request: settings events during it and shortly after carry it.
 * @return true when one was noted.
 */
static bool note_client_id(httpd_req_t *req)
{
    char id[sizeof(s_client_id)];
    if (httpd_req_get_hdr_value_str(req, "X-Client-Id", id, sizeof(id)) != ESP_OK || id[0] == '\0') {
        return false;
    }
    for (const char *p = id; *p != '\0'; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '-' ||
              *p == '_')) {
            return false;   /* never copied into an event as is */
        }
    }
    taskENTER_CRITICAL(&s_client_lock);
    strlcpy(s_client_id, id, sizeof(s_client_id));
    s_client_active = true;
    taskEXIT_CRITICAL(&s_client_lock);
    return true;
}

/* Every path either reads the request's body, hands it to the worker, or
 * answers and closes the connection (ESP_FAIL): esp_http_server never has a
 * body left to read away after the handler, which it would do on the server
 * task once the request deadline is over. */
static esp_err_t api_handle(httpd_req_t *req, web_req_t *rp)
{
    const web_route_t *rt = req->user_ctx;
    web_req_t r;
    memset(&r, 0, sizeof(r));
    r.req = req;
    r.sess = -1;
    pre_t pre = precheck(req, &r.on_ap, false);
    if (pre != PRE_OK) {
        return pre == PRE_CLOSE ? ESP_FAIL : ESP_OK;
    }
    /* GET and DELETE take no body: refuse one instead of reading it away
     * (a bad request, whatever its size). */
    if (rt->body == WEB_BODY_NONE && req->content_len > 0) {
        return refuse_close_msg(req, WEB_ERR_BAD_REQUEST, "This request takes no body", UINT32_MAX);
    }
    /* CSRF: a JSON or upload content type (a cross-site form cannot send either). */
    if (rt->body == WEB_BODY_JSON && !header_is(req, "Content-Type", "application/json", true)) {
        return req->content_len > 0 ? web_refuse_and_close(req, WEB_ERR_UNSUPPORTED_MEDIA_TYPE)
                                    : web_send_err(req, WEB_ERR_UNSUPPORTED_MEDIA_TYPE);
    }
    if (rt->body == WEB_BODY_UPLOAD && (!header_is(req, "Content-Type", "application/octet-stream", true) ||
                                         !header_is(req, "X-Requested-With", "onocoydiy", false))) {
        return web_refuse_and_close(req, WEB_ERR_UNSUPPORTED_MEDIA_TYPE);
    }
    size_t max_body = rt->max_body != 0 ? rt->max_body : WEB_JSON_MAX;
    if (rt->body == WEB_BODY_JSON && req->content_len > max_body) {
        return web_refuse_and_close(req, WEB_ERR_TOO_LARGE);
    }
    if (httpd_req_get_url_query_str(req, r.query, sizeof(r.query)) != ESP_OK) {
        r.query[0] = '\0';
    }

    r.sess = web_auth_session(req, true);
    rp->sess = r.sess;
    switch (rt->access) {
    case WEB_ACC_ADMIN:
        if (r.sess < 0) {
            return req->content_len > 0 ? web_refuse_and_close(req, WEB_ERR_UNAUTHORIZED)
                                        : web_send_err(req, WEB_ERR_UNAUTHORIZED);
        }
        r.admin = true;
        break;
    case WEB_ACC_PUBSTAR:
        r.admin = r.sess >= 0;
        if (!r.admin && !web_public_status()) {
            return web_send_error(req, WEB_ERR_FORBIDDEN, "Public status is off: log in", NULL, UINT32_MAX, NULL,
                                  NULL);
        }
        break;
    default:
        r.admin = r.sess >= 0;
        break;
    }

    if (rt->body == WEB_BODY_JSON) {
        if (read_body(req, req->content_len) != ESP_OK) {
            return ESP_FAIL;
        }
        web_bad_t bad = { 0 };
        r.body = web_parse_body(s_body, req->content_len, &bad);
        if (r.body == NULL) {
            return web_send_err(req, bad.err);
        }
    }
    bool stash = mutating(req->method) && note_client_id(req);
    esp_err_t ret = rt->fn(&r);
    if (stash) {
        taskENTER_CRITICAL(&s_client_lock);
        s_client_active = false;
        s_client_done_ms = now_ms();
        taskEXIT_CRITICAL(&s_client_lock);
    }
    cJSON_Delete(r.body);
    return ret;
}

static esp_err_t api_dispatch(httpd_req_t *req)
{
    web_req_t r = { .sess = -1 };
    bool close = request_begin(req);
    esp_err_t ret = request_end(req, close, api_handle(req, &r));
    if (r.sess >= 0) {
        health_note_admin_activity();   /* someone uses the page: the GNSS ladder's restart waits */
    }
    web_conn_request_done(req, r.sess >= 0);
    return ret;
}

/* ---- UI assets, 404/405, captive portal ----------------------------------------------------- */

static bool is_html(const web_asset_t *a)
{
    return strncmp(a->type, "text/html", 9) == 0;
}

static esp_err_t serve_asset(httpd_req_t *req, const web_asset_t *a)
{
    web_set_common_headers(req, false);
    httpd_resp_set_hdr(req, "ETag", s_etag);
    httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
    if (is_html(a)) {
        /* Only the device's own origin: scripts, styles, images and the WebSocket; no framing.
         * One exception, images only: the map tiles of the live view's optional station map,
         * which the page fetches only after the user asks for the map.
         * The Host was validated, so it is safe to name in the policy. */
        snprintf(s_csp, sizeof(s_csp),
                 "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' " WEB_MAP_TILE_ORIGIN "; "
                 "font-src 'self'; "
                 "connect-src 'self' ws://%s; object-src 'none'; base-uri 'none'; form-action 'self'; "
                 "frame-ancestors 'none'",
                 s_host);
        httpd_resp_set_hdr(req, "Content-Security-Policy", s_csp);
    }
    char inm[160];
    if (httpd_req_get_hdr_value_str(req, "If-None-Match", inm, sizeof(inm)) == ESP_OK && web_etag_match(inm, s_etag)) {
        httpd_resp_set_status(req, "304 Not Modified");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, a->type);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    /* HEAD: the headers GET would send (its Content-Length too), no body. */
    return httpd_resp_send(req, req->method == HTTP_HEAD ? NULL : (const char *)a->gz, (ssize_t)a->gz_len);
}

/*
 * Everything no route matched: the UI assets, 404/405, the captive portal on
 * the AP. The answer is decided before anything of a body is read; nothing
 * here takes one, so with a body that answer goes out with
 * "Connection: close" and the connection ends without reading it.
 */
static esp_err_t fallback_handle(httpd_req_t *req)
{
    bool on_ap = false;
    pre_t pre = precheck(req, &on_ap, false);
    if (pre != PRE_OK) {
        return pre == PRE_CLOSE ? ESP_FAIL : ESP_OK;
    }
    const web_asset_t *a = web_asset_find(web_ui_assets, web_ui_asset_count, req->uri);
    web_fallback_t what = web_fallback_classify(req->uri, req->method == HTTP_OPTIONS,
                                                req->method == HTTP_GET || req->method == HTTP_HEAD, a != NULL, on_ap,
                                                web_api_path_known);
    bool body = req->content_len > 0;
    switch (what) {
    case WEB_FB_ASSET:
    case WEB_FB_REDIRECT: {
        if (body) {
            web_resp_close_hdr(req);
        }
        esp_err_t ret = what == WEB_FB_ASSET ? serve_asset(req, a) : captive_redirect(req);
        if (body) {
            web_linger_close(req);
            return ESP_FAIL;
        }
        return ret;
    }
    case WEB_FB_METHOD:
        /* OPTIONS too: no CORS, ever */
        return body ? web_refuse_and_close(req, WEB_ERR_METHOD_NOT_ALLOWED)
                    : web_send_err(req, WEB_ERR_METHOD_NOT_ALLOWED);
    case WEB_FB_NOT_FOUND:
    default:
        return body ? web_refuse_and_close(req, WEB_ERR_NOT_FOUND) : web_send_err(req, WEB_ERR_NOT_FOUND);
    }
}

static esp_err_t fallback(httpd_req_t *req)
{
    bool close = request_begin(req);
    esp_err_t ret = request_end(req, close, fallback_handle(req));
    web_conn_request_done(req, false);
    return ret;
}

/* WebSocket frames: every frame, control frames included, ends here, which
 * ends its receive deadline. A plain GET of /ws (no upgrade) is an HTTP
 * request like any other: it may ask for the connection to end. */
static esp_err_t ws_frame(httpd_req_t *req)
{
    bool close = req->method == HTTP_GET && request_begin(req);
    esp_err_t ret = request_end(req, close, web_ws_handler(req));
    web_conn_request_done(req, false);
    return ret;
}

/* ---- sessions of the server ------------------------------------------------------------------- */

static void on_sess_close(httpd_handle_t hd, int fd)
{
    (void)hd;
    web_ws_on_close(fd);
    taskENTER_CRITICAL(&s_conn_lock);
    web_conn_close(&s_conns, fd);
    taskEXIT_CRITICAL(&s_conn_lock);
    close(fd);
}

/* ---- periodic tick and events -------------------------------------------------------------------- */

/* A BOOT-button admin reset clears the stored password through settings
 * (whose change event reloads it); a password set while the settings were
 * unavailable lives only here, so the reset count is watched too. */
static void check_admin_reset(void)
{
    static uint32_t seen;
    static bool known;
    health_status_t hs;
    if (health_get_status(&hs) != ESP_OK) {
        return;
    }
    if (known && hs.admin_resets != seen) {
        web_auth_admin_reset();
    }
    seen = hs.admin_resets;
    known = true;
}

static void tick_work(void *arg)
{
    (void)arg;
    atomic_store(&s_tick_queued, false);
    s_ticks++;
    /* Work queued to the server runs between requests: an armed deadline
     * now is stale (a receive after its handler) and must not refuse the
     * connection's next request. */
    taskENTER_CRITICAL(&s_conn_lock);
    web_conn_between_requests(&s_conns);
    taskEXIT_CRITICAL(&s_conn_lock);
    bool second = (s_ticks % (1000 / TICK_MS)) == 0;
    bool half = (s_ticks % (500 / TICK_MS)) == 0;

    if (atomic_exchange(&s_end_all_sessions, false)) {
        web_auth_end_sessions(-1);
    }
    if (atomic_exchange(&s_auth_reload, false)) {
        web_auth_reload();
    }
    if (atomic_exchange(&s_public_reload, false)) {
        load_public_status();
    }
    bool fw_event = atomic_exchange(&s_fw_changed, false);
    if (fw_event || half) {
        web_track_fw_state(fw_event);
    }
    if (atomic_exchange(&s_scan_done, false)) {
        web_note_scan_done();
    }
    unsigned changed = atomic_exchange(&s_caster_changed, 0u);
    for (size_t slot = 0; changed != 0 && slot < WEB_MAX_SLOTS; slot++) {
        if ((changed & (1u << slot)) == 0) {
            continue;
        }
        ntrip_caster_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (st == NULL) {
            st = malloc(sizeof(*st));
        }
        if (st != NULL && ntrip_server_get_status(slot, st) == ESP_OK) {
            char ev[96];
            snprintf(ev, sizeof(ev), "{\"type\":\"event\",\"event\":\"caster_state\",\"slot\":%u,\"state\":\"%s\"}",
                     (unsigned)slot, web_ntrip_state_name(st->state));
            web_ws_event(ev);
        }
        free(st);
    }
    if (second) {
        web_auth_expire();
        check_admin_reset();
    }
    web_ws_tick(second, half);
}

void web_kick(void)
{
    if (s_server != NULL && !atomic_exchange(&s_tick_queued, true)) {
        if (httpd_queue_work(s_server, tick_work, NULL) != ESP_OK) {
            atomic_store(&s_tick_queued, false);
        }
    }
}

static void on_tick_timer(void *arg)
{
    (void)arg;
    web_kick();
}

static void event_json(const char *event, const char *key, const char *value)
{
    char ev[160];
    if (key != NULL) {
        snprintf(ev, sizeof(ev), "{\"type\":\"event\",\"event\":\"%s\",\"%s\":\"%s\"}", event, key, value);
    } else {
        snprintf(ev, sizeof(ev), "{\"type\":\"event\",\"event\":\"%s\"}", event);
    }
    web_ws_event(ev);
}

static void on_station(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == STATION_EVENT_STATE_CHANGED && data != NULL) {
        event_json("station_state", "state", web_station_state_name(*(const station_state_t *)data));
    } else if (id == STATION_EVENT_FLASH_PROGRESS && data != NULL) {
        web_ws_note_flash_progress((const um98x_flash_progress_t *)data, false);
    } else if (id == STATION_EVENT_FLASH_ENDED && data != NULL) {
        web_ws_note_flash_progress((const um98x_flash_progress_t *)data, true);
    } else if (id == STATION_EVENT_RECEIVER_RESTARTED) {
        event_json("receiver_restarted", NULL, NULL);
    }
    web_kick();
}

static void on_ntrip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == NTRIP_EVENT_STATE_CHANGED && data != NULL) {
        size_t slot = *(const size_t *)data;
        if (slot < WEB_MAX_SLOTS) {
            atomic_fetch_or(&s_caster_changed, 1u << slot);
            web_kick();
        }
    }
}

static void on_fw(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == FW_UPDATE_EVENT_STATE_CHANGED && data != NULL) {
        event_json("update_state", "state", web_fw_state_name(*(const fw_update_state_t *)data));
        atomic_store(&s_fw_changed, true);
        web_kick();
    }
}

static void on_network(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    switch (id) {
    case NETWORK_EVENT_ONLINE:
        event_json("network", "what", "online");
        break;
    case NETWORK_EVENT_OFFLINE:
        event_json("network", "what", "offline");
        break;
    case NETWORK_EVENT_SETUP_AP_UP:
        event_json("network", "what", "ap_up");
        break;
    case NETWORK_EVENT_SETUP_AP_DOWN:
        event_json("network", "what", "ap_down");
        break;
    case NETWORK_EVENT_TIME_SYNCED:
        event_json("network", "what", "time_synced");
        break;
    case NETWORK_EVENT_SCAN_DONE:
        event_json("scan_done", NULL, NULL);
        atomic_store(&s_scan_done, true);
        break;
    default:
        return;
    }
    web_kick();
}

static void on_settings(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == SETTINGS_EVENT_FACTORY_RESET) {
        atomic_store(&s_end_all_sessions, true);
        atomic_store(&s_auth_reload, true);
        atomic_store(&s_public_reload, true);
        web_kick();
        return;
    }
    if (id != SETTINGS_EVENT_CHANGED || data == NULL) {
        return;
    }
    const settings_changed_t *c = data;
    char ns[sizeof(c->ns)];
    strlcpy(ns, c->ns, sizeof(ns));
    if (strcmp(ns, SETTINGS_NS_SYS) == 0 && (c->key[0] == '\0' || strcmp(c->key, SETTINGS_KEY_ADMIN_HASH) == 0)) {
        atomic_store(&s_auth_reload, true);
    }
    if (strcmp(ns, SETTINGS_NS_WEB) == 0) {
        atomic_store(&s_public_reload, true);
    }
    /* The namespace is ours (schema), plain ASCII: safe in the JSON string.
     * The client id (checked to [A-Za-z0-9_-] when it was noted) names the
     * page whose request changed it, when one did just now. */
    char client[sizeof(s_client_id)] = "";
    taskENTER_CRITICAL(&s_client_lock);
    if (s_client_id[0] != '\0' && (s_client_active || (uint32_t)(now_ms() - s_client_done_ms) < CLIENT_ID_HOLD_MS)) {
        strlcpy(client, s_client_id, sizeof(client));
    }
    taskEXIT_CRITICAL(&s_client_lock);
    char ev[160];
    if (client[0] != '\0') {
        snprintf(ev, sizeof(ev), "{\"type\":\"event\",\"event\":\"settings_changed\",\"ns\":\"%s\",\"client\":\"%s\"}", ns,
                 client);
    } else {
        snprintf(ev, sizeof(ev), "{\"type\":\"event\",\"event\":\"settings_changed\",\"ns\":\"%s\",\"client\":null}", ns);
    }
    web_ws_event(ev);
    web_kick();
}

static void reboot_work(void *arg)
{
    (void)arg;
    web_ws_rebooting(atomic_load(&s_reboot_reason));
    if (s_reboot_sem != NULL) {
        xSemaphoreGive(s_reboot_sem);
    }
}

static void on_lifecycle(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == LIFECYCLE_EVENT_PRE_REBOOT) {
        atomic_store(&s_reboot_reason, data != NULL ? (int)*(const lifecycle_reboot_reason_t *)data : 0);
        atomic_store(&s_rebooting, true);
        if (s_server != NULL) {
            (void)httpd_queue_work(s_server, reboot_work, NULL);
        }
    }
}

/* Before a planned restart: let pending responses go out, tell the WebSocket
 * clients, close them with 1012. Bounded by the hook budget. */
static void shutdown_hook(lifecycle_reboot_reason_t reason, uint32_t deadline_ms, void *ctx)
{
    (void)reason;   /* the PRE_REBOOT event already told the clients */
    (void)ctx;
    if (s_server == NULL) {
        return;
    }
    int64_t start = esp_timer_get_time();
    int64_t budget_us = (int64_t)(deadline_ms > 100 ? deadline_ms - 100 : deadline_ms) * 1000;
    if (budget_us > 1000000) {
        budget_us = 1000000;
    }
    while (web_worker_pending_responses() > 0 && esp_timer_get_time() - start < budget_us / 2) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    atomic_store(&s_rebooting, true);
    if (s_reboot_sem != NULL) {
        (void)xSemaphoreTake(s_reboot_sem, 0);
        if (httpd_queue_work(s_server, reboot_work, NULL) == ESP_OK) {
            int64_t left = budget_us - (esp_timer_get_time() - start);
            if (left > 0) {
                (void)xSemaphoreTake(s_reboot_sem, pdMS_TO_TICKS((uint32_t)(left / 1000)));
            }
        }
    }
    /* A moment for the close frames and the last responses to leave. */
    vTaskDelay(pdMS_TO_TICKS(50));
}

/* ---- cJSON in PSRAM -------------------------------------------------------------------------------- */

static void *json_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p != NULL ? p : malloc(size);
}

/* ---- public API ------------------------------------------------------------------------------------ */

esp_err_t web_init(void)
{
    /* Request parsing only (responses use the streaming writer); global
     * hooks, set before any other task parses JSON. */
    cJSON_Hooks hooks = { .malloc_fn = json_malloc, .free_fn = free };
    cJSON_InitHooks(&hooks);

    /* The UI digest keeps a development build whose version string does not
     * change from leaving a browser on a stale cached UI. */
    snprintf(s_etag, sizeof(s_etag), "\"%s-%s\"", esp_app_get_description()->version, web_ui_digest);
    web_conns_init(&s_conns, (uint32_t)CONFIG_WEB_REQUEST_DEADLINE_S * 1000u);
    load_public_status();
    web_auth_init();
    if (s_reboot_sem == NULL) {
        s_reboot_sem = xSemaphoreCreateBinary();
    }
    esp_err_t err = web_ws_init();
    if (err == ESP_OK) {
        err = web_worker_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    static const struct {
        const esp_event_base_t *base;
        esp_event_handler_t fn;
    } subs[] = {
        { &STATION_EVENT, on_station },     { &NTRIP_EVENT, on_ntrip },       { &FW_UPDATE_EVENT, on_fw },
        { &NETWORK_EVENT, on_network },     { &SETTINGS_EVENT, on_settings }, { &LIFECYCLE_EVENT, on_lifecycle },
    };
    for (size_t i = 0; i < sizeof(subs) / sizeof(subs[0]); i++) {
        err = esp_event_handler_register(*subs[i].base, ESP_EVENT_ANY_ID, subs[i].fn, NULL);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "event subscription failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    err = lifecycle_register_shutdown_hook(shutdown_hook, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown hook (%s): clients are not told about reboots", esp_err_to_name(err));
    }
    if (s_tick_timer == NULL) {
        const esp_timer_create_args_t args = { .callback = on_tick_timer, .name = "web_tick" };
        err = esp_timer_create(&args, &s_tick_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    ESP_LOGI(TAG, "%u UI asset(s), public status %s", (unsigned)web_ui_asset_count, s_public ? "on" : "off");
    return ESP_OK;
}

esp_err_t web_start(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }
    s_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    s_ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    load_hostname();

    /* The server's own warnings about a client that went away, a handshake
     * our check refused or a connection our handler closed (a refusal) are
     * expected client behaviour; our handlers log what matters. Fixed
     * levels: a later change of every tag's level ("*") keeps them. */
    static const char *const quiet[] = { "httpd_uri", "httpd_txrx", "httpd_ws" };
    for (size_t i = 0; i < sizeof(quiet) / sizeof(quiet[0]); i++) {
        esp_err_t lerr = diag_log_fix_level(quiet[i], ESP_LOG_ERROR);
        if (lerr != ESP_OK) {
            ESP_LOGW(TAG, "log level of %s not fixed (%s)", quiet[i], esp_err_to_name(lerr));
            esp_log_level_set(quiet[i], ESP_LOG_ERROR);
        }
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.task_priority = SERVER_PRIO;
    cfg.stack_size = SERVER_STACK;
    cfg.task_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    cfg.max_open_sockets = SERVER_MAX_SOCKETS;
    cfg.max_uri_handlers = SERVER_MAX_URIS;
    cfg.max_resp_headers = 12;
    cfg.max_req_hdr_len = SERVER_HDR_LEN;
    /* Not the server's own LRU purge: it would close an admin's quiet
     * WebSocket for anyone's new socket. on_sess_open keeps one socket free
     * by closing a connection that may go (web_conn.h). */
    cfg.lru_purge_enable = false;
    cfg.recv_wait_timeout = SERVER_RECV_TIMEOUT_S;
    cfg.send_wait_timeout = SERVER_SEND_TIMEOUT_S;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.open_fn = on_sess_open;
    cfg.close_fn = on_sess_close;
    esp_err_t err = httpd_start(&s_server, &cfg);
    if (err != ESP_OK) {
        s_server = NULL;
        return err;
    }

    for (size_t i = 0; i < web_api_route_count; i++) {
        const web_route_t *rt = &web_api_routes[i];
        httpd_uri_t u = {
            .uri = rt->uri,
            .method = rt->method,
            .handler = api_dispatch,
            .user_ctx = (void *)rt,
        };
        err = httpd_register_uri_handler(s_server, &u);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "route %s: %s", rt->uri, esp_err_to_name(err));
            break;
        }
    }
    if (err == ESP_OK) {
        httpd_uri_t ws = {
            .uri = "/ws",
            .method = HTTP_GET,
            .handler = ws_frame,
            .is_websocket = true,
            .handle_ws_control_frames = true,   /* answered in web_ws_handler, so every frame ends in a handler */
            .ws_pre_handshake_cb = web_ws_pre_handshake,
            .ws_post_handshake_cb = web_ws_post_handshake,
        };
        err = httpd_register_uri_handler(s_server, &ws);
    }
    if (err == ESP_OK) {
        /* Every method, HEAD included: HEAD on the page and the assets answers
         * as GET does without the body, HEAD on /api/ and /ws 405 (also
         * without a body). One route, so no handler slot is spent on it. */
        httpd_uri_t any = { .uri = "/*", .method = HTTP_ANY, .handler = fallback };
        err = httpd_register_uri_handler(s_server, &any);
    }
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }
    err = esp_timer_start_periodic(s_tick_timer, (uint64_t)TICK_MS * 1000);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "tick timer: %s", esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "listening on port 80 (hostname %s)", s_hostname);
    return ESP_OK;
}

esp_err_t web_stop(void)
{
    if (s_tick_timer != NULL) {
        (void)esp_timer_stop(s_tick_timer);
    }
    if (s_server == NULL) {
        return ESP_OK;
    }
    /* Stopping closes every session; the close callback forgets the WebSocket clients. */
    esp_err_t err = httpd_stop(s_server);
    s_server = NULL;
    return err;
}
