/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Local NTRIP caster: the task, the sockets, the hub consumers, the stored
 * config, the journal records and the status. The decisions, the parser and
 * every reply byte live in the pure core (lcast_core.c).
 *
 *   - One task ("lcast"), created the first time the caster is turned on and
 *     never deleted. Its stack, TCB and context (connection slots with their
 *     request and send buffers, the throttle table, scratch) are in PSRAM,
 *     allocated once. It never touches NVS or flash: config writes run in
 *     local_caster_set_config() on the caller's task, journal records are
 *     only queued with diag.
 *   - The listener ([::]:CONFIG_LOCAL_CASTER_PORT, dual stack) and at most
 *     LOCAL_CASTER_MAX_CONNS connections, all non-blocking, one select() per
 *     pass (20 ms while a rover streams, else 100 ms). A connection that does
 *     not pass lcast_addr_verdict() is closed right after accept(), before a
 *     byte is read, without a reply. One that sends nothing within
 *     LCAST_FIRST_BYTE_MS is closed; with every slot taken, a new connection
 *     takes the slot of one that cannot be a rover's (lcast_slot_pick), so
 *     idle connections from the LAN cannot keep a rover out.
 *   - A streaming rover owns one hub consumer, read with timeout 0 (no hub
 *     waiter, so one task may own several), and the next read happens only
 *     once the previous one is written. A rover that takes no byte for
 *     LOCAL_CASTER_SEND_STALL_MS is closed; the hub's lag limit drops whole
 *     old epochs for a slow one. Nothing else waits for a rover.
 *   - Planned restart: the shutdown hook ends every session (the NTRIP 2.0
 *     terminator when nothing is pending, then FIN) within 300 ms.
 *
 * Never logged: the Authorization header, the credentials a rover sent, the
 * request line, its User-Agent, or anything a rover sends after its request.
 * Refusal lines and a rover's connect and end lines are rate-limited.
 */
#include "local_caster.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "diag.h"
#include "esp_app_desc.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gnss_obs.h"
#include "lcast_core.h"
#include "lifecycle.h"
#include "lwip/sockets.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "settings.h"
#include "settings_schema.h"

static const char *TAG = "lcast";

/* The listener and LOCAL_CASTER_MAX_CONNS connections come on top of the
 * web server, the caster slots, captive DNS, the probe and the update
 * client (sdkconfig.defaults, "Networking"). */
_Static_assert(CONFIG_LWIP_MAX_SOCKETS >= 28 && CONFIG_LWIP_MAX_ACTIVE_TCP >= 28,
               "the local caster needs CONFIG_LWIP_MAX_SOCKETS and CONFIG_LWIP_MAX_ACTIVE_TCP of at least 28 "
               "(sdkconfig.defaults); an existing sdkconfig keeps its old value: set both to 28 in it or start a "
               "new build directory");
_Static_assert(sizeof(CONFIG_LWIP_LOCAL_HOSTNAME) <= LOCAL_CASTER_MOUNT_LEN,
               "the build's hostname must fit the default mountpoint");
_Static_assert(LCAST_CHUNK_DATA >= RTCM_HUB_MIN_READ_BUF, "a hub read must fit one chunk");
_Static_assert(LOCAL_CASTER_MAX_CONNS <= 16u, "the connection index goes into 4 bits of the journal records");

#define TX_CAP              (LCAST_CHUNK_HEAD + LCAST_CHUNK_DATA + LCAST_CHUNK_TAIL)
#define SELECT_STREAM_MS    20u
#define SELECT_IDLE_MS      100u
#define REPLY_MS            5000u     /* a reply that is not written in this long is dropped */
#define LCAST_LINGER_MS     2000u     /* after a reply: read and drop until EOF or this */
#define LISTEN_RETRY_MS     10000u
#define LOG_EVERY_MS        60000u    /* refusal lines: at most one a minute, with the count held back */
#define SHUTDOWN_MAX_MS     300u
#define LISTEN_BACKLOG      4
#define RX_DISCARD          1024u     /* bytes read from a rover per pass (dropped) */
#define DRAIN_MAX           8u        /* reads of RX_DISCARD before a planned close, so it ends with a FIN */
#define JQ_RETRY_MS         100u
#define STATS_MS            1000u
#define NETIF_LOOKUP_MS     1000u
#define MOUNT_RETRY_MS      60000u    /* the stored host name, after a read that failed */
#define KEEPALIVE_IDLE_S    10
#define KEEPALIVE_INTVL_S   5
#define KEEPALIVE_COUNT     3

typedef enum {
    CONN_FREE = 0,
    CONN_REQ,       /* reading the request */
    CONN_REPLY,     /* writing a source table or an error */
    CONN_LINGER,    /* reply written, our side shut: drain until EOF */
    CONN_STREAM,    /* a rover */
} conn_state_t;

typedef struct {
    int fd;
    conn_state_t state;
    uint32_t since_ms;          /* accept (REQ), reply start (REPLY), linger start (LINGER) */
    bool got_bytes;             /* REQ: a byte of the request arrived */
    uint64_t key;               /* lcast_client_key() of the peer */
    lcast_addr_t peer;          /* unmapped */
    char addr[LOCAL_CASTER_ADDR_LEN];
    char agent[LOCAL_CASTER_AGENT_LEN];
    lcast_req_t req;
    uint8_t tx[TX_CAP];         /* a reply, or one chunk (head reserve, payload, tail) */
    size_t tx_off;              /* next byte to send */
    size_t tx_end;              /* end of the pending bytes */
    size_t tx_payload;          /* RTCM bytes in them (counted once all are sent) */
    /* STREAM: */
    rtcm_hub_consumer_t hub;
    bool hdr_done;              /* the success reply is out */
    bool chunked;
    uint8_t version;
    bool listed;                /* journalled (DIAG_EV_ROVER_UP) */
    uint32_t order;             /* start order: the status lists the oldest first */
    uint32_t progress_ms;       /* the last byte the socket took, or when bytes became pending */
    int64_t start_us;           /* the success reply */
    uint32_t sta_ip_at_start;
    uint64_t bytes_sent;
    uint64_t epochs_dropped;
} conn_t;

typedef enum { LOGK_REFUSED = 0, LOGK_LOGIN, LOGK_BAD, LOGK_UP, LOGK_DOWN, LOGK_COUNT } logk_t;

#define LOG_LINE_MAX 112u

/* Log lines of one kind: at most a few a minute (refusals one, a rover's
 * connects and ends four each), the next ones held back and named (the first
 * of them, and how many more) when the minute is over. A rover client stuck
 * in a reconnect loop cannot flood the log ring. */
typedef struct {
    bool open;                  /* a minute is running, from at_ms */
    uint32_t at_ms;
    uint32_t used;              /* lines written in it */
    uint32_t held;              /* lines held back in it */
    char first[LOG_LINE_MAX];   /* the first held back */
} rate_log_t;

typedef struct {
    conn_t c[LOCAL_CASTER_MAX_CONNS];
    int lfd;
    local_caster_state_t state;
    int listen_errno;
    int listen_errno_logged;
    uint32_t cfg_gen;
    local_caster_cfg_t cfg;     /* applied, with the stored password */
    local_caster_cfg_t cfg_next;
    lcast_cred_t cred;
    lcast_throttles_t thr;
    lcast_jgate_t jgate;
    lcast_jq_t jq;
    uint32_t jq_try_ms;
    uint32_t stats_ms;
    uint32_t order;
    /* network, read every pass */
    esp_netif_t *sta;
    esp_netif_t *ap;
    uint32_t netif_lookup_ms;
    uint32_t sta_ip;
    uint32_t sta_gw;
    uint32_t ap_ip;
    uint32_t ap_mask;           /* the AP's netmask while it is up, else 0 */
    bool ap_ll_ok;
    uint8_t ap_ll[16];
    /* counters since boot */
    uint32_t sessions;
    uint32_t refused_not_local;
    uint32_t refused_login;
    uint32_t refused_throttled;
    uint32_t refused_full;
    uint32_t refused_bad_request;
    uint32_t stalled;
    uint64_t bytes_done;        /* RTCM bytes of the sessions that ended */
    rate_log_t logs[LOGK_COUNT];
    bool hub_err_logged;
    /* scratch (kept off the stack) */
    uint8_t rx[RX_DISCARD];
    gnss_obs_msg_stat_t obs[GNSS_OBS_MAX_MSG_TYPES];
    lcast_obs_msg_t obs_in[GNSS_OBS_MAX_MSG_TYPES];
    lcast_str_in_t str_in;
    gnss_obs_summary_t summary;
    gnss_obs_1033_t d1033;
    char str[LCAST_STR_MAX];
    local_caster_status_t st;
    int64_t st_start_us[LOCAL_CASTER_MAX_CLIENTS];
} ctx_t;

/* The config and its generation: set_config writes, the task copies. */
static SemaphoreHandle_t s_cfg_mutex;
static SemaphoreHandle_t s_set_mutex;   /* one set_config at a time (held over the settings write) */
static local_caster_cfg_t s_cfg;        /* under s_cfg_mutex; password = the stored one */
static bool s_saved;                    /* under s_cfg_mutex: a record is stored */
static uint32_t s_cfg_gen;              /* written under s_cfg_mutex, read atomically */
/* The default mountpoint (nothing stored): the stored host name, read again
 * only after the hostname setting changed (SETTINGS_EVENT_CHANGED), so the
 * status a page polls every second never reads NVS. */
static char s_default_mount[LOCAL_CASTER_MOUNT_LEN];   /* under s_cfg_mutex */
static bool s_mount_stale;              /* atomic: sys/hostname changed since it was read, or the read failed */
static bool s_mount_failed;             /* atomic: the last read failed (NVS not readable): */
static uint32_t s_mount_failed_ms;      /* atomic: then, so the next waits MOUNT_RETRY_MS */

static SemaphoreHandle_t s_wake;        /* config change or shutdown: the task looks now */
static SemaphoreHandle_t s_closed;      /* the task closed everything for the restart */
static bool s_shutdown;                 /* atomic */
static bool s_ready;                    /* local_caster_init() ran (never in safe mode) */

static char s_identifier[LOCAL_CASTER_MOUNT_LEN];   /* the host name in use this boot (STR record) */
static char s_server_ver[32];           /* "Server: NTRIP onodiy/<ver>" */

/* Status snapshot, published by the task every pass. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;             /* under s_lock */
static local_caster_status_t s_status = {
    .state = LOCAL_CASTER_OFF,
    .port = CONFIG_LOCAL_CASTER_PORT,
    .max_clients = LOCAL_CASTER_MAX_CLIENTS,
};
static int64_t s_start_us[LOCAL_CASTER_MAX_CLIENTS];   /* under s_lock: clients' success replies */

/* The hub's input rate for the STR record, sampled from init on (an
 * esp_timer, every LCAST_RATE_STEP_MS), so the source table has it whenever
 * the station has seen data, also right after the caster is turned on. */
static portMUX_TYPE s_rate_lock = portMUX_INITIALIZER_UNLOCKED;
static lcast_rate_t s_rate;             /* under s_rate_lock */
static esp_timer_handle_t s_rate_timer;

/* ---- small helpers ------------------------------------------------------------------------- */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void rate_tick(void *arg)
{
    (void)arg;
    rtcm_hub_stats_t hs;
    rtcm_hub_get_stats(&hs);
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_rate_lock);
    lcast_rate_sample(&s_rate, now, hs.bytes_in);
    portEXIT_CRITICAL(&s_rate_lock);
}

/* Bits per second the hub took in over the last minute (up to now). The ring
 * is read in place: a copy would put its 728 bytes on the caller's stack. */
static uint32_t bitrate_now(void)
{
    rtcm_hub_stats_t hs;
    rtcm_hub_get_stats(&hs);
    uint32_t now = now_ms();
    portENTER_CRITICAL(&s_rate_lock);
    uint32_t bps = lcast_rate_bps(&s_rate, now, hs.bytes_in);
    portEXIT_CRITICAL(&s_rate_lock);
    return bps;
}

static int64_t unix_now(void)
{
    time_t t = time(NULL);
    return t >= (time_t)LCAST_CLOCK_VALID_UNIX ? (int64_t)t : 0;
}

static bool would_block(void)
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

/* A socket address as lcast_addr_t (IPv4, or IPv6 with v4-mapped addresses). */
static bool sock_addr(const struct sockaddr_storage *ss, lcast_addr_t *out)
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

static void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

static void set_sock_opts(int fd)
{
    int on = 1;
    int idle = KEEPALIVE_IDLE_S;
    int intvl = KEEPALIVE_INTVL_S;
    int cnt = KEEPALIVE_COUNT;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

/* The station's host name (settings sys/hostname, else the build's). A
 * stored name the mountpoint rule refuses is passed over, as the network
 * component passes over an invalid one. Not from the caster task (NVS). */
static bool host_name(char *out, size_t size)
{
    char name[LOCAL_CASTER_MOUNT_LEN] = "";
    esp_err_t err = settings_get_str(SETTINGS_NS_SYS, SETTINGS_KEY_HOSTNAME, name, sizeof(name));
    if (err == ESP_OK && lcast_name_ok(name, sizeof(name))) {
        strlcpy(out, name, size);
        return true;
    }
    strlcpy(out, CONFIG_LWIP_LOCAL_HOSTNAME, size);
    /* None stored, or one that cannot be a mountpoint: the build's for good. Else (NVS not
     * readable now) the build's for the moment, and false: read it again later. */
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_NVS_INVALID_LENGTH;
}

static void defaults(local_caster_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    strlcpy(c->mountpoint, s_default_mount, sizeof(c->mountpoint));
    strlcpy(c->username, LOCAL_CASTER_DEFAULT_USER, sizeof(c->username));
}

/* The hostname setting changed (the default event loop): the default mountpoint is read again when next needed. */
static void on_settings_changed(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    const settings_changed_t *ev = data;
    if (id == SETTINGS_EVENT_CHANGED && ev != NULL && strcmp(ev->ns, SETTINGS_NS_SYS) == 0 &&
        (ev->key[0] == '\0' || strcmp(ev->key, SETTINGS_KEY_HOSTNAME) == 0)) {
        __atomic_store_n(&s_mount_failed, false, __ATOMIC_RELEASE);   /* a change is read at once */
        __atomic_store_n(&s_mount_stale, true, __ATOMIC_RELEASE);
    }
}

/* ---- log lines -------------------------------------------------------------------------------- */

static bool log_is_session(logk_t k)
{
    return k == LOGK_UP || k == LOGK_DOWN;
}

/* A refusal line always says how many were held back; a rover's line only when some were. */
static void log_line(logk_t k, const char *text, uint32_t more)
{
    if (log_is_session(k)) {
        if (more == 0) {
            ESP_LOGI(TAG, "%s", text);
        } else {
            ESP_LOGI(TAG, "%s (%u more held back)", text, (unsigned)more);
        }
    } else {
        ESP_LOGW(TAG, "%s (%u more held back)", text, (unsigned)more);
    }
}

/* The minute is over (or `force`: the task stops): name what was held back (that line opens the next minute). */
static void log_flush(ctx_t *x, logk_t k, uint32_t now, bool force)
{
    rate_log_t *l = &x->logs[k];
    if (!l->open || (!force && (uint32_t)(now - l->at_ms) < LOG_EVERY_MS)) {
        return;
    }
    l->open = false;
    if (l->held > 0) {
        log_line(k, l->first, l->held - 1u);
        l->open = true;
        l->at_ms = now;
        l->used = 1;
        l->held = 0;
    }
}

static void log_event(ctx_t *x, logk_t k, const char *text, uint32_t now)
{
    rate_log_t *l = &x->logs[k];
    log_flush(x, k, now, false);
    if (!l->open) {
        l->open = true;
        l->at_ms = now;
        l->used = 0;
        l->held = 0;
    }
    if (l->used < (log_is_session(k) ? 4u : 1u)) {
        l->used++;
        log_line(k, text, 0);
        return;
    }
    if (l->held++ == 0) {
        strlcpy(l->first, text, sizeof(l->first));
    }
}

/* log_event() with a printf-style line. */
static void log_eventf(ctx_t *x, logk_t k, uint32_t now, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void log_eventf(ctx_t *x, logk_t k, uint32_t now, const char *fmt, ...)
{
    char text[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    log_event(x, k, text, now);
}

/* ---- journal --------------------------------------------------------------------------------- */

static void jq_add(ctx_t *x, diag_event_t kind, int32_t a, int32_t b)
{
    if (lcast_jq_push(&x->jq, (uint16_t)kind, a, b, now_s())) {
        ESP_LOGW(TAG, "journal queue full: the oldest record was dropped");
    }
}

/* Try the queue's head (all of it with `all`, as long as diag takes them). */
static void journal_step(ctx_t *x, uint32_t now, bool all)
{
    if (!all && (uint32_t)(now - x->jq_try_ms) < JQ_RETRY_MS) {
        return;
    }
    x->jq_try_ms = now;
    for (;;) {
        lcast_jq_entry_t e;
        uint32_t expired = 0;
        bool have = lcast_jq_head(&x->jq, now_s(), &e, &expired);
        if (expired > 0) {
            ESP_LOGW(TAG, "%u journal record(s) dropped: not accepted within %u s", (unsigned)expired,
                     (unsigned)LCAST_JQ_MAX_S);
        }
        if (!have) {
            return;
        }
        esp_err_t err = diag_journal_append((diag_event_t)e.kind, e.a, e.b);
        if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NO_MEM) {
            return;   /* a rate limit, or diag's queue is full: later */
        }
        lcast_jq_pop(&x->jq);   /* written, or no journal: nothing to keep */
        if (!all) {
            return;
        }
    }
}

/* ---- connections ------------------------------------------------------------------------------ */

static uint32_t conn_index(const ctx_t *x, const conn_t *c)
{
    return (uint32_t)(c - x->c);
}

static void conn_close(conn_t *c)
{
    if (c->fd >= 0) {
        close(c->fd);
    }
    lcast_req_wipe(&c->req);
    c->fd = -1;
    c->state = CONN_FREE;
    c->tx_off = 0;
    c->tx_end = 0;
    c->tx_payload = 0;
}

static uint32_t streaming_count(const ctx_t *x)
{
    uint32_t n = 0;
    for (size_t i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
        n += x->c[i].state == CONN_STREAM;
    }
    return n;
}

/* Send what is pending. @return bytes taken (0 when the socket is full), -1 on an error. */
static int send_some(conn_t *c)
{
    if (c->tx_off >= c->tx_end) {
        return 0;
    }
    int n = send(c->fd, c->tx + c->tx_off, c->tx_end - c->tx_off, MSG_DONTWAIT);
    if (n > 0) {
        c->tx_off += (size_t)n;
        return n;
    }
    if (n < 0 && !would_block()) {
        return -1;
    }
    return 0;
}

/* Read and drop what the peer sent. @return false at EOF or on an error. */
static bool drain(ctx_t *x, conn_t *c, unsigned reads)
{
    for (unsigned i = 0; i < reads; i++) {
        int n = recv(c->fd, x->rx, sizeof(x->rx), MSG_DONTWAIT);
        if (n == 0 || (n < 0 && !would_block())) {
            return false;
        }
        if (n < 0) {
            break;
        }
    }
    return true;
}

/* select() says the socket takes data now (zero timeout). */
static bool writable_now(int fd)
{
    fd_set wr;
    FD_ZERO(&wr);
    FD_SET(fd, &wr);
    struct timeval tv = { 0, 0 };
    return select(fd + 1, NULL, &wr, NULL, &tv) > 0 && FD_ISSET(fd, &wr);
}

static bool net_changed(const ctx_t *x, const conn_t *c)
{
    return c->sta_ip_at_start != 0 && x->sta_ip != c->sta_ip_at_start;
}

/* End a rover's session: hub, the terminator when allowed, close, journal, log. */
static void stream_end(ctx_t *x, conn_t *c, local_caster_end_t reason)
{
    if (c->hub != NULL) {
        rtcm_hub_detach(c->hub);   /* no waiter was ever registered: returns at once */
        c->hub = NULL;
    }
    bool planned = reason == LOCAL_CASTER_END_SETTINGS || reason == LOCAL_CASTER_END_RESTART;
    if (planned) {
        /* Only between chunks, and only if the socket takes all five bytes:
         * lwIP reports a socket writable only with more than its low-water
         * mark free (thousands of bytes), so a writable one takes them whole.
         * A short write would leave half a terminator: then none goes out. */
        if (c->chunked && c->hdr_done && c->tx_off >= c->tx_end && writable_now(c->fd)) {
            int n = send(c->fd, LCAST_CHUNK_END, sizeof(LCAST_CHUNK_END) - 1u, MSG_DONTWAIT);
            if (n != (int)(sizeof(LCAST_CHUNK_END) - 1u)) {
                ESP_LOGD(TAG, "rover %s: terminator not sent whole (%d)", c->addr, n);
            }
        }
        /* Unread bytes would turn the close into a reset. */
        (void)drain(x, c, DRAIN_MAX);
    }
    uint32_t age_s = (uint32_t)((esp_timer_get_time() - c->start_us) / 1000000);
    uint32_t idx = conn_index(x, c);
    if (c->listed) {
        int32_t a;
        int32_t b;
        lcast_jrec_down(idx, reason, age_s, &a, &b);
        jq_add(x, DIAG_EV_ROVER_DOWN, a, b);
    } else {
        lcast_jgate_unlisted_end(&x->jgate);
    }
    if (planned) {
        /* At most 2 at once, and the task may stop right after (a restart): never held back. */
        ESP_LOGI(TAG, "rover %s disconnected after %u s (%s), %llu bytes", c->addr, (unsigned)age_s,
                 lcast_end_words(reason), (unsigned long long)c->bytes_sent);
    } else {
        log_eventf(x, LOGK_DOWN, now_ms(), "rover %s disconnected after %u s (%s), %llu bytes", c->addr,
                   (unsigned)age_s, lcast_end_words(reason), (unsigned long long)c->bytes_sent);
    }
    x->bytes_done += c->bytes_sent;
    c->bytes_sent = 0;
    c->listed = false;
    conn_close(c);
}

/* End every connection: sessions with `reason`, the others closed. */
static void end_all(ctx_t *x, local_caster_end_t reason, bool sessions_only)
{
    for (size_t i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
        conn_t *c = &x->c[i];
        if (c->state == CONN_STREAM) {
            stream_end(x, c, reason);
        } else if (c->state != CONN_FREE && !sessions_only) {
            conn_close(c);
        }
    }
}

/* The login was right: attach a hub consumer and queue the success reply. */
static bool stream_start(ctx_t *x, conn_t *c, lcast_flavour_t f, uint32_t now)
{
    char name[8];
    snprintf(name, sizeof(name), "lcast%u", (unsigned)conn_index(x, c));
    rtcm_hub_consumer_t h = NULL;
    esp_err_t err = rtcm_hub_attach(name, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rover %s refused: no hub consumer free (%s)", c->addr, esp_err_to_name(err));
        return false;
    }
    (void)rtcm_hub_set_max_lag(h, CONFIG_LOCAL_CASTER_MAX_LAG_MS);
    size_t len = lcast_reply_stream((char *)c->tx, sizeof(c->tx), f, s_server_ver, unix_now());
    c->hub = h;
    c->version = f == LCAST_FLAV_V1 ? 1u : 2u;
    c->chunked = f == LCAST_FLAV_V2_CHUNKED;
    c->hdr_done = false;
    c->tx_off = 0;
    c->tx_end = len;
    c->tx_payload = 0;
    c->state = CONN_STREAM;
    c->progress_ms = now;
    c->start_us = esp_timer_get_time();
    c->order = ++x->order;
    c->bytes_sent = 0;
    c->epochs_dropped = 0;
    c->sta_ip_at_start = x->sta_ip;
    c->listed = false;
    x->sessions++;
    /* Its User-Agent is in the status, not in the log (it is what the rover sent). */
    log_eventf(x, LOGK_UP, now, "rover %s connected (NTRIP %u.0)", c->addr, (unsigned)c->version);
    return true;
}

/* The source table's STR record from what the station observes now. */
static void build_str(ctx_t *x)
{
    size_t n = GNSS_OBS_MAX_MSG_TYPES;
    if (gnss_obs_get_msg_stats(x->obs, &n) != ESP_OK) {
        n = 0;
    }
    for (size_t i = 0; i < n; i++) {
        x->obs_in[i] = (lcast_obs_msg_t) {
            .msg = x->obs[i].msg,
            .expected_period_s = x->obs[i].expected_period_s,
            .count = x->obs[i].count,
            .period_ms = x->obs[i].period_ms,
            .age_ms = x->obs[i].age_ms,
        };
    }
    lcast_str_in_t *in = &x->str_in;
    memset(in, 0, sizeof(*in));
    in->n = lcast_str_msgs_from_obs(x->obs_in, n, in->msgs, LCAST_STR_MSGS_MAX);
    uint8_t max_bands = 0;
    if (gnss_obs_get_summary(&x->summary) == ESP_OK) {
        for (size_t s = 0; s < GNSS_SYS_COUNT; s++) {
            if (x->summary.sys[s].present && x->summary.sys[s].bands > max_bands) {
                max_bands = x->summary.sys[s].bands;
            }
        }
    }
    in->carrier = lcast_str_carrier(max_bands, in->msgs, in->n);
    in->generator = "";
    if (gnss_obs_get_1033(&x->d1033) == ESP_OK && x->d1033.valid) {
        in->generator = x->d1033.receiver_type;
    }
    in->mount = x->cfg.mountpoint;
    in->identifier = s_identifier;
    in->bitrate = bitrate_now();
    if (lcast_str_build(in, x->str, sizeof(x->str)) == 0) {
        x->str[0] = '\0';
    }
}

/* The request is complete, bad, too long or late: decide and answer. */
static void answer(ctx_t *x, conn_t *c, lcast_req_result_t res, uint32_t now)
{
    uint32_t t = now_s();
    uint64_t key = lcast_client_key(&c->peer);
    uint32_t wait = res == LCAST_REQ_DONE ? lcast_throttles_wait_s(&x->thr, key, t) : 0;
    lcast_decision_t d = lcast_decide(&c->req, res, x->cfg.mountpoint, &x->cred, wait, streaming_count(x),
                                      LOCAL_CASTER_MAX_CLIENTS);
    c->agent[0] = '\0';
    if (res == LCAST_REQ_DONE) {
        strlcpy(c->agent, c->req.agent, sizeof(c->agent));
    }
    lcast_req_wipe(&c->req);   /* the raw request and the decoded credentials go now */
    if (d.count_fail) {
        (void)lcast_throttles_fail(&x->thr, key, t);
    }
    if (d.login_ok) {
        lcast_throttles_ok(&x->thr, key);
    }
    if (d.action == LCAST_ACT_STREAM) {
        if (stream_start(x, c, d.flavour, now)) {
            return;
        }
        d.action = LCAST_ACT_E503;   /* no hub consumer */
    }
    size_t len;
    int64_t unix_s = unix_now();
    if (d.action == LCAST_ACT_SOURCETABLE) {
        build_str(x);
        len = lcast_reply_sourcetable((char *)c->tx, sizeof(c->tx), d.flavour, x->str, s_server_ver, unix_s);
    } else {
        switch (d.action) {
        case LCAST_ACT_E401:
            x->refused_login++;
            if (d.count_fail) {
                log_eventf(x, LOGK_LOGIN, now, "wrong login from %s", c->addr);
            }
            break;
        case LCAST_ACT_E429:
            x->refused_throttled++;
            break;
        case LCAST_ACT_E503:
            x->refused_full++;
            break;
        default:
            x->refused_bad_request++;
            log_eventf(x, LOGK_BAD, now, "bad request from %s", c->addr);
            break;
        }
        len = lcast_reply_error((char *)c->tx, sizeof(c->tx), d.action, d.flavour, x->cfg.mountpoint, wait,
                                s_server_ver, unix_s);
    }
    if (len == 0) {
        conn_close(c);
        return;
    }
    c->tx_off = 0;
    c->tx_end = len;
    c->tx_payload = 0;
    c->state = CONN_REPLY;
    c->since_ms = now;
}

static void step_req(ctx_t *x, conn_t *c, uint32_t now)
{
    size_t room;
    uint8_t *dst = lcast_req_room(&c->req, &room);
    lcast_req_result_t res = c->req.result;
    if (room > 0) {
        int n = recv(c->fd, dst, room, MSG_DONTWAIT);
        if (n > 0) {
            c->got_bytes = true;
            res = lcast_req_added(&c->req, (size_t)n);
        } else if (n == 0 || !would_block()) {
            conn_close(c);   /* gone before its request was complete */
            return;
        }
    }
    if (!c->got_bytes && (uint32_t)(now - c->since_ms) >= LCAST_FIRST_BYTE_MS) {
        /* Nothing sent: no request to answer. Its slot goes back at once. */
        x->refused_bad_request++;
        log_eventf(x, LOGK_BAD, now, "silent connection from %s", c->addr);
        conn_close(c);
        return;
    }
    if (res == LCAST_REQ_NEED_MORE && (uint32_t)(now - c->since_ms) < LOCAL_CASTER_REQUEST_TIMEOUT_MS) {
        return;
    }
    answer(x, c, res, now);
}

static void step_reply(conn_t *c, uint32_t now)
{
    if (send_some(c) < 0) {
        conn_close(c);
        return;
    }
    if (c->tx_off >= c->tx_end) {
        shutdown(c->fd, SHUT_WR);
        c->state = CONN_LINGER;
        c->since_ms = now;
        return;
    }
    if ((uint32_t)(now - c->since_ms) >= REPLY_MS) {
        conn_close(c);
    }
}

static void step_linger(ctx_t *x, conn_t *c, uint32_t now)
{
    if (!drain(x, c, 1) || (uint32_t)(now - c->since_ms) >= LCAST_LINGER_MS) {
        conn_close(c);
    }
}

/* Send what is pending; the end of a block counts its payload. @return false when the session ended. */
static bool stream_pump(ctx_t *x, conn_t *c, uint32_t now)
{
    if (c->tx_off >= c->tx_end) {
        return true;
    }
    int n = send_some(c);
    if (n < 0) {
        stream_end(x, c, net_changed(x, c) ? LOCAL_CASTER_END_NETWORK : LOCAL_CASTER_END_LOST);
        return false;
    }
    if (n > 0) {
        c->progress_ms = now;
    }
    if (c->tx_off >= c->tx_end) {
        c->bytes_sent += c->tx_payload;
        c->tx_payload = 0;
        c->hdr_done = true;
        return true;
    }
    if ((uint32_t)(now - c->progress_ms) >= LOCAL_CASTER_SEND_STALL_MS) {
        x->stalled++;
        stream_end(x, c, LOCAL_CASTER_END_STALLED);
        return false;
    }
    return true;
}

static void step_stream(ctx_t *x, conn_t *c, uint32_t now)
{
    /* Whatever a rover sends (a GGA sentence is its position) is read and dropped, unlogged. */
    int n = recv(c->fd, x->rx, sizeof(x->rx), MSG_DONTWAIT);
    if (n == 0) {
        stream_end(x, c, LOCAL_CASTER_END_CLOSED);
        return;
    }
    if (n < 0 && !would_block()) {
        stream_end(x, c, net_changed(x, c) ? LOCAL_CASTER_END_NETWORK : LOCAL_CASTER_END_LOST);
        return;
    }
    if (net_changed(x, c)) {
        stream_end(x, c, LOCAL_CASTER_END_NETWORK);   /* lwIP aborts it anyway; this names it */
        return;
    }
    if (!stream_pump(x, c, now) || c->tx_off < c->tx_end) {
        return;
    }
    /* Nothing pending: the next whole frames. */
    size_t len = 0;
    esp_err_t err = rtcm_hub_read(c->hub, c->tx + LCAST_CHUNK_HEAD, LCAST_CHUNK_DATA, &len, 0);
    if (err == ESP_OK && len > 0) {
        size_t start = LCAST_CHUNK_HEAD;
        size_t total = len;
        if (c->chunked) {
            total = lcast_chunk_wrap(c->tx, len, &start);
        }
        c->tx_off = start;
        c->tx_end = start + total;
        c->tx_payload = len;
        c->progress_ms = now;
        (void)stream_pump(x, c, now);
    } else if (err != ESP_OK && err != ESP_ERR_TIMEOUT && err != ESP_ERR_INVALID_STATE && !x->hub_err_logged) {
        /* ESP_ERR_INVALID_STATE: held for receiver work; the hub starts this rover over after it. */
        x->hub_err_logged = true;
        ESP_LOGW(TAG, "hub read: %s", esp_err_to_name(err));
    }
}

/* A session that has streamed LCAST_JGATE_AFTER_S is journalled when the gate has a token. */
static void list_sessions(ctx_t *x)
{
    int64_t t_us = esp_timer_get_time();
    for (size_t i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
        conn_t *c = &x->c[i];
        if (c->state != CONN_STREAM || c->listed || !c->hdr_done) {
            continue;
        }
        uint32_t age_s = (uint32_t)((t_us - c->start_us) / 1000000);
        uint32_t unlisted = 0;
        if (lcast_jgate_try_list(&x->jgate, age_s, now_s(), &unlisted)) {
            int32_t a;
            int32_t b;
            lcast_jrec_up((uint32_t)i, c->version, &c->peer, age_s, unlisted, &a, &b);
            jq_add(x, DIAG_EV_ROVER_UP, a, b);
            c->listed = true;
        }
    }
}

/* ---- listener and network ------------------------------------------------------------------- */

static void close_listener(ctx_t *x)
{
    if (x->lfd >= 0) {
        close(x->lfd);
        x->lfd = -1;
    }
}

static bool open_listener(ctx_t *x)
{
#if CONFIG_LWIP_IPV6
    int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
#else
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
#endif
    int e = 0;
    if (fd < 0) {
        e = errno;
    } else {
        int on = 1;
        (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
#if CONFIG_LWIP_IPV6
        int off = 0;
        (void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off));
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons(CONFIG_LOCAL_CASTER_PORT);
#else
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons(CONFIG_LOCAL_CASTER_PORT);
        sa.sin_addr.s_addr = htonl(INADDR_ANY);
#endif
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 || listen(fd, LISTEN_BACKLOG) != 0) {
            e = errno;
            close(fd);
            fd = -1;
        }
    }
    if (fd < 0) {
        x->state = LOCAL_CASTER_ERROR;
        x->listen_errno = e != 0 ? e : EIO;
        if (x->listen_errno != x->listen_errno_logged) {
            ESP_LOGW(TAG, "cannot listen on port %u (errno %d), retrying in %u s", (unsigned)CONFIG_LOCAL_CASTER_PORT,
                     x->listen_errno, (unsigned)(LISTEN_RETRY_MS / 1000u));
            x->listen_errno_logged = x->listen_errno;
        }
        return false;
    }
    set_nonblocking(fd);
    x->lfd = fd;
    x->listen_errno = 0;
    x->listen_errno_logged = 0;
    ESP_LOGI(TAG, "listening on port %u, mountpoint %s", (unsigned)CONFIG_LOCAL_CASTER_PORT, x->cfg.mountpoint);
    return true;
}

/* The station's IPv4 address and gateway, the AP's addresses (lock-free reads of the netifs). */
static void read_netifs(ctx_t *x, uint32_t now)
{
    if ((x->sta == NULL || x->ap == NULL) && (uint32_t)(now - x->netif_lookup_ms) >= NETIF_LOOKUP_MS) {
        x->netif_lookup_ms = now;
        if (x->sta == NULL) {
            x->sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");   /* created once, never destroyed */
        }
        if (x->ap == NULL) {
            x->ap = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");     /* none in the QEMU build */
        }
    }
    esp_netif_ip_info_t ip;
    x->sta_ip = 0;
    x->sta_gw = 0;
    if (x->sta != NULL && esp_netif_is_netif_up(x->sta) && esp_netif_get_ip_info(x->sta, &ip) == ESP_OK) {
        x->sta_ip = ip.ip.addr;
        x->sta_gw = ip.gw.addr;
    }
#if CONFIG_NETWORK_QEMU_ETHERNET
    /* QEMU's user-mode network delivers every host connection from its gateway. */
    x->sta_gw = 0;
#endif
    x->ap_ip = 0;
    x->ap_mask = 0;
    x->ap_ll_ok = false;
    if (x->ap != NULL && esp_netif_get_ip_info(x->ap, &ip) == ESP_OK) {
        x->ap_ip = ip.ip.addr;
        if (esp_netif_is_netif_up(x->ap)) {
            x->ap_mask = ip.netmask.addr;   /* its subnet is the hotspot's only while it is up */
        }
    }
#if CONFIG_LWIP_IPV6
    esp_ip6_addr_t ll;
    if (x->ap != NULL && esp_netif_get_ip6_linklocal(x->ap, &ll) == ESP_OK) {
        memcpy(x->ap_ll, ll.addr, sizeof(x->ap_ll));
        x->ap_ll_ok = true;
    }
#endif
}

static void accept_all(ctx_t *x, uint32_t now)
{
    for (unsigned i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
        struct sockaddr_storage ss;
        socklen_t sl = sizeof(ss);
        int fd = accept(x->lfd, (struct sockaddr *)&ss, &sl);
        if (fd < 0) {
            return;   /* none waiting (or no socket left: lwIP drops that connection) */
        }
        /* Who it is, before a byte is read. */
        lcast_addr_t peer;
        lcast_addr_t local;
        struct sockaddr_storage ls;
        socklen_t ll = sizeof(ls);
        bool ok = sock_addr(&ss, &peer) && getsockname(fd, (struct sockaddr *)&ls, &ll) == 0 && sock_addr(&ls, &local);
        lcast_addr_verdict_t v = ok ? lcast_addr_verdict(&peer, &local, x->sta_gw, x->ap_ip, x->ap_mask,
                                                         x->ap_ll_ok ? x->ap_ll : NULL)
                                    : LCAST_ADDR_NOT_PRIVATE;
        if (v != LCAST_ADDR_OK) {
            close(fd);
            x->refused_not_local++;
            char text[LOCAL_CASTER_ADDR_LEN];
            (void)lcast_addr_text(ok ? &peer : NULL, text, sizeof(text));
            log_eventf(x, LOGK_REFUSED, now, "refused %s: not on the local network", text);
            continue;
        }
        /* A slot: a free one, else one a connection that cannot be a rover's gives up (lcast_slot_pick). */
        lcast_addr_unmap(&peer);
        uint64_t key = lcast_client_key(&peer);
        lcast_slot_view_t views[LOCAL_CASTER_MAX_CONNS];
        for (size_t k = 0; k < LOCAL_CASTER_MAX_CONNS; k++) {
            const conn_t *s = &x->c[k];
            views[k] = (lcast_slot_view_t) {
                .kind = s->state == CONN_FREE     ? LCAST_SLOT_FREE
                        : s->state == CONN_REQ    ? LCAST_SLOT_REQ
                        : s->state == CONN_LINGER ? LCAST_SLOT_LINGER
                                                  : LCAST_SLOT_BUSY,
                .key = s->key,
                .since_ms = s->since_ms,
                .got_bytes = s->got_bytes,
            };
        }
        lcast_slot_pick_t pick = lcast_slot_pick(views, LOCAL_CASTER_MAX_CONNS, key, now);
        if (pick.slot < 0) {
            close(fd);
            x->refused_full++;
            continue;
        }
        conn_t *c = &x->c[pick.slot];
        if (pick.evict) {
            conn_close(c);   /* a request not sent, a client's second one, or an answer already out */
        }
        set_nonblocking(fd);
        set_sock_opts(fd);
        lcast_req_reset(&c->req);
        c->fd = fd;
        c->key = key;
        c->got_bytes = false;
        c->peer = peer;
        (void)lcast_addr_text(&peer, c->addr, sizeof(c->addr));
        c->agent[0] = '\0';
        c->state = CONN_REQ;
        c->since_ms = now;
        c->tx_off = 0;
        c->tx_end = 0;
        c->tx_payload = 0;
    }
}

/* Sleep until something can be read or written, at most one select timeout. */
static void wait_io(ctx_t *x)
{
    fd_set rd;
    fd_set wr;
    FD_ZERO(&rd);
    FD_ZERO(&wr);
    int maxfd = x->lfd;
    FD_SET(x->lfd, &rd);
    bool streaming = false;
    for (size_t i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
        conn_t *c = &x->c[i];
        if (c->state == CONN_FREE) {
            continue;
        }
        if (c->fd > maxfd) {
            maxfd = c->fd;
        }
        /* Linger and stream reads are polled each pass: a peer that keeps
         * sending is read at most RX_DISCARD bytes a pass, never in a loop. */
        if (c->state == CONN_REQ) {
            FD_SET(c->fd, &rd);
        }
        if (c->state == CONN_STREAM) {
            streaming = true;
        }
        if ((c->state == CONN_REPLY || c->state == CONN_STREAM) && c->tx_off < c->tx_end) {
            FD_SET(c->fd, &wr);
        }
    }
    struct timeval tv = { .tv_sec = 0, .tv_usec = (streaming ? SELECT_STREAM_MS : SELECT_IDLE_MS) * 1000 };
    if (select(maxfd + 1, &rd, &wr, NULL, &tv) < 0) {
        vTaskDelay(pdMS_TO_TICKS(SELECT_STREAM_MS));   /* never a busy loop */
    }
}

/* ---- status -------------------------------------------------------------------------------- */

static void publish(ctx_t *x, uint32_t now)
{
    bool stats = (uint32_t)(now - x->stats_ms) >= STATS_MS;
    if (stats) {
        x->stats_ms = now;
    }
    local_caster_status_t *st = &x->st;
    memset(st, 0, sizeof(*st));
    st->state = x->state;
    st->port = CONFIG_LOCAL_CASTER_PORT;
    st->max_clients = LOCAL_CASTER_MAX_CLIENTS;
    uint64_t live = 0;
    /* Streaming rovers, oldest first. */
    uint32_t after = 0;
    for (;;) {
        conn_t *next = NULL;
        for (size_t i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
            conn_t *c = &x->c[i];
            if (c->state == CONN_STREAM && c->order > after && (next == NULL || c->order < next->order)) {
                next = c;
            }
        }
        if (next == NULL) {
            break;
        }
        after = next->order;
        if (stats && next->hub != NULL) {
            rtcm_hub_consumer_stats_t cs;
            if (rtcm_hub_get_consumer_stats(next->hub, &cs) == ESP_OK) {
                next->epochs_dropped = cs.epochs_dropped;
            }
        }
        live += next->bytes_sent;
        if (st->client_count >= LOCAL_CASTER_MAX_CLIENTS) {
            continue;
        }
        local_caster_client_t *o = &st->clients[st->client_count];
        strlcpy(o->addr, next->addr, sizeof(o->addr));
        o->ntrip_version = next->version;
        o->chunked = next->chunked;
        o->bytes_sent = next->bytes_sent;
        o->epochs_dropped = next->epochs_dropped;
        strlcpy(o->agent, next->agent, sizeof(o->agent));
        x->st_start_us[st->client_count] = next->start_us;
        st->client_count++;
    }
    st->sessions = x->sessions;
    st->bytes_sent = x->bytes_done + live;
    st->refused_not_local = x->refused_not_local;
    st->refused_login = x->refused_login;
    st->refused_throttled = x->refused_throttled;
    st->refused_full = x->refused_full;
    st->refused_bad_request = x->refused_bad_request;
    st->stalled = x->stalled;
    st->listen_errno = x->state == LOCAL_CASTER_ERROR ? x->listen_errno : 0;
    portENTER_CRITICAL(&s_lock);
    s_status = *st;
    memcpy(s_start_us, x->st_start_us, sizeof(s_start_us));
    portEXIT_CRITICAL(&s_lock);
}

/* ---- config apply -------------------------------------------------------------------------- */

/* A new config: sessions that logged in under other values end, and an off turns everything off. */
static void apply_config(ctx_t *x)
{
    if (__atomic_load_n(&s_cfg_gen, __ATOMIC_ACQUIRE) == x->cfg_gen) {
        return;   /* every pass: no lock unless something changed */
    }
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    uint32_t gen = s_cfg_gen;
    x->cfg_next = s_cfg;
    xSemaphoreGive(s_cfg_mutex);
    local_caster_cfg_t *n = &x->cfg_next;
    bool was_on = x->cfg.enabled;
    bool mount_changed = strcmp(n->mountpoint, x->cfg.mountpoint) != 0;
    bool login_changed = mount_changed || strcmp(n->username, x->cfg.username) != 0 ||
                         strcmp(n->password, x->cfg.password) != 0;
    if (!n->enabled) {
        end_all(x, LOCAL_CASTER_END_SETTINGS, false);
        close_listener(x);
        if (was_on) {
            ESP_LOGI(TAG, "off");
        }
    } else if (login_changed) {
        end_all(x, LOCAL_CASTER_END_SETTINGS, true);
    }
    lcast_secure_zero(&x->cfg, sizeof(x->cfg));
    x->cfg = *n;
    lcast_secure_zero(n, sizeof(*n));
    if (!lcast_cred_set(&x->cred, x->cfg.username, x->cfg.password)) {
        lcast_secure_zero(&x->cred, sizeof(x->cred));   /* cannot happen with the rules: nobody logs in */
    }
    x->cfg_gen = gen;
    if (x->cfg.enabled && was_on && mount_changed && x->lfd >= 0) {
        ESP_LOGI(TAG, "listening on port %u, mountpoint %s", (unsigned)CONFIG_LOCAL_CASTER_PORT, x->cfg.mountpoint);
    }
}

/* ---- the task ------------------------------------------------------------------------------- */

static void caster_task(void *arg)
{
    ctx_t *x = arg;
    for (;;) {
        if (__atomic_load_n(&s_shutdown, __ATOMIC_ACQUIRE)) {
            end_all(x, LOCAL_CASTER_END_RESTART, false);
            close_listener(x);
            journal_step(x, now_ms(), true);
            for (int k = 0; k < LOGK_COUNT; k++) {
                log_flush(x, (logk_t)k, now_ms(), true);   /* what was held back, before the restart */
            }
            x->state = LOCAL_CASTER_OFF;
            publish(x, now_ms());
            xSemaphoreGive(s_closed);
            for (;;) {
                vTaskDelay(portMAX_DELAY);
            }
        }
        apply_config(x);
        uint32_t now = now_ms();
        if (!x->cfg.enabled) {
            x->state = LOCAL_CASTER_OFF;
            x->listen_errno = 0;
            journal_step(x, now, true);
            publish(x, now);
            (void)xSemaphoreTake(s_wake, portMAX_DELAY);
            continue;
        }
        if (x->lfd < 0 && !open_listener(x)) {
            publish(x, now);
            (void)xSemaphoreTake(s_wake, pdMS_TO_TICKS(LISTEN_RETRY_MS));
            continue;
        }
        wait_io(x);

        now = now_ms();
        read_netifs(x, now);
        x->state = x->sta_ip != 0 ? LOCAL_CASTER_LISTENING : LOCAL_CASTER_WAIT_NETWORK;
        /* The connections first, new ones after: a request already waiting
         * in its socket is read before the slot policy looks at it. */
        for (size_t i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
            conn_t *c = &x->c[i];
            switch (c->state) {
            case CONN_REQ:
                step_req(x, c, now);
                break;
            case CONN_REPLY:
                step_reply(c, now);
                break;
            case CONN_LINGER:
                step_linger(x, c, now);
                break;
            case CONN_STREAM:
                step_stream(x, c, now);
                break;
            case CONN_FREE:
            default:
                break;
            }
        }
        accept_all(x, now);
        list_sessions(x);
        journal_step(x, now, false);
        for (int k = 0; k < LOGK_COUNT; k++) {
            log_flush(x, (logk_t)k, now, false);
        }
        publish(x, now);
    }
}

static void note_no_task(bool on)
{
    portENTER_CRITICAL(&s_lock);
    s_status.state = on ? LOCAL_CASTER_ERROR : LOCAL_CASTER_OFF;
    s_status.listen_errno = on ? ENOMEM : 0;
    portEXIT_CRITICAL(&s_lock);
}

static bool task_exists(void)
{
    portENTER_CRITICAL(&s_lock);
    bool t = s_task != NULL;
    portEXIT_CRITICAL(&s_lock);
    return t;
}

/* Create the task the first time (under s_set_mutex). */
static esp_err_t ensure_task(void)
{
    if (task_exists()) {
        return ESP_OK;
    }
    ctx_t *x = heap_caps_calloc(1, sizeof(ctx_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (x == NULL) {
        ESP_LOGE(TAG, "no memory for the context (%u bytes of PSRAM)", (unsigned)sizeof(ctx_t));
        note_no_task(true);
        return ESP_ERR_NO_MEM;
    }
    x->lfd = -1;
    for (size_t i = 0; i < LOCAL_CASTER_MAX_CONNS; i++) {
        x->c[i].fd = -1;
    }
    uint32_t t = now_s();
    lcast_throttles_init(&x->thr, t);
    lcast_jgate_init(&x->jgate, t);
    x->netif_lookup_ms = now_ms() - NETIF_LOOKUP_MS;
    /* Stack and TCB in PSRAM: the task does no flash or NVS work. Never deleted. */
    TaskHandle_t h = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(caster_task, "lcast", CONFIG_LOCAL_CASTER_TASK_STACK, x,
                                        CONFIG_LOCAL_CASTER_TASK_PRIO, &h, tskNO_AFFINITY,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        heap_caps_free(x);
        ESP_LOGE(TAG, "cannot create the task");
        note_no_task(true);
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_lock);
    s_task = h;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

/* ---- shutdown -------------------------------------------------------------------------------- */

/* Planned restart (registered after the caster slots', so they keep their
 * epoch drain): every session ends (terminator, FIN), the listener closes. */
static void on_shutdown(lifecycle_reboot_reason_t reason, uint32_t deadline_ms, void *ctx)
{
    (void)reason;
    (void)ctx;
    if (!task_exists()) {
        return;
    }
    __atomic_store_n(&s_shutdown, true, __ATOMIC_RELEASE);
    xSemaphoreGive(s_wake);
    uint32_t wait = deadline_ms < SHUTDOWN_MAX_MS ? deadline_ms : SHUTDOWN_MAX_MS;
    (void)xSemaphoreTake(s_closed, pdMS_TO_TICKS(wait));
}

/* ---- API ------------------------------------------------------------------------------------- */

esp_err_t local_caster_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    s_cfg_mutex = xSemaphoreCreateMutex();
    s_set_mutex = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    s_closed = xSemaphoreCreateBinary();
    if (s_cfg_mutex == NULL || s_set_mutex == NULL || s_wake == NULL || s_closed == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (!host_name(s_default_mount, sizeof(s_default_mount))) {
        /* NVS not readable now: the build's name until a later read works. */
        __atomic_store_n(&s_mount_failed_ms, now_ms(), __ATOMIC_RELEASE);
        __atomic_store_n(&s_mount_failed, true, __ATOMIC_RELEASE);
        __atomic_store_n(&s_mount_stale, true, __ATOMIC_RELEASE);
    }
    strlcpy(s_identifier, s_default_mount, sizeof(s_identifier));
    (void)lcast_sanitize(esp_app_get_description()->version, s_server_ver, sizeof(s_server_ver), false);

    /* Room for a longer record of a later version (only appended fields). */
    uint8_t blob[2u * LCAST_CFG_BLOB_MAX];
    size_t len = sizeof(blob);
    local_caster_cfg_t cfg;
    esp_err_t err = settings_get_blob(SETTINGS_NS_LCAST, SETTINGS_KEY_LCAST_CFG, blob, &len);
    bool loaded = err == ESP_OK && lcast_cfg_decode(blob, len, &cfg);
    if (!loaded) {
        defaults(&cfg);
        if (err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH) {
            ESP_LOGW(TAG, "stored config does not decode: off, with the defaults");
        }
    }
    lcast_secure_zero(blob, sizeof(blob));
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg = cfg;
    s_saved = loaded;
    __atomic_store_n(&s_cfg_gen, 1u, __ATOMIC_RELEASE);
    xSemaphoreGive(s_cfg_mutex);
    bool on = cfg.enabled;
    lcast_secure_zero(&cfg, sizeof(cfg));

    esp_err_t herr = lifecycle_register_shutdown_hook(on_shutdown, NULL);
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown hook (%s): a planned restart cuts rover sessions without a clean close",
                 esp_err_to_name(herr));
    }
    herr = esp_event_handler_register(SETTINGS_EVENT, SETTINGS_EVENT_CHANGED, on_settings_changed, NULL);
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "no settings events (%s): the default mountpoint follows a rename only after a restart",
                 esp_err_to_name(herr));
    }
    /* The input rate from now on (a sample every second, the first at once). */
    rate_tick(NULL);
    const esp_timer_create_args_t targs = { .callback = rate_tick, .name = "lcast_rate" };
    herr = esp_timer_create(&targs, &s_rate_timer);
    if (herr == ESP_OK) {
        herr = esp_timer_start_periodic(s_rate_timer, (uint64_t)LCAST_RATE_STEP_MS * 1000u);
    }
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "no rate timer (%s): the source table says bit rate 0", esp_err_to_name(herr));
    }
    s_ready = true;
    if (!on) {
        ESP_LOGI(TAG, "off");
    }
    return ESP_OK;
}

esp_err_t local_caster_start(void)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_set_mutex, portMAX_DELAY);
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    bool on = s_cfg.enabled;
    xSemaphoreGive(s_cfg_mutex);
    esp_err_t err = on ? ensure_task() : ESP_OK;
    xSemaphoreGive(s_set_mutex);
    return err;
}

esp_err_t local_caster_get_config(local_caster_cfg_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    /* Nothing stored: the default mountpoint is the stored host name, read
     * from NVS only after the hostname setting changed (a rename since the
     * boot shows; a page polls this every second). A read that fails keeps
     * the name it has and is tried again, at most once a minute. */
    if (__atomic_exchange_n(&s_mount_stale, false, __ATOMIC_ACQ_REL)) {
        uint32_t now = now_ms();
        bool wait = __atomic_load_n(&s_mount_failed, __ATOMIC_ACQUIRE) &&
                    (uint32_t)(now - __atomic_load_n(&s_mount_failed_ms, __ATOMIC_ACQUIRE)) < MOUNT_RETRY_MS;
        char name[LOCAL_CASTER_MOUNT_LEN];
        if (wait || !host_name(name, sizeof(name))) {
            if (!wait) {
                __atomic_store_n(&s_mount_failed_ms, now, __ATOMIC_RELEASE);
                __atomic_store_n(&s_mount_failed, true, __ATOMIC_RELEASE);
            }
            __atomic_store_n(&s_mount_stale, true, __ATOMIC_RELEASE);
        } else {
            __atomic_store_n(&s_mount_failed, false, __ATOMIC_RELEASE);
            xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
            strlcpy(s_default_mount, name, sizeof(s_default_mount));
            xSemaphoreGive(s_cfg_mutex);
        }
    }
    local_caster_cfg_t cfg;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    cfg = s_cfg;
    if (!s_saved) {
        strlcpy(cfg.mountpoint, s_default_mount, sizeof(cfg.mountpoint));
    }
    xSemaphoreGive(s_cfg_mutex);
    cfg.password_set = cfg.password[0] != '\0';
    lcast_secure_zero(cfg.password, sizeof(cfg.password));
    *out = cfg;
    return ESP_OK;
}

esp_err_t local_caster_set_config(const local_caster_cfg_t *cfg, const char **field, const char **rule)
{
    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_set_mutex, portMAX_DELAY);
    local_caster_cfg_t next;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    next = s_cfg;   /* for the stored password */
    xSemaphoreGive(s_cfg_mutex);
    bool stored = next.password[0] != '\0';
    if (!lcast_cfg_check(cfg, stored, field, rule)) {
        lcast_secure_zero(&next, sizeof(next));
        xSemaphoreGive(s_set_mutex);
        return ESP_ERR_INVALID_ARG;
    }
    next.enabled = cfg->enabled;
    strlcpy(next.mountpoint, cfg->mountpoint, sizeof(next.mountpoint));
    strlcpy(next.username, cfg->username, sizeof(next.username));
    if (cfg->password[0] != '\0') {
        lcast_secure_zero(next.password, sizeof(next.password));
        strlcpy(next.password, cfg->password, sizeof(next.password));
    }
    next.password_set = next.password[0] != '\0';

    uint8_t blob[LCAST_CFG_BLOB_MAX];
    size_t len = lcast_cfg_encode(&next, blob, sizeof(blob));
    esp_err_t err = len > 0 ? settings_set_blob(SETTINGS_NS_LCAST, SETTINGS_KEY_LCAST_CFG, blob, len)
                            : ESP_ERR_INVALID_SIZE;
    lcast_secure_zero(blob, sizeof(blob));
    if (err != ESP_OK) {
        lcast_secure_zero(&next, sizeof(next));
        xSemaphoreGive(s_set_mutex);
        return err;   /* nothing changed */
    }
    bool on = next.enabled;
    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    s_cfg = next;
    s_saved = true;
    __atomic_store_n(&s_cfg_gen, s_cfg_gen + 1u, __ATOMIC_RELEASE);
    xSemaphoreGive(s_cfg_mutex);
    lcast_secure_zero(&next, sizeof(next));

    err = ESP_OK;
    if (on) {
        err = ensure_task();
    }
    if (task_exists()) {
        xSemaphoreGive(s_wake);
    } else {
        note_no_task(on);
    }
    xSemaphoreGive(s_set_mutex);
    return err;
}

void local_caster_get_status(local_caster_status_t *out)
{
    if (out == NULL) {
        return;
    }
    int64_t start[LOCAL_CASTER_MAX_CLIENTS];
    portENTER_CRITICAL(&s_lock);
    *out = s_status;
    memcpy(start, s_start_us, sizeof(start));
    portEXIT_CRITICAL(&s_lock);
    out->port = CONFIG_LOCAL_CASTER_PORT;
    out->max_clients = LOCAL_CASTER_MAX_CLIENTS;
    int64_t t = esp_timer_get_time();
    for (size_t i = 0; i < out->client_count && i < LOCAL_CASTER_MAX_CLIENTS; i++) {
        int64_t s = (t - start[i]) / 1000000;
        out->clients[i].connected_s = s < 0 ? 0u : (s > UINT32_MAX ? UINT32_MAX : (uint32_t)s);
    }
}
