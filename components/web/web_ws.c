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
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "station.h"
#include "web.h"
#include "web_enums.h"
#include "web_internal.h"
#include "web_parse.h"

static const char *TAG = "web";

#define WS_MAX_CLIENTS     4
#define WS_MAX_PUBLIC      2      /* the rest stay free for admin pages */
#define WS_IN_MAX          256u
#define WS_STALL_CLOSE_S   30u
#define WS_SEND_TIMEOUT_MS 3000
#define SEC_TEXT_MAX       1024u
#define CST_TEXT_MAX       768u
#define MSG_SCRATCH        16384u
#define EVENTS_MAX         16
#define EVENT_TEXT_MAX     160
#define EVENTS_PER_S       10
#define LOG_CHUNK          4096u

#define CLOSE_GOING_AWAY_RESTART 1012
#define CLOSE_POLICY             1008
#define CLOSE_TOO_BIG            1009
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
    if (hd != NULL) {
        uint8_t payload[2] = { (uint8_t)(code >> 8), (uint8_t)(code & 0xFF) };
        httpd_ws_frame_t f = {
            .final = true,
            .type = HTTPD_WS_TYPE_CLOSE,
            .payload = payload,
            .len = sizeof(payload),
        };
        (void)httpd_ws_send_frame_async(hd, c->fd, &f);
        (void)httpd_sess_trigger_close(hd, c->fd);
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
            (void)httpd_sess_trigger_close(hd, c->fd);
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

esp_err_t web_ws_pre_handshake(httpd_req_t *req)
{
    s_pending_sess = -1;
    if (!web_ws_precheck(req)) {
        return ESP_FAIL;
    }
    /* Not touched yet: a refused handshake must not keep a session alive
     * (a hidden tab retrying would); a successful one touches it. */
    int sess = web_auth_session(req, false);
    if (sess < 0 && !web_public_status()) {
        web_send_error(req, WEB_ERR_FORBIDDEN, "Public status is off: log in", NULL, UINT32_MAX, NULL, NULL);
        return ESP_FAIL;
    }
    /* Public viewers get at most WS_MAX_PUBLIC slots, so they can never
     * take every slot from the admin's pages. */
    if (web_ws_client_count() >= WS_MAX_CLIENTS || (sess < 0 && public_count() >= WS_MAX_PUBLIC)) {
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
             WEB_API_VERSION, c->admin ? "true" : "false", c->admin ? "\"status\",\"progress\"" : "\"status\"");
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
    send_hello(c);
    return ESP_OK;
}

esp_err_t web_ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        /* A plain GET without the upgrade headers. */
        return web_send_err(req, WEB_ERR_BAD_REQUEST);
    }
    int fd = httpd_req_to_sockfd(req);
    ws_client_t *c = find(fd);
    httpd_ws_frame_t f;
    memset(&f, 0, sizeof(f));
    esp_err_t err = httpd_ws_recv_frame(req, &f, 0);
    if (err != ESP_OK) {
        return err;
    }
    if (c == NULL || c->closing) {
        return ESP_OK;
    }
    if (f.len > WS_IN_MAX) {
        client_close(c, CLOSE_TOO_BIG);
        return ESP_OK;
    }
    uint8_t buf[WS_IN_MAX + 1];
    if (f.len > 0) {
        f.payload = buf;
        err = httpd_ws_recv_frame(req, &f, f.len);
        if (err != ESP_OK) {
            return err;
        }
    }
    buf[f.len] = '\0';
    /* Every message from the client extends its session. */
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
} ws_event_t;

static portMUX_TYPE s_ev_lock = portMUX_INITIALIZER_UNLOCKED;
static ws_event_t s_events[EVENTS_MAX];
static size_t s_ev_head;
static size_t s_ev_count;
static uint32_t s_ev_tokens = EVENTS_PER_S;
static uint32_t s_ev_refill_s;

/* Coalescing key: the event name and, for caster events, the slot. */
static size_t event_key(const char *json, char *key, size_t cap)
{
    const char *e = strstr(json, "\"event\":\"");
    if (e == NULL) {
        key[0] = '\0';
        return 0;
    }
    e += 9;
    size_t n = strcspn(e, "\"");
    const char *slot = strstr(json, "\"slot\":");
    size_t sl = slot != NULL ? strcspn(slot + 7, ",}") : 0;
    snprintf(key, cap, "%.*s/%.*s", (int)n, e, (int)sl, slot != NULL ? slot + 7 : "");
    return strlen(key);
}

static bool keyed(const char *json)
{
    /* State events replace an older one of the same kind still waiting; the rest are kept in order. */
    return strstr(json, "\"event\":\"station_state\"") != NULL || strstr(json, "\"event\":\"caster_state\"") != NULL ||
           strstr(json, "\"event\":\"update_state\"") != NULL;
}

void web_ws_event(const char *json)
{
    if (json == NULL || strlen(json) >= EVENT_TEXT_MAX) {
        return;
    }
    char key[48];
    char other[48];
    bool coalesce = keyed(json);
    if (coalesce) {
        event_key(json, key, sizeof(key));
    }
    portENTER_CRITICAL(&s_ev_lock);
    bool replaced = false;
    for (size_t i = 0; coalesce && i < s_ev_count; i++) {
        ws_event_t *e = &s_events[(s_ev_head + i) % EVENTS_MAX];
        event_key(e->text, other, sizeof(other));
        if (strcmp(key, other) == 0) {
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
        strlcpy(s_events[(s_ev_head + s_ev_count) % EVENTS_MAX].text, json, EVENT_TEXT_MAX);
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

EXT_RAM_BSS_ATTR static web_status_snap_t s_snap;
EXT_RAM_BSS_ATTR static char s_sec[2][WEB_SEC_COUNT][SEC_TEXT_MAX];
EXT_RAM_BSS_ATTR static char s_cst[2][WEB_MAX_SLOTS][CST_TEXT_MAX];
EXT_RAM_BSS_ATTR static char s_scratch[MSG_SCRATCH];
static uint32_t s_sec_crc[2][WEB_SEC_COUNT];
static uint32_t s_cst_crc[2][WEB_MAX_SLOTS];
static bool s_sec_changed[2][WEB_SEC_COUNT];
static bool s_cst_changed[2][WEB_MAX_SLOTS];
static bool s_have_prev[2];
static uint32_t s_seq[2];

/* Build one form's sections and note what changed since the previous second. */
static void build_form(int form)
{
    bool pub = form == 1;
    for (int sec = 0; sec < WEB_SEC_COUNT; sec++) {
        wj_t w;
        wj_init_members(&w, s_sec[form][sec], SEC_TEXT_MAX);
        web_build_section(&w, &s_snap, (web_section_t)sec, pub);
        if (!wj_ok(&w)) {
            s_sec[form][sec][0] = '\0';
        }
        uint32_t crc = web_crc32(0, s_sec[form][sec], strlen(s_sec[form][sec]));
        s_sec_changed[form][sec] = !s_have_prev[form] || crc != s_sec_crc[form][sec];
        s_sec_crc[form][sec] = crc;
    }
    for (size_t slot = 0; slot < s_snap.slots && slot < WEB_MAX_SLOTS; slot++) {
        wj_t w;
        wj_init_elements(&w, s_cst[form][slot], CST_TEXT_MAX);
        web_build_caster_summary(&w, &s_snap, slot, pub);
        if (!wj_ok(&w)) {
            s_cst[form][slot][0] = '\0';
        }
        uint32_t crc = web_crc32(0, s_cst[form][slot], strlen(s_cst[form][slot]));
        s_cst_changed[form][slot] = !s_have_prev[form] || crc != s_cst_crc[form][slot];
        s_cst_crc[form][slot] = crc;
    }
    s_have_prev[form] = true;
}

static bool append(size_t *len, const char *text, size_t n)
{
    if (*len + n + 1 > sizeof(s_scratch)) {
        return false;
    }
    memcpy(s_scratch + *len, text, n);
    *len += n;
    s_scratch[*len] = '\0';
    return true;
}

/* Compose one status message (full or delta) of a form into s_scratch; 0 if it does not fit. */
static size_t compose(int form, bool full)
{
    int head = snprintf(s_scratch, sizeof(s_scratch),
                        "{\"type\":\"status\",\"seq\":%u,\"full\":%s,\"uptime_s\":%u,\"data\":{",
                        (unsigned)++s_seq[form], full ? "true" : "false", (unsigned)s_snap.uptime_s);
    if (head <= 0 || (size_t)head >= sizeof(s_scratch)) {
        return 0;
    }
    size_t len = (size_t)head;
    bool ok = true;
    bool first = true;
    for (int sec = 0; ok && sec < WEB_SEC_COUNT; sec++) {
        size_t n = strlen(s_sec[form][sec]);
        if (n > 0 && (full || s_sec_changed[form][sec])) {
            ok = (first || append(&len, ",", 1)) && append(&len, s_sec[form][sec], n);
            first = false;
        }
    }
    bool any_caster = false;
    for (size_t slot = 0; slot < s_snap.slots && slot < WEB_MAX_SLOTS; slot++) {
        any_caster |= full || s_cst_changed[form][slot];
    }
    if (ok && any_caster) {
        static const char key[] = "\"casters\":[";
        ok = (first || append(&len, ",", 1)) && append(&len, key, sizeof(key) - 1);
        bool first_slot = true;
        for (size_t slot = 0; ok && slot < s_snap.slots && slot < WEB_MAX_SLOTS; slot++) {
            size_t n = strlen(s_cst[form][slot]);
            if (n > 0 && (full || s_cst_changed[form][slot])) {
                ok = (first_slot || append(&len, ",", 1)) && append(&len, s_cst[form][slot], n);
                first_slot = false;
            }
        }
        ok = ok && append(&len, "]", 1);
    }
    ok = ok && append(&len, "}}", 2);
    return ok ? len : 0;
}

static void push_status(void)
{
    bool want[2] = { false, false };
    bool pub = web_public_status();   /* off: no public form for anyone (such clients are being closed) */
    for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
        ws_client_t *c = &s_clients[i];
        if (c->used && !c->closing && (c->topics & WEB_TOPIC_STATUS) != 0 && (c->admin || pub)) {
            want[c->admin ? 0 : 1] = true;
        }
    }
    if (!want[0] && !want[1]) {
        s_have_prev[0] = s_have_prev[1] = false;
        return;
    }
    web_snap_status(&s_snap);
    for (int form = 0; form < 2; form++) {
        if (!want[form]) {
            s_have_prev[form] = false;
            continue;
        }
        build_form(form);
        ws_msg_t *delta = NULL;
        ws_msg_t *full = NULL;
        for (size_t i = 0; i < WS_MAX_CLIENTS; i++) {
            ws_client_t *c = &s_clients[i];
            if (!c->used || c->closing || (c->topics & WEB_TOPIC_STATUS) == 0 || (c->admin ? 0 : 1) != form) {
                continue;
            }
            ws_msg_t **slot = c->need_full ? &full : &delta;
            if (*slot == NULL) {
                size_t len = compose(form, c->need_full);
                if (len == 0 || (*slot = msg_new(WEB_MSG_STATUS, s_scratch, len)) == NULL) {
                    continue;
                }
            }
            c->need_full = false;
            enqueue(c, *slot);   /* a refused status sets need_full again */
        }
        msg_release(delta);
        msg_release(full);
    }
}

/* ---- progress ------------------------------------------------------------------------------------------- */

void web_ws_progress(const web_progress_t *p)
{
    wj_t w;
    wj_init(&w, s_scratch, sizeof(s_scratch), NULL, NULL);
    web_build_progress(&w, p);
    if (wj_ok(&w)) {
        broadcast(WEB_MSG_PROGRESS, s_scratch, w.len, true, WEB_TOPIC_PROGRESS);
    }
}

static portMUX_TYPE s_flash_lock = portMUX_INITIALIZER_UNLOCKED;
static um98x_flash_progress_t s_flash;
static bool s_flash_new;
static um98x_flash_phase_t s_flash_last_phase = UM98X_FLASH_IDLE;

void web_ws_note_flash_progress(const um98x_flash_progress_t *p)
{
    portENTER_CRITICAL(&s_flash_lock);
    s_flash = *p;
    s_flash_new = true;
    portEXIT_CRITICAL(&s_flash_lock);
}

static void push_flash_progress(void)
{
    um98x_flash_progress_t p;
    portENTER_CRITICAL(&s_flash_lock);
    bool fresh = s_flash_new;
    s_flash_new = false;
    p = s_flash;
    portEXIT_CRITICAL(&s_flash_lock);
    if (!fresh) {
        return;
    }
    bool done = p.phase == UM98X_FLASH_DONE || p.phase == UM98X_FLASH_FAILED || p.phase == UM98X_FLASH_NEEDS_USER;
    if (done && p.phase == s_flash_last_phase) {
        return;   /* the final message went out already */
    }
    s_flash_last_phase = p.phase;
    web_progress_t m = {
        .job = "gnss_flash",
        .phase = web_flash_phase_name(p.phase),
        .pct = p.bytes_total > 0 ? (int)((uint64_t)p.bytes_sent * 100u / p.bytes_total) : -1,
        .bytes = -1,
        .done = done,
        .result = done ? (p.phase == UM98X_FLASH_DONE ? "ok" : "failed") : NULL,
        .err = done && p.phase != UM98X_FLASH_DONE ? (esp_err_t)p.last_err : ESP_OK,
        .flash = &p,
    };
    if (m.done && m.err == ESP_OK && p.phase != UM98X_FLASH_DONE) {
        m.err = ESP_FAIL;
    }
    web_ws_progress(&m);
}

static uint32_t s_upload_seq;

static void push_upload_progress(void)
{
    web_upload_progress_t u;
    web_upload_progress(&u);
    if (u.job == NULL || u.seq == s_upload_seq) {
        return;
    }
    s_upload_seq = u.seq;
    web_progress_t m = {
        .job = u.job,
        .phase = u.phase,
        .pct = u.total > 0 ? (int)((uint64_t)u.bytes * 100u / u.total) : -1,
        .bytes = u.bytes,
        .total = u.total,
        .done = u.done,
        .result = u.result,
        .err = u.err,
    };
    web_ws_progress(&m);
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
        push_upload_progress();
        push_flash_progress();
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
    return ESP_OK;
}
