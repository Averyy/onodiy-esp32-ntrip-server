/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * RTOS and driver glue for the network component. Decisions (backoff, when
 * the AP is up, the recovery ladder, the captive DNS reply, which drops are
 * journalled) live in the pure cores net_core.c, net_ladder.c and
 * captive_dns.c.
 *
 * Tasks:
 *  - "net" (core 0): owns the Wi-Fi state. Event handlers only forward IDF
 *    events into its queue; it reconnects, raises and drops the AP, runs
 *    recovery actions, keeps the fallback DNS and SNTP servers in place and
 *    updates the status snapshot.
 *  - "netprobe": the upstream probe (name lookup + TCP connect), so a slow
 *    lookup never delays a reconnect.
 *  - "dns": the captive-portal responder, only while the AP is up.
 */
#include "network.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "captive_dns.h"
#include "diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ledger.h"
#include "lifecycle.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "net_core.h"
#include "net_ladder.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "network";

ESP_EVENT_DEFINE_BASE(NETWORK_EVENT);

_Static_assert((int)NETWORK_PROBE_NONE == (int)NET_PROBE_NONE && (int)NETWORK_PROBE_OK == (int)NET_PROBE_OK &&
                   (int)NETWORK_PROBE_NO_IP == (int)NET_PROBE_NO_IP &&
                   (int)NETWORK_PROBE_DNS_FAILED == (int)NET_PROBE_DNS_FAILED &&
                   (int)NETWORK_PROBE_NO_UPSTREAM == (int)NET_PROBE_NO_UPSTREAM,
               "probe results match the ladder core");
_Static_assert((int)NETWORK_AP_SETUP == (int)NET_AP_SETUP && (int)NETWORK_AP_FALLBACK == (int)NET_AP_FALLBACK &&
                   (int)NETWORK_AP_LINGER == (int)NET_AP_LINGER && (int)NETWORK_AP_RECOVERY == (int)NET_AP_RECOVERY,
               "AP reasons match the core");
_Static_assert((int)NETWORK_LADDER_WHY_UPSTREAM_OK == (int)NET_WHY_UPSTREAM_OK &&
                   (int)NETWORK_LADDER_WHY_NO_UPSTREAM == (int)NET_WHY_NO_UPSTREAM &&
                   (int)NETWORK_LADDER_WHY_JOURNAL_OPEN == (int)NET_WHY_JOURNAL_OPEN &&
                   (int)NETWORK_LADDER_WHY_INTERVAL == (int)NET_WHY_INTERVAL &&
                   (int)NETWORK_LADDER_WHY_INHIBITED == (int)NET_WHY_INHIBITED &&
                   (int)NETWORK_LADDER_WHY_AP_IN_USE == (int)NET_WHY_AP_IN_USE &&
                   (int)NETWORK_LADDER_WHY_NO_REBOOT_TRIGGER == (int)NET_WHY_NO_REBOOT_TRIGGER &&
                   (int)NETWORK_LADDER_WHY_AP_OVERRIDDEN == (int)NET_WHY_AP_OVERRIDDEN,
               "ladder reasons match the core");
_Static_assert((int)NETWORK_LADDER_TRIG_PROTECTED == (int)NET_TRIG_P &&
                   (int)NETWORK_LADDER_TRIG_ASSOC_NO_IP == (int)NET_TRIG_A &&
                   (int)NETWORK_LADDER_TRIG_UNREACHED == (int)NET_TRIG_S,
               "ladder triggers match the core");
_Static_assert(sizeof(((network_scan_entry_t *)0)->ssid) == sizeof(((net_scan_entry_t *)0)->ssid),
               "scan entries carry the same SSID size");
_Static_assert(CONFIG_NETWORK_RECOVERY_AP_S > 0 && CONFIG_NETWORK_STA_FALLBACK_S > 0, "AP timings");

/* Tasks and queue. */
#define NET_TASK_STACK    5120    /* settings writes when trial credentials are confirmed */
#define NET_TASK_PRIO     6
#define NET_TASK_CORE     0       /* with the Wi-Fi driver; the GNSS path is on core 1 */
#define NET_QUEUE_LEN     24
#define NET_POLL_MS       250
#define PROBE_TASK_STACK  4096
#define PROBE_TASK_PRIO   4
#define DNS_TASK_STACK    3072
#define DNS_TASK_PRIO     4
#define DNS_RECV_TIMEOUT_MS 500
#define DNS_STOP_WAIT_MS  2000

/* Behaviour (PROVISIONAL, to be confirmed on hardware). */
#define AP_LINGER_S          60      /* AP stays this long after the station got its IP */
#define AP_MAX_CLIENTS       4
#define APPLY_DELAY_MS       1000    /* new credentials: let the HTTP reply go out first */
#define DISCONNECT_WAIT_MS   3000    /* deliberate disconnect: connect anyway if no event came */
#define CONNECT_WATCHDOG_MS  60000   /* a connect attempt with no event at all counts as failed */
#define SCAN_WATCHDOG_MS     20000   /* a scan without SCAN_DONE is abandoned (it would block connects) */
#define RSSI_INTERVAL_S      5
#define CHECK_INTERVAL_S     30      /* fallback DNS and SNTP server re-check */
#define PROBE_CONNECT_MS     5000
#define START_WAIT_MS        5000
#define STA_RETRY_ATTEMPT_CAP 64
#define EVENT_POST_TICKS     pdMS_TO_TICKS(20)
#define LADDER_JOURNAL_A     DIAG_LADDER_NETWORK   /* DIAG_EV_LADDER_STEP a */
#define LADDER_OUTCOME_STARTED  DIAG_STEP_STARTED
#define LADDER_OUTCOME_HEALTHY  DIAG_STEP_HEALTHY
#define LADDER_OUTCOME_HELD     DIAG_STEP_FAILED      /* withheld: refused */

#define SNTP_POOL "pool.ntp.org"
#if defined(CONFIG_LWIP_DHCP_GET_NTP_SRV) && CONFIG_LWIP_SNTP_MAX_SERVERS >= 2
#define SNTP_WITH_DHCP 1
#else
#define SNTP_WITH_DHCP 0
#endif

/* ---- messages to the net task ------------------------------------------------------ */

typedef enum {
    MSG_START = 1,
    MSG_STA_START,
    MSG_STA_STOP,
    MSG_STA_CONNECTED,
    MSG_STA_DISCONNECTED,
    MSG_GOT_IP,
    MSG_LOST_IP,
    MSG_AP_START,
    MSG_AP_STOP,
    MSG_AP_CLIENTS,
    MSG_SCAN_DONE,
    MSG_TIME_SYNC,
    MSG_RECOVER,
    MSG_AP_REQUEST,
} msg_type_t;

typedef struct {
    uint8_t type;
    uint8_t reason;     /* disconnect reason, recovery action */
    int8_t rssi;
} net_msg_t;

typedef enum {
    STA_IDLE = 0,       /* not configured, Wi-Fi stopped, or being restarted */
    STA_CONNECTING,     /* esp_wifi_connect() issued */
    STA_CONNECTED,      /* associated (IP or not) */
    STA_WAIT_RETRY,     /* backoff before the next attempt */
    STA_DISCONNECTING,  /* deliberate disconnect, connect again on its event */
} sta_state_t;

/* ---- state -------------------------------------------------------------------------- */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_inited;
static esp_netif_t *s_sta;
static esp_netif_t *s_ap;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_queue_overflow;
static volatile bool s_ap_clients_queued;   /* one MSG_AP_CLIENTS at most in the queue */

/* Readers' snapshot; everything below "under s_lock" is shared with API callers. */
static network_status_t s_st;               /* under s_lock */
static int64_t s_probe_at_us;               /* under s_lock: when the last probe finished */
static bool s_probe_valid;                  /* under s_lock */
static bool s_probe_running;                /* under s_lock */
static bool s_scanning;                     /* under s_lock */
static int64_t s_scan_started_us;           /* under s_lock */
static bool s_wifi_sta_on;                  /* under s_lock: Wi-Fi runs with the station interface */
static size_t s_scan_count;                 /* under s_lock */
EXT_RAM_BSS_ATTR static net_scan_entry_t s_scan[NETWORK_SCAN_MAX];  /* under s_lock */
EXT_RAM_BSS_ATTR static net_scan_entry_t s_scan_tmp[NETWORK_SCAN_MAX]; /* net task */

static char s_new_ssid[33];                 /* under s_lock: credentials waiting to be applied */
static char s_new_pass[65];
static bool s_new_pending;
static bool s_new_trial;                    /* under s_lock: apply them as a trial (pending credentials) */
static int64_t s_apply_at_us;

/* Credentials (under s_lock): the station's stored ones and the pending ones
 * (changed, not confirmed by an IP yet; see network.h). */
static char s_act_ssid[33];
static char s_act_pass[65];
static char s_pend_ssid[33];
static char s_pend_pass[65];
static bool s_pend_valid;
static bool s_cred_reverted;

static esp_err_t s_start_err;               /* MSG_START result */
static SemaphoreHandle_t s_start_done;

/* Net task only. */
static bool s_started;
static bool s_wifi_started;
static sta_state_t s_sta_state;
static uint32_t s_attempt;
static int64_t s_retry_at_us;
static int64_t s_deadline_us;               /* DISCONNECTING / CONNECTING watchdog */
static uint32_t s_wifi_start_fails;          /* esp_wifi_start() failures in a row */
static int64_t s_wifi_retry_at_us;           /* next start attempt after a failure */
static bool s_sta_configured;
static bool s_associated;
static bool s_has_ip;
static bool s_had_link;
static uint32_t s_offline_since_s;
static uint32_t s_ip_since_s;
static uint32_t s_noip_since_s;             /* associated without an IP since (trigger A) */
static bool s_outage;                       /* the address was lost after the station had one (DIAG_EV_WIFI_UP) */
static uint32_t s_outage_attempts;          /* connect attempts since */
static bool s_up_pending;                   /* a DIAG_EV_WIFI_UP record waits to be written */
static int32_t s_up_a;
static int32_t s_up_b;
static net_cred_trial_t s_trial;            /* pending credentials in use, on trial */
static bool s_trial_assoc;                  /* associated since the trial started */
static bool s_trial_revert_ok;              /* stored credentials to go back to */
static char s_ssid[33];
static char s_pass[65];
static uint32_t s_ap_reasons;               /* what the AP is up for (0 = down) */
static bool s_recovery;
static uint32_t s_recovery_start_s;
static bool s_scan_after_start;
static bool s_fallback_dns;
static bool s_sntp_started;
static uint32_t s_last_tick_s;
static uint32_t s_last_rssi_s;
static uint32_t s_last_check_s;
static net_drop_log_t s_drop;
static bool s_drop_pending;
static uint8_t s_drop_reason;
static int8_t s_drop_rssi;
static char s_hostname[33];
static char s_portal_uri[32];
static uint8_t s_ap_addr[4];
static esp_ip4_addr_t s_dns1;
static esp_ip4_addr_t s_dns2;
static bool s_dns1_ok;
static bool s_dns2_ok;

/* DNS responder. */
static TaskHandle_t s_dns_task;             /* net task */
static volatile bool s_dns_stop;
static bool s_dns_restart;                  /* net task: start again once the stopping task is gone */
static SemaphoreHandle_t s_dns_done;
static uint8_t s_dns_rx[CAPTIVE_DNS_MAX_QUERY];
static uint8_t s_dns_tx[CAPTIVE_DNS_MAX_REPLY];

/* Probe. */
static TaskHandle_t s_probe_task;
static EventGroupHandle_t s_probe_events;
#define PROBE_IDLE_BIT BIT0

/* Ladder (network_ladder_tick, under s_lock while deciding). */
static net_ladder_t s_ladder;
static uint8_t s_ladder_note;               /* last journalled skip/withhold: (rung << 4) | why */

/* Survives software, panic and watchdog resets: when the last network-ladder
 * reboot was. Stamped with the image generation (lifecycle). */
static RTC_NOINIT_ATTR net_rtc_t s_rtc;

/* ---- small helpers ------------------------------------------------------------------- */

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void post_event(network_event_t id)
{
    (void)esp_event_post(NETWORK_EVENT, id, NULL, 0, EVENT_POST_TICKS);
}

static void send_msg(uint8_t type, uint8_t reason, int8_t rssi)
{
    net_msg_t m = { .type = type, .reason = reason, .rssi = rssi };
    if (s_queue == NULL || xQueueSend(s_queue, &m, 0) != pdTRUE) {
        s_queue_overflow = true;   /* the net task resyncs from the driver */
        if (type == MSG_AP_CLIENTS) {
            __atomic_store_n(&s_ap_clients_queued, false, __ATOMIC_SEQ_CST);   /* the resync counts the clients */
        }
    }
}

static void set_status_str(char *dst, size_t size, const char *src)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(dst, src, size);
    taskEXIT_CRITICAL(&s_lock);
}

static bool ap_is_up(void)
{
    return s_ap_reasons != 0;
}

/* ---- event handlers (default event loop: forward only) ------------------------------- */

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    switch (id) {
    case WIFI_EVENT_STA_START:
        send_msg(MSG_STA_START, 0, 0);
        break;
    case WIFI_EVENT_STA_STOP:
        send_msg(MSG_STA_STOP, 0, 0);
        break;
    case WIFI_EVENT_STA_CONNECTED:
        send_msg(MSG_STA_CONNECTED, 0, 0);
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        const wifi_event_sta_disconnected_t *d = data;
        send_msg(MSG_STA_DISCONNECTED, d ? d->reason : 0, d ? d->rssi : 0);
        break;
    }
    case WIFI_EVENT_AP_START:
        send_msg(MSG_AP_START, 0, 0);
        break;
    case WIFI_EVENT_AP_STOP:
        send_msg(MSG_AP_STOP, 0, 0);
        break;
    case WIFI_EVENT_AP_STACONNECTED:
    case WIFI_EVENT_AP_STADISCONNECTED:
        /* Any client can produce these at will on an open AP: one message
         * stands for all of them, so they cannot crowd out the others. */
        if (!__atomic_exchange_n(&s_ap_clients_queued, true, __ATOMIC_SEQ_CST)) {
            send_msg(MSG_AP_CLIENTS, 0, 0);
        }
        break;
    case WIFI_EVENT_SCAN_DONE:
        send_msg(MSG_SCAN_DONE, 0, 0);
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == IP_EVENT_STA_GOT_IP) {
        send_msg(MSG_GOT_IP, 0, 0);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        send_msg(MSG_LOST_IP, 0, 0);
    }
}

static void on_sntp_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    send_msg(MSG_TIME_SYNC, 0, 0);
}

/* ---- captive DNS responder ------------------------------------------------------------ */

static void dns_task(void *arg)
{
    (void)arg;
    int sock = -1;
    while (!s_dns_stop) {
        if (sock < 0) {
            sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
            struct sockaddr_in local = { .sin_family = AF_INET, .sin_port = htons(53) };
            memcpy(&local.sin_addr.s_addr, s_ap_addr, 4);
            struct timeval tv = { .tv_sec = 0, .tv_usec = DNS_RECV_TIMEOUT_MS * 1000 };
            if (sock < 0 || bind(sock, (struct sockaddr *)&local, sizeof(local)) != 0 ||
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
                ESP_LOGW(TAG, "captive DNS: socket setup failed (errno %d), retrying", errno);
                if (sock >= 0) {
                    close(sock);
                    sock = -1;
                }
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        struct sockaddr_in from;
        socklen_t from_len = sizeof(from);
        int n = recvfrom(sock, s_dns_rx, sizeof(s_dns_rx), 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0) {
            continue;   /* timeout: check the stop flag */
        }
        size_t len = captive_dns_reply(s_dns_rx, (size_t)n, s_ap_addr, s_dns_tx, sizeof(s_dns_tx));
        if (len > 0) {
            (void)sendto(sock, s_dns_tx, len, 0, (struct sockaddr *)&from, from_len);
        }
    }
    if (sock >= 0) {
        close(sock);
    }
    xSemaphoreGive(s_dns_done);
    vTaskDelete(NULL);
}

/*
 * One DNS task at a time. A task told to stop is forgotten only once it has
 * said it is gone (s_dns_done); a start while it is still on its way out
 * waits for that (s_dns_restart, picked up by housekeeping), so the stop flag
 * of a running task is never cleared under it and no second task binds the
 * port beside it.
 */
static void dns_reap(void)
{
    if (s_dns_task != NULL && s_dns_stop && xSemaphoreTake(s_dns_done, 0) == pdTRUE) {
        s_dns_task = NULL;
    }
}

static void dns_start(void)
{
    dns_reap();
    if (s_dns_task != NULL) {
        if (s_dns_stop) {
            s_dns_restart = true;   /* the previous task is still exiting */
        }
        return;
    }
    s_dns_restart = false;
    s_dns_stop = false;
    if (xTaskCreate(dns_task, "dns", DNS_TASK_STACK, NULL, DNS_TASK_PRIO, &s_dns_task) != pdPASS) {
        s_dns_task = NULL;
        ESP_LOGE(TAG, "captive DNS: task not started");
    }
}

static void dns_stop(void)
{
    s_dns_restart = false;
    if (s_dns_task == NULL || s_dns_stop) {
        dns_reap();
        return;
    }
    s_dns_stop = true;
    if (xSemaphoreTake(s_dns_done, pdMS_TO_TICKS(DNS_STOP_WAIT_MS)) == pdTRUE) {
        s_dns_task = NULL;
    } else {
        ESP_LOGW(TAG, "captive DNS: task still stopping");
    }
}

/* ---- upstream probe ------------------------------------------------------------------ */

/* TCP connect within PROBE_CONNECT_MS. A refused connection also proves the
 * remote host answered. */
static bool tcp_reachable(const struct sockaddr_in *sa)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        return false;
    }
    bool ok = false;
    int flags = fcntl(s, F_GETFL, 0);
    if (flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0) {
        int r = connect(s, (const struct sockaddr *)sa, sizeof(*sa));
        if (r == 0) {
            ok = true;
        } else if (errno == EINPROGRESS) {
            fd_set wset;
            FD_ZERO(&wset);
            FD_SET(s, &wset);
            struct timeval tv = { .tv_sec = PROBE_CONNECT_MS / 1000, .tv_usec = (PROBE_CONNECT_MS % 1000) * 1000 };
            if (select(s + 1, NULL, &wset, NULL, &tv) > 0) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) == 0) {
                    ok = (err == 0 || err == ECONNREFUSED);
                }
            }
        } else if (errno == ECONNREFUSED) {
            ok = true;
        }
    }
    close(s);
    return ok;
}

static bool resolver_reachable(const esp_ip4_addr_t *ip, bool valid)
{
    if (!valid) {
        return false;
    }
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons(53) };
    sa.sin_addr.s_addr = ip->addr;
    return tcp_reachable(&sa);
}

static network_probe_t run_probe(void)
{
    if (!network_is_online()) {
        return NETWORK_PROBE_NO_IP;
    }
    char port[8];
    snprintf(port, sizeof(port), "%d", CONFIG_NETWORK_PROBE_PORT);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(CONFIG_NETWORK_PROBE_HOST, port, &hints, &res);
    if (rc == 0 && res != NULL && res->ai_addr != NULL) {
        bool ok = tcp_reachable((const struct sockaddr_in *)res->ai_addr);
        freeaddrinfo(res);
        if (ok || resolver_reachable(&s_dns1, s_dns1_ok) || resolver_reachable(&s_dns2, s_dns2_ok)) {
            return NETWORK_PROBE_OK;   /* the internet answers (the probe host alone may be down) */
        }
        return network_is_online() ? NETWORK_PROBE_NO_UPSTREAM : NETWORK_PROBE_NO_IP;
    }
    if (res != NULL) {
        freeaddrinfo(res);
    }
    if (!network_is_online()) {
        return NETWORK_PROBE_NO_IP;
    }
    if (resolver_reachable(&s_dns1, s_dns1_ok) || resolver_reachable(&s_dns2, s_dns2_ok)) {
        return NETWORK_PROBE_DNS_FAILED;
    }
    return NETWORK_PROBE_NO_UPSTREAM;
}

static const char *probe_name(network_probe_t p)
{
    switch (p) {
    case NETWORK_PROBE_OK:
        return "upstream reachable";
    case NETWORK_PROBE_NO_IP:
        return "no IP";
    case NETWORK_PROBE_DNS_FAILED:
        return "name lookup failed, internet reachable";
    case NETWORK_PROBE_NO_UPSTREAM:
        return "IP but no upstream";
    default:
        return "none";
    }
}

static void probe_task(void *arg)
{
    (void)arg;
    network_probe_t last = NETWORK_PROBE_NONE;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        network_probe_t r = run_probe();
        taskENTER_CRITICAL(&s_lock);
        s_st.probe = r;
        s_st.upstream_ok = (r == NETWORK_PROBE_OK);
        s_probe_at_us = esp_timer_get_time();
        s_probe_valid = true;
        s_probe_running = false;
        taskEXIT_CRITICAL(&s_lock);
        xEventGroupSetBits(s_probe_events, PROBE_IDLE_BIT);
        if (r != last) {
            ESP_LOGI(TAG, "upstream probe: %s", probe_name(r));
            last = r;
        }
    }
}

/* Start a probe unless one runs. */
static void probe_kick(void)
{
    bool start = false;
    taskENTER_CRITICAL(&s_lock);
    if (!s_probe_running) {
        s_probe_running = true;
        start = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (start) {
        xEventGroupClearBits(s_probe_events, PROBE_IDLE_BIT);
        xTaskNotifyGive(s_probe_task);
    }
}

/* ---- station ------------------------------------------------------------------------- */

static bool scanning(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool s = s_scanning;
    taskEXIT_CRITICAL(&s_lock);
    return s;
}

static void set_wifi_sta_on(bool on)
{
    taskENTER_CRITICAL(&s_lock);
    s_wifi_sta_on = on;
    taskEXIT_CRITICAL(&s_lock);
}

static void build_sta_config(wifi_config_t *c)
{
    memset(c, 0, sizeof(*c));
    memcpy(c->sta.ssid, s_ssid, strnlen(s_ssid, sizeof(c->sta.ssid)));
    memcpy(c->sta.password, s_pass, strnlen(s_pass, sizeof(c->sta.password)));
    c->sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c->sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    /* With a password, never join an open or WEP network of the same name. */
    c->sta.threshold.authmode = (s_pass[0] != '\0') ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    c->sta.threshold.rssi = -127;
    c->sta.failure_retry_cnt = 2;   /* then try the next access point of the same network */
    c->sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
}

/* The station lost its address (link down, DHCP restart, Wi-Fi restart, IP lost). */
static void ip_lost(void)
{
    if (!s_has_ip) {
        return;
    }
    s_has_ip = false;
    s_offline_since_s = now_s();
    s_noip_since_s = s_offline_since_s;   /* counts only while still associated */
    s_outage = true;
    s_outage_attempts = 0;
    taskENTER_CRITICAL(&s_lock);
    s_st.online = false;
    s_st.ip[0] = '\0';
    taskEXIT_CRITICAL(&s_lock);
    post_event(NETWORK_EVENT_OFFLINE);
}

static void schedule_retry(void)
{
    uint32_t delay_ms = net_backoff_ms(s_attempt);
    if (s_attempt < STA_RETRY_ATTEMPT_CAP) {
        s_attempt++;
    }
    s_retry_at_us = esp_timer_get_time() + (int64_t)delay_ms * 1000;
    s_sta_state = STA_WAIT_RETRY;
}

static void connect_now(void)
{
    if (!s_sta_configured || !s_wifi_started) {
        s_sta_state = STA_IDLE;
        return;
    }
    if (scanning()) {
        /* esp_wifi_connect() fails during a scan: try again once it is done. */
        s_sta_state = STA_WAIT_RETRY;
        s_retry_at_us = esp_timer_get_time();
        return;
    }
    esp_err_t err = esp_wifi_connect();
    if (err == ESP_OK) {
        if (s_outage && s_outage_attempts < UINT32_MAX) {
            s_outage_attempts++;
        }
        s_sta_state = STA_CONNECTING;
        s_deadline_us = esp_timer_get_time() + (int64_t)CONNECT_WATCHDOG_MS * 1000;
    } else {
        ESP_LOGW(TAG, "connect: %s", esp_err_to_name(err));
        schedule_retry();
    }
}

static void disconnect_then_connect(void)
{
    s_sta_state = STA_DISCONNECTING;
    s_deadline_us = esp_timer_get_time() + (int64_t)DISCONNECT_WAIT_MS * 1000;
    esp_err_t err = esp_wifi_disconnect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "disconnect: %s", esp_err_to_name(err));
        connect_now();
    }
}

static esp_err_t wifi_start(void)
{
    esp_err_t err = esp_wifi_start();
    if (err != ESP_OK) {
        /* Retried from housekeeping (station) or the AP policy, with backoff. */
        s_wifi_retry_at_us = esp_timer_get_time() + (int64_t)net_backoff_ms(s_wifi_start_fails) * 1000;
        if (s_wifi_start_fails < UINT32_MAX) {
            s_wifi_start_fails++;
        }
        ESP_LOGE(TAG, "esp_wifi_start: %s (retry in %u ms)", esp_err_to_name(err),
                 (unsigned)net_backoff_ms(s_wifi_start_fails - 1));
        return err;
    }
    s_wifi_start_fails = 0;
    s_wifi_started = true;
    /* Power save off after every start: no DTIM latency, no stalls. */
    err = esp_wifi_set_ps(WIFI_PS_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power save off: %s", esp_err_to_name(err));
    }
    wifi_mode_t mode = WIFI_MODE_NULL;
    (void)esp_wifi_get_mode(&mode);
    set_wifi_sta_on(mode == WIFI_MODE_STA || mode == WIFI_MODE_APSTA);
    return ESP_OK;
}

static void wifi_stop(void)
{
    s_sta_state = STA_IDLE;
    set_wifi_sta_on(false);
    taskENTER_CRITICAL(&s_lock);
    s_scanning = false;   /* a stop ends any scan */
    taskEXIT_CRITICAL(&s_lock);
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    s_wifi_started = false;
    s_associated = false;
    s_ap_reasons = 0;   /* the AP stops with Wi-Fi: the policy brings it back if still wanted */
    taskENTER_CRITICAL(&s_lock);
    s_st.associated = false;
    s_st.ap_reasons = 0;
    taskEXIT_CRITICAL(&s_lock);
    ip_lost();
}

/* Wi-Fi may be (re)started now: not running, and no failed start waiting out its backoff. */
static bool wifi_start_due(void)
{
    return !s_wifi_started && (s_wifi_start_fails == 0 || esp_timer_get_time() >= s_wifi_retry_at_us);
}

/* Start Wi-Fi for the configured station (MSG_STA_START connects). */
static void start_station(void)
{
    wifi_config_t cfg;
    build_sta_config(&cfg);
    esp_err_t err = esp_wifi_set_mode(ap_is_up() ? WIFI_MODE_APSTA : WIFI_MODE_STA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    memset(&cfg, 0, sizeof(cfg));
    if (err == ESP_OK) {
        (void)wifi_start();   /* a failure schedules the next try */
        return;
    }
    ESP_LOGE(TAG, "station config: %s", esp_err_to_name(err));
    s_wifi_retry_at_us = esp_timer_get_time() + (int64_t)net_backoff_ms(s_wifi_start_fails) * 1000;
    if (s_wifi_start_fails < UINT32_MAX) {
        s_wifi_start_fails++;
    }
}

/* Use the credentials in s_ssid/s_pass from now on. */
static void apply_credentials(void)
{
    s_sta_configured = net_ssid_valid(s_ssid);
    taskENTER_CRITICAL(&s_lock);
    s_st.sta_configured = s_sta_configured;
    s_st.sta_credentials_unreadable = false;
    strlcpy(s_st.ssid, s_ssid, sizeof(s_st.ssid));
    taskEXIT_CRITICAL(&s_lock);
    if (!s_sta_configured || !s_started) {
        return;
    }
    s_attempt = 0;
    net_drop_log_reset(&s_drop);
    bool was_linked = (s_sta_state == STA_CONNECTED || s_sta_state == STA_CONNECTING);
    if (was_linked && s_wifi_started) {
        (void)esp_wifi_disconnect();
    }
    wifi_mode_t mode = ap_is_up() ? WIFI_MODE_APSTA : WIFI_MODE_STA;
    esp_err_t err = esp_wifi_set_mode(mode);
    wifi_config_t cfg;
    build_sta_config(&cfg);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    }
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "station config: %s", esp_err_to_name(err));
        return;
    }
    ESP_LOGI(TAG, "station credentials for \"%s\" applied", s_ssid);
    if (!s_wifi_started) {
        if (wifi_start() == ESP_OK) {
            /* MSG_STA_START connects */
            s_sta_state = STA_IDLE;
        }
        return;
    }
    if (was_linked) {
        s_sta_state = STA_DISCONNECTING;
        s_deadline_us = esp_timer_get_time() + (int64_t)DISCONNECT_WAIT_MS * 1000;
    } else {
        connect_now();
    }
}

/* ---- fallback DNS and SNTP servers ---------------------------------------------------- */

static bool dns_is(esp_netif_dns_type_t type, const esp_ip4_addr_t *want)
{
    esp_netif_dns_info_t d;
    if (esp_netif_get_dns_info(s_sta, type, &d) != ESP_OK || d.ip.type != ESP_IPADDR_TYPE_V4) {
        return false;
    }
    return d.ip.u_addr.ip4.addr == want->addr;
}

static void set_dns(esp_netif_dns_type_t type, const esp_ip4_addr_t *ip)
{
    esp_netif_dns_info_t d = { 0 };
    d.ip.type = ESP_IPADDR_TYPE_V4;
    d.ip.u_addr.ip4 = *ip;
    esp_err_t err = esp_netif_set_dns_info(s_sta, type, &d);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set DNS server: %s", esp_err_to_name(err));
    }
}

/* DHCP (initial lease and every renewal) overwrites the servers, so this
 * runs after each new IP and every CHECK_INTERVAL_S. */
static void ensure_fallback_dns(void)
{
    if (!s_fallback_dns || !s_has_ip) {
        return;
    }
    bool changed = false;
    if (s_dns1_ok && !dns_is(ESP_NETIF_DNS_MAIN, &s_dns1)) {
        set_dns(ESP_NETIF_DNS_MAIN, &s_dns1);
        changed = true;
    }
    if (s_dns2_ok && !dns_is(ESP_NETIF_DNS_BACKUP, &s_dns2)) {
        set_dns(ESP_NETIF_DNS_BACKUP, &s_dns2);
        changed = true;
    }
    if (changed) {
        ESP_LOGI(TAG, "DNS: fallback servers %s, %s", CONFIG_NETWORK_FALLBACK_DNS1, CONFIG_NETWORK_FALLBACK_DNS2);
    }
}

/* A DHCP-provided NTP server replaces the whole server list on every lease
 * renewal; put the pool back behind it. */
static void ensure_sntp_servers(void)
{
#if SNTP_WITH_DHCP
    if (s_sntp_started && esp_sntp_getservername(1) == NULL) {
        esp_sntp_setservername(1, SNTP_POOL);
    }
#endif
}

static void sntp_setup(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG(SNTP_POOL);
    cfg.start = false;
    cfg.wait_for_sync = false;
#if SNTP_WITH_DHCP
    /* DHCP's server (if any) takes index 0; the pool is restored at index 1
     * after every new IP (and by ensure_sntp_servers after renewals). */
    cfg.server_from_dhcp = true;
    cfg.renew_servers_after_new_IP = true;
    cfg.ip_event_to_renew = IP_EVENT_STA_GOT_IP;
    cfg.index_of_first_server = 1;
#endif
    esp_err_t err = esp_netif_sntp_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SNTP init: %s", esp_err_to_name(err));
    }
}

/* ---- the AP ------------------------------------------------------------------------------- */

static uint8_t ap_client_count(void)
{
    if (!ap_is_up()) {
        return 0;
    }
    wifi_sta_list_t list;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return 0;
    }
    return (uint8_t)list.num;
}

static void build_ap_config(wifi_config_t *c)
{
    memset(c, 0, sizeof(*c));
    size_t len = strnlen(s_st.ap_ssid, sizeof(c->ap.ssid));
    memcpy(c->ap.ssid, s_st.ap_ssid, len);
    c->ap.ssid_len = (uint8_t)len;
    c->ap.channel = (s_associated && s_st.channel != 0) ? s_st.channel : CONFIG_NETWORK_AP_CHANNEL;
    c->ap.max_connection = AP_MAX_CLIENTS;
    c->ap.authmode = WIFI_AUTH_OPEN;

    char pass[65] = "";
    esp_err_t err = settings_is_available()
                        ? settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_AP_PASS, pass, sizeof(pass))
                        : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "setup AP password not readable (%s): AP is open", esp_err_to_name(err));
    }
    if (err == ESP_OK && pass[0] != '\0') {
        if (net_ap_password_valid(pass)) {
            memcpy(c->ap.password, pass, strlen(pass));
            c->ap.authmode = WIFI_AUTH_WPA2_PSK;
            c->ap.pairwise_cipher = WIFI_CIPHER_TYPE_CCMP;
        } else {
            ESP_LOGE(TAG, "setup AP password unusable (8-63 characters needed): AP stays open");
        }
    }
    memset(pass, 0, sizeof(pass));
}

static void ap_up(uint32_t reasons)
{
    if (!s_wifi_started && !wifi_start_due()) {
        return;   /* a failed start waits out its backoff */
    }
    wifi_config_t cfg;
    build_ap_config(&cfg);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    if (err == ESP_OK && !s_wifi_started) {
        err = wifi_start();
    } else if (err == ESP_OK) {
        set_wifi_sta_on(true);
    }
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "setup AP: %s", esp_err_to_name(err));
        return;
    }
    s_ap_reasons = reasons;
    ESP_LOGW(TAG, "AP \"%s\" up (%s%s%s%s)", s_st.ap_ssid, (reasons & NET_AP_SETUP) ? "setup" : "",
             (reasons & NET_AP_FALLBACK) ? " offline fallback" : "", (reasons & NET_AP_LINGER) ? " linger" : "",
             (reasons & NET_AP_RECOVERY) ? " recovery" : "");
}

static void ap_down(void)
{
    s_ap_reasons = 0;
    if (s_sta_configured) {
        esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "AP off: %s", esp_err_to_name(err));
        }
    } else {
        wifi_stop();   /* nothing left to run */
    }
    ESP_LOGI(TAG, "AP down");
}

static void apply_ap_policy(void)
{
    uint32_t now = now_s();
    net_ap_input_t in = {
        .settings_available = settings_is_available(),
        .sta_configured = s_sta_configured,
        .has_ip = s_has_ip,
        .offline_s = now - s_offline_since_s,
        .since_ip_s = now - s_ip_since_s,
        .reasons_now = s_ap_reasons,
        .recovery_requested = s_recovery,
        .recovery_age_s = now - s_recovery_start_s,
        .ap_clients = ap_client_count(),
        .fallback_after_s = CONFIG_NETWORK_STA_FALLBACK_S,
        .recovery_lifetime_s = CONFIG_NETWORK_RECOVERY_AP_S,
        .linger_s = AP_LINGER_S,
    };
    bool expired = false;
    uint32_t want = net_ap_reasons(&in, &expired);
    if (expired) {
        s_recovery = false;
        ESP_LOGI(TAG, "recovery AP lifetime over");
    }
    if (want != 0 && !ap_is_up()) {
        ap_up(want);
    } else if (want == 0 && ap_is_up()) {
        ap_down();
    } else if (want != 0) {
        s_ap_reasons = want;
    }
    uint32_t left = 0;
    if (s_recovery && in.recovery_age_s < CONFIG_NETWORK_RECOVERY_AP_S) {
        left = CONFIG_NETWORK_RECOVERY_AP_S - in.recovery_age_s;
    }
    taskENTER_CRITICAL(&s_lock);
    s_st.ap_reasons = (uint8_t)s_ap_reasons;
    s_st.ap_clients = (uint8_t)in.ap_clients;
    s_st.recovery_ap_left_s = left;
    taskEXIT_CRITICAL(&s_lock);
}

/* ---- journal ---------------------------------------------------------------------------- */

/* DIAG_EV_WIFI_UP: kept until the journal takes it (its per-kind limit), dropped without a journal. */
static void flush_up_record(void)
{
    if (!s_up_pending) {
        return;
    }
    esp_err_t err = diag_journal_append(DIAG_EV_WIFI_UP, s_up_a, s_up_b);
    if (err != ESP_ERR_INVALID_STATE) {
        s_up_pending = false;
    }
}

static void flush_drop_record(void)
{
    if (!s_drop_pending) {
        return;
    }
    esp_err_t err = diag_journal_append(DIAG_EV_WIFI_DISCONNECT, s_drop_reason, s_drop_rssi);
    if (err != ESP_ERR_INVALID_STATE) {
        s_drop_pending = false;   /* written, or the journal is unavailable: nothing to keep */
    }
}

/* ---- message handling ---------------------------------------------------------------- */

static void start_scan_internal(void)
{
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    taskENTER_CRITICAL(&s_lock);
    bool busy = s_scanning;
    s_scanning = true;
    s_scan_started_us = esp_timer_get_time();
    taskEXIT_CRITICAL(&s_lock);
    if (busy) {
        return;
    }
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_scanning = false;
        taskEXIT_CRITICAL(&s_lock);
    }
}

static void handle_scan_done(void)
{
    if (!scanning()) {
        (void)esp_wifi_clear_ap_list();   /* not a scan of ours: keep the cache */
        return;
    }
    size_t count = 0;
    wifi_ap_record_t rec;
    while (esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {
        net_scan_entry_t e = { .rssi = rec.rssi, .channel = rec.primary, .secured = rec.authmode != WIFI_AUTH_OPEN };
        memcpy(e.ssid, rec.ssid, sizeof(e.ssid) - 1);
        e.ssid[sizeof(e.ssid) - 1] = '\0';
        count = net_scan_insert(s_scan_tmp, count, NETWORK_SCAN_MAX, &e);
    }
    (void)esp_wifi_clear_ap_list();
    taskENTER_CRITICAL(&s_lock);
    memcpy(s_scan, s_scan_tmp, count * sizeof(s_scan[0]));
    s_scan_count = count;
    s_scanning = false;
    taskEXIT_CRITICAL(&s_lock);
    post_event(NETWORK_EVENT_SCAN_DONE);
}

static void handle_disconnected(uint8_t reason, int8_t rssi)
{
    bool was_linked = s_associated;
    s_associated = false;
    s_trial_assoc = false;   /* only the next association can confirm trial credentials */
    taskENTER_CRITICAL(&s_lock);
    s_st.associated = false;
    s_st.last_disconnect_reason = reason;
    taskEXIT_CRITICAL(&s_lock);
    ip_lost();   /* IDF's default handler has already cleared the address */

    if (s_sta_state == STA_DISCONNECTING) {
        connect_now();   /* deliberate: no backoff */
        return;
    }
    if (s_sta_state == STA_IDLE || !s_sta_configured) {
        return;          /* stopping, restarting or unconfigured */
    }
    if (!was_linked) {
        taskENTER_CRITICAL(&s_lock);
        s_st.connect_failures++;
        taskEXIT_CRITICAL(&s_lock);
    }
    if (net_drop_log_should(&s_drop, was_linked, reason, now_s())) {
        if (was_linked) {
            ESP_LOGW(TAG, "link lost: reason %u, RSSI %d dBm", reason, rssi);
        } else {
            ESP_LOGI(TAG, "connect failed: reason %u", reason);
        }
        if (!s_drop_pending) {
            s_drop_pending = true;
            s_drop_reason = reason;
            s_drop_rssi = rssi;
        }
        flush_drop_record();
    }
    schedule_retry();
}

/* ---- credentials on trial ------------------------------------------------------------- */

static esp_err_t store_pair(const char *key_ssid, const char *key_pass, const char *ssid, const char *pass)
{
    settings_batch_t b;
    esp_err_t err = settings_batch_begin(SETTINGS_NS_WIFI, &b);
    if (err != ESP_OK) {
        return err;
    }
    err = settings_batch_set_str(b, key_ssid, ssid);
    if (err == ESP_OK) {
        err = settings_batch_set_str(b, key_pass, pass);
    }
    if (err != ESP_OK) {
        settings_batch_abort(b);
        return err;
    }
    return settings_batch_commit(b);
}

static esp_err_t erase_pending_keys(void)
{
    esp_err_t e1 = settings_erase_key(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PEND_SSID);
    esp_err_t e2 = settings_erase_key(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PEND_PASS);
    return e1 != ESP_OK ? e1 : e2;
}

/* The pending credentials got an IP: they become the station's. The new
 * pair is written before the pending one is erased, so a reset in between
 * only tries them first once more. */
static void trial_confirmed(void)
{
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_act_ssid, s_ssid, sizeof(s_act_ssid));
    strlcpy(s_act_pass, s_pass, sizeof(s_act_pass));
    memset(s_pend_pass, 0, sizeof(s_pend_pass));
    s_pend_ssid[0] = '\0';
    s_pend_valid = false;
    s_cred_reverted = false;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "new Wi-Fi credentials for \"%s\" confirmed", s_ssid);
    if (!settings_is_available()) {
        return;
    }
    esp_err_t err = store_pair(SETTINGS_KEY_WIFI_SSID, SETTINGS_KEY_WIFI_PASS, s_ssid, s_pass);
    if (err == ESP_OK) {
        err = erase_pending_keys();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "confirmed Wi-Fi credentials not stored: %s", esp_err_to_name(err));
    }
}

/* No IP in time with the pending credentials: back to the stored ones. */
static void trial_revert(void)
{
    if (!s_trial_revert_ok) {
        ESP_LOGW(TAG, "Wi-Fi credentials for \"%s\" got no IP in %u s; no previous credentials, still trying",
                 s_ssid, (unsigned)NET_CRED_TRIAL_S);
        return;
    }
    char tried[33];
    strlcpy(tried, s_ssid, sizeof(tried));
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_ssid, s_act_ssid, sizeof(s_ssid));
    strlcpy(s_pass, s_act_pass, sizeof(s_pass));
    s_cred_reverted = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "Wi-Fi credentials for \"%s\" got no IP in %u s: back to \"%s\" (the new ones stay pending)",
             tried, (unsigned)NET_CRED_TRIAL_S, s_ssid);
    apply_credentials();
}

static void handle_got_ip(void)
{
    esp_netif_ip_info_t info;
    char ip[16] = "";
    if (esp_netif_get_ip_info(s_sta, &info) == ESP_OK) {
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&info.ip));
    }
    bool was = s_has_ip;
    s_has_ip = true;
    s_ip_since_s = now_s();
    s_attempt = 0;
    if (s_outage && !was) {
        /* The address is back (not the first one of this boot). */
        s_outage = false;
        s_up_pending = true;
        s_up_a = (int32_t)(s_outage_attempts > INT32_MAX ? INT32_MAX : s_outage_attempts);
        uint32_t out_s = s_ip_since_s - s_offline_since_s;
        s_up_b = (int32_t)(out_s > INT32_MAX ? INT32_MAX : out_s);
        flush_up_record();
    }
    /* Only an IP on a link the trial credentials made confirms them (an
     * event of the previous link can still arrive after the switch); the
     * timeout is left to housekeeping. */
    if (s_trial.on && s_trial_assoc && net_cred_trial_tick(&s_trial, true, s_ip_since_s) == NET_TRIAL_CONFIRMED) {
        trial_confirmed();
    }
    net_drop_log_reset(&s_drop);
    set_status_str(s_st.ip, sizeof(s_st.ip), ip);
    taskENTER_CRITICAL(&s_lock);
    s_st.online = true;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "online: %s", ip);
    ensure_fallback_dns();
    if (!s_sntp_started) {
        esp_err_t err = esp_netif_sntp_start();
        if (err == ESP_OK) {
            s_sntp_started = true;
        } else {
            ESP_LOGW(TAG, "SNTP start: %s", esp_err_to_name(err));
        }
    }
    probe_kick();
    if (!was) {
        post_event(NETWORK_EVENT_ONLINE);
    }
}

static void do_recovery(network_recovery_t action)
{
    /* A restart also helps when Wi-Fi did not start: it tries once more now. */
    if (!s_sta_configured || (!s_wifi_started && action != NETWORK_RECOVER_RESTART_WIFI)) {
        ESP_LOGW(TAG, "recovery %d skipped: station not running", (int)action);
        return;
    }
    switch (action) {
    case NETWORK_RECOVER_RECONNECT:
        ESP_LOGW(TAG, "recovery: reconnect");
        s_attempt = 0;
        if (s_sta_state == STA_CONNECTED) {
            disconnect_then_connect();
        } else if (s_sta_state == STA_WAIT_RETRY || s_sta_state == STA_IDLE) {
            connect_now();
        }
        break;
    case NETWORK_RECOVER_RENEW_DHCP:
        ESP_LOGW(TAG, "recovery: renew DHCP, fallback DNS servers");
        s_fallback_dns = s_dns1_ok || s_dns2_ok;
        taskENTER_CRITICAL(&s_lock);
        s_st.fallback_dns = s_fallback_dns;
        taskEXIT_CRITICAL(&s_lock);
        if (s_sta_state == STA_CONNECTED) {
            esp_err_t err = esp_netif_dhcpc_stop(s_sta);
            if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                ESP_LOGW(TAG, "DHCP stop: %s", esp_err_to_name(err));
            }
            ip_lost();
            err = esp_netif_dhcpc_start(s_sta);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "DHCP start: %s", esp_err_to_name(err));
                disconnect_then_connect();
            }
        } else {
            s_attempt = 0;
            if (s_sta_state == STA_WAIT_RETRY || s_sta_state == STA_IDLE) {
                connect_now();
            }
        }
        break;
    case NETWORK_RECOVER_RESTART_WIFI:
        ESP_LOGW(TAG, "recovery: restart Wi-Fi");
        wifi_stop();   /* the AP goes down with it */
        s_attempt = 0;
        s_wifi_start_fails = 0;   /* an explicit restart tries at once */
        if (esp_wifi_set_mode(WIFI_MODE_STA) == ESP_OK) {
            (void)wifi_start();   /* MSG_STA_START connects; a failed start is retried */
        }
        apply_ap_policy();   /* the AP again, if it is still wanted */
        break;
    default:
        break;
    }
}

static esp_err_t handle_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    s_started = true;
    s_offline_since_s = now_s();
    if (settings_is_available()) {
        char ssid[33] = "";
        char pass[65] = "";
        bool unreadable = false;
        esp_err_t e1 = settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_SSID, ssid, sizeof(ssid));
        esp_err_t e2 = settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PASS, pass, sizeof(pass));
        switch (net_sta_credentials(e1 == ESP_OK, ssid, e2 == ESP_OK, pass)) {
        case NET_CRED_OK:
            strlcpy(s_ssid, ssid, sizeof(s_ssid));
            strlcpy(s_pass, pass, sizeof(s_pass));
            break;
        case NET_CRED_UNREADABLE:
            /* Never an open network in place of the stored one. */
            unreadable = true;
            ESP_LOGE(TAG, "Wi-Fi password for \"%s\" unreadable (%s)", ssid,
                     esp_err_to_name(e2 != ESP_OK ? e2 : ESP_ERR_INVALID_SIZE));
            break;
        case NET_CRED_NONE:
        default:
            break;
        }
        /* Pending credentials (changed, not confirmed yet) are tried first. */
        char pssid[33] = "";
        e1 = settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PEND_SSID, pssid, sizeof(pssid));
        e2 = settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PEND_PASS, pass, sizeof(pass));
        bool pend = net_sta_credentials(e1 == ESP_OK, pssid, e2 == ESP_OK, pass) == NET_CRED_OK;
        taskENTER_CRITICAL(&s_lock);
        strlcpy(s_act_ssid, s_ssid, sizeof(s_act_ssid));
        strlcpy(s_act_pass, s_pass, sizeof(s_act_pass));
        if (pend) {
            strlcpy(s_pend_ssid, pssid, sizeof(s_pend_ssid));
            strlcpy(s_pend_pass, pass, sizeof(s_pend_pass));
            s_pend_valid = true;
        }
        s_st.sta_credentials_unreadable = unreadable && !pend;
        taskEXIT_CRITICAL(&s_lock);
        if (pend) {
            s_trial_revert_ok = net_ssid_valid(s_ssid);
            ESP_LOGI(TAG, "pending Wi-Fi credentials for \"%s\" are tried first (%u s)%s", pssid,
                     (unsigned)NET_CRED_TRIAL_S, s_trial_revert_ok ? "" : "; no other credentials stored");
            strlcpy(s_ssid, pssid, sizeof(s_ssid));
            strlcpy(s_pass, pass, sizeof(s_pass));
            net_cred_trial_start(&s_trial, now_s());
            s_trial_assoc = false;
        } else if (unreadable) {
            ESP_LOGE(TAG, "station not started: enter the Wi-Fi credentials again on the setup page");
        }
        memset(pass, 0, sizeof(pass));
    } else {
        ESP_LOGW(TAG, "settings unavailable: no AP opens on its own (hold BOOT for the recovery AP)");
    }
    s_sta_configured = net_ssid_valid(s_ssid);
    taskENTER_CRITICAL(&s_lock);
    s_st.sta_configured = s_sta_configured;
    strlcpy(s_st.ssid, s_ssid, sizeof(s_st.ssid));
    taskEXIT_CRITICAL(&s_lock);

    esp_err_t err = ESP_OK;
    if (s_sta_configured) {
        wifi_config_t cfg;
        build_sta_config(&cfg);
        err = esp_wifi_set_mode(ap_is_up() ? WIFI_MODE_APSTA : WIFI_MODE_STA);
        if (err == ESP_OK) {
            err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        }
        memset(&cfg, 0, sizeof(cfg));
        if (err == ESP_OK && !s_wifi_started) {
            err = wifi_start();   /* MSG_STA_START connects */
        } else if (err == ESP_OK) {
            connect_now();        /* already running (recovery AP opened first) */
        }
        ESP_LOGI(TAG, "station: connecting to \"%s\"", s_ssid);
    } else if (settings_is_available()) {
        s_scan_after_start = true;   /* fill the setup page's network list */
        ESP_LOGI(TAG, "no station credentials: setup AP");
    }
    apply_ap_policy();
    return err;
}

static void handle_msg(const net_msg_t *m)
{
    switch (m->type) {
    case MSG_START:
        s_start_err = handle_start();
        xSemaphoreGive(s_start_done);
        break;
    case MSG_STA_START:
        set_wifi_sta_on(true);
        if (s_scan_after_start && !s_sta_configured) {
            s_scan_after_start = false;
            start_scan_internal();
        }
        if (s_sta_configured && (s_sta_state == STA_IDLE || s_sta_state == STA_WAIT_RETRY)) {
            connect_now();
        }
        break;
    case MSG_STA_STOP:
        set_wifi_sta_on(false);
        break;
    case MSG_STA_CONNECTED: {
        s_associated = true;
        s_sta_state = STA_CONNECTED;
        if (!s_has_ip) {
            s_noip_since_s = now_s();
        }
        if (s_trial.on) {
            s_trial_assoc = true;   /* a link the trial credentials made */
        }
        if (s_had_link) {
            taskENTER_CRITICAL(&s_lock);
            s_st.reconnects++;
            taskEXIT_CRITICAL(&s_lock);
        }
        s_had_link = true;
        wifi_ap_record_t ap;
        bool have = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        taskENTER_CRITICAL(&s_lock);
        s_st.associated = true;
        if (have) {
            s_st.rssi = ap.rssi;
            s_st.channel = ap.primary;
        }
        taskEXIT_CRITICAL(&s_lock);
        break;
    }
    case MSG_STA_DISCONNECTED:
        handle_disconnected(m->reason, m->rssi);
        break;
    case MSG_GOT_IP:
        handle_got_ip();
        break;
    case MSG_LOST_IP:
        ip_lost();
        break;
    case MSG_AP_START:
        dns_start();
        taskENTER_CRITICAL(&s_lock);
        s_st.setup_ap_active = true;
        taskEXIT_CRITICAL(&s_lock);
        post_event(NETWORK_EVENT_SETUP_AP_UP);
        break;
    case MSG_AP_STOP:
        dns_stop();
        taskENTER_CRITICAL(&s_lock);
        s_st.setup_ap_active = false;
        s_st.ap_clients = 0;
        taskEXIT_CRITICAL(&s_lock);
        post_event(NETWORK_EVENT_SETUP_AP_DOWN);
        break;
    case MSG_AP_CLIENTS: {
        /* Cleared before the count is read: a later event queues again. */
        __atomic_store_n(&s_ap_clients_queued, false, __ATOMIC_SEQ_CST);
        uint8_t n = ap_client_count();
        taskENTER_CRITICAL(&s_lock);
        s_st.ap_clients = n;
        taskEXIT_CRITICAL(&s_lock);
        break;
    }
    case MSG_SCAN_DONE:
        handle_scan_done();
        break;
    case MSG_TIME_SYNC:
        taskENTER_CRITICAL(&s_lock);
        s_st.time_synced = true;
        taskEXIT_CRITICAL(&s_lock);
        /* The availability ledger's fallback clock (it prefers GNSS time). */
        ledger_set_utc((int64_t)time(NULL), LEDGER_CLOCK_SNTP);
        post_event(NETWORK_EVENT_TIME_SYNCED);
        break;
    case MSG_RECOVER:
        do_recovery((network_recovery_t)m->reason);
        break;
    case MSG_AP_REQUEST:
        s_recovery = true;
        s_recovery_start_s = now_s();
        ESP_LOGW(TAG, "%s AP requested for %d s", m->reason ? "recovery" : "setup", CONFIG_NETWORK_RECOVERY_AP_S);
        apply_ap_policy();
        break;
    default:
        break;
    }
}

/* Some events were lost (full queue): read the truth back from the driver. */
static void resync(void)
{
    s_queue_overflow = false;
    ESP_LOGW(TAG, "event queue overflow: resyncing");
    wifi_ap_record_t ap;
    bool linked = s_wifi_started && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    esp_netif_ip_info_t info = { 0 };
    bool ip = linked && esp_netif_get_ip_info(s_sta, &info) == ESP_OK && info.ip.addr != 0;
    if (!linked && s_associated) {
        handle_disconnected(0, 0);
    } else if (linked && !s_associated) {
        s_associated = true;
        s_sta_state = STA_CONNECTED;
        taskENTER_CRITICAL(&s_lock);
        s_st.associated = true;
        taskEXIT_CRITICAL(&s_lock);
    }
    if (ip && !s_has_ip) {
        handle_got_ip();
    } else if (!ip && s_has_ip) {
        ip_lost();
    }
    /* A lost STA_START leaves a configured station idle for good. */
    if (s_wifi_started && s_sta_configured && s_sta_state == STA_IDLE) {
        set_wifi_sta_on(true);
        connect_now();
    }
    /* A lost AP_START or AP_STOP: the captive DNS and the status follow the AP. */
    wifi_mode_t mode = WIFI_MODE_NULL;
    bool ap_running = s_wifi_started && esp_wifi_get_mode(&mode) == ESP_OK &&
                      (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA);
    if (ap_running) {
        dns_start();
    } else {
        dns_stop();
    }
    uint8_t clients = ap_client_count();
    taskENTER_CRITICAL(&s_lock);
    s_st.setup_ap_active = ap_running;
    s_st.ap_clients = ap_running ? clients : 0;
    taskEXIT_CRITICAL(&s_lock);
    /* A lost SCAN_DONE is left to the scan watchdog: a scan still running
     * keeps its results. */
}

static void housekeeping(void)
{
    if (s_queue_overflow) {
        resync();
    }
    if (s_dns_restart) {
        dns_start();   /* runs once the previous task has gone */
    }
    int64_t now_us = esp_timer_get_time();

    bool apply = false;
    bool trial = false;
    taskENTER_CRITICAL(&s_lock);
    if (s_new_pending && now_us >= s_apply_at_us) {
        strlcpy(s_ssid, s_new_ssid, sizeof(s_ssid));
        strlcpy(s_pass, s_new_pass, sizeof(s_pass));
        memset(s_new_pass, 0, sizeof(s_new_pass));
        s_new_pending = false;
        trial = s_new_trial;
        s_trial_revert_ok = net_ssid_valid(s_act_ssid);
        apply = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (apply) {
        if (trial) {
            net_cred_trial_start(&s_trial, now_s());
            s_trial_assoc = false;
        } else {
            s_trial.on = false;
        }
        apply_credentials();
    }

    taskENTER_CRITICAL(&s_lock);
    bool scan_stuck = s_scanning && now_us - s_scan_started_us > (int64_t)SCAN_WATCHDOG_MS * 1000;
    if (scan_stuck) {
        s_scanning = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (scan_stuck) {
        ESP_LOGW(TAG, "scan did not finish: abandoned");
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_clear_ap_list();
    }

    if (s_started && s_sta_configured && !ap_is_up() && wifi_start_due()) {
        /* Wi-Fi did not start (a failed start or restart): nothing else would try again. */
        ESP_LOGW(TAG, "starting Wi-Fi again");
        start_station();
    }

    switch (s_sta_state) {
    case STA_WAIT_RETRY:
        if (now_us >= s_retry_at_us && !scanning()) {
            connect_now();
        }
        break;
    case STA_DISCONNECTING:
        if (now_us >= s_deadline_us) {
            connect_now();
        }
        break;
    case STA_CONNECTING:
        if (now_us >= s_deadline_us) {
            ESP_LOGW(TAG, "connect attempt timed out");
            schedule_retry();
        }
        break;
    default:
        break;
    }

    uint32_t now = now_s();
    if (now == s_last_tick_s) {
        return;
    }
    s_last_tick_s = now;
    if (s_started) {
        apply_ap_policy();
    }
    if (s_associated && now - s_last_rssi_s >= RSSI_INTERVAL_S) {
        s_last_rssi_s = now;
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            taskENTER_CRITICAL(&s_lock);
            s_st.rssi = ap.rssi;
            s_st.channel = ap.primary;
            taskEXIT_CRITICAL(&s_lock);
        }
    }
    if (now - s_last_check_s >= CHECK_INTERVAL_S) {
        s_last_check_s = now;
        ensure_fallback_dns();
        ensure_sntp_servers();
    }
    flush_drop_record();
    flush_up_record();
    if (net_cred_trial_tick(&s_trial, false, now) == NET_TRIAL_REVERT) {
        trial_revert();
    }

    uint32_t retry_in = 0;
    if (s_sta_state == STA_WAIT_RETRY && s_retry_at_us > now_us) {
        retry_in = (uint32_t)((s_retry_at_us - now_us + 999999) / 1000000);
    }
    taskENTER_CRITICAL(&s_lock);
    s_st.offline_s = (s_sta_configured && !s_has_ip) ? now - s_offline_since_s : 0;
    s_st.assoc_no_ip_s = (s_associated && !s_has_ip) ? now - s_noip_since_s : 0;
    s_st.retry_in_s = retry_in;
    s_st.cred_trial = s_trial.on;
    s_st.cred_trial_left_s = net_cred_trial_left_s(&s_trial, now);
    s_st.cred_pending = s_pend_valid;
    strlcpy(s_st.pending_ssid, s_pend_valid ? s_pend_ssid : "", sizeof(s_st.pending_ssid));
    s_st.cred_reverted = s_cred_reverted && s_pend_valid;
    taskEXIT_CRITICAL(&s_lock);
}

static void net_task(void *arg)
{
    (void)arg;
    for (;;) {
        net_msg_t m;
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(NET_POLL_MS)) == pdTRUE) {
            handle_msg(&m);
        }
        housekeeping();
    }
}

/* ---- init ------------------------------------------------------------------------------ */

static void load_hostname(void)
{
    char name[33] = "";
    if (settings_is_available() &&
        settings_get_str(SETTINGS_NS_SYS, SETTINGS_KEY_HOSTNAME, name, sizeof(name)) == ESP_OK && name[0] != '\0') {
        if (net_hostname_valid(name)) {
            strlcpy(s_hostname, name, sizeof(s_hostname));
        } else {
            ESP_LOGW(TAG, "stored hostname \"%s\" is not valid: using \"%s\"", name, CONFIG_LWIP_LOCAL_HOSTNAME);
        }
    }
    if (s_hostname[0] != '\0') {
        esp_err_t err = esp_netif_set_hostname(s_sta, s_hostname);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "hostname: %s", esp_err_to_name(err));
        }
    }
}

static void load_fallback_dns(void)
{
    s_dns1_ok = esp_netif_str_to_ip4(CONFIG_NETWORK_FALLBACK_DNS1, &s_dns1) == ESP_OK && s_dns1.addr != 0;
    s_dns2_ok = esp_netif_str_to_ip4(CONFIG_NETWORK_FALLBACK_DNS2, &s_dns2) == ESP_OK && s_dns2.addr != 0;
    if (!s_dns1_ok || !s_dns2_ok) {
        ESP_LOGE(TAG, "fallback DNS servers \"%s\", \"%s\": not IPv4 addresses", CONFIG_NETWORK_FALLBACK_DNS1,
                 CONFIG_NETWORK_FALLBACK_DNS2);
    }
}

/* DHCP option 114 points phones at the setup page. The option keeps a
 * pointer, so the URI lives in static memory. Set once, before the AP first
 * starts: the server is still unstarted (setting needs that) and keeps the
 * option across AP restarts. Not stopped first: a stopped server would not
 * be started again by the AP start. */
static void setup_ap_netif(void)
{
    esp_netif_ip_info_t info;
    if (esp_netif_get_ip_info(s_ap, &info) != ESP_OK) {
        ESP_LOGE(TAG, "AP address unknown");
        return;
    }
    memcpy(s_ap_addr, &info.ip.addr, 4);
    snprintf(s_portal_uri, sizeof(s_portal_uri), "http://" IPSTR "/", IP2STR(&info.ip));
    esp_err_t err = esp_netif_dhcps_option(s_ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, s_portal_uri,
                                           strlen(s_portal_uri));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "captive portal DHCP option: %s", esp_err_to_name(err));
    }
}

static void setup_ap_name(void)
{
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        (void)esp_read_mac(mac, ESP_MAC_WIFI_STA);
    }
    char name[33];
    net_ap_ssid(CONFIG_NETWORK_AP_SSID_PREFIX, mac, name, sizeof(name));
    set_status_str(s_st.ap_ssid, sizeof(s_st.ap_ssid), name);
}

static void account_boot(void)
{
    lifecycle_boot_info_t bi;
    lifecycle_get_boot_info(&bi);
    /* A block from before another image's run (still valid by its own
     * checks) starts afresh, like a power cycle. */
    net_rtc_on_boot(&s_rtc, lifecycle_rtc_stamp_ok(s_rtc.stamp), lifecycle_rtc_stamp(),
                    bi.prev_reboot_reason == LIFECYCLE_REBOOT_NETWORK_LADDER, bi.prev_session_uptime_s);
}

esp_err_t network_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    account_boot();
    taskENTER_CRITICAL(&s_lock);
    s_st.probe_age_s = UINT32_MAX;
    taskEXIT_CRITICAL(&s_lock);

    s_queue = xQueueCreate(NET_QUEUE_LEN, sizeof(net_msg_t));
    s_start_done = xSemaphoreCreateBinary();
    s_dns_done = xSemaphoreCreateBinary();
    s_probe_events = xEventGroupCreate();
    if (s_queue == NULL || s_start_done == NULL || s_dns_done == NULL || s_probe_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_probe_events, PROBE_IDLE_BIT);

    s_sta = esp_netif_create_default_wifi_sta();
    s_ap = esp_netif_create_default_wifi_ap();
    if (s_sta == NULL || s_ap == NULL) {
        return ESP_FAIL;
    }
    load_hostname();
    load_fallback_dns();
    setup_ap_netif();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    cfg.nvs_enable = 0;   /* credentials live in settings */
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_set_country_code(CONFIG_NETWORK_WIFI_COUNTRY, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "country code \"%s\" rejected (%s): worldwide default", CONFIG_NETWORK_WIFI_COUNTRY,
                 esp_err_to_name(err));
    }
    setup_ap_name();

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, on_sntp_event, NULL, NULL);
    }
    if (err != ESP_OK) {
        return err;
    }
    sntp_setup();

    if (xTaskCreatePinnedToCore(net_task, "net", NET_TASK_STACK, NULL, NET_TASK_PRIO, &s_task, NET_TASK_CORE) !=
        pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(probe_task, "netprobe", PROBE_TASK_STACK, NULL, PROBE_TASK_PRIO, &s_probe_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    net_ladder_init(&s_ladder);
    s_inited = true;
    ESP_LOGI(TAG, "init: AP name \"%s\", country %s", s_st.ap_ssid, CONFIG_NETWORK_WIFI_COUNTRY);
    return ESP_OK;
}

esp_err_t network_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t m = { .type = MSG_START };
    if (xQueueSend(s_queue, &m, pdMS_TO_TICKS(START_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (xSemaphoreTake(s_start_done, pdMS_TO_TICKS(START_WAIT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return s_start_err;
}

/* ---- public API ---------------------------------------------------------------------- */

esp_err_t network_get_status(network_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    int64_t now_us = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    *out = s_st;
    out->probe_age_s = s_probe_valid ? (uint32_t)((now_us - s_probe_at_us) / 1000000) : UINT32_MAX;
    taskEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

bool network_is_online(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool online = s_st.online && s_st.associated;
    taskEXIT_CRITICAL(&s_lock);
    return online;
}

/* Hand credentials to the net task, applied APPLY_DELAY_MS from now (under s_lock). */
static void queue_apply_locked(const char *ssid, const char *pass, bool trial)
{
    strlcpy(s_new_ssid, ssid, sizeof(s_new_ssid));
    strlcpy(s_new_pass, pass, sizeof(s_new_pass));
    s_new_trial = trial;
    s_new_pending = true;
    s_apply_at_us = esp_timer_get_time() + (int64_t)APPLY_DELAY_MS * 1000;
}

esp_err_t network_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || !net_ssid_valid(ssid) || !net_password_valid(password)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    bool have = net_ssid_valid(s_act_ssid);
    bool same = have && strcmp(s_act_ssid, ssid) == 0 && strcmp(s_act_pass, password) == 0;
    taskEXIT_CRITICAL(&s_lock);
    net_cred_plan_t plan = net_cred_plan(have, same);

    if (settings_is_available()) {
        esp_err_t err;
        if (plan == NET_CRED_PLAN_TRIAL) {
            err = store_pair(SETTINGS_KEY_WIFI_PEND_SSID, SETTINGS_KEY_WIFI_PEND_PASS, ssid, password);
        } else {
            err = store_pair(SETTINGS_KEY_WIFI_SSID, SETTINGS_KEY_WIFI_PASS, ssid, password);
            if (err == ESP_OK) {
                esp_err_t e = erase_pending_keys();
                if (e != ESP_OK) {
                    ESP_LOGW(TAG, "pending Wi-Fi credentials not erased: %s", esp_err_to_name(e));
                }
            }
        }
        if (err != ESP_OK) {
            return err;
        }
    } else {
        ESP_LOGW(TAG, "settings unavailable: Wi-Fi credentials are used for this boot only");
    }
    taskENTER_CRITICAL(&s_lock);
    if (plan == NET_CRED_PLAN_TRIAL) {
        strlcpy(s_pend_ssid, ssid, sizeof(s_pend_ssid));
        strlcpy(s_pend_pass, password, sizeof(s_pend_pass));
        s_pend_valid = true;
    } else {
        strlcpy(s_act_ssid, ssid, sizeof(s_act_ssid));
        strlcpy(s_act_pass, password, sizeof(s_act_pass));
        memset(s_pend_pass, 0, sizeof(s_pend_pass));
        s_pend_ssid[0] = '\0';
        s_pend_valid = false;
    }
    s_cred_reverted = false;
    queue_apply_locked(ssid, password, plan == NET_CRED_PLAN_TRIAL);
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "Wi-Fi credentials for \"%s\" %s", ssid,
             plan == NET_CRED_PLAN_TRIAL ? "stored as pending: tried now, kept if they get an IP"
                                         : "stored");
    return ESP_OK;
}

esp_err_t network_retry_pending_credentials(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    bool pend = s_pend_valid;
    if (pend) {
        s_cred_reverted = false;
        queue_apply_locked(s_pend_ssid, s_pend_pass, true);
    }
    taskEXIT_CRITICAL(&s_lock);
    return pend ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t network_discard_pending_credentials(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    bool pend = s_pend_valid;
    taskEXIT_CRITICAL(&s_lock);
    if (!pend) {
        return ESP_ERR_NOT_FOUND;
    }
    if (settings_is_available()) {
        esp_err_t err = erase_pending_keys();
        if (err != ESP_OK) {
            return err;
        }
    }
    taskENTER_CRITICAL(&s_lock);
    bool on_trial = s_st.cred_trial || (s_new_pending && s_new_trial);
    memset(s_pend_pass, 0, sizeof(s_pend_pass));
    s_pend_ssid[0] = '\0';
    s_pend_valid = false;
    s_cred_reverted = false;
    if (on_trial && net_ssid_valid(s_act_ssid)) {
        queue_apply_locked(s_act_ssid, s_act_pass, false);   /* back to the station's credentials */
    } else if (s_new_pending && s_new_trial) {
        s_new_pending = false;
    }
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "pending Wi-Fi credentials discarded");
    return ESP_OK;
}

esp_err_t network_scan_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_wifi_sta_on && !s_scanning;
    if (ok) {
        s_scanning = true;
        s_scan_started_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_scanning = false;
        taskEXIT_CRITICAL(&s_lock);
        return (err == ESP_ERR_WIFI_STATE) ? ESP_ERR_INVALID_STATE : err;
    }
    return ESP_OK;
}

esp_err_t network_get_scan_results(network_scan_entry_t *entries, size_t max_entries, size_t *out_count)
{
    if (out_count == NULL || (entries == NULL && max_entries > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    size_t n = s_scan_count < max_entries ? s_scan_count : max_entries;
    for (size_t i = 0; i < n; i++) {
        memcpy(entries[i].ssid, s_scan[i].ssid, sizeof(entries[i].ssid));
        entries[i].rssi = s_scan[i].rssi;
        entries[i].channel = s_scan[i].channel;
        entries[i].secured = s_scan[i].secured;
    }
    taskEXIT_CRITICAL(&s_lock);
    *out_count = n;
    return ESP_OK;
}

esp_err_t network_start_setup_ap(bool recovery)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!recovery && !settings_is_available()) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t m = { .type = MSG_AP_REQUEST, .reason = recovery ? 1 : 0 };
    return xQueueSend(s_queue, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t network_request_recovery(network_recovery_t action)
{
    if (action != NETWORK_RECOVER_RECONNECT && action != NETWORK_RECOVER_RENEW_DHCP &&
        action != NETWORK_RECOVER_RESTART_WIFI) {
        return ESP_ERR_INVALID_ARG;
    }
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_inited && s_st.sta_configured;
    taskEXIT_CRITICAL(&s_lock);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t m = { .type = MSG_RECOVER, .reason = (uint8_t)action };
    return xQueueSend(s_queue, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t network_probe_upstream(uint32_t timeout_ms)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    probe_kick();
    if (timeout_ms == 0) {
        return ESP_ERR_TIMEOUT;
    }
    EventBits_t bits = xEventGroupWaitBits(s_probe_events, PROBE_IDLE_BIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(timeout_ms));
    if (!(bits & PROBE_IDLE_BIT)) {
        return ESP_ERR_TIMEOUT;
    }
    taskENTER_CRITICAL(&s_lock);
    network_probe_t r = s_st.probe;
    taskEXIT_CRITICAL(&s_lock);
    switch (r) {
    case NETWORK_PROBE_OK:
        return ESP_OK;
    case NETWORK_PROBE_DNS_FAILED:
        return ESP_ERR_NOT_FOUND;
    case NETWORK_PROBE_NO_UPSTREAM:
        return ESP_FAIL;
    default:
        return ESP_ERR_INVALID_STATE;
    }
}

/* ---- recovery ladder ------------------------------------------------------------------- */

static void ladder_journal(uint32_t outcome, net_rung_t rung, uint32_t why)
{
    int32_t b = (int32_t)(((why & 0xFFu) << 16) | (uint32_t)diag_ladder_b((diag_step_outcome_t)outcome, (uint32_t)rung));
    (void)diag_journal_append(DIAG_EV_LADDER_STEP, LADDER_JOURNAL_A, b);
}

static const char *rung_name(net_rung_t r)
{
    switch (r) {
    case NET_RUNG_RECONNECT:
        return "reconnect";
    case NET_RUNG_RENEW_DHCP:
        return "renew DHCP";
    case NET_RUNG_RESTART_WIFI:
        return "restart Wi-Fi";
    case NET_RUNG_REBOOT:
        return "reboot";
    default:
        return "-";
    }
}

esp_err_t network_ladder_tick(const network_ladder_input_t *in)
{
    if (in == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t now = now_s();
    int64_t now_us = esp_timer_get_time();
    net_ladder_in_t lin = {
        .caster_down = in->caster_down,
        .rtcm_healthy = in->rtcm_healthy,
        .unreached_s = in->slot_unreached_s,
        .journal_open = in->flash_journal_open,
        .journal_unknown = in->flash_journal_unknown,
        .reboot_inhibited = lifecycle_reboot_inhibited(NULL),
        .since_reboot_s = net_rtc_age_now(&s_rtc, now),
        .min_reboot_interval_s = in->min_reboot_interval_s,
    };
    taskENTER_CRITICAL(&s_lock);
    lin.sta_configured = s_st.sta_configured;
    lin.assoc_no_ip_s = s_st.assoc_no_ip_s;
    lin.ap_clients = s_st.ap_clients;
    lin.probe_valid = s_probe_valid;
    lin.probe = (net_probe_t)s_st.probe;
    lin.probe_at_s = now - (uint32_t)((now_us - s_probe_at_us) / 1000000);
    net_ladder_out_t out = net_ladder_tick(&s_ladder, &lin, now);
    s_st.ladder_triggers = net_ladder_triggers(&lin);
    s_st.ladder_active = s_ladder.phase != NET_LADDER_IDLE;
    s_st.ladder_rung = s_ladder.last_rung;
    if (out.act == NET_LADDER_SKIP || out.act == NET_LADDER_WITHHELD) {
        s_st.ladder_withheld = (uint8_t)out.why;
    } else if (out.act == NET_LADDER_RESET) {
        s_st.ladder_withheld = NETWORK_LADDER_WHY_NONE;
    }
    if (out.act == NET_LADDER_ACT) {
        s_st.ladder_actions++;
    }
    taskEXIT_CRITICAL(&s_lock);

    switch (out.act) {
    case NET_LADDER_PASS:
        /* rung code 0 = "pass started", the reason byte = the triggers holding now */
        s_ladder_note = 0;
        ladder_journal(LADDER_OUTCOME_STARTED, NET_RUNG_NONE, out.triggers);
        ESP_LOGW(TAG, "network ladder: pass started (%s%s%s)", (out.triggers & NET_TRIG_P) ? " protected caster down" : "",
                 (out.triggers & NET_TRIG_A) ? " associated without IP" : "",
                 (out.triggers & NET_TRIG_S) ? " caster not reached" : "");
        break;
    case NET_LADDER_PROBE:
        probe_kick();
        break;
    case NET_LADDER_ACT:
        s_ladder_note = 0;
        ladder_journal(LADDER_OUTCOME_STARTED, out.rung, out.why);
        if (out.why == NET_WHY_AP_OVERRIDDEN) {
            ESP_LOGW(TAG, "network ladder: %s although an AP client is connected (held back for %u s already)",
                     rung_name(out.rung), (unsigned)NET_LADDER_AP_HOLD_MAX_S);
        }
        if (out.rung == NET_RUNG_REBOOT) {
            ESP_LOGE(TAG, "network ladder: reboot (protected caster down, the fault is on this side)");
            (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_NETWORK_LADDER);
        } else {
            ESP_LOGW(TAG, "network ladder: %s", rung_name(out.rung));
            network_recovery_t action = (out.rung == NET_RUNG_RECONNECT)    ? NETWORK_RECOVER_RECONNECT
                                        : (out.rung == NET_RUNG_RENEW_DHCP) ? NETWORK_RECOVER_RENEW_DHCP
                                                                            : NETWORK_RECOVER_RESTART_WIFI;
            net_msg_t m = { .type = MSG_RECOVER, .reason = (uint8_t)action };
            if (xQueueSend(s_queue, &m, 0) != pdTRUE) {
                ESP_LOGW(TAG, "network ladder: queue full, %s dropped", rung_name(out.rung));
            }
        }
        break;
    case NET_LADDER_SKIP:
    case NET_LADDER_WITHHELD: {
        uint8_t note = (uint8_t)(((uint32_t)out.rung << 4) | (uint32_t)out.why);
        if (note != s_ladder_note) {
            s_ladder_note = note;
            ladder_journal(LADDER_OUTCOME_HELD, out.rung, out.why);
            ESP_LOGI(TAG, "network ladder: %s %s (reason %d)", rung_name(out.rung),
                     out.act == NET_LADDER_SKIP ? "skipped" : "withheld", (int)out.why);
        }
        break;
    }
    case NET_LADDER_RESET:
        s_ladder_note = 0;
        if (out.rung != NET_RUNG_NONE) {
            ladder_journal(LADDER_OUTCOME_HEALTHY, out.rung, NET_WHY_NONE);
        }
        ESP_LOGI(TAG, "network ladder: healthy again");
        break;
    default:
        break;
    }
    return ESP_OK;
}
