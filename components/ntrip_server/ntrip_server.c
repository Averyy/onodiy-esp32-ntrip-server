/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Caster slots: one task per slot, created at init and never deleted. The
 * decisions (state, backoff, reply classification, request bytes) live in
 * the pure core (ntrip_proto.c); this file does the I/O around it:
 *
 *   - plain TCP over lwIP sockets (non-blocking connect bounded by
 *     CONFIG_NTRIP_TCP_CONNECT_TIMEOUT_MS), TLS through esp-tls (timeout_ms =
 *     CONFIG_NTRIP_CONNECT_TIMEOUT_MS; CA bundle with cross-signed
 *     verification, a slot's own CA PEM, or no check when the user chose
 *     it); at most one TLS handshake at a time, the protected slot first;
 *   - keepalive 10/5/3 s and TCP_NODELAY on every caster socket; after the
 *     connect the socket is non-blocking and every wait is a select() of at
 *     most 100 ms, so commands (config change, restart, pause, shutdown)
 *     are seen quickly;
 *   - write-stall watchdog (CONFIG_NTRIP_SEND_TIMEOUT_MS without a byte
 *     accepted), EOF polling while streaming, the request/reply deadlines;
 *   - teardown: the NTRIP 2.0 terminator (only between chunks, <= 1 s), TLS
 *     close_notify, then close(); never a half-close on a live session;
 *   - IP and Wi-Fi events, pause, per-slot stats and progress stamps,
 *     restart_slot, persistence of long backoffs in settings cstN/backoff;
 *   - the Onocoy login guard's side of the I/O: the reply body read for every
 *     error reply, the lines a caster sends during a session (kept, matched,
 *     a duplicate or banned report ending the session at once), the class
 *     an Onocoy reason names, the stop's log line and journal record, and
 *     the user's retry.
 *
 * Network state: with ESP-IDF's default station handlers a Wi-Fi disconnect
 * takes the interface down at once: the address is cleared and lwIP aborts
 * every TCP session bound to it (IP_EVENT_STA_LOST_IP only follows later,
 * when the lost-IP timer runs out). A move to another access point of the
 * network keeps the interface up with its address instead (the network
 * component), and the sessions with it. So the network counts as up only
 * from IP_EVENT_STA_GOT_IP, and as down from the moment the station's
 * interface goes down (IP_EVENT_NETIF_DOWN, never at a move that keeps it),
 * a station stop or LOST_IP on; sessions then close as "no network", never
 * as failures.
 * Association alone is not enough: until DHCP has an address a connect
 * fails with "no route" and would be counted. The flag is also checked
 * against the interface itself (address and up state), so a session that
 * the disconnect aborted is never counted even if that abort is seen before
 * this file's event handler ran.
 *
 * Nothing here logs a password, a request line, an Onocoy mountpoint or a
 * v2 username; reply text is shown sanitised with those values masked.
 */
#include "ntrip_server.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "diag.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ledger.h"
#include "lifecycle.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509.h"
#include "ntrip_cfg_blob.h"
#include "ntrip_proto.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "ntrip";

ESP_EVENT_DEFINE_BASE(NTRIP_EVENT);

/* The core mirrors the public enums value for value. */
_Static_assert((int)NTRIP_CLS_DNS == (int)NTRIP_FAIL_DNS && (int)NTRIP_CLS_CONNECT == (int)NTRIP_FAIL_CONNECT &&
                   (int)NTRIP_CLS_TLS == (int)NTRIP_FAIL_TLS && (int)NTRIP_CLS_TIMEOUT == (int)NTRIP_FAIL_TIMEOUT &&
                   (int)NTRIP_CLS_REMOTE_CLOSE == (int)NTRIP_FAIL_REMOTE_CLOSE &&
                   (int)NTRIP_CLS_WRITE_STALL == (int)NTRIP_FAIL_WRITE_STALL &&
                   (int)NTRIP_CLS_KEEPALIVE == (int)NTRIP_FAIL_KEEPALIVE && (int)NTRIP_CLS_AUTH == (int)NTRIP_FAIL_AUTH &&
                   (int)NTRIP_CLS_MOUNT_BUSY == (int)NTRIP_FAIL_MOUNT_BUSY &&
                   (int)NTRIP_CLS_CONFIG == (int)NTRIP_FAIL_CONFIG && (int)NTRIP_CLS_BANNED == (int)NTRIP_FAIL_BANNED &&
                   (int)NTRIP_CLS_LOGIN_BLOCKED == (int)NTRIP_FAIL_LOGIN_BLOCKED &&
                   (int)NTRIP_CLS_IN_USE == (int)NTRIP_FAIL_IN_USE &&
                   (int)NTRIP_CLS_REPLACED == (int)NTRIP_FAIL_REPLACED &&
                   (int)NTRIP_CLS_LOCAL == (int)NTRIP_FAIL_LOCAL &&
                   (int)NTRIP_CLS_COUNT == (int)NTRIP_FAIL_CLASS_COUNT,
               "core failure classes must match ntrip_fail_class_t");
_Static_assert((int)NTRIP_SM_DISABLED == (int)NTRIP_STATE_DISABLED &&
                   (int)NTRIP_SM_WAIT_NETWORK == (int)NTRIP_STATE_WAIT_NETWORK &&
                   (int)NTRIP_SM_WAIT_DATA == (int)NTRIP_STATE_WAIT_DATA &&
                   (int)NTRIP_SM_CONNECTING == (int)NTRIP_STATE_CONNECTING &&
                   (int)NTRIP_SM_STREAMING == (int)NTRIP_STATE_STREAMING &&
                   (int)NTRIP_SM_BACKOFF == (int)NTRIP_STATE_BACKOFF &&
                   (int)NTRIP_SM_AUTH_FAILED == (int)NTRIP_STATE_AUTH_FAILED &&
                   (int)NTRIP_SM_PAUSED == (int)NTRIP_STATE_PAUSED &&
                   (int)NTRIP_SM_CONFIG_ERROR == (int)NTRIP_STATE_CONFIG_ERROR &&
                   (int)NTRIP_SM_BANNED == (int)NTRIP_STATE_BANNED &&
                   (int)NTRIP_SM_STOPPED == (int)NTRIP_STATE_STOPPED && (int)NTRIP_STATE_STOPPED == 10,
               "core states must match ntrip_state_t");
_Static_assert((int)NTRIP_FAIL_LOGIN_BLOCKED == 12 && (int)NTRIP_FAIL_IN_USE == 13 && (int)NTRIP_FAIL_REPLACED == 14 &&
                   (int)NTRIP_FAIL_LOCAL == 15,
               "the API and the journal carry these values");
_Static_assert((int)NTRIP_PROF_FAST == (int)NTRIP_BACKOFF_FAST &&
                   (int)NTRIP_PROF_STANDARD == (int)NTRIP_BACKOFF_STANDARD &&
                   (int)NTRIP_PROF_POLITE == (int)NTRIP_BACKOFF_POLITE,
               "core profiles must match ntrip_backoff_profile_t");
_Static_assert(CONFIG_NTRIP_MAX_CASTERS <= 24, "one event-group bit per slot");

#define TICK_MS             100u    /* longest single wait while a session is open */
#define IDLE_WAIT_MS        200u    /* idle loop: fresh-data gate and backoff checks */
#define PUBLISH_MS          250u
#define ATTACH_RETRY_MS     5000u
#define TX_BUF_SIZE         (NTRIP_CHUNK_HEAD_RESERVE + NTRIP_CHUNK_MAX_PAYLOAD + NTRIP_CHUNK_TAIL)
#define HIST_QUEUE_LEN 4      /* connection history records waiting for the journal, per slot */
#define RX_SCRATCH          256u
#define CA_PEM_MAX          8192u
#define TLS_MIN_INTERNAL_HEAP (64u * 1024u)   /* below this, non-protected TLS slots wait */
#define LOW_HEAP_RETRY_MS   5000u
#define STOP_WAIT_MS        2000u
#define DRAIN_MS            1000u   /* a planned close waits this long at most for the end of the epoch being sent */
#define ONOCOY_LAG_MS       1000u
#define NET_SETTLE_MS       200u    /* a failure while the network looks up: wait this long for a drop to show */
#define NET_SETTLE_STEP_MS  20u
#define CLOCK_VALID_UNIX    1704067200   /* 2024-01-01: before this the clock is not set */

#define CMD_CONFIG  (1u << 0)
#define CMD_RESTART (1u << 1)
#define CMD_RETRY   (1u << 2)   /* the user's retry: clear a stop and the backoff */

/* Status hints only the server knows (after the core's). */
enum {
    HINT_NO_RTCM = NTRIP_HINT_COUNT,
    HINT_LOW_MEMORY,
    HINT_CA_PEM,
    HINT_NO_HUB,
    HINT_BAD_FIELDS,
    HINT_TOTAL,
};

static const char *const k_hint_text[HINT_TOTAL] = {
    [NTRIP_HINT_NONE] = "",
    [NTRIP_HINT_V1_ONLY] = "caster appears to be NTRIP 1.0 only",
    [NTRIP_HINT_SOURCETABLE] = "caster sent its sourcetable: wrong host or protocol?",
    [NTRIP_HINT_MOUNT_HELD] = "mountpoint held by another source (another device?)",
    [NTRIP_HINT_TLS_CERT] = "TLS certificate failed",
    [NTRIP_HINT_TLS_TRY_PLAIN] = "TLS keeps failing; plain port 2101 is an option",
    [NTRIP_HINT_CASTER_BUSY] = "caster busy or restarting",
    [NTRIP_HINT_BANNED] = "caster reports this source as banned",
    [NTRIP_HINT_AGENT] = "caster rejected the agent string",
    [NTRIP_HINT_UNEXPECTED] = "unexpected reply from the caster",
    [NTRIP_HINT_NO_REPLY] = "no reply from the caster",
    [HINT_NO_RTCM] = "waiting for RTCM from the receiver",
    [HINT_LOW_MEMORY] = "low memory: TLS connect deferred",
    [HINT_CA_PEM] = "custom CA certificate missing or too large",
    [HINT_NO_HUB] = "RTCM source not available",
    [HINT_BAD_FIELDS] = "stored settings invalid: edit and save the slot",
};

static const char *const k_class_name[NTRIP_FAIL_CLASS_COUNT] = {
    "none", "DNS", "connect", "TLS", "timeout", "remote close", "write stall",
    "keepalive", "auth", "mount busy", "config", "banned", "login blocked", "in use", "replaced", "local error",
};

/* Why the login guard stopped a slot (log line and status hint, at most 63 characters). */
static const char *const k_stop_text[NTRIP_STOP_COUNT] = {
    [NTRIP_STOP_NONE] = "",
    [NTRIP_STOP_REJECTED] = "stopped: Onocoy refused the login 3 times",
    [NTRIP_STOP_LOGIN_BLOCKED] = "stopped: Onocoy is blocking logins from this connection",
    [NTRIP_STOP_IN_USE] = "stopped: the credential is in use by another stream",
    [NTRIP_STOP_KICKED] = "stopped: sessions kept being taken over",
    [NTRIP_STOP_DUPLICATE] = "stopped: Onocoy reports a duplicate stream",
    [NTRIP_STOP_BANNED] = "stopped: Onocoy reports the credential as banned",
};

typedef enum {
    IO_OK = 0,
    IO_ABORT,      /* a command or network change ends the attempt: not a failure */
    IO_FAIL,       /* failure described by the ntrip_failure_t */
} io_result_t;

typedef struct {
    size_t idx;
    TaskHandle_t task;
    rtcm_hub_consumer_t consumer;
    bool attach_wait;
    uint32_t attach_at_ms;
    bool gate_held;              /* the TLS handshake slot is ours */

    /* fixed buffers (PSRAM when available) */
    uint8_t tx[TX_BUF_SIZE];
    char req[NTRIP_REQUEST_MAX];
    uint8_t rx[RX_SCRATCH];
    ntrip_reply_t reply;

    ntrip_caster_cfg_t cfg;      /* task's copy, with the password */
    bool guard;                  /* the slot counts as Onocoy: the login guard applies */

    /* session */
    int fd;
    esp_tls_t *tls;
    volatile bool open;          /* streaming session (after the caster's OK) */
    bool chunked;
    bool mid_chunk;
    ntrip_epoch_track_t epochs;  /* epochs sent: state across hub reads */
    bool draining;               /* a planned close waits for the end of the epoch being sent (drain_step) */
    bool drained;                /* ... that epoch is over */
    uint32_t drain_start_ms;
    uint32_t drain_until_ms;
    bool paused_seen;
    bool logged_unexpected;
    ntrip_sess_text_t stext;     /* lines the caster sends during the session */
    ntrip_stop_t stop_seen;      /* the stop already logged and journalled */
    uint32_t last_tx_ms;
    uint32_t net_gen;
    int last_errno;
    bool tls_err;                /* the last failed TLS read or write failed inside TLS, not on the socket */

    ntrip_sm_t sm;
    uint8_t prev_state;

    /* connection history: records waiting for the journal (its rate limits) */
    ntrip_hist_t hist;
    struct {
        uint16_t kind;
        int32_t a;
        int32_t b;
    } jq[HIST_QUEUE_LEN];
    uint8_t jq_n;

    /* DNS cache */
    char cache_host[64];
    struct sockaddr_storage cache_addr;
    socklen_t cache_len;

    /* stats (task-owned working copy, published to s_status) */
    ntrip_caster_status_t st;
    uint16_t hint;
    uint32_t tls_fail_streak;
    uint32_t low_heap_until_ms;
    bool low_heap_wait;
    int32_t day;
    ntrip_rate_clock_t rate;     /* this boot, today: online as the ledger counts it (the data rate's time base; the day's figures are the ledger's) */
    uint32_t lost_reported;      /* this boot, today: the protected slot's lost epochs handed to the ledger */
    uint64_t dropped_epochs_base;
    uint32_t last_pub_ms;
    uint32_t max_lag_seen;
    bool ledger_known;
    bool ledger_enabled;
    bool ledger_protected;
    bool ledger_streaming;
    /* Onocoy day figures: since when the slot counts today */
    int32_t exp_day;             /* the UTC day exp_from_sod belongs to (-1 = none) */
    uint32_t exp_from_sod;       /* second of that day the slot started to count (UINT32_MAX = not yet) */
    bool counted_known;          /* account() ran this boot */
    bool was_counted;            /* ... and the slot counted then */

    /* shared with API callers */
    volatile uint32_t cmd;
    volatile uint32_t progress_ms;
    volatile uint32_t state_since_ms;
} slot_t;

static ntrip_caster_cfg_t s_cfg[CONFIG_NTRIP_MAX_CASTERS];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_loaded;   /* slot configs read from settings (not in safe mode) */

static slot_t *s_slots[CONFIG_NTRIP_MAX_CASTERS];
static EXT_RAM_BSS_ATTR ntrip_caster_status_t s_status[CONFIG_NTRIP_MAX_CASTERS];
static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_cfg_mutex;
static SemaphoreHandle_t s_tls_sem;
static EventGroupHandle_t s_kick;
static char s_agent[NTRIP_AGENT_MAX];

static volatile bool s_running;
static volatile bool s_paused;
static volatile bool s_shutdown;
static volatile uint32_t s_shutdown_drain_until;   /* set before s_shutdown: sessions finish their epoch until then */
static volatile bool s_net_up;           /* station got an IP and has not dropped since */
static volatile uint32_t s_net_gen;      /* bumped when the address returns or changes: older sockets are dead */
static esp_netif_t *volatile s_sta_netif;
static volatile uint32_t s_tls_protected_waiting;

/* ---- small helpers ------------------------------------------------------------ */

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

/* The connection history counts in seconds of uptime (no wrap). */
static uint32_t uptime_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static int32_t utc_day(void)
{
    time_t t = time(NULL);
    return (t < CLOCK_VALID_UNIX) ? -1 : (int32_t)(t / 86400);
}

static int64_t unix_now(void)
{
    time_t t = time(NULL);
    return (t < CLOCK_VALID_UNIX) ? 0 : (int64_t)t;
}

static void kick(size_t slot)
{
    if (s_kick != NULL) {
        xEventGroupSetBits(s_kick, (EventBits_t)1u << slot);
    }
}

static void kick_all(void)
{
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        kick(i);
    }
}

static void wait_kick(slot_t *s, uint32_t ms)
{
    if (ms == 0) {
        return;
    }
    xEventGroupWaitBits(s_kick, (EventBits_t)1u << s->idx, pdTRUE, pdFALSE, pdMS_TO_TICKS(ms));
}

static uint32_t take_cmd(slot_t *s)
{
    return __atomic_exchange_n(&s->cmd, 0u, __ATOMIC_SEQ_CST);
}

static bool fits(const char *s, size_t size)
{
    return strnlen(s, size) < size;
}

static bool host_is_ip(const char *host)
{
    struct in_addr a4;
    if (inet_pton(AF_INET, host, &a4) == 1) {
        return true;
    }
#if CONFIG_LWIP_IPV6
    struct in6_addr a6;
    return inet_pton(AF_INET6, host, &a6) == 1;
#else
    return false;
#endif
}

/* The slot name is only a label, but it goes into logs: no control characters. */
static bool has_bad_name(const char *s)
{
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20u || c == 0x7Fu) {
            return true;
        }
    }
    return false;
}

/* ---- config persistence ---------------------------------------------------------- */

static void slot_defaults(size_t slot, ntrip_caster_cfg_t *out)
{
    if (slot == 0) {
        ntrip_server_onocoy_defaults(out);
    } else {
        memset(out, 0, sizeof(*out));
        out->port = 2101;
        out->proto = NTRIP_PROTO_V2;
        out->trust_mode = NTRIP_TRUST_BUNDLE;
        out->backoff_profile = NTRIP_BACKOFF_STANDARD;
    }
}

static void slot_ns(size_t slot, char *ns, size_t size)
{
    snprintf(ns, size, SETTINGS_NS_CASTER_FMT, (unsigned)slot);
}

/* A custom CA PEM is stored under its own key in the slot namespace. */
static bool ca_pem_present(size_t slot)
{
    char ns[16];
    slot_ns(slot, ns, sizeof(ns));
    uint8_t probe;
    size_t len = sizeof(probe);
    esp_err_t err = settings_get_blob(ns, SETTINGS_KEY_CST_CA_PEM, &probe, &len);
    return err == ESP_OK || err == ESP_ERR_NVS_INVALID_LENGTH;   /* present (longer than the probe) */
}

static esp_err_t load_slot(size_t slot, ntrip_caster_cfg_t *out)
{
    char ns[16];
    slot_ns(slot, ns, sizeof(ns));
    uint8_t buf[NTRIP_CFG_BLOB_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = settings_get_blob(ns, SETTINGS_KEY_CST_CFG, buf, &len);
    if (err == ESP_OK && !ntrip_cfg_decode(buf, len, out)) {
        ESP_LOGW(TAG, "slot %u: stored config unreadable, using defaults", (unsigned)slot);
        err = ESP_ERR_INVALID_VERSION;
    }
    memset(buf, 0, sizeof(buf));
    if (err != ESP_OK) {
        slot_defaults(slot, out);
    }
    return err;
}

static ntrip_fields_t fields_of(const ntrip_caster_cfg_t *c)
{
    ntrip_fields_t f = {
        .host = c->host,
        .port = c->port,
        .mountpoint = c->mountpoint,
        .username = c->username,
        .password = c->password,
        .label = c->label,
        .v2 = c->proto == NTRIP_PROTO_V2,
        .enabled = c->enabled,
        .onocoy = ntrip_slot_is_onocoy(c),
    };
    return f;
}

/* ---- network events --------------------------------------------------------------- */

static esp_netif_t *sta_netif(void)
{
    esp_netif_t *sta = s_sta_netif;
    if (sta == NULL) {
        sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");   /* created once, never destroyed */
        s_sta_netif = sta;
    }
    return sta;
}

/* The station interface is up with an address (lock-free reads of the netif). */
static bool sta_has_address(void)
{
    esp_netif_t *sta = sta_netif();
    esp_netif_ip_info_t ip;
    return sta != NULL && esp_netif_is_netif_up(sta) && esp_netif_get_ip_info(sta, &ip) == ESP_OK &&
           ip.ip.addr != 0;
}

static void on_net_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT) {
        if (id == WIFI_EVENT_STA_STOP) {
            s_net_up = false;   /* the interface goes with Wi-Fi: the sessions close as "no network" */
        }
    } else if (base == IP_EVENT) {
        if (id == IP_EVENT_NETIF_DOWN) {
            /* The address is gone and lwIP aborted the sessions bound to
             * it (a disconnect that was not a move keeping the address):
             * they close as "no network". */
            const ip_event_netif_status_t *e = (const ip_event_netif_status_t *)data;
            if (e != NULL && e->esp_netif == sta_netif()) {
                s_net_up = false;
            }
        } else if (id == IP_EVENT_STA_GOT_IP) {
            const ip_event_got_ip_t *e = (const ip_event_got_ip_t *)data;
            if (!s_net_up || (e != NULL && e->ip_changed)) {
                s_net_gen++;   /* a socket from before this address is dead (the same lease reports no change) */
            }
            s_net_up = true;
        } else if (id == IP_EVENT_STA_LOST_IP) {
            s_net_up = false;
        }
    }
    kick_all();
}

static void refresh_network_state(void)
{
    if (sta_has_address()) {
        s_net_up = true;
    }
}

/* New connections and open sessions need the station's address. */
static bool net_ready(void)
{
    return s_net_up && sta_has_address();
}

/*
 * A failure while the network looks up: a drop that aborted the socket may
 * not be visible yet (the abort happens inside the interface going down).
 * Wait a moment for it before the failure is counted.
 */
static bool net_lost_settled(void)
{
    for (uint32_t waited = 0; net_ready() && waited < NET_SETTLE_MS; waited += NET_SETTLE_STEP_MS) {
        vTaskDelay(pdMS_TO_TICKS(NET_SETTLE_STEP_MS));
    }
    return !net_ready();
}

/* ---- session I/O ------------------------------------------------------------------------ */

static void set_sock_opts(int fd)
{
    int on = 1;
    int idle = NTRIP_KEEPALIVE_IDLE_S;
    int intvl = NTRIP_KEEPALIVE_INTVL_S;
    int cnt = NTRIP_KEEPALIVE_COUNT;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
}

static void set_nonblocking(int fd)
{
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) {
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
    }
}

/* Error of the socket after a failed call (errno may be stale after mbedTLS). */
static int sock_error(slot_t *s)
{
    int err = 0;
    socklen_t len = sizeof(err);
    if (s->fd >= 0 && getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err != 0) {
        return err;
    }
    return s->last_errno;
}

/* What a failed TLS read or write stands for: a reset from the peer, the
 * socket's own error, or (0, tls_err) a failure inside TLS on this station
 * (memory, the crypto driver, a record it could not process). PROVISIONAL:
 * the codes a board returns under low internal memory are to be confirmed
 * on hardware. */
static void note_tls_error(slot_t *s, ssize_t r)
{
    if (r == MBEDTLS_ERR_NET_CONN_RESET) {
        s->last_errno = ECONNRESET;
        s->tls_err = false;
    } else if (r == MBEDTLS_ERR_NET_RECV_FAILED || r == MBEDTLS_ERR_NET_SEND_FAILED) {
        s->last_errno = errno;   /* sock_error prefers the socket's SO_ERROR */
        s->tls_err = false;
    } else {
        s->last_errno = 0;
        s->tls_err = true;
    }
}

/* The class of the last failed send or receive: only the caster's own end
 * (REMOTE_CLOSE) may score as a take-over on a guard slot. */
static ntrip_cls_t io_error_class(slot_t *s)
{
    if (s->tls != NULL && s->tls_err) {
        return NTRIP_CLS_TLS;
    }
    return ntrip_errno_class(sock_error(s));
}

/* > 0 bytes sent, 0 would block, < 0 error. */
static int sess_send(slot_t *s, const uint8_t *p, size_t n)
{
    if (s->tls != NULL) {
        ssize_t r = esp_tls_conn_write(s->tls, p, n);
        if (r > 0) {
            return (int)r;
        }
        if (r == 0 || r == ESP_TLS_ERR_SSL_WANT_WRITE || r == ESP_TLS_ERR_SSL_WANT_READ) {
            return 0;
        }
        note_tls_error(s, r);
        return -1;
    }
    ssize_t r = send(s->fd, p, n, MSG_DONTWAIT);
    if (r > 0) {
        return (int)r;
    }
    if (r == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    s->last_errno = errno;
    s->tls_err = false;
    return -1;
}

/* > 0 bytes read, 0 nothing yet, -1 closed by the caster, -2 error. */
static int sess_recv(slot_t *s, uint8_t *buf, size_t n)
{
    if (s->tls != NULL) {
        ssize_t r = esp_tls_conn_read(s->tls, buf, n);
        if (r > 0) {
            return (int)r;
        }
        if (r == 0 || r == MBEDTLS_ERR_SSL_CONN_EOF) {
            return -1;
        }
        if (r == ESP_TLS_ERR_SSL_WANT_READ || r == ESP_TLS_ERR_SSL_WANT_WRITE) {
            return 0;
        }
        note_tls_error(s, r);
        return -2;
    }
    ssize_t r = recv(s->fd, buf, n, MSG_DONTWAIT);
    if (r > 0) {
        return (int)r;
    }
    if (r == 0) {
        return -1;
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return 0;
    }
    s->last_errno = errno;
    s->tls_err = false;
    return -2;
}

/* Wait until the socket can be written (or read) or ms pass. */
static void sess_wait(slot_t *s, bool for_write, uint32_t ms)
{
    if (!for_write && s->tls != NULL && esp_tls_get_bytes_avail(s->tls) > 0) {
        return;
    }
    if (s->fd < 0) {
        vTaskDelay(pdMS_TO_TICKS(ms));
        return;
    }
    fd_set set;
    FD_ZERO(&set);
    FD_SET(s->fd, &set);
    struct timeval tv = { .tv_sec = (time_t)(ms / 1000u), .tv_usec = (suseconds_t)((ms % 1000u) * 1000u) };
    (void)select(s->fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv);
}

/* A planned close is pending: the reboot's shutdown, new settings, or a
 * restart by the health monitor. A session first finishes its epoch
 * (drain_step). */
static bool close_planned(const slot_t *s)
{
    return s_shutdown || (s->cmd & (CMD_CONFIG | CMD_RESTART)) != 0;
}

/* The session still has time to finish its epoch before a planned close. */
static bool draining(const slot_t *s)
{
    return s->draining && (int32_t)(now_ms() - s->drain_until_ms) < 0;
}

/* Commands or events that end any attempt or session at once (a planned
 * close once the session's epoch is out, or its time is up). */
static bool hard_abort(slot_t *s)
{
    return (close_planned(s) && !draining(s)) || !s_running || !net_ready() || (s->open && s->net_gen != s_net_gen);
}

/* A connection attempt also stops for a pause (nothing has been streamed yet). */
static bool connect_abort(slot_t *s)
{
    return hard_abort(s) || s_paused || !s->cfg.enabled;
}

typedef enum { WR_DONE = 0, WR_STALL, WR_ERROR, WR_ABORT } write_result_t;

/*
 * Write all of p. Every accepted byte is progress; nothing accepted for
 * stall_ms is a write stall. Once part of a chunk (or frame) is out, the
 * rest goes out before anything else, or the session is closed.
 */
static write_result_t write_all(slot_t *s, const uint8_t *p, size_t n, uint32_t stall_ms, bool count)
{
    size_t off = 0;
    uint32_t last = now_ms();
    while (off < n) {
        int r = sess_send(s, p + off, n - off);
        if (r > 0) {
            off += (size_t)r;
            s->mid_chunk = off < n;
            last = now_ms();
            s->last_tx_ms = last;
            s->progress_ms = last;
            if (count) {
                ntrip_sm_sent(&s->sm, (size_t)r, last);
            }
            continue;
        }
        if (r < 0) {
            return WR_ERROR;
        }
        if ((uint32_t)(now_ms() - last) >= stall_ms) {
            return WR_STALL;
        }
        if (hard_abort(s)) {
            return WR_ABORT;
        }
        sess_wait(s, true, TICK_MS);
    }
    s->mid_chunk = false;
    return WR_DONE;
}

/* Close the socket. Clean: v2 terminator between chunks, TLS close_notify. */
static void session_close(slot_t *s, bool clean)
{
    if (clean && s->open && s->chunked && !s->mid_chunk) {
        const char *t = NTRIP_CHUNK_TERMINATOR;
        (void)write_all(s, (const uint8_t *)t, strlen(t), NTRIP_TERMINATOR_MS, false);
    }
    if (s->tls != NULL) {
        if (clean) {
            mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)esp_tls_get_ssl_context(s->tls);
            if (ssl != NULL) {
                (void)mbedtls_ssl_close_notify(ssl);   /* best effort, non-blocking */
            }
        }
        esp_tls_conn_destroy(s->tls);   /* closes the socket */
        s->tls = NULL;
        s->fd = -1;
    } else if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
    s->open = false;
    s->mid_chunk = false;
    s->paused_seen = false;
    s->draining = false;
    s->drained = false;
}

/* ---- connect ------------------------------------------------------------------------------- */

static void fail_with(ntrip_failure_t *f, ntrip_cls_t cls, ntrip_row_t row, bool reached)
{
    memset(f, 0, sizeof(*f));
    f->cls = cls;
    f->row = row;
    f->reached_caster = reached;
}

/* A connect the caster's host answered with a reset: it was reached. lwIP
 * reports a reset answer to a SYN as ECONNRESET; other stacks use ECONNREFUSED. */
static bool connect_refused(int err)
{
    return err == ECONNREFUSED || err == ECONNRESET;
}

static io_result_t resolve(slot_t *s, struct sockaddr_storage *addr, socklen_t *alen, ntrip_failure_t *f)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", (unsigned)s->cfg.port);
    struct addrinfo hints = { 0 };
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(s->cfg.host, port, &hints, &res);
    if (rc == 0 && res != NULL && res->ai_addrlen <= sizeof(*addr)) {
        memcpy(addr, res->ai_addr, res->ai_addrlen);
        *alen = (socklen_t)res->ai_addrlen;
        freeaddrinfo(res);
        strlcpy(s->cache_host, s->cfg.host, sizeof(s->cache_host));
        memcpy(&s->cache_addr, addr, sizeof(*addr));
        s->cache_len = *alen;
    } else {
        if (res != NULL) {
            freeaddrinfo(res);
        }
        if (s->cache_len == 0 || strcmp(s->cache_host, s->cfg.host) != 0) {
            fail_with(f, NTRIP_CLS_DNS, NTRIP_ROW_TRANSIENT, false);
            s->st.last_err = ESP_ERR_NOT_FOUND;
            return IO_FAIL;
        }
        memcpy(addr, &s->cache_addr, sizeof(*addr));
        *alen = s->cache_len;
        s->st.dns_cached_used++;
        ESP_LOGW(TAG, "cst%u: DNS failed for %s, using the last address", (unsigned)s->idx, s->cfg.host);
    }
    /* the port of the cached address may be stale */
    if (addr->ss_family == AF_INET) {
        struct sockaddr_in *a = (struct sockaddr_in *)addr;
        a->sin_port = htons(s->cfg.port);
        inet_ntop(AF_INET, &a->sin_addr, s->st.resolved_ip, sizeof(s->st.resolved_ip));
    }
#if CONFIG_LWIP_IPV6
    else if (addr->ss_family == AF_INET6) {
        struct sockaddr_in6 *a = (struct sockaddr_in6 *)addr;
        a->sin6_port = htons(s->cfg.port);
        inet_ntop(AF_INET6, &a->sin6_addr, s->st.resolved_ip, sizeof(s->st.resolved_ip));
    }
#endif
    return IO_OK;
}

static io_result_t tcp_connect(slot_t *s, const struct sockaddr_storage *addr, socklen_t alen, ntrip_failure_t *f)
{
    int fd = socket(addr->ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0) {
        fail_with(f, NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false);
        s->st.last_err = ESP_ERR_NO_MEM;
        return IO_FAIL;
    }
    s->fd = fd;
    set_sock_opts(fd);
    set_nonblocking(fd);
    if (connect(fd, (const struct sockaddr *)addr, alen) == 0) {
        return IO_OK;
    }
    if (errno != EINPROGRESS) {
        int err = errno;
        fail_with(f, NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false);
        f->refused = connect_refused(err);   /* else no route or a local failure: not reached */
        s->last_errno = err;
        s->st.last_err = ESP_FAIL;
        return IO_FAIL;
    }
    uint32_t start = now_ms();
    for (;;) {
        if (connect_abort(s)) {
            return IO_ABORT;
        }
        uint32_t el = now_ms() - start;
        if (el >= CONFIG_NTRIP_TCP_CONNECT_TIMEOUT_MS) {
            fail_with(f, NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false);
            s->st.last_err = ESP_ERR_TIMEOUT;
            return IO_FAIL;
        }
        fd_set w;
        FD_ZERO(&w);
        FD_SET(fd, &w);
        uint32_t ms = CONFIG_NTRIP_TCP_CONNECT_TIMEOUT_MS - el;
        ms = ms < TICK_MS ? ms : TICK_MS;
        struct timeval tv = { .tv_sec = 0, .tv_usec = (suseconds_t)(ms * 1000u) };
        if (select(fd + 1, NULL, &w, NULL, &tv) > 0) {
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                fail_with(f, NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false);
                f->refused = connect_refused(err);
                s->last_errno = err;
                s->st.last_err = ESP_FAIL;
                return IO_FAIL;
            }
            return IO_OK;
        }
    }
}

/* "No check" mode: the user chose it for this slot; nothing else changes. */
static esp_err_t attach_no_verify(void *conf)
{
    mbedtls_ssl_conf_authmode((mbedtls_ssl_config *)conf, MBEDTLS_SSL_VERIFY_NONE);
    return ESP_OK;
}

static char *load_ca_pem(slot_t *s, size_t *bytes)
{
    char ns[16];
    slot_ns(s->idx, ns, sizeof(ns));
    char *pem = heap_caps_malloc(CA_PEM_MAX + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (pem == NULL) {
        pem = malloc(CA_PEM_MAX + 1);
    }
    if (pem == NULL) {
        return NULL;
    }
    size_t len = CA_PEM_MAX;
    if (settings_get_blob(ns, SETTINGS_KEY_CST_CA_PEM, pem, &len) != ESP_OK || len == 0) {
        free(pem);
        return NULL;
    }
    if (pem[len - 1] != '\0') {
        pem[len++] = '\0';   /* PEM must be NUL-terminated, the NUL counted */
    }
    *bytes = len;
    return pem;
}

static io_result_t tls_connect(slot_t *s, ntrip_failure_t *f)
{
    tls_keep_alive_cfg_t ka = {
        .keep_alive_enable = true,
        .keep_alive_idle = NTRIP_KEEPALIVE_IDLE_S,
        .keep_alive_interval = NTRIP_KEEPALIVE_INTVL_S,
        .keep_alive_count = NTRIP_KEEPALIVE_COUNT,
    };
    esp_tls_cfg_t cfg = { 0 };
    cfg.timeout_ms = CONFIG_NTRIP_CONNECT_TIMEOUT_MS;
    cfg.keep_alive_cfg = &ka;
    /* Connect to the resolved (or cached) address; name checks and SNI use the configured host. */
    cfg.common_name = host_is_ip(s->cfg.host) ? NULL : s->cfg.host;
    char *pem = NULL;
    switch (s->cfg.trust_mode) {
    case NTRIP_TRUST_CUSTOM_CA: {
        size_t bytes = 0;
        pem = load_ca_pem(s, &bytes);
        if (pem == NULL) {
            fail_with(f, NTRIP_CLS_TLS, NTRIP_ROW_CONFIG, false);
            s->hint = HINT_CA_PEM;
            s->st.last_err = ESP_ERR_NOT_FOUND;
            return IO_FAIL;
        }
        cfg.cacert_buf = (const unsigned char *)pem;
        cfg.cacert_bytes = (unsigned int)bytes;
        break;
    }
    case NTRIP_TRUST_INSECURE:
        cfg.crt_bundle_attach = attach_no_verify;
        break;
    case NTRIP_TRUST_BUNDLE:
    default:
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        break;
    }

    s->tls = esp_tls_init();
    if (s->tls == NULL) {
        free(pem);
        fail_with(f, NTRIP_CLS_TLS, NTRIP_ROW_TRANSIENT, false);
        s->st.last_err = ESP_ERR_NO_MEM;
        return IO_FAIL;
    }
    const char *ip = s->st.resolved_ip[0] != '\0' ? s->st.resolved_ip : s->cfg.host;
    int rc = esp_tls_conn_new_sync(ip, (int)strlen(ip), s->cfg.port, &cfg, s->tls);
    free(pem);
    if (rc == 1) {
        int fd = -1;
        if (esp_tls_get_conn_sockfd(s->tls, &fd) == ESP_OK && fd >= 0) {
            s->fd = fd;
            int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            set_nonblocking(fd);
        }
        mbedtls_ssl_context *ssl = (mbedtls_ssl_context *)esp_tls_get_ssl_context(s->tls);
        if (ssl != NULL) {
            const char *v = mbedtls_ssl_get_version(ssl);
            const char *c = mbedtls_ssl_get_ciphersuite(ssl);
            strlcpy(s->st.tls_version, v ? v : "", sizeof(s->st.tls_version));
            strlcpy(s->st.tls_cipher, c ? c : "", sizeof(s->st.tls_cipher));
        }
        return IO_OK;
    }

    esp_tls_conn_state_t state = ESP_TLS_INIT;
    (void)esp_tls_get_conn_state(s->tls, &state);
    esp_tls_error_handle_t eh = NULL;
    int code = 0;
    int flags = 0;
    int sock_err = 0;
    esp_err_t last = ESP_FAIL;
    if (esp_tls_get_error_handle(s->tls, &eh) == ESP_OK) {
        last = esp_tls_get_and_clear_last_error(eh, &code, &flags);
        (void)esp_tls_get_and_clear_error_type(eh, ESP_TLS_ERR_TYPE_SYSTEM, &sock_err);
    }
    s->st.last_err = last;
    esp_tls_conn_destroy(s->tls);
    s->tls = NULL;
    s->fd = -1;
    if (state == ESP_TLS_INIT) {
        /* never got a TCP connection: refused by the host, or timed out / failed locally */
        fail_with(f, NTRIP_CLS_CONNECT, NTRIP_ROW_TRANSIENT, false);
        f->refused = connect_refused(sock_err);
        s->last_errno = sock_err;
    } else if (flags != 0 || code == -MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        fail_with(f, NTRIP_CLS_TLS, NTRIP_ROW_CONFIG, true);   /* never retried in plaintext */
        s->hint = NTRIP_HINT_TLS_CERT;
    } else {
        fail_with(f, NTRIP_CLS_TLS, NTRIP_ROW_TRANSIENT, true);
    }
    return IO_FAIL;
}

/*
 * At most one TLS handshake at a time, the protected slot first. Taken right
 * before the handshake (after the name lookup) and given back right after
 * it, so a slow lookup never holds the protected slot back. A slot that is
 * not protected hands the gate straight back if the protected slot started
 * waiting while it was taking it.
 */
static bool tls_gate_take(slot_t *s)
{
    bool prot = s->cfg.protected_slot;
    if (prot) {
        __atomic_add_fetch(&s_tls_protected_waiting, 1u, __ATOMIC_SEQ_CST);
    }
    bool got = false;
    while (!got && !connect_abort(s)) {
        s->progress_ms = now_ms();
        if (!prot && __atomic_load_n(&s_tls_protected_waiting, __ATOMIC_SEQ_CST) > 0) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        got = xSemaphoreTake(s_tls_sem, pdMS_TO_TICKS(TICK_MS)) == pdTRUE;
        if (got && !prot && __atomic_load_n(&s_tls_protected_waiting, __ATOMIC_SEQ_CST) > 0) {
            xSemaphoreGive(s_tls_sem);   /* the protected slot goes first */
            got = false;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
    if (prot) {
        __atomic_sub_fetch(&s_tls_protected_waiting, 1u, __ATOMIC_SEQ_CST);
    }
    if (got) {
        s->gate_held = true;
    }
    return got;
}

/* What a caster's text must never show: the password, the user, and an Onocoy slot's mountpoint (a credential). */
static void slot_secrets(const slot_t *s, const char *secrets[3])
{
    secrets[0] = s->cfg.password;
    secrets[1] = s->guard ? s->cfg.mountpoint : NULL;
    secrets[2] = s->cfg.username;
}

static void record_reply(slot_t *s)
{
    const ntrip_reply_t *r = &s->reply;
    const char *secrets[3];
    slot_secrets(s, secrets);
    ntrip_sanitize(r->line, r->line_len, s->st.last_status_line, sizeof(s->st.last_status_line), secrets, 3);
    s->st.last_status_code = r->verdict.status_code;
    ntrip_sanitize(r->server, strlen(r->server), s->st.server_header, sizeof(s->st.server_header), secrets, 3);
}

static io_result_t read_reply(slot_t *s, ntrip_failure_t *f)
{
    ntrip_reply_init(&s->reply, s->cfg.proto == NTRIP_PROTO_V2);
    s->reply.body_always = s->guard;   /* Onocoy's reason may be in any error reply's body */
    ntrip_rp_phase_t phase = s->reply.phase;
    uint32_t phase_start = now_ms();
    bool done = false;
    while (!done) {
        s->progress_ms = now_ms();
        if (s->reply.phase != phase) {
            phase = s->reply.phase;
            phase_start = now_ms();
        }
        uint32_t limit = ntrip_reply_phase_ms(&s->reply);
        uint32_t el = now_ms() - phase_start;
        if (el >= limit) {
            ntrip_reply_end(&s->reply, false);
            break;
        }
        if (connect_abort(s)) {
            return IO_ABORT;
        }
        int r = sess_recv(s, s->rx, sizeof(s->rx));
        if (r > 0) {
            size_t used = 0;
            done = ntrip_reply_feed(&s->reply, s->rx, (size_t)r, &used);
            continue;
        }
        if (r < 0) {
            ntrip_reply_end(&s->reply, true);
            break;
        }
        uint32_t left = limit - el;
        sess_wait(s, false, left < TICK_MS ? left : TICK_MS);
    }
    record_reply(s);
    const ntrip_verdict_t *v = &s->reply.verdict;
    if (v->success) {
        return IO_OK;
    }
    *f = ntrip_reply_failure(v, s->guard);   /* on a guard slot, the class Onocoy's reason names */
    s->hint = v->hint;
    s->st.last_err = ESP_FAIL;
    return IO_FAIL;
}

static io_result_t send_request(slot_t *s, ntrip_failure_t *f)
{
    const ntrip_caster_cfg_t *c = &s->cfg;
    bool v2 = c->proto == NTRIP_PROTO_V2;
    ntrip_wire_names_t w;
    ntrip_wire_names(s->guard, v2, c->mountpoint, c->username, c->label, &w);
    size_t n = v2 ? ntrip_build_v2_request(s->req, sizeof(s->req), c->host, c->port, c->tls, w.mount, w.user,
                                           c->password, s_agent, c->v2_raw_body)
                  : ntrip_build_v1_request(s->req, sizeof(s->req), c->password, w.mount, s_agent);
    if (n == 0) {
        fail_with(f, NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, true);
        s->hint = HINT_BAD_FIELDS;
        s->st.last_err = ESP_ERR_INVALID_ARG;
        return IO_FAIL;
    }
    write_result_t wr = write_all(s, (const uint8_t *)s->req, n, NTRIP_REQUEST_WRITE_MS, false);
    ntrip_secure_zero(s->req, sizeof(s->req));
    switch (wr) {
    case WR_DONE:
        return IO_OK;
    case WR_ABORT:
        return IO_ABORT;
    case WR_STALL:
        fail_with(f, NTRIP_CLS_TIMEOUT, NTRIP_ROW_TRANSIENT, true);
        s->st.last_err = ESP_ERR_TIMEOUT;
        return IO_FAIL;
    default:
        fail_with(f, io_error_class(s), NTRIP_ROW_TRANSIENT, true);
        s->st.last_err = ESP_FAIL;
        return IO_FAIL;
    }
}

static void tls_gate_give(slot_t *s)
{
    if (s->gate_held) {
        s->gate_held = false;
        xSemaphoreGive(s_tls_sem);
    }
}

static io_result_t do_connect(slot_t *s, ntrip_failure_t *f)
{
    struct sockaddr_storage addr;
    socklen_t alen = 0;
    s->st.resolved_ip[0] = '\0';
    s->st.tls_version[0] = '\0';
    s->st.tls_cipher[0] = '\0';
    s->st.last_status_line[0] = '\0';   /* evidence of this attempt only */
    s->st.last_status_code = 0;
    s->st.server_header[0] = '\0';
    s->hint = NTRIP_HINT_NONE;
    io_result_t r = resolve(s, &addr, &alen, f);
    if (r != IO_OK) {
        return r;
    }
    if (connect_abort(s)) {
        return IO_ABORT;
    }
    if (s->cfg.tls) {
        if (!tls_gate_take(s)) {
            return IO_ABORT;   /* ended while waiting for the gate: no attempt made */
        }
        r = tls_connect(s, f);
        tls_gate_give(s);   /* handshake done: the next slot may start its own */
    } else {
        r = tcp_connect(s, &addr, alen, f);
    }
    if (r != IO_OK) {
        return r;
    }
    r = send_request(s, f);
    if (r != IO_OK) {
        return r;
    }
    return read_reply(s, f);
}

/* ---- stats and status ------------------------------------------------------------------- */

/* `enabled` is the configuration: time before the client starts or while it
 * shuts down still counts as downtime in the ledger. */
static void ledger_report(slot_t *s)
{
    bool enabled = s->cfg.enabled;
    bool streaming = s->sm.state == NTRIP_SM_STREAMING;
    if (!s->ledger_known || enabled != s->ledger_enabled || s->cfg.protected_slot != s->ledger_protected ||
        streaming != s->ledger_streaming) {
        s->ledger_known = true;
        s->ledger_enabled = enabled;
        s->ledger_protected = s->cfg.protected_slot;
        s->ledger_streaming = streaming;
        ledger_set_slot(s->idx, enabled, s->cfg.protected_slot, streaming);
    }
}

static void account(slot_t *s, uint32_t now)
{
    int32_t day = utc_day();
    if (day >= 0 && day != s->day) {
        if (s->day >= 0) {
            /* new UTC day; until the first trusted clock everything counts as today */
            s->rate.on_ms = 0;
            s->lost_reported = 0;
            s->st.epochs_sent_today = 0;
            s->st.bytes_sent_today = 0;
            s->dropped_epochs_base += s->st.epochs_dropped_today;
            s->st.epochs_dropped_today = 0;
        }
        s->day = day;
    }
    /* The data rate's time base: only what the ledger counts as online, a
     * session past the caster's OK that streams fresh RTCM (not a session
     * left open through a pause or a stale stream, not the connect), each
     * pass by the state it began in, as the ledger counts it. */
    ntrip_rate_in_t rin = {
        .enabled = s->cfg.enabled && s_running,
        .session_open = s->open,
        .streaming = s->sm.state == NTRIP_SM_STREAMING,
        .rtcm_ok = ledger_rtcm_ok(),
    };
    ntrip_rate_clock_tick(&s->rate, now, &rin);
    /* Since when the protected slot counts today: from 00:00 UTC when it
     * counted already (or at this boot's first look: the ledger has the
     * downtime before it), else from the moment it was enabled and set up. */
    bool counted = s->cfg.protected_slot && s->cfg.enabled && s_running && ntrip_server_onocoy_ready(&s->cfg);
    time_t t = time(NULL);
    if (day >= 0 && t >= CLOCK_VALID_UNIX) {
        if (day != s->exp_day) {
            s->exp_day = day;
            s->exp_from_sod = UINT32_MAX;
        }
        if (counted && s->exp_from_sod == UINT32_MAX) {
            s->exp_from_sod = (!s->counted_known || s->was_counted) ? 0u : (uint32_t)(t % 86400);
        }
    }
    s->counted_known = true;
    s->was_counted = counted;
}

static void publish(slot_t *s, uint32_t now, bool force)
{
    if (!force && (uint32_t)(now - s->last_pub_ms) < PUBLISH_MS) {
        return;
    }
    s->last_pub_ms = now;
    ntrip_caster_status_t *st = &s->st;
    st->state = (ntrip_state_t)s->sm.state;
    st->reason = (ntrip_fail_class_t)s->sm.reason;
    st->retry_reason = (ntrip_fail_class_t)ntrip_sm_retry_class(&s->sm);
    st->backoff_attempt = s->sm.attempt;
    st->next_retry_s = (ntrip_sm_retry_in_ms(&s->sm, now) + 999u) / 1000u;
    st->connected_s = s->open ? (now - s->sm.session_start_ms) / 1000u : 0;
    st->unreached_s = ntrip_sm_unreached_ms(&s->sm, now) / 1000u;
    if (!s->open) {
        st->connected_since = 0;
    }
    /* The protected slot's epochs lost while online this boot (one a second
     * expected), handed to the ledger as the count grows, so the day keeps
     * them across restarts. Never taken back: a count that shrinks again
     * (epochs that caught up) leaves what was reported. */
    if (s->cfg.protected_slot) {
        uint32_t lost = ntrip_lost_epochs((uint32_t)(s->rate.on_ms / 1000u), st->epochs_sent_today);
        if (lost > s->lost_reported) {
            ledger_count_lost_epochs(lost - s->lost_reported);
            s->lost_reported = lost;
        }
    }
    /* Every "today" figure from the availability ledger (kept across
     * restarts), so the day's online and offline times agree wherever they
     * are shown; without a trusted clock that is the last known day plus
     * the time not yet given a day. */
    ledger_day_t ld = { .utc_day = -1 };
    bool have_ld = s->idx < LEDGER_MAX_SLOTS && ledger_get_today(&ld) == ESP_OK;
    st->today_online_s = have_ld ? ld.up_s[s->idx] : 0;
    st->today_offline_s = have_ld ? ld.down_s[s->idx] : 0;

    if (s->consumer != NULL) {
        rtcm_hub_consumer_stats_t cs;
        if (rtcm_hub_get_consumer_stats(s->consumer, &cs) == ESP_OK) {
            st->frames_dropped = cs.frames_dropped;
            st->bytes_dropped = cs.bytes_dropped;
            st->epochs_dropped = cs.epochs_dropped;
            st->frames_skipped_idle = cs.frames_skipped_idle;
            st->lag_drops = cs.lag_drops;
            st->laps = cs.laps;
            st->lag_ms_now = s->open ? cs.lag_ms : 0;
            if (st->lag_ms_now > s->max_lag_seen) {
                s->max_lag_seen = st->lag_ms_now;
            }
            st->max_lag_ms = s->max_lag_seen;
            if (cs.epochs_dropped < s->dropped_epochs_base) {
                s->dropped_epochs_base = cs.epochs_dropped;
            }
            st->epochs_dropped_today = (uint32_t)(cs.epochs_dropped - s->dropped_epochs_base);
        }
    }

    /* The Onocoy day figures, only for a protected slot that is enabled and
     * set up: numbers for a slot that cannot stream would only mislead. */
    ntrip_day_in_t din = {
        .counted = have_ld && s->cfg.protected_slot && s->cfg.enabled && ntrip_server_onocoy_ready(&s->cfg),
        .online_s = st->today_online_s,
        .offline_s = st->today_offline_s,
        .rate_online_s = (uint32_t)(s->rate.on_ms / 1000u),
        .epochs_sent_today = st->epochs_sent_today,
        .lost_epochs = have_ld ? ld.lost_epochs : s->lost_reported,
    };
    time_t t = time(NULL);
    if (din.counted && t >= CLOCK_VALID_UNIX && s->exp_day == (int32_t)(t / 86400) && s->exp_from_sod != UINT32_MAX &&
        ld.utc_day == s->exp_day) {
        uint32_t sod = (uint32_t)(t % 86400);
        din.day_known = true;
        din.elapsed_s = sod > s->exp_from_sod ? sod - s->exp_from_sod : 0;
        /* Unplanned downtime as the availability ledger counts it (power cuts included). */
        uint32_t down = ld.down_s[s->idx];
        uint32_t planned = ld.planned_s[s->idx];
        din.unplanned_offline_s = down > planned ? down - planned : 0;
    }
    ntrip_day_out_t dout;
    ntrip_day_metrics(&din, &dout);
    st->day_metrics_valid = dout.valid;
    st->graced_uptime_permille = dout.graced_uptime_permille;
    st->streak_ok = dout.streak_ok;
    st->data_rate_permille = dout.data_rate_permille;
    st->data_rate_valid = dout.rate_valid;
    st->budget_s = dout.budget_s;
    st->budget_left_s = dout.budget_left_s;
    st->graced_uptime_so_far_permille = dout.graced_so_far_permille;
    st->so_far_valid = dout.so_far_valid;

    uint16_t hint = s->hint;
    if (s->sm.hint != NTRIP_HINT_NONE) {
        hint = s->sm.hint;
    }
    if (hint == NTRIP_HINT_NONE && s->cfg.enabled && s_running && s->sm.state == NTRIP_SM_WAIT_DATA &&
        !s->sm.stagger_pending) {
        hint = s->consumer == NULL ? HINT_NO_HUB : HINT_NO_RTCM;
    }
    if (s->low_heap_wait && hint == NTRIP_HINT_NONE) {
        hint = HINT_LOW_MEMORY;
    }
    strlcpy(st->hint, hint < HINT_TOTAL ? k_hint_text[hint] : "", sizeof(st->hint));

    /* The login guard: its stop, or where it counts, before any other hint. */
    st->stop_reason = s->sm.stop;
    st->login_rejects = s->sm.rejects;
    st->login_blocks = s->sm.blocks;
    /* The score as the stop sees it (and as stored): a "device reconnected"
     * line adds 2, so the raw score may pass the stop score. */
    st->kicked_sessions = s->sm.kick_score < NTRIP_ONO_KICK_STOP_SCORE ? s->sm.kick_score
                                                                        : (uint8_t)NTRIP_ONO_KICK_STOP_SCORE;
    /* Waiting or connecting in the incident: the guard's count, not the
     * verdict hint of the attempt before (that one is stale while the next
     * attempt connects). */
    bool waiting = s->sm.state != NTRIP_SM_STREAMING;
    if (s->sm.stop != NTRIP_STOP_NONE && s->sm.stop < NTRIP_STOP_COUNT) {
        strlcpy(st->hint, k_stop_text[s->sm.stop], sizeof(st->hint));
    } else if (waiting && s->sm.blocks > 0) {
        snprintf(st->hint, sizeof(st->hint), "Onocoy is blocking logins (%u of %u)", (unsigned)s->sm.blocks,
                 (unsigned)NTRIP_ONO_BLOCKED_STOP);
    } else if (waiting && s->sm.rejects > 0) {
        snprintf(st->hint, sizeof(st->hint), "Onocoy refused the login (%u of %u)", (unsigned)s->sm.rejects,
                 (unsigned)NTRIP_ONO_REJECT_STOP);
    } else if (waiting && s->sm.inuse > 0) {
        snprintf(st->hint, sizeof(st->hint), "Onocoy says the credential is in use (%u of %u)", (unsigned)s->sm.inuse,
                 (unsigned)NTRIP_ONO_INUSE_STOP);
    } else if (waiting && s->sm.kick_score > 0) {
        snprintf(st->hint, sizeof(st->hint), "sessions ended soon after login (%u of %u)",
                 (unsigned)st->kicked_sessions, (unsigned)NTRIP_ONO_KICK_STOP_SCORE);
    }

    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status[s->idx] = *st;
        xSemaphoreGive(s_status_mutex);
    }
}

static void note_state(slot_t *s, uint32_t now)
{
    if (s->sm.state != s->prev_state) {
        s->prev_state = (uint8_t)s->sm.state;
        s->state_since_ms = s->sm.state_since_ms;
        size_t idx = s->idx;
        (void)esp_event_post(NTRIP_EVENT, NTRIP_EVENT_STATE_CHANGED, &idx, sizeof(idx), 0);
        ledger_report(s);
        publish(s, now, true);
    }
}

/* ---- backoff persistence -------------------------------------------------------------------- */

static void persist_backoff(slot_t *s, uint32_t now)
{
    if (!ntrip_sm_persist_due(&s->sm)) {
        return;
    }
    ntrip_backoff_rec_t rec;
    ntrip_sm_persist_get(&s->sm, now, &rec);
    uint8_t buf[NTRIP_BACKOFF_REC_MAX];
    size_t len = ntrip_backoff_rec_encode(&rec, buf, sizeof(buf));
    char ns[16];
    slot_ns(s->idx, ns, sizeof(ns));
    esp_err_t err = len > 0 ? settings_set_state_blob(ns, SETTINGS_KEY_CST_BACKOFF, buf, len) : ESP_ERR_INVALID_SIZE;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "cst%u: backoff state not saved: %s", (unsigned)s->idx, esp_err_to_name(err));
    }
    ntrip_sm_persist_done(&s->sm);   /* never retried in a loop: flash wear */
}

static void restore_backoff(slot_t *s, uint32_t now)
{
    char ns[16];
    slot_ns(s->idx, ns, sizeof(ns));
    uint8_t buf[NTRIP_BACKOFF_REC_MAX];
    size_t len = sizeof(buf);
    ntrip_backoff_rec_t rec;
    if (settings_get_blob(ns, SETTINGS_KEY_CST_BACKOFF, buf, &len) == ESP_OK &&
        ntrip_backoff_rec_decode(buf, len, &rec)) {
        ntrip_sm_restore(&s->sm, &rec, now);
        uint32_t wait_ms = ntrip_sm_retry_in_ms(&s->sm, now);
        if (s->sm.stop != NTRIP_STOP_NONE && s->sm.stop < NTRIP_STOP_COUNT) {
            s->stop_seen = s->sm.stop;   /* journalled when it happened */
            ESP_LOGW(TAG, "cst%u (%s): %s (before the restart); not retrying until you act", (unsigned)s->idx,
                     s->cfg.name, k_stop_text[s->sm.stop]);
        } else if (wait_ms > 0 && rec.row == NTRIP_ROW_NONE && s->sm.silent_run >= NTRIP_ONO_SILENT_RUN) {
            ESP_LOGW(TAG, "cst%u (%s): %u attempts without a reply before the restart; first attempt in %u s",
                     (unsigned)s->idx, s->cfg.name, (unsigned)s->sm.silent_run, (unsigned)((wait_ms + 999u) / 1000u));
        } else if (wait_ms > 0) {
            ESP_LOGI(TAG, "cst%u: waiting out a stored %s backoff (%u s)", (unsigned)s->idx,
                     k_class_name[(int)rec.cls < (int)NTRIP_FAIL_CLASS_COUNT ? (int)rec.cls : 0],
                     (unsigned)((wait_ms + 999u) / 1000u));
        }
    }
}

/* ---- slot task ------------------------------------------------------------------------------- */

static ntrip_sm_cfg_t sm_cfg_of(const ntrip_caster_cfg_t *c)
{
    ntrip_sm_cfg_t sc = {
        .profile = (ntrip_prof_t)c->backoff_profile,
        .protected_slot = c->protected_slot,
        .stall_limit_ms = (c->protected_slot ? CONFIG_NTRIP_PROTECTED_SOURCE_STALL_S : CONFIG_NTRIP_SOURCE_STALL_S) *
                          1000u,
        .auth_window_ms = CONFIG_NTRIP_AUTH_TRANSIENT_WINDOW_S * 1000u,
        .fast_late_cap_ms = CONFIG_NTRIP_PROTECTED_MAX_BACKOFF_S * 1000u,
        .onocoy_guard = ntrip_slot_is_onocoy(c),
    };
    return sc;
}

static uint32_t cred_hash_of(const ntrip_caster_cfg_t *c)
{
    return ntrip_cred_hash(c->host, c->port, c->proto == NTRIP_PROTO_V2, c->mountpoint, c->username, c->password,
                           c->label);
}

static void copy_cfg(slot_t *s)
{
    ntrip_caster_cfg_t next;
    taskENTER_CRITICAL(&s_lock);
    next = s_cfg[s->idx];
    taskEXIT_CRITICAL(&s_lock);
    ntrip_secure_zero(s->cfg.password, sizeof(s->cfg.password));
    s->cfg = next;
    ntrip_secure_zero(next.password, sizeof(next.password));
    s->guard = ntrip_slot_is_onocoy(&s->cfg);
}

static void apply_lag(slot_t *s)
{
    if (s->consumer != NULL) {
        uint32_t lag = s->cfg.protected_slot ? ONOCOY_LAG_MS : CONFIG_RTCM_HUB_MAX_LAG_MS;
        esp_err_t err = rtcm_hub_set_max_lag(s->consumer, lag);
        if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
            ESP_LOGW(TAG, "cst%u: lag limit not set: %s", (unsigned)s->idx, esp_err_to_name(err));
        }
    }
}

static void ensure_consumer(slot_t *s, uint32_t now)
{
    if (s->consumer != NULL || (s->attach_wait && (int32_t)(now - s->attach_at_ms) < 0)) {
        return;
    }
    char name[8];
    snprintf(name, sizeof(name), "cst%u", (unsigned)s->idx);
    if (rtcm_hub_attach(name, &s->consumer) != ESP_OK) {
        s->consumer = NULL;
        s->attach_wait = true;
        s->attach_at_ms = now + ATTACH_RETRY_MS;
        return;
    }
    s->attach_wait = false;
    apply_lag(s);
}

static bool hub_fresh(void)
{
    rtcm_hub_stats_t hs;
    rtcm_hub_get_stats(&hs);
    return hs.frames_in > 0 && hs.last_frame_age_ms <= NTRIP_FRESH_DATA_MS;
}

/* ---- connection history -------------------------------------------------------------------- */

/* Queue one record; the oldest gives way if the journal has refused them for long. */
static void hist_queue(slot_t *s, uint16_t kind, int32_t a, int32_t b)
{
    if (s->jq_n == HIST_QUEUE_LEN) {
        memmove(&s->jq[0], &s->jq[1], (HIST_QUEUE_LEN - 1u) * sizeof(s->jq[0]));
        s->jq_n--;
        ESP_LOGD(TAG, "cst%u: connection history record dropped (journal busy)", (unsigned)s->idx);
    }
    s->jq[s->jq_n].kind = kind;
    s->jq[s->jq_n].a = a;
    s->jq[s->jq_n].b = b;
    s->jq_n++;
}

/* Write what waits, in order, until the journal refuses one (its rate limit: kept for later). */
static void hist_flush(slot_t *s)
{
    while (s->jq_n > 0) {
        esp_err_t err = diag_journal_append((diag_event_t)s->jq[0].kind, s->jq[0].a, s->jq[0].b);
        if (err == ESP_ERR_INVALID_STATE || err == ESP_ERR_NO_MEM) {
            return;   /* refused by a rate limit, or the write queue is full: next loop */
        }
        memmove(&s->jq[0], &s->jq[1], (size_t)(s->jq_n - 1u) * sizeof(s->jq[0]));
        s->jq_n--;   /* written, or no journal: nothing to keep */
    }
}

/* A session that reached STREAMING ended; cls = NONE when it was not a caster failure. */
static void hist_down(slot_t *s, ntrip_cls_t cls)
{
    int32_t a;
    int32_t b;
    if (ntrip_hist_down(&s->hist, (uint32_t)s->idx, (uint8_t)cls, uptime_s(), &a, &b)) {
        hist_queue(s, DIAG_EV_CASTER_DOWN, a, b);
        hist_flush(s);
    }
}

/* The login guard stopped the slot: log and journal it once per stop. */
static void note_stop(slot_t *s)
{
    ntrip_stop_t stop = s->sm.stop;
    if (stop == s->stop_seen) {
        return;
    }
    s->stop_seen = stop;
    if (stop == NTRIP_STOP_NONE || stop >= NTRIP_STOP_COUNT) {
        return;
    }
    ESP_LOGW(TAG, "cst%u (%s): %s; not retrying until you act", (unsigned)s->idx, s->cfg.name, k_stop_text[stop]);
    hist_queue(s, DIAG_EV_CASTER_STOP, (int32_t)((s->idx & 0xFFu) | ((uint32_t)stop << 8)), 0);
    hist_flush(s);
}

static void end_planned(slot_t *s, uint32_t now, bool clean)
{
    bool was_open = s->open;
    session_close(s, clean);
    ntrip_sm_closed(&s->sm, now);
    if (was_open) {
        s->st.connected_since = 0;
        hist_down(s, NTRIP_CLS_NONE);
    }
}

static void handle_commands(slot_t *s, uint32_t now)
{
    uint32_t cmd = take_cmd(s);
    if (cmd & CMD_CONFIG) {
        char old_host[64];
        strlcpy(old_host, s->cfg.host, sizeof(old_host));
        bool was_protected = s->cfg.protected_slot;
        if (s->open || s->fd >= 0 || s->tls != NULL) {
            end_planned(s, now, true);
        }
        copy_cfg(s);
        if (s->cfg.protected_slot && !was_protected) {
            /* Pointed at Onocoy now: the epochs it lost before, unprotected, are
             * not the protected slot's, so they are never reported (publish). */
            s->lost_reported = ntrip_lost_epochs((uint32_t)(s->rate.on_ms / 1000u), s->st.epochs_sent_today);
        }
        if (strcmp(old_host, s->cfg.host) != 0) {
            s->cache_len = 0;
            s->cache_host[0] = '\0';
        }
        ntrip_sm_cfg_t sc = sm_cfg_of(&s->cfg);
        ntrip_sm_user_edit(&s->sm, &sc, cred_hash_of(&s->cfg), now);   /* also ends a stop */
        s->tls_fail_streak = 0;
        s->hint = NTRIP_HINT_NONE;
        s->low_heap_wait = false;
        s->st.last_session_text[0] = '\0';
        s->stext.last[0] = '\0';
        note_stop(s);
        apply_lag(s);
        persist_backoff(s, now);
        ledger_report(s);
        ESP_LOGI(TAG, "cst%u (%s): settings changed%s", (unsigned)s->idx, s->cfg.name,
                 s->cfg.enabled ? "" : ", disabled");
    }
    if (cmd & CMD_RETRY) {
        bool was_stopped = s->sm.stop != NTRIP_STOP_NONE;
        ntrip_sm_retry(&s->sm, now);
        s->hint = NTRIP_HINT_NONE;
        note_stop(s);
        persist_backoff(s, now);
        ESP_LOGI(TAG, "cst%u (%s): %s at the user's request", (unsigned)s->idx, s->cfg.name,
                 was_stopped ? "stop cleared, one attempt now" : "retrying now");
    }
    if (cmd & CMD_RESTART) {
        if (s->open || s->fd >= 0 || s->tls != NULL) {
            end_planned(s, now, true);
        }
        s->st.restarts++;
        ESP_LOGW(TAG, "cst%u (%s): restarted by the health monitor", (unsigned)s->idx, s->cfg.name);
    }
    if (s->open && s->net_gen != s_net_gen) {
        /* the address changed: the old socket is dead, no clean close possible */
        end_planned(s, now, false);
        ntrip_sm_stagger(&s->sm, now, esp_random());
        ESP_LOGI(TAG, "cst%u (%s): address changed, reconnecting", (unsigned)s->idx, s->cfg.name);
    }
}

static void count_failure(slot_t *s, const ntrip_failure_t *f, bool after_streaming)
{
    if (after_streaming) {
        s->st.disconnects++;
    } else {
        s->st.failures++;
    }
    if (f->cls < NTRIP_CLS_COUNT) {
        s->st.failures_by_class[f->cls]++;
    }
    if (f->cls == NTRIP_CLS_TLS) {
        s->tls_fail_streak++;
        if (s->tls_fail_streak >= NTRIP_TLS_PLAIN_HINT_AFTER && s->guard && s->hint == NTRIP_HINT_NONE) {
            s->hint = NTRIP_HINT_TLS_TRY_PLAIN;   /* offered, never applied */
        }
    } else if (f->reached_caster) {
        s->tls_fail_streak = 0;
    }
}

static void session_failed(slot_t *s, ntrip_cls_t cls, uint32_t now)
{
    if (net_lost_settled()) {
        /* The session died while the network was down: a drop, not a caster
         * failure; the gate waits for the network instead of a backoff. */
        session_close(s, false);
        s->st.connected_since = 0;
        s->st.disconnects++;
        ntrip_sm_closed(&s->sm, now);
        hist_down(s, NTRIP_CLS_NONE);   /* not a caster failure */
        ESP_LOGW(TAG, "cst%u (%s): session to %s:%u ended while the network was down (%s)", (unsigned)s->idx,
                 s->cfg.name, s->cfg.host, (unsigned)s->cfg.port, k_class_name[cls]);
        return;
    }
    ntrip_failure_t f;
    fail_with(&f, cls, NTRIP_ROW_TRANSIENT, true);
    if (s->guard) {
        /* What the caster said during the session (a take-over, a ban) travels with its end. */
        f.onocoy = (uint8_t)ntrip_sess_text_reason(&s->stext);
        f.cls = ntrip_guard_class(&f);
        cls = f.cls;
    }
    session_close(s, false);
    s->st.connected_since = 0;
    count_failure(s, &f, true);
    hist_down(s, cls);
    uint32_t d = ntrip_sm_failed(&s->sm, &f, now, esp_random());
    if (s->sm.state == NTRIP_SM_STOPPED) {
        ESP_LOGW(TAG, "cst%u (%s): session to %s:%u ended (%s)", (unsigned)s->idx, s->cfg.name, s->cfg.host,
                 (unsigned)s->cfg.port, k_class_name[cls]);
        note_stop(s);
        return;
    }
    ESP_LOGW(TAG, "cst%u (%s): session to %s:%u ended (%s); retry in %u s", (unsigned)s->idx, s->cfg.name,
             s->cfg.host, (unsigned)s->cfg.port, k_class_name[cls], (unsigned)((d + 999u) / 1000u));
}

static void connect_once(slot_t *s, uint32_t now)
{
    if (s->cfg.tls) {
        if (!s->cfg.protected_slot && heap_caps_get_free_size(MALLOC_CAP_INTERNAL) < TLS_MIN_INTERNAL_HEAP) {
            if (!s->low_heap_wait) {
                ESP_LOGW(TAG, "cst%u (%s): low internal memory, TLS connect deferred", (unsigned)s->idx, s->cfg.name);
            }
            s->low_heap_wait = true;
            s->low_heap_until_ms = now + LOW_HEAP_RETRY_MS;
            return;
        }
        s->low_heap_wait = false;
    }
    session_close(s, false);   /* never two sessions for a slot */
    uint32_t t0 = now_ms();
    if (s->guard) {
        /* The last attempt's verdict hint would show through the whole
         * connect (up to the reply timeout) as if it were this attempt's. */
        s->hint = NTRIP_HINT_NONE;
    }
    ntrip_sm_connecting(&s->sm, t0);
    note_state(s, t0);
    ESP_LOGI(TAG, "cst%u (%s): connecting to %s:%u (NTRIP %s%s)", (unsigned)s->idx, s->cfg.name, s->cfg.host,
             (unsigned)s->cfg.port, s->cfg.proto == NTRIP_PROTO_V2 ? "2.0" : "1.0", s->cfg.tls ? ", TLS" : "");

    ntrip_failure_t f;
    memset(&f, 0, sizeof(f));
    io_result_t r = do_connect(s, &f);
    tls_gate_give(s);   /* do_connect gives it back after the handshake: never left held */
    uint32_t t1 = now_ms();
    if (r == IO_FAIL && net_lost_settled()) {
        r = IO_ABORT;   /* the network went down meanwhile: not a failure */
        ESP_LOGI(TAG, "cst%u (%s): attempt ended by a network loss (%s), not counted", (unsigned)s->idx, s->cfg.name,
                 k_class_name[f.cls]);
    }
    if (r == IO_ABORT) {
        session_close(s, false);
        ntrip_sm_closed(&s->sm, t1);
        return;
    }
    if (r == IO_FAIL) {
        session_close(s, false);
        count_failure(s, &f, false);   /* a reply's class is the guard's already (ntrip_reply_failure) */
        ntrip_hist_failed(&s->hist, uptime_s());
        uint32_t d = ntrip_sm_failed(&s->sm, &f, t1, esp_random());
        if (s->sm.state == NTRIP_SM_STOPPED) {
            ESP_LOGW(TAG, "cst%u (%s): %s:%u refused (%s): \"%.80s\"", (unsigned)s->idx, s->cfg.name, s->cfg.host,
                     (unsigned)s->cfg.port, k_class_name[f.cls], s->st.last_status_line);
            note_stop(s);
        } else if (s->st.last_status_line[0] != '\0' && f.reached_caster) {
            ESP_LOGW(TAG, "cst%u (%s): %s:%u refused (%s): \"%.80s\"; retry in %u s", (unsigned)s->idx, s->cfg.name,
                     s->cfg.host, (unsigned)s->cfg.port, k_class_name[f.cls], s->st.last_status_line,
                     (unsigned)((d + 999u) / 1000u));
        } else {
            ESP_LOGW(TAG, "cst%u (%s): %s:%u failed (%s); retry in %u s", (unsigned)s->idx, s->cfg.name,
                     s->cfg.host, (unsigned)s->cfg.port, k_class_name[f.cls], (unsigned)((d + 999u) / 1000u));
        }
        return;
    }

    /* accepted: RTCM may flow now, starting with the station metadata */
    s->open = true;
    s->chunked = s->cfg.proto == NTRIP_PROTO_V2 && !s->cfg.v2_raw_body;
    s->mid_chunk = false;
    ntrip_epoch_track_init(&s->epochs);
    s->paused_seen = false;
    s->logged_unexpected = false;
    ntrip_sess_text_start(&s->stext);
    s->last_tx_ms = t1;
    s->net_gen = s_net_gen;
    s->tls_fail_streak = 0;
    s->hint = NTRIP_HINT_NONE;
    if (s->consumer != NULL) {
        (void)rtcm_hub_resync(s->consumer);
    }
    ntrip_sm_connected(&s->sm, t1);
    s->st.connects++;
    s->st.connected_since = unix_now();
    int32_t ha;
    int32_t hb;
    if (ntrip_hist_up(&s->hist, (uint32_t)s->idx, uptime_s(), &ha, &hb)) {
        hist_queue(s, DIAG_EV_CASTER_UP, ha, hb);
        hist_flush(s);
    }
    ESP_LOGI(TAG, "cst%u (%s): streaming to %s:%u (%s)", (unsigned)s->idx, s->cfg.name, s->cfg.host,
             (unsigned)s->cfg.port, s->st.last_status_line);
}

/* A line the caster sent during the session is complete: show it, log the first of the session. */
static void session_text(slot_t *s)
{
    strlcpy(s->st.last_session_text, s->stext.last, sizeof(s->st.last_session_text));
    if (!s->logged_unexpected) {
        ESP_LOGW(TAG, "cst%u (%s): caster sent \"%s\"", (unsigned)s->idx, s->cfg.name, s->stext.last);
        s->logged_unexpected = true;
    }
}

/*
 * Read what the caster sends while streaming: nothing is expected, EOF ends
 * the session. Every line is kept (status last_session_text) and matched for
 * Onocoy's reasons; on a guard slot a duplicate-stream or banned report ends
 * the session at once (the guard then stops the slot).
 */
static bool poll_incoming(slot_t *s, uint32_t now)
{
    const char *secrets[3];
    slot_secrets(s, secrets);
    for (int i = 0; i < 4; i++) {
        int r = sess_recv(s, s->rx, sizeof(s->rx));
        if (r == 0) {
            return true;
        }
        if (r < 0) {
            if (ntrip_sess_text_end(&s->stext, secrets, 3)) {
                session_text(s);   /* a last line without its newline */
            }
            session_failed(s, r == -1 ? NTRIP_CLS_REMOTE_CLOSE : io_error_class(s), now);
            return false;
        }
        if (ntrip_sess_text_feed(&s->stext, s->rx, (size_t)r, secrets, 3)) {
            session_text(s);
            ntrip_onocoy_reason_t why = ntrip_sess_text_reason(&s->stext);
            if (s->guard && (why == NTRIP_ONO_DUPLICATE || why == NTRIP_ONO_BANNED)) {
                session_failed(s, NTRIP_CLS_BANNED, now);
                return false;
            }
        }
    }
    return true;
}

static void stream_step(slot_t *s, uint32_t now)
{
    if (!poll_incoming(s, now)) {
        return;
    }
    if (s->consumer == NULL) {
        wait_kick(s, TICK_MS);
        return;
    }
    /*
     * A pause for receiver work holds the hub at an epoch boundary
     * (rtcm_hub_hold, before the pause flag is set): this slot sends what is
     * left before the hold point at its own pace, then stays connected and
     * idle (the core closes the session at the stall limit). The release
     * (before the flag clears) makes the hub start this consumer over at its
     * next read: sticky copies, then the next whole epoch. A pause without a
     * hold (none is ever set without the hub) idles at once and resyncs on
     * the resume, as before holds existed.
     */
    if (s_paused && !rtcm_hub_held()) {
        s->paused_seen = true;
        wait_kick(s, TICK_MS);
        return;
    }
    if (s->paused_seen) {
        s->paused_seen = false;
        (void)rtcm_hub_resync(s->consumer);
        ntrip_epoch_track_init(&s->epochs);   /* the hub restarts at an epoch start */
    }
    size_t len = 0;
    esp_err_t err = rtcm_hub_read(s->consumer, s->tx + NTRIP_CHUNK_HEAD_RESERVE, NTRIP_CHUNK_MAX_PAYLOAD, &len,
                                  TICK_MS);
    if (err == ESP_ERR_INVALID_STATE) {
        if (s->draining) {
            s->drained = true;   /* the epoch being sent is over (or the hub's hold point is reached) */
            return;
        }
        wait_kick(s, TICK_MS);   /* held, and every frame before the hold point is out */
        return;
    }
    if (err != ESP_OK || len == 0) {
        if (err != ESP_ERR_TIMEOUT) {
            wait_kick(s, TICK_MS);   /* hub not ready: do not spin */
        }
        return;
    }
    if (rtcm_hub_restarted(s->consumer)) {
        ntrip_epoch_track_init(&s->epochs);   /* the hub started over at an epoch start (end of a hold) */
    }
    uint32_t frames = 0;
    uint32_t epochs = 0;
    ntrip_count_frames(s->tx + NTRIP_CHUNK_HEAD_RESERVE, len, &s->epochs, &frames, &epochs);
    const uint8_t *p = s->tx + NTRIP_CHUNK_HEAD_RESERVE;
    size_t n = len;
    if (s->chunked) {
        size_t start = 0;
        n = ntrip_chunk_wrap(s->tx, len, &start);
        p = s->tx + start;
    }
    write_result_t wr = write_all(s, p, n, CONFIG_NTRIP_SEND_TIMEOUT_MS, true);
    uint32_t t = now_ms();
    switch (wr) {
    case WR_DONE:
        s->st.bytes_sent += len;
        s->st.bytes_sent_today += len;
        s->st.frames_sent += frames;
        s->st.epochs_sent += epochs;
        s->st.epochs_sent_today += epochs;
        break;
    case WR_STALL:
        session_failed(s, NTRIP_CLS_WRITE_STALL, t);
        break;
    case WR_ERROR:
        session_failed(s, io_error_class(s), t);
        break;
    case WR_ABORT:
    default:
        end_planned(s, t, false);   /* mid-chunk: no terminator, just close */
        break;
    }
}

/*
 * Before a planned close the session sends the rest of the epoch it is inside
 * (rtcm_hub_finish_epoch) and nothing of the next, so the caster's last epoch
 * is a whole one: the hub hands out frames as they arrive, so the last chunk
 * of a session closed at once ends inside an epoch whenever the close falls
 * in the receiver's MSM burst, and only terminated epochs count for Onocoy.
 * Bounded by DRAIN_MS, on a reboot by half the shutdown budget that was left
 * (on_shutdown). @return true while the session drains (this pass streamed),
 * false when the close may go ahead.
 */
static bool drain_step(slot_t *s, uint32_t now)
{
    if (!s->open || !close_planned(s) || s->consumer == NULL) {
        s->draining = false;
        return false;
    }
    uint32_t until = s_shutdown ? __atomic_load_n(&s_shutdown_drain_until, __ATOMIC_SEQ_CST) : now + DRAIN_MS;
    if (!s->draining) {
        s->draining = true;
        /* No MSM run open in what this session sent (ntrip_count_frames):
         * it stands between two epochs, or after some station messages,
         * which are never part of an MSM run. */
        s->drained = s->epochs.open_mask == 0;
        s->drain_start_ms = now;
        s->drain_until_ms = (int32_t)(until - (now + DRAIN_MS)) < 0 ? until : now + DRAIN_MS;
        if (!s->drained) {
            (void)rtcm_hub_finish_epoch(s->consumer);
        }
    } else if (s_shutdown && (int32_t)(until - s->drain_until_ms) < 0) {
        s->drain_until_ms = until;   /* a reboot during a drain for new settings: its budget counts */
    }
    if (!s->drained && draining(s)) {
        stream_step(s, now);
        if (s->open && !s->drained) {
            return true;
        }
        if (s->open) {
            ESP_LOGI(TAG, "cst%u (%s): ending the session after the epoch it was sending (%u ms)", (unsigned)s->idx,
                     s->cfg.name, (unsigned)(now_ms() - s->drain_start_ms));
        }
    }
    if (s->open && !s->drained && s->epochs.open_mask != 0) {
        ESP_LOGW(TAG, "cst%u (%s): closing inside an epoch: its end did not come within %u ms", (unsigned)s->idx,
                 s->cfg.name, (unsigned)(now_ms() - s->drain_start_ms));
    }
    s->draining = false;
    return false;
}

/* One pass of the slot: commands, the state machine and what it asks for. */
static void slot_step(slot_t *s, uint32_t now)
{
    handle_commands(s, now);

    bool net = net_ready();   /* the address goes with the link (see "Network state") */
    ntrip_sm_in_t in = {
        .enabled = s->cfg.enabled && s_running && !s_shutdown,
        .have_ip = net,
        .link_up = net,
        .paused = s_paused,
        .fresh_data = s->consumer != NULL && hub_fresh(),
        .session_open = s->open,
        .silent_ms = s->open ? now - s->last_tx_ms : 0,
    };
    ntrip_close_why_t why = NTRIP_WHY_NONE;
    ntrip_sm_act_t act = ntrip_sm_poll(&s->sm, &in, now, esp_random(), &why);
    note_state(s, now);

    if (act == NTRIP_ACT_CLOSE) {
        static const char *const k_why[] = { "", "disabled", "no network", "paused, idle limit reached",
                                             "no RTCM from the receiver" };
        ESP_LOGI(TAG, "cst%u (%s): closing session (%s)", (unsigned)s->idx, s->cfg.name,
                 k_why[(size_t)why < sizeof(k_why) / sizeof(k_why[0]) ? (size_t)why : 0]);
        end_planned(s, now, why != NTRIP_WHY_NO_IP);
    } else if (act == NTRIP_ACT_CONNECT) {
        if (s->low_heap_wait && (int32_t)(now - s->low_heap_until_ms) < 0) {
            wait_kick(s, IDLE_WAIT_MS);
        } else {
            connect_once(s, now);
        }
    } else if (s->open) {
        stream_step(s, now);
    } else {
        uint32_t wait = IDLE_WAIT_MS;
        uint32_t retry = ntrip_sm_retry_in_ms(&s->sm, now);
        if (retry > 0 && retry < wait) {
            wait = retry;
        }
        wait_kick(s, wait);
    }
}

static void slot_task(void *arg)
{
    slot_t *s = (slot_t *)arg;
    uint32_t now = now_ms();
    copy_cfg(s);
    ntrip_sm_cfg_t sc = sm_cfg_of(&s->cfg);
    ntrip_sm_init(&s->sm, &sc, cred_hash_of(&s->cfg), now);
    s->prev_state = (uint8_t)s->sm.state;
    s->state_since_ms = now;
    ntrip_rate_clock_init(&s->rate, now);
    ntrip_hist_init(&s->hist);
    restore_backoff(s, now);

    for (;;) {
        now = now_ms();
        s->progress_ms = now;
        ensure_consumer(s, now);
        if (!drain_step(s, now)) {
            slot_step(s, now);
        }

        now = now_ms();
        note_state(s, now);
        hist_flush(s);
        persist_backoff(s, now);
        account(s, now);
        ledger_report(s);
        publish(s, now, false);
    }
}

/* ---- lifecycle ----------------------------------------------------------------------------- */

static bool any_session_open(void)
{
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        if (s_slots[i] != NULL && s_slots[i]->open) {
            return true;
        }
    }
    return false;
}

static void wait_sessions_closed(uint32_t budget_ms)
{
    uint32_t start = now_ms();
    while (any_session_open() && (uint32_t)(now_ms() - start) < budget_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

/* Planned reboot: every slot sends the rest of the epoch it is inside
 * (drain_step, at most half the budget left), then closes its session
 * (terminator, close_notify, close) so the caster frees the mountpoint at
 * once. Bounded by the lifecycle shutdown budget; a slot still in a TLS
 * handshake is skipped. */
static void on_shutdown(lifecycle_reboot_reason_t reason, uint32_t deadline_ms, void *ctx)
{
    (void)reason;
    (void)ctx;
    uint32_t drain = deadline_ms / 2u < DRAIN_MS ? deadline_ms / 2u : DRAIN_MS;
    __atomic_store_n(&s_shutdown_drain_until, now_ms() + drain, __ATOMIC_SEQ_CST);
    __atomic_store_n(&s_shutdown, true, __ATOMIC_SEQ_CST);
    kick_all();
    wait_sessions_closed(deadline_ms);
}

static esp_err_t create_slot(size_t i)
{
    slot_t *s = heap_caps_calloc(1, sizeof(slot_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s == NULL) {
        s = calloc(1, sizeof(slot_t));
    }
    if (s == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s->idx = i;
    s->fd = -1;
    s->day = -1;
    s->exp_day = -1;
    s->exp_from_sod = UINT32_MAX;
    s_slots[i] = s;
    char name[configMAX_TASK_NAME_LEN];
    snprintf(name, sizeof(name), "cst%u", (unsigned)i);
    if (xTaskCreatePinnedToCore(slot_task, name, CONFIG_NTRIP_TASK_STACK, s, CONFIG_NTRIP_TASK_PRIO, &s->task,
                                tskNO_AFFINITY) != pdPASS) {
        s_slots[i] = NULL;
        free(s);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t ntrip_server_init(void)
{
    if (s_loaded) {
        return ESP_OK;
    }
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        (void)load_slot(i, &s_cfg[i]);
        if (s_cfg[i].test_onocoy && !ntrip_test_onocoy_allowed()) {
            /* Marked in a bench or QEMU build: this build judges the slot by its host. */
            ESP_LOGW(TAG, "slot %u: stored as a test Onocoy slot, which this build does not allow: ignored",
                     (unsigned)i);
            (void)ntrip_server_apply_host_rules(&s_cfg[i], NULL);
        }
    }
    s_status_mutex = xSemaphoreCreateMutex();
    s_cfg_mutex = xSemaphoreCreateMutex();
    s_tls_sem = xSemaphoreCreateBinary();
    s_kick = xEventGroupCreate();
    if (s_status_mutex == NULL || s_cfg_mutex == NULL || s_tls_sem == NULL || s_kick == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(s_tls_sem);
    const esp_app_desc_t *app = esp_app_get_description();
    if (ntrip_build_agent(app->version, s_agent, sizeof(s_agent)) == 0) {
        ntrip_build_agent("", s_agent, sizeof(s_agent));
    }
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        s_status[i].state = NTRIP_STATE_DISABLED;
    }
    s_loaded = true;

    esp_err_t err = lifecycle_register_shutdown_hook(on_shutdown, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_net_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_net_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(IP_EVENT, IP_EVENT_NETIF_DOWN, on_net_event, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_STOP, on_net_event, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }
    for (size_t i = 0; i < ntrip_server_slot_count() && err == ESP_OK; i++) {
        err = create_slot(i);
    }
    if (err != ESP_OK) {
        return err;
    }
    ESP_LOGI(TAG, "%d caster slots, agent \"%s\"", CONFIG_NTRIP_MAX_CASTERS, s_agent);
    return ESP_OK;
}

esp_err_t ntrip_server_start(void)
{
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    refresh_network_state();
    s_running = true;
    kick_all();
    return ESP_OK;
}

esp_err_t ntrip_server_stop(void)
{
    if (!s_loaded) {
        return ESP_OK;
    }
    s_running = false;
    kick_all();
    wait_sessions_closed(STOP_WAIT_MS);
    return ESP_OK;
}

size_t ntrip_server_slot_count(void)
{
    return CONFIG_NTRIP_MAX_CASTERS;
}

esp_err_t ntrip_server_get_config(size_t slot, ntrip_caster_cfg_t *out, bool with_secret)
{
    if (out == NULL || slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    *out = s_cfg[slot];
    taskEXIT_CRITICAL(&s_lock);
    out->password_set = out->password[0] != '\0';
    out->clear_password = false;
    out->ca_pem_set = ca_pem_present(slot);
    if (!with_secret) {
        memset(out->password, 0, sizeof(out->password));
    }
    return ESP_OK;
}

/* The cross-slot rules against the stored configs, with `next` in `slot`. */
static ntrip_policy_t check_policy(size_t slot, const ntrip_caster_cfg_t *next)
{
    ntrip_policy_slot_t pol[CONFIG_NTRIP_MAX_CASTERS];
    char hosts[CONFIG_NTRIP_MAX_CASTERS][64];
    taskENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        const ntrip_caster_cfg_t *c = (i == slot) ? next : &s_cfg[i];
        memcpy(hosts[i], c->host, sizeof(hosts[i]));
        pol[i].enabled = c->enabled;
        pol[i].dup_ack = c->dup_stream_ack;
        pol[i].onocoy = c->test_onocoy && ntrip_test_onocoy_allowed();   /* the host part follows below */
    }
    taskEXIT_CRITICAL(&s_lock);
    for (size_t i = 0; i < ntrip_server_slot_count(); i++) {
        hosts[i][sizeof(hosts[i]) - 1] = '\0';
        pol[i].host = hosts[i];
    }
    return ntrip_check_policy(pol, ntrip_server_slot_count(), slot);
}

esp_err_t ntrip_server_set_config(size_t slot, const ntrip_caster_cfg_t *cfg, ntrip_policy_t *policy)
{
    if (policy != NULL) {
        *policy = NTRIP_POLICY_OK;
    }
    if (cfg == NULL || slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!fits(cfg->name, sizeof(cfg->name)) || !fits(cfg->host, sizeof(cfg->host)) ||
        !fits(cfg->mountpoint, sizeof(cfg->mountpoint)) || !fits(cfg->username, sizeof(cfg->username)) ||
        !fits(cfg->password, sizeof(cfg->password)) || !fits(cfg->label, sizeof(cfg->label)) ||
        !ntrip_cfg_enums_valid(cfg)) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_cfg_mutex, portMAX_DELAY);
    ntrip_caster_cfg_t next = *cfg;
    next.clear_password = false;
    next.ca_pem_set = false;   /* output only */
    (void)ntrip_server_apply_host_rules(&next, NULL);   /* never refuses without a requested profile */
    char old_host[sizeof(next.host)];
    char old_mount[sizeof(next.mountpoint)];
    taskENTER_CRITICAL(&s_lock);
    memcpy(old_host, s_cfg[slot].host, sizeof(old_host));
    memcpy(old_mount, s_cfg[slot].mountpoint, sizeof(old_mount));
    taskEXIT_CRITICAL(&s_lock);
    old_host[sizeof(old_host) - 1] = '\0';
    old_mount[sizeof(old_mount) - 1] = '\0';
    if (cfg->clear_password) {
        memset(next.password, 0, sizeof(next.password));
    } else if (next.password[0] == '\0') {
        /* Keep the stored password only for the same host: never hand one
         * caster's password to another. */
        taskENTER_CRITICAL(&s_lock);
        if (strcmp(next.host, s_cfg[slot].host) == 0) {
            memcpy(next.password, s_cfg[slot].password, sizeof(next.password));
        }
        taskEXIT_CRITICAL(&s_lock);
    }
    next.password_set = next.password[0] != '\0';

    esp_err_t err = ESP_OK;
    ntrip_fields_t f = fields_of(&next);
    if (has_bad_name(next.name) || ntrip_validate_fields(&f) != NTRIP_FIELDS_OK ||
        ntrip_server_rtk2go_check(old_host, old_mount, next.host, next.mountpoint) != NTRIP_RTK2GO_OK) {
        err = ESP_ERR_INVALID_ARG;
    } else {
        ntrip_policy_t rule = check_policy(slot, &next);
        if (rule != NTRIP_POLICY_OK) {
            err = ESP_ERR_NOT_ALLOWED;
            if (policy != NULL) {
                *policy = rule;
            }
        }
    }
    uint8_t buf[NTRIP_CFG_BLOB_MAX];
    size_t len = 0;
    if (err == ESP_OK) {
        len = ntrip_cfg_encode(&next, buf, sizeof(buf));
        err = len == 0 ? ESP_ERR_INVALID_SIZE : ESP_OK;
    }
    if (err == ESP_OK) {
        char ns[16];
        slot_ns(slot, ns, sizeof(ns));
        err = settings_set_blob(ns, SETTINGS_KEY_CST_CFG, buf, len);
    }
    ntrip_secure_zero(buf, sizeof(buf));
    if (err == ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_cfg[slot] = next;
        taskEXIT_CRITICAL(&s_lock);
        if (s_slots[slot] != NULL) {
            __atomic_or_fetch(&s_slots[slot]->cmd, CMD_CONFIG, __ATOMIC_SEQ_CST);
            kick(slot);   /* only this slot reconnects */
        }
    }
    ntrip_secure_zero(next.password, sizeof(next.password));
    xSemaphoreGive(s_cfg_mutex);
    return err;
}

esp_err_t ntrip_server_get_status(size_t slot, ntrip_caster_status_t *out)
{
    if (out == NULL || slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->state = NTRIP_STATE_DISABLED;
    slot_t *s = s_slots[slot];
    if (!s_loaded || s == NULL || s_status_mutex == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    *out = s_status[slot];
    xSemaphoreGive(s_status_mutex);
    uint32_t now = now_ms();
    out->state_age_s = (now - s->state_since_ms) / 1000u;
    out->progress_age_ms = now - s->progress_ms;
    return ESP_OK;
}

esp_err_t ntrip_server_restart_slot(size_t slot)
{
    if (slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded || s_slots[slot] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_or_fetch(&s_slots[slot]->cmd, CMD_RESTART, __ATOMIC_SEQ_CST);
    kick(slot);
    return ESP_OK;
}

esp_err_t ntrip_server_retry_slot(size_t slot)
{
    if (slot >= ntrip_server_slot_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_loaded || s_slots[slot] == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    bool enabled = s_cfg[slot].enabled;
    taskEXIT_CRITICAL(&s_lock);
    if (!enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_or_fetch(&s_slots[slot]->cmd, CMD_RETRY, __ATOMIC_SEQ_CST);
    kick(slot);
    return ESP_OK;
}

esp_err_t ntrip_server_set_paused(bool paused)
{
    if (!s_loaded) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_paused != paused) {
        ESP_LOGI(TAG, "%s", paused ? "paused for receiver work" : "resumed");
    }
    s_paused = paused;
    ledger_set_planned(paused);
    kick_all();
    return ESP_OK;
}

