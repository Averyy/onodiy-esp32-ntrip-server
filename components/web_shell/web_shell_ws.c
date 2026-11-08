/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * WebSocket clients (/ws): hello, 1 Hz status deltas, events, job progress
 * and the log, with a bounded queue per client. Server task only, except
 * web_ws_event(), web_ws_note_flash_progress() and web_ws_publish(), which
 * copy their input and hand it over.
 *
 * Status deltas: once a second the status object is built once per form
 * (admin, public), section by section; each section's text is compared with
 * the previous second's by CRC-32 (casters per slot) and one message carries
 * only what changed. A new client, and one whose queue dropped a status
 * message, gets the next message with every section ("full": true).
 *
 * Sending never blocks the server for long: a queued message goes out only
 * while the socket has room (select() with no wait), and WebSocket sockets
 * get a short send timeout. A client whose queue stays full for 30 s is
 * closed with 4003.
 */
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>

#include "diag.h"
#include "web_shell_internal.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "web";

#define WS_MAX_CLIENTS     4
#define WS_MAX_PUBLIC      2      /* the rest stay free for admin pages */
#define WS_IN_MAX          256u
#define WS_STALL_CLOSE_S   30u
#define WS_SEND_TIMEOUT_MS 3000
#define SEC_TEXT_MAX       1280u  /* one status section: the receiver's is the largest (test_web_live sizes its worst case) */
#define CST_TEXT_MAX       896u   /* one caster summary: about 780 bytes at worst (escaped name and hint, stop_reason) */
#define MSG_SCRATCH        16384u
#define EVENTS_MAX         16
#define EVENT_TEXT_MAX     160
#define EVENTS_PER_S       10
#define LOG_CHUNK          4096u

#define CLOSE_GOING_AWAY_RESTART 1012
#define CLOSE_POLICY             1008
#define CLOSE_TOO_BIG            1009
#define CLOSE_TRY_AGAIN_LATER    1013
#define CLOSE_SESSION_ENDED      4001
#define CLOSE_TOO_SLOW           4003

/* ---- messages ------------------------------------------------------------------------------ */

typedef struct ws_msg {
    int refs;                /* server task only */
    web_msg_kind_t kind;
    size_t len;
    char data[];
} ws_msg_t;

static ws_msg_t *msg_new(web_msg_kind_t kind, const char *text, size_t len)
{
    ws_msg_t *m = heap_caps_malloc(sizeof(*m) + len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (m == NULL) {
        m = malloc(sizeof(*m) + len + 1);
    }
    if (m == NULL) {
        return NULL;
    }
    m->refs = 1;
    m->kind = kind;
    m->len = len;
    memcpy(m->data, text, len);
    m->data[len] = '\0';
    return m;
}

static void msg_release(ws_msg_t *m)
{
    if (m != NULL && --m->refs <= 0) {
        free(m);
    }
}

/* ---- clients --------------------------------------------------------------------------------- */

typedef struct {
    bool used;
    bool closing;
    int fd;
    bool admin;
    int sess;
    uint32_t gen;
    uint32_t topics;
    bool need_full;
    bool log_gap;
    uint32_t log_cursor;        /* log lines up to here were queued for this client */
    ws_msg_t *q[WEB_WSQ_MAX_MSGS];
    size_t qn;
    uint32_t refused_since_s;   /* 0 = the last message was taken */
} ws_client_t;

static ws_client_t s_clients[WS_MAX_CLIENTS];

/* Handshake in progress: what the pre-handshake check found (server task, one request at a time). */
static int s_pending_sess = -1;

static ws_client_t *find(int fd)
{
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (s_clients[i].used && s_clients[i].fd == fd) {
            return &s_clients[i];
        }
    }
    return NULL;
}

size_t web_ws_client_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        n += s_clients[i].used ? 1 : 0;
    }
    return n;
}

static void queue_clear(ws_client_t *c)
{
    for (size_t i = 0; i < c->qn; i++) {
        msg_release(c->q[i]);
        c->q[i] = NULL;
    }
    c->qn = 0;
}

static void client_close(ws_client_t *c, uint16_t code)
{
    httpd_handle_t hd = web_server();
    if (!c->used || c->closing) {
        return;
    }
    c->closing = true;
    queue_clear(c);
    web_conn_ws_closing(c->fd);
    if (hd != NULL) {
        uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
        httpd_ws_frame_t f = {
            .final = true,
            .type = HTTPD_WS_TYPE_CLOSE,
            .payload = payload,
            .len = sizeof(payload),
        };
        (void)httpd_ws_send_frame_async(hd, c->fd, &f);
        web_conn_shut(c->fd);   /* after the close frame, which is on its way */
    }
}

void web_ws_on_close(int fd)
{
    ws_client_t *c = find(fd);
    if (c != NULL) {
        queue_clear(c);
        memset(c, 0, sizeof(*c));
        c->fd = -1;
    }
}

static bool writable(int fd)
{
    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = { 0, 0 };
    return select(fd + 1, NULL, &wfds, NULL, &tv) > 0 && FD_ISSET(fd, &wfds);
}

static void drain(ws_client_t *c)
{
    httpd_handle_t hd = web_server();
    while (hd != NULL && c->used && !c->closing && c->qn > 0) {
        if (!writable(c->fd)) {
            break;
        }
        ws_msg_t *m = c->q[0];
        httpd_ws_frame_t f = {
            .final = true,
            .type = HTTPD_WS_TYPE_TEXT,
            .payload = (uint8_t *)m->data,
            .len = m->len,
        };
        esp_err_t err = httpd_ws_send_frame_async(hd, c->fd, &f);
        memmove(&c->q[0], &c->q[1], (c->qn - 1) * sizeof(c->q[0]));
        c->qn--;
        c->q[c->qn] = NULL;
        msg_release(m);
        if (err != ESP_OK) {
            /* Half a frame may be on the wire: the connection cannot continue. */
            c->closing = true;
            queue_clear(c);
            web_conn_ws_closing(c->fd);
            web_conn_shut(c->fd);
            break;
        }
    }
}

/* Offer one message to a client's queue (the queue rules of web_wsq_admit). */
static void enqueue(ws_client_t *c, ws_msg_t *m)
{
    if (!c->used || c->closing) {
        return;
    }
    web_msg_kind_t kinds[WEB_WSQ_MAX_MSGS];
    size_t sizes[WEB_WSQ_MAX_MSGS];
    for (size_t i = 0; i < c->qn; i++) {
        kinds[i] = c->q[i]->kind;
        sizes[i] = c->q[i]->len;
    }
    web_wsq_decision_t d = web_wsq_admit(kinds, sizes, c->qn, m->kind, m->len);
    if (!d.accept) {
        if (m->kind == WEB_MSG_STATUS) {
            c->need_full = true;
        } else if (m->kind == WEB_MSG_LOG) {
            c->log_gap = true;
        }
        uint32_t now = web_now_s();
        if (c->refused_since_s == 0) {
            c->refused_since_s = now == 0 ? 1 : now;
        } else if ((uint32_t)(now - c->refused_since_s) >= WS_STALL_CLOSE_S) {
            ESP_LOGW(TAG, "WebSocket client too slow: closing it");
            client_close(c, CLOSE_TOO_SLOW);
        }
        return;
    }
    c->refused_since_s = 0;
    if (d.evict >= 0) {
        msg_release(c->q[d.evict]);
        memmove(&c->q[d.evict], &c->q[d.evict + 1], (c->qn - (size_t)d.evict - 1) * sizeof(c->q[0]));
        c->qn--;
    }
    m->refs++;
    c->q[c->qn++] = m;
}

/* A message for every client that matches (admin_only, topic bit; 0 = any topic). */
static void broadcast(web_msg_kind_t kind, const char *text, size_t len, bool admin_only, uint32_t topic)
{
    ws_msg_t *m = NULL;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_client_t *c = &s_clients[i];
        if (!c->used || c->closing || (admin_only && !c->admin) || (topic != 0 && (c->topics & topic) == 0)) {
            continue;
        }
        if (m == NULL && (m = msg_new(kind, text, len)) == NULL) {
            return;
        }
        enqueue(c, m);
    }
    msg_release(m);
}

/* ---- handshake and client messages ----------------------------------------------------------------- */

static size_t public_count(void)
{
    size_t n = 0;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        n += (s_clients[i].used && !s_clients[i].admin) ? 1 : 0;
    }
    return n;
}

/* Why a handshake was refused: one value per refusal text. */
typedef enum {
    WS_REFUSED_HOST_ORIGIN = 0,
    WS_REFUSED_BODY,
    WS_REFUSED_NO_SESSION,
    WS_REFUSED_PUBLIC_FULL,
    WS_REFUSED_ALL_FULL,
    WS_REFUSED_COUNT,
} ws_refused_t;

static const char *const k_refused_text[WS_REFUSED_COUNT] = {
    [WS_REFUSED_HOST_ORIGIN] = "Host or Origin not accepted",
    [WS_REFUSED_BODY] = "a body with the handshake",
    [WS_REFUSED_NO_SESSION] = "no session, and public status is off",
    [WS_REFUSED_PUBLIC_FULL] = "every public viewer slot is taken",
    [WS_REFUSED_ALL_FULL] = "every WebSocket slot is taken",
};

/* Refused handshakes: at most one line a minute per client and reason (a
 * page that keeps retrying shows up once, with its address, and its "no
 * session" lines never hide a full slot met by another tab of the same
 * client). "+N not logged" counts the lines that pair held back. */
EXT_RAM_BSS_ATTR static web_lograte_t s_refused_log;

static void log_refused(httpd_req_t *req, ws_refused_t why)
{
    web_addr_t a;
    bool known = web_req_peer(req, &a);
    uint32_t held = 0;
    if (!web_lograte_allow(&s_refused_log, known ? web_client_key(&a) : 0, (uint8_t)why, web_now_s(), &held)) {
        return;
    }
    char who[48];
    (void)web_addr_text(known ? &a : NULL, who, sizeof(who));
    const char *text = (unsigned)why < WS_REFUSED_COUNT ? k_refused_text[why] : "?";
    if (held > 0) {
        ESP_LOGW(TAG, "WebSocket refused for %s: %s (+%u not logged)", who, text, (unsigned)held);
    } else {
        ESP_LOGW(TAG, "WebSocket refused for %s: %s", who, text);
    }
}

esp_err_t web_ws_pre_handshake(httpd_req_t *req)
{
    s_pending_sess = -1;
    if (!web_ws_precheck(req)) {
        log_refused(req, WS_REFUSED_HOST_ORIGIN);
        return ESP_FAIL;
    }
    /* A handshake carries no body. The server would read one away after the
     * handshake, off our request deadline's books: refused and closed. */
    if (web_req_body_of(req) != WEB_REQ_BODY_NONE) {
        log_refused(req, WS_REFUSED_BODY);
        return web_refuse_and_close(req, WEB_ERR_BAD_REQUEST);
    }
    /* Not touched yet: a refused handshake must not keep a session alive
     * (a hidden tab retrying would); a successful one touches it. */
    int sess = web_auth_session(req, false);
    if (sess < 0 && !web_public_status()) {
        log_refused(req, WS_REFUSED_NO_SESSION);
        web_send_error(req, WEB_ERR_FORBIDDEN, "Public status is off: log in", NULL, UINT32_MAX, NULL, NULL);
        return ESP_FAIL;
    }
    /* Public viewers get at most WS_MAX_PUBLIC slots, so they can never
     * take every slot from the admin's pages. */
    if (web_ws_client_count() >= WS_MAX_CLIENTS || (sess < 0 && public_count() >= WS_MAX_PUBLIC)) {
        log_refused(req, sess < 0 && web_ws_client_count() < WS_MAX_CLIENTS ? WS_REFUSED_PUBLIC_FULL
                                                                            : WS_REFUSED_ALL_FULL);
        web_send_err(req, WEB_ERR_TOO_MANY_CLIENTS);
        return ESP_FAIL;
    }
    s_pending_sess = sess;
    return ESP_OK;
}

static void send_hello(ws_client_t *c)
{
    char text[160];
    snprintf(text, sizeof(text), "{\"type\":\"hello\",\"api\":%d,\"admin\":%s,\"topics\":[%s],\"status_period_ms\":1000}",
             WEB_SHELL_API_VERSION, c->admin ? "true" : "false", c->admin ? "\"status\",\"progress\"" : "\"status\"");
    ws_msg_t *m = msg_new(WEB_MSG_OTHER, text, strlen(text));
    if (m != NULL) {
        enqueue(c, m);
        msg_release(m);
    }
    drain(c);
}

esp_err_t web_ws_post_handshake(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    ws_client_t *c = NULL;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].used) {
            c = &s_clients[i];
            break;
        }
    }
    if (c == NULL) {
        return ESP_FAIL;
    }
    memset(c, 0, sizeof(*c));
    c->used = true;
    c->fd = fd;
    c->sess = s_pending_sess;
    c->admin = s_pending_sess >= 0;
    c->gen = web_auth_session_gen(s_pending_sess);
    c->topics = c->admin ? (WEB_TOPIC_STATUS | WEB_TOPIC_PROGRESS) : WEB_TOPIC_STATUS;
    c->need_full = true;
    s_pending_sess = -1;
    if (c->admin) {
        web_auth_session_touch(c->sess);
    }
    /* A slow client must not hold the server for the full HTTP send timeout. */
    struct timeval tv = { .tv_sec = WS_SEND_TIMEOUT_MS / 1000, .tv_usec = (WS_SEND_TIMEOUT_MS % 1000) * 1000 };
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    web_conn_ws_open(fd, c->admin);
    send_hello(c);
    return ESP_OK;
}

/* Control frames (the route asks for them, so every frame reaches a handler
 * and ends its receive deadline): answered as the server would itself. They
 * never extend a session: only the page's own messages do. */
static esp_err_t control_frame(httpd_req_t *req, ws_client_t *c, httpd_ws_frame_t *f)
{
    httpd_ws_frame_t reply = { .final = true };
    switch (f->type) {
    case HTTPD_WS_TYPE_PING:
        reply.type = HTTPD_WS_TYPE_PONG;
        reply.payload = f->len > 0 ? f->payload : NULL;
        reply.len = f->len;
        return httpd_ws_send_frame(req, &reply);
    case HTTPD_WS_TYPE_CLOSE:
        /* The server closes the socket after this frame; answer the close
         * unless ours went out first. */
        if (c == NULL || !c->closing) {
            if (c != NULL) {
                c->closing = true;
                queue_clear(c);
                web_conn_ws_closing(c->fd);
            }
            reply.type = HTTPD_WS_TYPE_CLOSE;
            return httpd_ws_send_frame(req, &reply);
        }
        return ESP_OK;
    default:   /* PONG */
        return ESP_OK;
    }
}

esp_err_t web_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* A plain GET without the upgrade headers. A body is never read: the
         * server would read it away after this handler. */
        return web_req_body_of(req) != WEB_REQ_BODY_NONE ? web_refuse_and_close(req, WEB_ERR_BAD_REQUEST)
                                                          : web_send_err(req, WEB_ERR_BAD_REQUEST);
    }
    int fd = httpd_req_to_sockfd(req);
    ws_client_t *c = find(fd);
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof(f));
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (f.len > WS_IN_MAX) {
        /* (control frames carry at most 125 bytes) */
        if (c == NULL || c->closing) {
            return ESP_FAIL;   /* the rest of the frame stays unread: drop the connection */
        }
        client_close(c, CLOSE_TOO_BIG);
        return ESP_OK;
    }
    /* Read the whole frame, also when it is ignored, so the next one starts in step. */
    uint8_t buf[WS_IN_MAX + 1];
    if (f.len > 0) {
        f.payload = buf;
        err = httpd_ws_recv_frame(req, &f, f.len);
        if (err != ESP_OK) {
            return err;
        }
    }
    buf[f.len] = '\0';
    if (f.type == HTTPD_WS_TYPE_PING || f.type == HTTPD_WS_TYPE_PONG || f.type == HTTPD_WS_TYPE_CLOSE) {
        return control_frame(req, c, &f);
    }
    if (c == NULL || c->closing) {
        return ESP_OK;
    }
    /* Every message from the page extends its session. */
    if (c->admin && web_auth_session_alive(c->sess, c->gen)) {
        web_auth_session_touch(c->sess);
    }
    if (f.type != HTTPD_WS_TYPE_TEXT) {
        return ESP_OK;
    }
    web_ws_sub_t sub = { 0 };
    switch (web_parse_ws_message((const char *)buf, f.len, &sub)) {
    case WEB_WS_MSG_SUBSCRIBE: {
        uint32_t topics = sub.topics;
        if (!c->admin) {
            topics &= WEB_TOPIC_STATUS;   /* admin topics are ignored for public clients */
        }
        if ((topics & WEB_TOPIC_STATUS) != 0 && (c->topics & WEB_TOPIC_STATUS) == 0) {
            c->need_full = true;
        }
        if ((topics & WEB_TOPIC_LOG) != 0) {
            /* The log continues after the client's own cursor (what it read
             * over HTTP), or with new lines only. */
            if (sub.has_log_cursor) {
                c->log_cursor = sub.log_cursor;
                c->log_gap = false;
            } else if ((c->topics & WEB_TOPIC_LOG) == 0) {
                c->log_cursor = diag_log_head();
                c->log_gap = false;
            }
        }
        c->topics = topics;
        break;
    }
    case WEB_WS_MSG_PING: {
        if (c->admin && web_auth_session_alive(c->sess, c->gen)) {
            web_shell_admin_activity();   /* the page is open: the GNSS ladder's restart waits */
        }
        static const char pong[] = "{\"type\":\"pong\"}";
        ws_msg_t *m = msg_new(WEB_MSG_OTHER, pong, sizeof(pong) - 1);
        if (m != NULL) {
            enqueue(c, m);
            msg_release(m);
        }
        drain(c);
        break;
    }
    default:
        break;
    }
    return ESP_OK;
}

void web_ws_close_public(void)
{
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_client_t *c = &s_clients[i];
        if (c->used && !c->admin) {
            client_close(c, CLOSE_POLICY);
        }
    }
}

bool web_ws_evict(int fd)
{
    ws_client_t *c = find(fd);
    if (c == NULL) {
        return false;
    }
    client_close(c, CLOSE_TRY_AGAIN_LATER);
    return true;
}

void web_ws_close_sessions(uint32_t mask)
{
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_client_t *c = &s_clients[i];
        if (c->used && c->admin && c->sess >= 0 && (mask & (1u << c->sess)) != 0 &&
            !web_auth_session_alive(c->sess, c->gen)) {
            client_close(c, CLOSE_SESSION_ENDED);
        }
    }
}

/* ---- events -------------------------------------------------------------------------------------------- */

typedef struct {
    char text[EVENT_TEXT_MAX];
    char key[48];
} ws_event_t;

static portMUX_TYPE s_ev_lock = portMUX_INITIALIZER_UNLOCKED;
static ws_event_t s_events[EVENTS_MAX];
static size_t s_ev_head;
static size_t s_ev_count;
static uint32_t s_ev_tokens = EVENTS_PER_S;
static uint32_t s_ev_refill_s;

void web_ws_event(const char *json)
{
    if (json == NULL || strlen(json) >= EVENT_TEXT_MAX) {
        return;
    }
    char key[48] = "";
    const web_shell_config_t *cfg=web_shell_config();
    bool coalesce=cfg->event_key && cfg->event_key(cfg->ctx,json,key,sizeof(key));
    key[sizeof(key)-1]=0;
    coalesce=coalesce && key[0];
    portENTER_CRITICAL(&s_ev_lock);
    bool replaced = false;
    for (size_t i = 0; coalesce && i < s_ev_count; i++) {
        ws_event_t *e = &s_events[(s_ev_head + i) % EVENTS_MAX];
        if (strcmp(key, e->key) == 0) {
            strlcpy(e->text, json, sizeof(e->text));
            replaced = true;
            break;
        }
    }
    if (!replaced) {
        if (s_ev_count == EVENTS_MAX) {
            s_ev_head = (s_ev_head + 1) % EVENTS_MAX;   /* drop the oldest */
            s_ev_count--;
        }
        ws_event_t *e=&s_events[(s_ev_head+s_ev_count)%EVENTS_MAX];
        strlcpy(e->text,json,sizeof(e->text));
        strlcpy(e->key,coalesce ? key : "",sizeof(e->key));
        s_ev_count++;
    }
    portEXIT_CRITICAL(&s_ev_lock);
}

static void flush_events(void)
{
    uint32_t now = web_now_s();
    if (now != s_ev_refill_s) {
        s_ev_refill_s = now;
        s_ev_tokens = EVENTS_PER_S;
    }
    while (s_ev_tokens > 0) {
        ws_event_t ev;
        bool have = false;
        portENTER_CRITICAL(&s_ev_lock);
        if (s_ev_count > 0) {
            ev = s_events[s_ev_head];
            s_ev_head = (s_ev_head + 1) % EVENTS_MAX;
            s_ev_count--;
            have = true;
        }
        portEXIT_CRITICAL(&s_ev_lock);
        if (!have) {
            break;
        }
        s_ev_tokens--;
        broadcast(WEB_MSG_EVENT, ev.text, strlen(ev.text), true, 0);
    }
}

/* ---- status -------------------------------------------------------------------------------------------- */

EXT_RAM_BSS_ATTR static char s_scratch[MSG_SCRATCH];
EXT_RAM_BSS_ATTR static web_shell_baseline_t s_baseline[2];
EXT_RAM_BSS_ATTR static web_shell_frame_t s_frame;
EXT_RAM_BSS_ATTR static char s_frame_text[WEB_SHELL_STATUS_MAX];
static uint32_t s_seq[2];

esp_err_t web_shell_status_rest(web_req_t *r)
{
    const web_shell_registry_t *reg=web_shell_registry();
    const void *snap=NULL;
    uint32_t uptime=0;
    if (!reg->status.capture(reg->status.ctx,&snap,&uptime)) return web_send_internal(r->req,ESP_FAIL);
    size_t cap;
    char *buf=web_http_buf(&cap);
    web_resp_t resp;
    web_resp_begin(&resp,r->req,200,buf,cap);
    bool ok=web_shell_status_stream(reg,snap,uptime,!r->admin,&resp.w);
    reg->status.release(reg->status.ctx,snap);
    if (!ok) return resp.headers_sent ? ESP_FAIL : web_send_internal(r->req,ESP_ERR_INVALID_SIZE);
    return web_resp_end(&resp);
}

static void push_status(void)
{
    bool want[2]={false,false};
    bool pub=web_public_status();
    for (size_t i=0;i<WS_MAX_CLIENTS;i++) {
        ws_client_t *c=&s_clients[i];
        if (c->used && !c->closing && (c->topics & WEB_TOPIC_STATUS) && (c->admin || pub)) want[c->admin ? 0 : 1]=true;
    }
    if (!want[0] && !want[1]) { s_baseline[0].valid=s_baseline[1].valid=false; return; }
    const web_shell_registry_t *reg=web_shell_registry();
    const void *snap=NULL;
    uint32_t uptime=0;
    if (!reg->status.capture(reg->status.ctx,&snap,&uptime)) return;
    web_shell_frame_t *frame=&s_frame;
    for (int form=0;form<2;form++) {
        if (!want[form]) { s_baseline[form].valid=false; continue; }
        if (!web_shell_frame_build(reg,snap,uptime,form==1,frame)) continue;
        bool removed=web_shell_frame_removed(frame,&s_baseline[form]);
        ws_msg_t *delta=NULL,*full=NULL;
        bool failed=false;
        for (size_t i=0;i<WS_MAX_CLIENTS;i++) {
            ws_client_t *c=&s_clients[i];
            if (!c->used || c->closing || !(c->topics & WEB_TOPIC_STATUS) || (c->admin ? 0 : 1) != form) continue;
            if (removed || !s_baseline[form].valid) c->need_full=true;
            ws_msg_t **slot=c->need_full ? &full : &delta;
            if (!*slot) {
                size_t len=web_shell_frame_render(reg,frame,&s_baseline[form],true,c->need_full,
                                                 ++s_seq[form],s_scratch,reg->status_cap);
                if (!len || !(*slot=msg_new(WEB_MSG_STATUS,s_scratch,len))) { failed=true; c->need_full=true; continue; }
            }
            c->need_full=false;
            enqueue(c,*slot);
        }
        /* Allocation/composition failure cannot advance a shared baseline:
         * recipients that did get this cycle resync with everyone next time. */
        if (!failed) web_shell_frame_commit(frame,&s_baseline[form]);
        else for (size_t i=0;i<WS_MAX_CLIENTS;i++) if ((s_clients[i].admin ? 0 : 1)==form) s_clients[i].need_full=true;
        msg_release(delta); msg_release(full);
    }
    reg->status.release(reg->status.ctx,snap);
}

void web_shell_http_progress(const char *json)
{
    if (json && strlen(json) < web_shell_registry()->status_cap)
        broadcast(WEB_MSG_PROGRESS,json,strlen(json),true,WEB_TOPIC_PROGRESS);
}

/* ---- log ------------------------------------------------------------------------------------------------- */

EXT_RAM_BSS_ATTR static char s_log_text[LOG_CHUNK];

/* Every client reads the log from its own cursor: a message carries the
 * lines after `from` up to `cursor`, so the client can tell it continues
 * exactly where its own reading (HTTP or earlier messages) stopped. A
 * message the client's queue cannot take now is not lost: the cursor stays
 * and the same lines go out later. `gap` only when the ring overwrote lines
 * before this client read them. */
static void push_log(void)
{
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_client_t *c = &s_clients[i];
        if (!c->used || c->closing || !c->admin || (c->topics & WEB_TOPIC_LOG) == 0 || c->qn >= WEB_WSQ_MAX_MSGS) {
            continue;
        }
        uint32_t from = c->log_cursor;
        diag_log_read_info_t info = { 0 };
        size_t n = diag_log_read_ex(DIAG_LOG_THIS_BOOT, &c->log_cursor, s_log_text, sizeof(s_log_text), &info);
        if (n == 0 && !info.gap) {
            continue;
        }
        wj_t w;
        wj_init(&w, s_scratch, sizeof(s_scratch), NULL, NULL);
        wj_obj_open(&w, NULL);
        wj_str(&w, "type", "log");
        wj_uint(&w, "from", from);
        wj_uint(&w, "cursor", c->log_cursor);
        wj_bool(&w, "gap", info.gap || c->log_gap);
        wj_strn(&w, "text", s_log_text, n);
        wj_obj_close(&w);
        if (!wj_ok(&w)) {
            c->log_gap = true;   /* these lines do not fit one message (escaped): skipped, said so next time */
            continue;
        }
        ws_msg_t *m = msg_new(WEB_MSG_LOG, s_scratch, w.len);
        if (m == NULL) {
            c->log_cursor = from;   /* no memory now: try again next time */
            continue;
        }
        bool had_gap = c->log_gap;
        enqueue(c, m);
        bool taken = m->refs > 1;
        msg_release(m);
        if (taken) {
            c->log_gap = false;
        } else {
            c->log_cursor = from;   /* not queued: the same lines next time, so no gap */
            c->log_gap = had_gap;
        }
    }
}

/* ---- tick, publish, reboot -------------------------------------------------------------------------------- */

void web_ws_tick(bool second, bool half_second)
{
    flush_events();
    if (half_second) {
        const web_shell_config_t *cfg=web_shell_config();
        if (cfg->progress) cfg->progress(cfg->ctx);
        push_log();
    }
    if (second) {
        /* Ended sessions (expiry, logout elsewhere) lose their clients. */
        for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
            ws_client_t *c = &s_clients[i];
            if (c->used && !c->closing && c->admin && !web_auth_session_alive(c->sess, c->gen)) {
                client_close(c, CLOSE_SESSION_ENDED);
            }
        }
        push_status();
    }
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        drain(&s_clients[i]);
    }
}

typedef struct {
    uint32_t topic;
    ws_msg_t *msg;
} publish_t;

static void publish_work(void *arg)
{
    publish_t *p = arg;
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_client_t *c = &s_clients[i];
        if (c->used && !c->closing && (c->topics & p->topic) != 0 && (c->admin || p->topic == WEB_TOPIC_STATUS)) {
            enqueue(c, p->msg);
            drain(c);
        }
    }
    msg_release(p->msg);
    free(p);
}

esp_err_t web_ws_publish(const char *topic, const char *json)
{
    if (topic == NULL || json == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint32_t bit = strcmp(topic, "status") == 0     ? WEB_TOPIC_STATUS
                   : strcmp(topic, "progress") == 0 ? WEB_TOPIC_PROGRESS
                   : strcmp(topic, "log") == 0      ? WEB_TOPIC_LOG
                                                    : 0;
    httpd_handle_t hd = web_server();
    if (bit == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hd == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strlen(json) >= web_shell_registry()->status_cap) return ESP_ERR_INVALID_SIZE;
    web_msg_kind_t kind = bit == WEB_TOPIC_STATUS ? WEB_MSG_STATUS : bit == WEB_TOPIC_LOG ? WEB_MSG_LOG : WEB_MSG_PROGRESS;
    publish_t *p = malloc(sizeof(*p));
    ws_msg_t *m = msg_new(kind, json, strlen(json));
    if (p == NULL || m == NULL) {
        free(p);
        free(m);
        return ESP_ERR_NO_MEM;
    }
    p->topic = bit;
    p->msg = m;
    if (httpd_queue_work(hd, publish_work, p) != ESP_OK) {
        free(p);
        free(m);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool s_reboot_sent;

void web_ws_rebooting(int reason)
{
    if (s_reboot_sent) {
        return;
    }
    s_reboot_sent = true;
    char text[96];
    snprintf(text, sizeof(text), "{\"type\":\"event\",\"event\":\"rebooting\",\"reason\":\"%s\"}",
             reason >= 0 ? web_reboot_reason_name((lifecycle_reboot_reason_t)reason) : "unknown");
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_client_t *c = &s_clients[i];
        if (!c->used || c->closing) {
            continue;
        }
        if (c->admin) {
            /* Ahead of everything still queued: the reboot is what matters now. */
            queue_clear(c);
            ws_msg_t *m = msg_new(WEB_MSG_EVENT, text, strlen(text));
            if (m != NULL) {
                enqueue(c, m);
                msg_release(m);
            }
            drain(c);
        }
        client_close(c, CLOSE_GOING_AWAY_RESTART);
    }
}

esp_err_t web_ws_init(void)
{
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        memset(&s_clients[i], 0, sizeof(s_clients[i]));
        s_clients[i].fd = -1;
    }
    s_reboot_sent = false;
    memset(s_baseline,0,sizeof(s_baseline));
    web_shell_frame_attach(&s_frame,s_frame_text,sizeof(s_frame_text));
    web_lograte_init(&s_refused_log);
    return ESP_OK;
}
