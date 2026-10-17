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
 *    lookup never delays a reconnect (its stack in PSRAM: no flash work).
 *  - "dns": the captive-portal responder, only while the AP is up.
 *  - the mDNS component's own task (<hostname>.local, the _http service;
 *    its stack is in PSRAM, sdkconfig.defaults, and so are its static
 *    packet and name buffers, linker.lf); its rename callback only copies
 *    the new name for the net task to log.
 *
 * IPv6: each interface gets a link-local address when its link comes up
 * (the station's association, the AP's start, the emulated port's link in
 * the QEMU build), and no other. No SLAAC: lwIP turns stateless
 * autoconfiguration on for every interface it adds, whatever
 * CONFIG_LWIP_IPV6_AUTOCONFIG says (off, that option only keeps esp_netif
 * from setting it), so ip6_link_up turns it off in the lwIP context right
 * before it creates the link-local address, at every link-up. An address
 * that is not link-local is logged as a warning ("IPv6 address ... is not
 * link-local"). The link-local address is enough for the mDNS responder to
 * answer AAAA queries for <name>.local at once, instead of leaving every
 * dual-stack client to wait out its AAAA timeout, and the web server (a
 * dual-stack socket) answers on it.
 *
 * Moving between access points (CONFIG_NETWORK_ROAM; the decisions are in
 * net_roam.c): the station configuration turns on 802.11k and 802.11v
 * (rm_enabled, btm_enabled), so ESP-IDF's supplicant answers an access
 * point's BSS transition request and moves the station by itself. Its
 * request handler is wrapped at link time (CMakeLists.txt) and drops a
 * request while a move must not happen. Such a move reaches the net task as
 * a disconnect with reason WIFI_REASON_ROAMING, after which the driver
 * connects by itself: the net task waits as for its own connect instead of
 * scheduling one. The station's own move is a deliberate reconnect (the
 * connect scan joins the strongest access point), after scans for its SSID
 * that never touch the setup page's list. A move that reaches an address is
 * journalled as DIAG_EV_WIFI_ROAM, instead of a drop and the return after it.
 *
 * A move keeps the station's address and its TCP sessions when it can
 * (net_roam.h, "Keeping the address across a move"): ESP-IDF's default
 * station handlers would take the interface down at the disconnect, so the
 * two interface actions they call are wrapped at link time (CMakeLists.txt)
 * and leave the interface up for the disconnect of a move and the
 * association after it, as ESP-IDF's own handlers do for its roaming app
 * (ESP_WIFI_ROAMING_IP_RENEW_SKIP). on_sta_link, registered after those
 * handlers, tells the net task what was decided. Back on a link, DHCP is
 * asked to confirm the address (the link down and up in lwIP, as on another
 * network), the mDNS responder starts again on the interface, and the move
 * completes with DHCP's answer. A move that fails, takes longer than
 * NET_HOLD_AWAY_S or loses its address takes the interface down after all,
 * as an ordinary disconnect does at once: every session closes and the
 * casters reconnect.
 *
 * QEMU development build (CONFIG_NETWORK_QEMU_ETHERNET, sdkconfig.qemu):
 * Wi-Fi is never initialised or started. The emulated OpenCores Ethernet
 * port is created as the station interface instead (its key and IP events),
 * its link events stand in for the association, and there is no AP. Every
 * difference is inside an #if on that option.
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
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "ledger.h"
#include "lifecycle.h"
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"
#include "lwip/netdb.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "mdns.h"
#include "net_core.h"
#include "net_ladder.h"
#include "net_roam.h"
#include "sdkconfig.h"
#include "settings.h"

#if CONFIG_NETWORK_ROAM && CONFIG_ESP_WIFI_WNM_SUPPORT
#include "esp_wnm.h"
#endif

#if CONFIG_NETWORK_QEMU_ETHERNET
#if !CONFIG_ETH_USE_OPENETH
#error "CONFIG_NETWORK_QEMU_ETHERNET needs the OpenCores Ethernet MAC (QEMU only)"
#endif
#include "esp_eth.h"
#include "esp_eth_mac_openeth.h"
#endif

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
_Static_assert(CONFIG_NETWORK_RECOVERY_AP_S > 0 && CONFIG_NETWORK_STA_FALLBACK_S > 0 && CONFIG_NETWORK_AP_LINGER_S > 0,
               "AP timings");
_Static_assert(WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT == NET_REASON_4WAY_HANDSHAKE_TIMEOUT &&
                   WIFI_REASON_NO_AP_FOUND == NET_REASON_NO_AP_FOUND && WIFI_REASON_AUTH_FAIL == NET_REASON_AUTH_FAIL &&
                   WIFI_REASON_HANDSHAKE_TIMEOUT == NET_REASON_HANDSHAKE_TIMEOUT &&
                   WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY == NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY &&
                   WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD == NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD,
               "disconnect reasons of the credential trial match the core");
_Static_assert(sizeof(((network_status_t *)0)->mdns_host) == 33, "mDNS host label as in the API");
#if CONFIG_NETWORK_ROAM
_Static_assert(WIFI_REASON_ROAMING == NET_REASON_ROAMING, "the supplicant's move keeps the address (net_hold_t)");
#if CONFIG_ESP_WIFI_ROAMING_IP_RENEW_SKIP
#error "ESP_WIFI_ROAMING_IP_RENEW_SKIP would keep the address behind the station's back: network.c keeps it itself"
#endif
#endif

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

/* Behaviour (PROVISIONAL, to be confirmed on hardware). The AP's linger
 * after the station got its IP is CONFIG_NETWORK_AP_LINGER_S. */
#define AP_MAX_CLIENTS       4
#define APPLY_DELAY_MS       NET_CRED_APPLY_DELAY_MS   /* new credentials: let the HTTP reply go out first */
#define DISCONNECT_WAIT_MS   3000    /* deliberate disconnect: connect anyway if no event came */
#define CONNECT_WATCHDOG_MS  60000   /* a connect attempt with no event at all counts as failed */
#define SCAN_WATCHDOG_MS     20000   /* a scan without SCAN_DONE is abandoned (it would block connects) */
#define ROAM_SCAN_MAX_MS     60      /* a roaming scan's longest wait on a channel (only the station's network answers) */
#define ROAM_SCAN_HOME_MS    60      /* and its time back on the station's channel between two channels */
#define ROAM_SCAN_KEEP       8       /* access points of the network a roaming scan hands on (the strongest) */
#define ROAM_GAP_TEXT        "5 min" /* NET_ROAM_BTM_GAP_S in words (logs) */
#define ROAM_STEER_READ_S    3       /* the access point's 802.11v support is read this long into an association */
_Static_assert(NET_ROAM_BTM_GAP_S == 300u, "ROAM_GAP_TEXT names the gap");
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
    MSG_GOT_IP6,        /* reason: 0 station, 1 access point */
    MSG_MOVE_NOW,       /* the station's own move at once (network_move_now) */
} msg_type_t;

typedef struct {
    uint8_t type;
    uint8_t reason;     /* disconnect reason, recovery action */
    int8_t rssi;
    uint8_t held;       /* MSG_STA_CONNECTED, MSG_STA_DISCONNECTED: the interface kept its address (net_hold_t) */
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
#if !CONFIG_NETWORK_QEMU_ETHERNET
static esp_netif_t *s_ap;                   /* no AP interface in the QEMU build */
#endif
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
static uint8_t s_cred_reverted_why;         /* network_cred_revert_t of s_cred_reverted */
/* The net task's trial (s_trial) as API readers see it: copied under s_lock
 * whenever the net task changes it, in the same critical section as the
 * credential state that goes with it, so the status never shows half of a
 * change (publish_cred_locked). */
static net_cred_trial_t s_trial_pub;
/* Credential writes to settings, from API calls and from the net task's
 * confirmation, one at a time (the RAM pairs stay under s_lock). */
static SemaphoreHandle_t s_cred_mux;

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
#if !CONFIG_NETWORK_QEMU_ETHERNET
static char s_portal_uri[32];               /* setup_ap_netif (no AP interface in the QEMU build) */
#endif
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

/* A planned restart is under way (lifecycle shutdown hook): our own Wi-Fi
 * stop on the way down is not a link loss. */
static volatile bool s_leaving;

#if CONFIG_NETWORK_ROAM
/* Moving between access points (net_roam.c decides; net task unless noted). */
typedef enum {
    ROAM_HOLD_NONE = 0,
    ROAM_HOLD_UPDATE,       /* an update holds the update slot or a restart lock (checked live too) */
    ROAM_HOLD_AP,           /* the station's own access point is up: a move would take its channel along */
    ROAM_HOLD_TRIAL,        /* changed credentials are on trial or about to be applied */
    ROAM_HOLD_GAP,          /* the last move is less than NET_ROAM_BTM_GAP_S ago */
} roam_hold_t;
static net_roam_t s_roam;
static volatile uint8_t s_btm_hold;         /* roam_hold_t for the access point's requests: net task writes, Wi-Fi task reads */
static bool s_roam_scanning;                /* under s_lock: a roaming scan runs */
static int64_t s_roam_scan_started_us;      /* under s_lock */
static bool s_user_scan_queued;             /* under s_lock: a user's scan starts once the roaming scan is done */
static bool s_steer_read;                   /* ap_steering read for this association */
/* A move under way: from its start to the address on the new link. */
static bool s_move_on;
static uint8_t s_move_by;                   /* net_roam_by_t */
static uint32_t s_move_start_s;
static bool s_move_from_known;              /* the access point it left is known */
static uint8_t s_move_bssid[6];             /* that access point (never journalled or logged) */
static uint8_t s_move_ch_from;
static int8_t s_move_rssi_from;
static bool s_move_assoc;                   /* associated on the way */
static uint32_t s_move_assoc_s;             /* when */
static uint8_t s_move_ch_to;
static int8_t s_move_rssi_to;
static bool s_move_same;                    /* the association is with the access point it left */
static bool s_move_rec_pending;             /* a DIAG_EV_WIFI_ROAM record waits for the journal */
static int32_t s_move_rec_a;
static int32_t s_move_rec_b;
/* Keeping the address across a move (net_hold_t). s_hold_mux serialises
 * s_hold with what is done to the station interface for it: the wrapped
 * netif actions on the event task, the outcomes on the net task. */
static SemaphoreHandle_t s_hold_mux;
static net_hold_t s_hold;                   /* under s_hold_mux */
typedef enum { VERDICT_NONE = 0, VERDICT_PASSED, VERDICT_HELD } verdict_t;
static volatile uint8_t s_disc_verdict;     /* the wrapped disconnected action's decision, for on_sta_link */
static volatile uint8_t s_conn_verdict;     /* the wrapped connected action's decision, for on_sta_link */
static bool s_hold_on;                      /* net task: a hold it was told about runs (its log lines) */
static bool s_lease_ok;                     /* net task: enough of the DHCP lease left (lease_refresh) */
static uint32_t s_lease_ip;                 /* net task: the address read with it */
#endif

/* mDNS: the responder renamed itself after a conflict (set by its callback,
 * logged by the net task), and the name the net task logged last. */
static volatile bool s_mdns_renamed;
static char s_mdns_logged[33];
static bool s_mdns_on;                      /* the responder runs (until the shutdown hook's goodbye) */

#if CONFIG_LWIP_IPV6
/* Net task: the link-local address logged last per interface (0 station, 1 AP). */
EXT_RAM_BSS_ATTR static char s_ip6_logged[2][48];
/* Net task: addresses other than link-local already warned about, per interface. */
#define IP6_WARNED_MAX 4
EXT_RAM_BSS_ATTR static uint8_t s_ip6_warned[2][IP6_WARNED_MAX][16];
EXT_RAM_BSS_ATTR static uint8_t s_ip6_warned_n[2];
#endif

/* Survives software, panic and watchdog resets: when the last network-ladder
 * reboot was. Stamped with the image generation (lifecycle). */
static RTC_NOINIT_ATTR net_rtc_t s_rtc;

#if CONFIG_NETWORK_QEMU_ETHERNET
#define QEMU_ETH_LABEL "QEMU Ethernet"   /* the status "SSID": what the station is connected through */
static esp_eth_handle_t s_eth;
static volatile bool s_eth_link;            /* link up (Ethernet events), for the resync */
#endif

/* ---- small helpers ------------------------------------------------------------------- */

static uint32_t now_s(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000);
}

static void post_event(network_event_t id)
{
    (void)esp_event_post(NETWORK_EVENT, id, NULL, 0, EVENT_POST_TICKS);
}

static void send_msg_held(uint8_t type, uint8_t reason, int8_t rssi, bool held)
{
    net_msg_t m = { .type = type, .reason = reason, .rssi = rssi, .held = held ? 1 : 0 };
    if (s_queue == NULL || xQueueSend(s_queue, &m, 0) != pdTRUE) {
        s_queue_overflow = true;   /* the net task resyncs from the driver */
        if (type == MSG_AP_CLIENTS) {
            __atomic_store_n(&s_ap_clients_queued, false, __ATOMIC_SEQ_CST);   /* the resync counts the clients */
        }
    }
}

static void send_msg(uint8_t type, uint8_t reason, int8_t rssi)
{
    send_msg_held(type, reason, rssi, false);
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

/* The credential fields of the status from the state under s_lock (call
 * with s_lock held): every change of that state publishes at once, so the
 * next status read matches what the change decided (net_cred_view). */
static void publish_cred_locked(void)
{
    int64_t now_us = esp_timer_get_time();
    net_cred_in_t in = {
        .pend_valid = s_pend_valid,
        .pend_ssid = s_pend_ssid,
        .reverted = s_cred_reverted,
        .reverted_why = s_cred_reverted_why,
        .trial_running = s_trial_pub.on,
        .trial_left_s = net_cred_trial_left_s(&s_trial_pub, (uint32_t)(now_us / 1000000)),
        .trial_queued = s_new_pending && s_new_trial,
        .other_queued = s_new_pending && !s_new_trial,
        .apply_left_ms = (s_new_pending && s_apply_at_us > now_us) ? (uint32_t)((s_apply_at_us - now_us) / 1000) : 0,
    };
    net_cred_view_t v;
    net_cred_view(&in, &v);
    s_st.cred_trial = v.cred_trial;
    s_st.cred_trial_left_s = v.cred_trial_left_s;
    s_st.cred_pending = v.cred_pending;
    strlcpy(s_st.pending_ssid, v.pending_ssid, sizeof(s_st.pending_ssid));
    s_st.cred_reverted = v.cred_reverted;
    s_st.cred_reverted_why = v.cred_reverted_why;
}

/* Net task: s_trial changed (started, ended); readers see it now. */
static void trial_publish(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_trial_pub = s_trial;
    publish_cred_locked();
    taskEXIT_CRITICAL(&s_lock);
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
    /* WIFI_EVENT_STA_CONNECTED and _DISCONNECTED: on_sta_link. */
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

/*
 * The station's association and its end. Registered for these two events
 * after ESP-IDF's default station handlers (esp_netif_create_default_wifi_sta
 * registers them), so it runs after them in the same dispatch (the handlers
 * of one event run in the order they were registered) and passes on what
 * the wrapped interface actions decided for this event: the interface kept
 * its address across a move (net_hold_t). No decision (roaming off, or the
 * default handler did not act) is an ordinary link change.
 */
static void on_sta_link(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
#if CONFIG_NETWORK_ROAM
    volatile uint8_t *verdict = id == WIFI_EVENT_STA_CONNECTED ? &s_conn_verdict : &s_disc_verdict;
    bool held = __atomic_exchange_n(verdict, (uint8_t)VERDICT_NONE, __ATOMIC_SEQ_CST) == VERDICT_HELD;
#else
    const bool held = false;
#endif
    if (id == WIFI_EVENT_STA_CONNECTED) {
        send_msg_held(MSG_STA_CONNECTED, 0, 0, held);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        send_msg_held(MSG_STA_DISCONNECTED, d ? (uint8_t)d->reason : 0, d ? d->rssi : 0, held);
    }
}

#if CONFIG_NETWORK_ROAM
static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

void __real_esp_netif_action_disconnected(void *esp_netif, esp_event_base_t base, int32_t event_id, void *data);
void __real_esp_netif_action_connected(void *esp_netif, esp_event_base_t base, int32_t event_id, void *data);
void __wrap_esp_netif_action_disconnected(void *esp_netif, esp_event_base_t base, int32_t event_id, void *data);
void __wrap_esp_netif_action_connected(void *esp_netif, esp_event_base_t base, int32_t event_id, void *data);

/* The station's own interface, from one of its default handlers (anything else passes straight through). */
static bool sta_action(void *esp_netif, esp_event_base_t base, int32_t event_id, int32_t want)
{
    return s_hold_mux != NULL && esp_netif != NULL && esp_netif == s_sta && base == WIFI_EVENT && event_id == want;
}

/*
 * ESP-IDF's default station handlers call these two actions at every
 * disconnect and association (wrapped at link time, CMakeLists.txt). A
 * disconnect the hold keeps leaves the interface up with its address and
 * its sessions, and the association after it skips the connected action
 * (the interface is up already), as ESP-IDF's own handlers do for its
 * roaming app with ESP_WIFI_ROAMING_IP_RENEW_SKIP. Everything else goes to
 * the original. Default event loop task.
 */
void __wrap_esp_netif_action_disconnected(void *esp_netif, esp_event_base_t base, int32_t event_id, void *data)
{
    if (!sta_action(esp_netif, base, event_id, WIFI_EVENT_STA_DISCONNECTED)) {
        __real_esp_netif_action_disconnected(esp_netif, base, event_id, data);
        return;
    }
    const wifi_event_sta_disconnected_t *d = data;
    xSemaphoreTake(s_hold_mux, portMAX_DELAY);
    bool keep = net_hold_disconnect(&s_hold, d != NULL ? d->reason : 0, now_ms());
    if (!keep) {
        __real_esp_netif_action_disconnected(esp_netif, base, event_id, data);
    }
    xSemaphoreGive(s_hold_mux);
    __atomic_store_n(&s_disc_verdict, (uint8_t)(keep ? VERDICT_HELD : VERDICT_PASSED), __ATOMIC_SEQ_CST);
}

void __wrap_esp_netif_action_connected(void *esp_netif, esp_event_base_t base, int32_t event_id, void *data)
{
    if (!sta_action(esp_netif, base, event_id, WIFI_EVENT_STA_CONNECTED)) {
        __real_esp_netif_action_connected(esp_netif, base, event_id, data);
        return;
    }
    xSemaphoreTake(s_hold_mux, portMAX_DELAY);
    bool held = net_hold_connected(&s_hold, now_ms());
    if (!held) {
        __real_esp_netif_action_connected(esp_netif, base, event_id, data);
    }
    xSemaphoreGive(s_hold_mux);
    __atomic_store_n(&s_conn_verdict, (uint8_t)(held ? VERDICT_HELD : VERDICT_PASSED), __ATOMIC_SEQ_CST);
}
#endif

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == IP_EVENT_STA_GOT_IP) {
        send_msg(MSG_GOT_IP, 0, 0);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        send_msg(MSG_LOST_IP, 0, 0);
    } else if (id == IP_EVENT_GOT_IP6 && data != NULL) {
        const ip_event_got_ip6_t *e = data;
        if (e->esp_netif == s_sta) {
            send_msg(MSG_GOT_IP6, 0, 0);
        }
#if !CONFIG_NETWORK_QEMU_ETHERNET
        else if (e->esp_netif == s_ap) {
            send_msg(MSG_GOT_IP6, 1, 0);
        }
#endif
    }
}

#if CONFIG_NETWORK_QEMU_ETHERNET
/* The emulated link stands in for the station's association. */
static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_eth_link = true;
        send_msg(MSG_STA_CONNECTED, 0, 0);
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_eth_link = false;
        send_msg(MSG_STA_DISCONNECTED, 0, 0);
    }
}

/* The OpenCores MAC as the station interface: the same interface key, so the
 * web server and the casters find it, and the station's IP events, so every
 * listener (this file, SNTP renewal, the casters) needs nothing else. */
static esp_err_t qemu_eth_init(void)
{
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.if_key = "WIFI_STA_DEF";
    base.if_desc = "qemu-eth";
    base.get_ip_event = IP_EVENT_STA_GOT_IP;
    base.lost_ip_event = IP_EVENT_STA_LOST_IP;
    esp_netif_config_t ncfg = { .base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH };
    s_sta = esp_netif_new(&ncfg);
    if (s_sta == NULL) {
        return ESP_FAIL;
    }
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.autonego_timeout_ms = 100;   /* the emulated PHY answers at once */
    esp_eth_mac_t *mac = esp_eth_mac_new_openeth(&mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_generic(&phy_cfg);
    if (mac == NULL || phy == NULL) {
        return ESP_FAIL;
    }
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_err_t err = esp_eth_driver_install(&cfg, &s_eth);
    if (err != ESP_OK) {
        return err;
    }
    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth);
    if (glue == NULL) {
        return ESP_FAIL;
    }
    err = esp_netif_attach(s_sta, glue);
    if (err != ESP_OK) {
        return err;
    }
    return esp_event_handler_instance_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL, NULL);
}
#endif

static void on_sntp_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    send_msg(MSG_TIME_SYNC, 0, 0);
}

/* ---- IPv6 link-local -------------------------------------------------------------------- */

#if CONFIG_LWIP_IPV6
/* lwIP context: no stateless address autoconfiguration on this interface.
 * lwIP turns it on for every interface it adds (netif_add), and esp_netif
 * removes and adds the interface again at every stop and start, so this runs
 * at every link-up. CONFIG_LWIP_IPV6_AUTOCONFIG=n only keeps esp_netif from
 * copying its own flag into the interface; lwIP's default stays on. */
static esp_err_t ip6_autoconfig_off(void *ctx)
{
    struct netif *lw = esp_netif_get_netif_impl((esp_netif_t *)ctx);
    if (lw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
#if LWIP_IPV6_AUTOCONFIG
    netif_set_ip6_autoconfig_enabled(lw, 0);
#endif
    return ESP_OK;
}
#endif

/* The interface's link came up: give it its link-local address (the link
 * going down clears every IPv6 address, so this runs on every link-up), and
 * no other. Autoconfiguration is turned off first: it needs a valid
 * link-local address in the first slot, so a router advertisement can never
 * be acted on in between. */
static void ip6_link_up(esp_netif_t *netif, const char *what)
{
#if CONFIG_LWIP_IPV6
    if (netif == NULL) {
        ESP_LOGW(TAG, "IPv6 link-local on %s not created: %s", what, esp_err_to_name(ESP_ERR_INVALID_STATE));
        return;
    }
    esp_err_t err = esp_netif_tcpip_exec(ip6_autoconfig_off, netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IPv6 autoconfiguration on %s not turned off: %s", what, esp_err_to_name(err));
    }
    err = esp_netif_create_ip6_linklocal(netif);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IPv6 link-local on %s not created: %s", what, esp_err_to_name(err));
    }
#else
    (void)netif;
    (void)what;
#endif
}

#if CONFIG_LWIP_IPV6
/* The lower-case text of an address (RFC 5952), as the web log writes addresses. */
static bool ip6_text(const esp_ip6_addr_t *a, char *text, size_t cap)
{
    if (inet_ntop(AF_INET6, a->addr, text, cap) == NULL) {
        return false;
    }
    for (char *c = text; *c != '\0'; c++) {
        if (*c >= 'A' && *c <= 'F') {
            *c = (char)(*c - 'A' + 'a');
        }
    }
    return true;
}

/* An address that is not link-local must never be there (ip6_link_up turns
 * autoconfiguration off): say so, once per interface and address. */
static void ip6_check_others(esp_netif_t *netif, uint8_t which)
{
    esp_ip6_addr_t all[LWIP_IPV6_NUM_ADDRESSES];
    int n = esp_netif_get_all_ip6(netif, all);
    for (int i = 0; i < n && i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        uint8_t raw[16];
        memcpy(raw, all[i].addr, sizeof(raw));
        if (net_ip6_is_link_local(raw)) {
            continue;
        }
        bool seen = false;
        for (uint8_t k = 0; k < s_ip6_warned_n[which] && !seen; k++) {
            seen = memcmp(s_ip6_warned[which][k], raw, sizeof(raw)) == 0;
        }
        if (seen) {
            continue;
        }
        uint8_t slot = s_ip6_warned_n[which] < IP6_WARNED_MAX ? s_ip6_warned_n[which]++ : IP6_WARNED_MAX - 1;
        memcpy(s_ip6_warned[which][slot], raw, sizeof(raw));
        char text[48];
        if (ip6_text(&all[i], text, sizeof(text))) {
            ESP_LOGW(TAG, "IPv6 address %s on %s is not link-local", text, which == 0 ? "station" : "access point");
        }
    }
}
#endif

/* An address passed duplicate detection (IP_EVENT_GOT_IP6): say which
 * link-local address the interface has, once per interface and address, and
 * warn about any other address. */
static void ip6_note(uint8_t which)
{
#if CONFIG_LWIP_IPV6
    esp_netif_t *netif = s_sta;
#if !CONFIG_NETWORK_QEMU_ETHERNET
    if (which == 1) {
        netif = s_ap;
    }
#endif
    if (which > 1 || netif == NULL) {
        return;
    }
    esp_ip6_addr_t ll;
    char text[48];
    if (esp_netif_get_ip6_linklocal(netif, &ll) == ESP_OK && ip6_text(&ll, text, sizeof(text)) &&
        strcmp(text, s_ip6_logged[which]) != 0) {
        strlcpy(s_ip6_logged[which], text, sizeof(s_ip6_logged[which]));
        ESP_LOGI(TAG, "IPv6 link-local %s on %s", text, which == 0 ? "station" : "access point");
    }
    ip6_check_others(netif, which);
#else
    (void)which;
#endif
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

#if CONFIG_NETWORK_ROAM
static void hold_cut(const char *why);
#endif

/* A scan runs (the user's or a roaming one): esp_wifi_connect() would fail. */
static bool scanning(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool s = s_scanning;
#if CONFIG_NETWORK_ROAM
    s = s || s_roam_scanning;
#endif
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
#if CONFIG_NETWORK_ROAM && CONFIG_ESP_WIFI_RRM_SUPPORT
    c->sta.rm_enabled = 1;    /* 802.11k: the access point may ask for radio measurements (a mesh picks nodes by them) */
#endif
#if CONFIG_NETWORK_ROAM && CONFIG_ESP_WIFI_WNM_SUPPORT
    c->sta.btm_enabled = 1;   /* 802.11v: the access point may ask the station to move (filtered: moving between access points) */
#endif
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

/* The next connect attempt in delay_ms. */
static void schedule_retry_ms(uint32_t delay_ms)
{
    if (s_attempt < STA_RETRY_ATTEMPT_CAP) {
        s_attempt++;
    }
    s_retry_at_us = esp_timer_get_time() + (int64_t)delay_ms * 1000;
    s_sta_state = STA_WAIT_RETRY;
}

/* The next connect attempt after the backoff. */
static void schedule_retry(void)
{
    schedule_retry_ms(net_backoff_ms(s_attempt));
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
#if CONFIG_NETWORK_QEMU_ETHERNET
    /* No radio: Wi-Fi is never started (and nothing calls this there:
     * wifi_start_due() is false, the AP, recovery and credential paths
     * return before it). */
    return ESP_ERR_NOT_SUPPORTED;
#else
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
#endif
}

static void wifi_stop(void)
{
#if CONFIG_NETWORK_ROAM
    hold_cut("Wi-Fi stops");
#endif
    s_sta_state = STA_IDLE;
    set_wifi_sta_on(false);
    taskENTER_CRITICAL(&s_lock);
    s_scanning = false;   /* a stop ends any scan */
#if CONFIG_NETWORK_ROAM
    s_roam_scanning = false;
    s_user_scan_queued = false;
#endif
    taskEXIT_CRITICAL(&s_lock);
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_stop: %s", esp_err_to_name(err));
    }
    s_wifi_started = false;
    s_associated = false;
#if CONFIG_NETWORK_ROAM
    net_roam_disassociated(&s_roam);
    s_move_on = false;   /* a move cut short by the stop: an outage like any other */
    s_steer_read = false;
#endif
    s_ap_reasons = 0;   /* the AP stops with Wi-Fi: the policy brings it back if still wanted */
    taskENTER_CRITICAL(&s_lock);
    s_st.associated = false;
    s_st.bssid_known = false;
    s_st.ap_steering = -1;
    s_st.tx_power_known = false;
    s_st.ap_reasons = 0;
    s_st.ap_close_in_s = UINT32_MAX;
    taskEXIT_CRITICAL(&s_lock);
    ip_lost();
}

/* Wi-Fi may be (re)started now: not running, and no failed start waiting out its backoff. */
static bool wifi_start_due(void)
{
#if CONFIG_NETWORK_QEMU_ETHERNET
    return false;   /* no radio */
#else
    return !s_wifi_started && (s_wifi_start_fails == 0 || esp_timer_get_time() >= s_wifi_retry_at_us);
#endif
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
#if CONFIG_NETWORK_QEMU_ETHERNET
    ESP_LOGW(TAG, "QEMU: Wi-Fi credentials for \"%s\" kept, the emulated Ethernet stays in use", s_ssid);
    return;
#endif
#if CONFIG_NETWORK_ROAM
    hold_cut("new Wi-Fi credentials");
#endif
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
#if CONFIG_NETWORK_QEMU_ETHERNET
    (void)reasons;
    return;   /* no radio, no setup AP */
#endif
    if (!s_wifi_started && !wifi_start_due()) {
        return;   /* a failed start waits out its backoff */
    }
    wifi_config_t cfg;
    build_ap_config(&cfg);
    bool secured = cfg.ap.authmode != WIFI_AUTH_OPEN;
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
    /* After Wi-Fi is up, outside its init burst. The password is never logged. */
    const char *why = (reasons & NET_AP_RECOVERY)   ? "recovery"
                      : (reasons & NET_AP_SETUP)    ? "setup"
                      : (reasons & NET_AP_FALLBACK) ? "offline fallback"
                                                    : "linger";
    ESP_LOGW(TAG, "setup AP \"%s\" up (%s, %s), page at http://%u.%u.%u.%u/", s_st.ap_ssid, why,
             secured ? "WPA2" : "open", s_ap_addr[0], s_ap_addr[1], s_ap_addr[2], s_ap_addr[3]);
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
        .linger_s = CONFIG_NETWORK_AP_LINGER_S,
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
    /* What the AP is up for now (s_ap_reasons stays 0 when it could not come up). */
    uint32_t close_in = net_ap_close_in_s(&in, s_ap_reasons);
    taskENTER_CRITICAL(&s_lock);
    s_st.ap_reasons = (uint8_t)s_ap_reasons;
    s_st.ap_clients = (uint8_t)in.ap_clients;
    s_st.recovery_ap_left_s = left;
    s_st.ap_close_in_s = close_in;
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

#if CONFIG_NETWORK_ROAM
/* DIAG_EV_WIFI_ROAM: kept until the journal takes it, like the others. */
static void flush_move_record(void)
{
    if (!s_move_rec_pending) {
        return;
    }
    esp_err_t err = diag_journal_append(DIAG_EV_WIFI_ROAM, s_move_rec_a, s_move_rec_b);
    if (err != ESP_ERR_INVALID_STATE) {
        s_move_rec_pending = false;
    }
}
#endif

/* ---- mDNS -------------------------------------------------------------------------------- */

/* The name in use: the stored sys/hostname, else the build's default. */
static const char *hostname_in_use(void)
{
    return s_hostname[0] != '\0' ? s_hostname : CONFIG_LWIP_LOCAL_HOSTNAME;
}

/* The responder renamed itself (a lost probe, or another host answering for
 * our name): runs in the mDNS task while it holds its own lock, so it only
 * copies the name and leaves the log line to the net task. A name too long
 * for the status (a 31- or 32-character hostname with a "-N" suffix) is
 * not shown: the UI then offers the IP address. */
static void on_mdns_renamed(const char *hostname, void *arg)
{
    (void)arg;
    bool fits = hostname != NULL && strlen(hostname) < sizeof(s_st.mdns_host);
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_st.mdns_host, fits ? hostname : "", sizeof(s_st.mdns_host));
    taskEXIT_CRITICAL(&s_lock);
    s_mdns_renamed = true;
}

/* Net task: say once that the name is taken (the renamed one is never stored:
 * the next boot probes the configured name again). */
static void mdns_note_rename(void)
{
    if (!s_mdns_renamed) {
        return;
    }
    s_mdns_renamed = false;
    char now[33];
    taskENTER_CRITICAL(&s_lock);
    strlcpy(now, s_st.mdns_host, sizeof(now));
    taskEXIT_CRITICAL(&s_lock);
    if (now[0] == '\0') {
        ESP_LOGW(TAG, "mDNS: %s.local is taken on this network, and the name the station moved to is too long to "
                      "show: open it by its IP address",
                 s_mdns_logged);
        return;
    }
    if (strcmp(now, s_mdns_logged) != 0) {
        ESP_LOGW(TAG, "mDNS: %s.local is taken on this network: this station answers as %s.local", s_mdns_logged, now);
        strlcpy(s_mdns_logged, now, sizeof(s_mdns_logged));
    }
}

/*
 * <hostname>.local and the web page as an _http._tcp service, on the
 * station interface and the AP (the predefined interfaces of the component;
 * in the QEMU build the emulated Ethernet port carries the station's key and
 * events). Never fatal: without it the station is reached by its IP.
 *
 * The service has no instance name of its own: the component then names it
 * after the current host name (get_default_instance_name). After a host
 * name conflict the component renames the host and probes the services
 * again under the new label (restart_all_pcbs), so the service follows the
 * host name, and a device that already announced "<name>._http._tcp" keeps
 * it. One case remains in the component (espressif/mdns 1.13.1): its
 * service collision check applies the length tie-break to authoritative
 * answers too (mdns_receive.c, check_srv_collision), so when only the
 * service instance name collides, with a device whose SRV target is
 * shorter than ours, the station still wins that name and the other device
 * renames its service. To be checked on the bench.
 */
static void mdns_setup(void)
{
    const char *name = hostname_in_use();
    strlcpy(s_mdns_logged, name, sizeof(s_mdns_logged));
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS not started (%s): the station is reached by its IP address only", esp_err_to_name(err));
        return;
    }
    err = mdns_hostname_set(name);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS name %s not set (%s): the station is reached by its IP address only", name,
                 esp_err_to_name(err));
        mdns_free();
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_st.mdns_host, name, sizeof(s_st.mdns_host));
    taskEXIT_CRITICAL(&s_lock);
    mdns_txt_item_t txt[] = { { .key = "path", .value = "/" } };
    err = mdns_service_add(NULL, "_http", "_tcp", 80, txt, sizeof(txt) / sizeof(txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: web page not advertised (%s); %s.local still answers", esp_err_to_name(err), name);
    }
    err = mdns_register_hostname_changed_callback(on_mdns_renamed, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS: renames after a name conflict are not followed (%s)", esp_err_to_name(err));
    }
    s_mdns_on = true;
    ESP_LOGI(TAG, "mDNS: answering as %s.local (web page as service %s)", name, name);
}

/* Before a planned restart (a new hostname takes effect through one), while
 * the network is still up: removing the services makes the pinned mDNS
 * component send its goodbye, which carries the web service's PTR record
 * with TTL 0 and nothing else (its remove-all and free paths withdraw no
 * SRV, TXT or A/AAAA record). DNS-SD browsers drop the entry at once; the
 * SRV record, the TXT record and the old name's addresses expire from
 * caches within their 120 s TTL, and until then a client using the old name
 * reaches the station and gets 421 bad_host. A goodbye of our own is not
 * possible: it would have to leave from UDP port 5353, which the
 * component's socket holds. Then the responder stops. The packet gets at
 * most min(MDNS_BYE_WAIT_MS, the hook budget left) to leave before Wi-Fi
 * stops. */
#define MDNS_BYE_WAIT_MS 150
#define MDNS_BYE_MIN_MS  20    /* less budget left: no goodbye (stopping the responder takes a moment too) */

static void mdns_goodbye(uint32_t deadline_ms)
{
    if (!s_mdns_on) {
        return;
    }
    if (deadline_ms < MDNS_BYE_MIN_MS) {
        ESP_LOGW(TAG, "mDNS: no time left for a goodbye");
        return;
    }
    int64_t start = esp_timer_get_time();
    taskENTER_CRITICAL(&s_lock);
    bool up = s_st.online || s_st.setup_ap_active;
    s_st.mdns_host[0] = '\0';   /* the name is no longer answered */
    taskEXIT_CRITICAL(&s_lock);
    (void)mdns_service_remove_all();   /* the goodbyes go out here */
    mdns_free();
    s_mdns_on = false;
    if (!up) {
        ESP_LOGI(TAG, "mDNS: stopped (no network to say goodbye on)");
        return;
    }
    uint32_t used_ms = (uint32_t)((esp_timer_get_time() - start) / 1000);
    uint32_t wait_ms = deadline_ms > used_ms ? deadline_ms - used_ms : 0;
    if (wait_ms > MDNS_BYE_WAIT_MS) {
        wait_ms = MDNS_BYE_WAIT_MS;
    }
    if (wait_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
    ESP_LOGI(TAG, "mDNS: goodbye sent");
}

/* ---- planned restart ------------------------------------------------------------------- */

/* Lifecycle shutdown hook (after the ledger's and the casters'): from here on
 * the Wi-Fi stop on the way down is ours, not a link loss; the mDNS
 * services say goodbye while the network is still up. */
static void on_shutdown(lifecycle_reboot_reason_t reason, uint32_t deadline_ms, void *ctx)
{
    (void)reason;
    (void)ctx;
    s_leaving = true;
    mdns_goodbye(deadline_ms);
}

/* ---- moving between access points ------------------------------------------------------ */

#if CONFIG_NETWORK_ROAM
/* What keeps any move from happening now (the access point's requests and
 * the station's own moves; the latter also wait for an established link with
 * an address, net_roam_tick's `held`). */
static roam_hold_t roam_hold(void)
{
    if (lifecycle_update_active() != LIFECYCLE_UPDATE_NONE || lifecycle_reboot_inhibited(NULL)) {
        return ROAM_HOLD_UPDATE;
    }
    if (ap_is_up()) {
        return ROAM_HOLD_AP;
    }
    taskENTER_CRITICAL(&s_lock);
    bool queued = s_new_pending;
    taskEXIT_CRITICAL(&s_lock);
    return (s_trial.on || queued) ? ROAM_HOLD_TRIAL : ROAM_HOLD_NONE;
}

static const char *roam_hold_text(roam_hold_t h)
{
    switch (h) {
    case ROAM_HOLD_UPDATE:
        return "an update runs";
    case ROAM_HOLD_AP:
        return "the station's own access point is up";
    case ROAM_HOLD_TRIAL:
        return "new Wi-Fi credentials are on trial";
    case ROAM_HOLD_GAP:
        return "the station moved less than " ROAM_GAP_TEXT " ago";
    default:
        return "";
    }
}

/* The start of a move, with the access point it leaves (its BSSID stays in RAM, for the "same one" check only). */
static void move_begin(net_roam_by_t by, bool known, const uint8_t bssid[6], uint8_t channel, int8_t rssi)
{
    s_move_on = true;
    s_move_by = (uint8_t)by;
    s_move_start_s = now_s();
    s_move_from_known = known;
    memcpy(s_move_bssid, bssid, sizeof(s_move_bssid));
    s_move_ch_from = channel;
    s_move_rssi_from = rssi;
    s_move_assoc = false;
    s_move_same = false;
}

/* The association a move led to (MSG_STA_CONNECTED). */
static void move_associated(bool have, const wifi_ap_record_t *ap)
{
    if (!s_move_on) {
        return;
    }
    s_move_assoc = true;
    s_move_assoc_s = now_s();
    s_move_ch_to = have ? ap->primary : 0;
    s_move_rssi_to = have ? ap->rssi : 0;
    s_move_same = have && s_move_from_known && memcmp(ap->bssid, s_move_bssid, sizeof(s_move_bssid)) == 0;
}

static const char *move_by_text(uint8_t by)
{
    return by == NET_ROAM_BY_AP ? "at the access point's request"
           : by == NET_ROAM_BY_REQUEST ? "on request"
                                       : "with a stronger signal";
}

/*
 * The move is complete: the address on the new link, or with the address
 * kept (net_hold_t) DHCP's answer there (or the end of the wait for it).
 * Its record stands for the gap, so no DIAG_EV_WIFI_UP follows it. Kept:
 * the seconds are those without a link, as nothing was without an address.
 */
static void move_done(bool kept)
{
    if (!s_move_on || !s_move_assoc) {
        return;
    }
    uint32_t secs = (kept ? s_move_assoc_s : now_s()) - s_move_start_s;
    s_move_on = false;
    s_outage = false;
    s_move_rec_a = net_roam_rec_a(s_move_ch_from, s_move_ch_to, (net_roam_by_t)s_move_by, s_move_same, kept);
    s_move_rec_b = net_roam_rec_b(s_move_rssi_from, s_move_rssi_to, secs);
    s_move_rec_pending = true;
    flush_move_record();
    const char *gap = kept ? "without a link, the address and the connections kept" : "without an address";
    if (s_move_same) {
        ESP_LOGW(TAG, "back on the same access point after all (channel %u, %d dBm), %u s %s", s_move_ch_to,
                 s_move_rssi_to, (unsigned)secs, gap);
        return;
    }
    taskENTER_CRITICAL(&s_lock);
    s_st.roams++;
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "moved to another access point %s: channel %u, %d dBm -> channel %u, %d dBm, %u s %s",
             move_by_text(s_move_by), s_move_ch_from, s_move_rssi_from, s_move_ch_to, s_move_rssi_to, (unsigned)secs,
             gap);
}

/* The user's scan of every network (the setup page's list). */
static esp_err_t user_scan_driver_start(void)
{
    wifi_scan_config_t sc = { .show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE };
    return esp_wifi_scan_start(&sc, false);
}

/* The roaming scan is over (read, failed or abandoned): a user's scan that waited for it starts now. */
static void roam_scan_end(void)
{
    taskENTER_CRITICAL(&s_lock);
    s_roam_scanning = false;
    bool user = s_user_scan_queued;
    s_user_scan_queued = false;
    if (user) {
        s_scan_started_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&s_lock);
    if (user && user_scan_driver_start() != ESP_OK) {
        taskENTER_CRITICAL(&s_lock);
        s_scanning = false;
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGW(TAG, "scan did not start: the list stays as it was");
        post_event(NETWORK_EVENT_SCAN_DONE);   /* the page stops waiting */
    }
}

/* A scan for the station's own SSID only, short on each channel: the setup page's list is not touched. */
static void roam_scan_start(uint32_t now)
{
    static uint8_t ssid[33];   /* the driver reads it after the call returned */
    taskENTER_CRITICAL(&s_lock);
    bool busy = s_scanning || s_roam_scanning;
    if (!busy) {
        s_roam_scanning = true;
        s_roam_scan_started_us = esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&s_lock);
    if (busy) {
        net_roam_scan_failed(&s_roam, now);
        return;
    }
    memset(ssid, 0, sizeof(ssid));
    memcpy(ssid, s_ssid, strnlen(s_ssid, sizeof(ssid) - 1));
    wifi_scan_config_t sc = {
        .ssid = ssid,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time = { .active = { .min = 0, .max = ROAM_SCAN_MAX_MS } },
        .home_chan_dwell_time = ROAM_SCAN_HOME_MS,
    };
    esp_err_t err = esp_wifi_scan_start(&sc, false);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "roaming scan: %s (tried again in %u s)", esp_err_to_name(err), (unsigned)NET_ROAM_RETRY_S);
        roam_scan_end();
        net_roam_scan_failed(&s_roam, now);
    }
}

/* SCAN_DONE of a roaming scan: the network's access points (its SSID, the security the station uses). */
static void roam_scan_done(void)
{
    net_roam_bss_t found[ROAM_SCAN_KEEP];
    size_t n = 0;
    size_t len = strnlen(s_ssid, 32);
    bool want_secured = s_pass[0] != '\0';
    wifi_ap_record_t rec;
    while (esp_wifi_scan_get_ap_record(&rec) == ESP_OK) {
        bool secured = rec.authmode != WIFI_AUTH_OPEN && rec.authmode != WIFI_AUTH_WEP && rec.authmode != WIFI_AUTH_OWE;
        if (strnlen((const char *)rec.ssid, sizeof(rec.ssid)) != len || memcmp(rec.ssid, s_ssid, len) != 0 ||
            secured != want_secured) {
            continue;
        }
        net_roam_bss_t e = { .rssi = rec.rssi, .channel = rec.primary };
        memcpy(e.bssid, rec.bssid, sizeof(e.bssid));
        size_t at = n;
        if (n < ROAM_SCAN_KEEP) {
            n++;
        } else {
            at = 0;   /* full: the weakest gives way to a stronger one */
            for (size_t i = 1; i < n; i++) {
                if (found[i].rssi < found[at].rssi) {
                    at = i;
                }
            }
            if (found[at].rssi >= e.rssi) {
                continue;
            }
        }
        found[at] = e;
    }
    (void)esp_wifi_clear_ap_list();
    roam_scan_end();
    net_roam_scan_done(&s_roam, found, n, now_s());
    if (s_roam.hits > 0) {
        ESP_LOGI(TAG, "roaming scan: a stronger access point, %d dBm on channel %u (%d dBm here), found %u of %u times",
                 s_roam.cand.rssi, s_roam.cand.channel, net_roam_avg_dbm(&s_roam), s_roam.hits,
                 (unsigned)NET_ROAM_HITS);
    } else {
        ESP_LOGI(TAG, "roaming scan: %u access point(s) of the network, none clearly stronger than here (%d dBm)",
                 (unsigned)n, net_roam_avg_dbm(&s_roam));
    }
}

/* ---- keeping the address across a move (net_hold_t) ---- */

_Static_assert(NET_HOLD_LEASE_MIN_S == 60u, "hold_closed_why names the lease minimum");

/* What lwIP's DHCP client holds for the station (lwIP context, through esp_netif_tcpip_exec). */
typedef struct {
    uint32_t ip;         /* the interface's address as lwIP holds it (0 = none) */
    bool bound;          /* DHCP bound */
    bool leased;         /* bound or renewing: not past the rebind time */
    uint32_t left_s;     /* lease time left */
} dhcp_view_t;

static esp_err_t dhcp_view_read(void *ctx)
{
    dhcp_view_t *v = ctx;
    memset(v, 0, sizeof(*v));
    struct netif *lw = esp_netif_get_netif_impl(s_sta);
    if (lw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    v->ip = ip4_addr_get_u32(netif_ip4_addr(lw));
    const struct dhcp *d = netif_dhcp_data(lw);
    if (d != NULL) {
        v->bound = d->state == DHCP_STATE_BOUND;
        v->leased = v->bound || d->state == DHCP_STATE_RENEWING;
        if (d->t0_timeout > d->lease_used) {
            uint64_t left = (uint64_t)(d->t0_timeout - d->lease_used) * DHCP_COARSE_TIMER_SECS;
            v->left_s = left > UINT32_MAX ? UINT32_MAX : (uint32_t)left;
        }
    }
    return ESP_OK;
}

/* The lease and the address the gate opens with: at each new address and every RSSI_INTERVAL_S while associated. */
static void lease_refresh(void)
{
    dhcp_view_t v;
    bool ok = s_has_ip && esp_netif_tcpip_exec(dhcp_view_read, &v) == ESP_OK;
    s_lease_ok = ok && net_hold_lease_ok(v.leased, v.left_s);
    s_lease_ip = ok ? v.ip : 0;
}

/* Why a move would not keep the address now; NULL: it would. */
static const char *hold_closed_why(void)
{
    taskENTER_CRITICAL(&s_lock);
    bool queued = s_new_pending;
    taskEXIT_CRITICAL(&s_lock);
    if (s_trial.on || queued) {
        return "new Wi-Fi credentials are on trial";
    }
    if (s_leaving) {
        return "a restart is under way";
    }
    if (!s_has_ip || !s_associated || !s_wifi_started) {
        return "no address";
    }
    if (!s_lease_ok) {
        return "less than 1 min of the DHCP lease left";
    }
    return NULL;
}

/* Every pass: whether a move may keep the address now (the gate the wrapped actions read). */
static void hold_gate(void)
{
    bool open = hold_closed_why() == NULL;
    xSemaphoreTake(s_hold_mux, portMAX_DELAY);
    net_hold_gate(&s_hold, open, s_lease_ip);
    xSemaphoreGive(s_hold_mux);
}

/* Right before the station's own disconnect: that disconnect keeps the address (with the gate open). */
static void hold_arm(void)
{
    const char *why = hold_closed_why();
    bool armed = false;
    if (why == NULL) {
        xSemaphoreTake(s_hold_mux, portMAX_DELAY);
        net_hold_gate(&s_hold, true, s_lease_ip);
        armed = net_hold_arm(&s_hold, now_ms());
        xSemaphoreGive(s_hold_mux);
    }
    if (!armed) {
        ESP_LOGI(TAG, "this move does not keep the address: %s", why != NULL ? why : "the previous move is not over");
    }
}

/* A disconnect that kept the address (MSG_STA_DISCONNECTED). */
static void hold_note_kept(void)
{
    s_hold_on = true;
    char ip[16];
    taskENTER_CRITICAL(&s_lock);
    strlcpy(ip, s_st.ip, sizeof(ip));
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "keeping the address %s and the connections while the station moves (up to %u s without a link)", ip,
             (unsigned)NET_HOLD_AWAY_S);
}

/* lwIP context: the link down and up again, as on another network: the DHCP
 * client asks to keep its address (INIT-REBOOT, the address stays in use
 * meanwhile) and the multicast memberships are reported again. */
static esp_err_t relink(void *ctx)
{
    struct netif *lw = esp_netif_get_netif_impl((esp_netif_t *)ctx);
    if (lw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    netif_set_link_down(lw);
    netif_set_link_up(lw);
    return ESP_OK;
}

/* The association after a kept disconnect (MSG_STA_CONNECTED): DHCP confirms the address on the new link. */
static void hold_back(uint8_t channel)
{
    esp_err_t err = esp_netif_tcpip_exec(relink, s_sta);
    if (err == ESP_OK) {
        xSemaphoreTake(s_hold_mux, portMAX_DELAY);
        net_hold_verify(&s_hold, now_ms());
        xSemaphoreGive(s_hold_mux);
    }
    /* The responder stopped on this interface at the disconnect and would start again only with a new address. */
    if (s_mdns_on) {
        esp_err_t e = mdns_netif_action(s_sta, MDNS_EVENT_ENABLE_IP4 | MDNS_EVENT_ENABLE_IP6);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "mDNS not restarted on the new access point (%s): reached by its IP address",
                     esp_err_to_name(e));
        }
    }
    char ip[16];
    taskENTER_CRITICAL(&s_lock);
    strlcpy(ip, s_st.ip, sizeof(ip));
    taskEXIT_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "associated again (channel %u): asking the DHCP server to confirm %s", channel, ip);
    } else {
        ESP_LOGW(TAG, "associated again (channel %u): %s not checked with the DHCP server (%s)", channel, ip,
                 esp_err_to_name(err));
    }
}

static void ip_text(uint32_t ip, char *out, size_t cap)
{
    esp_ip4_addr_t a = { .addr = ip };
    snprintf(out, cap, IPSTR, IP2STR(&a));
}

/* Every pass while a hold runs: its outcome. The interface is changed under s_hold_mux, the rest after it. */
static void hold_tick(void)
{
    xSemaphoreTake(s_hold_mux, portMAX_DELAY);
    uint8_t state = s_hold.state;
    xSemaphoreGive(s_hold_mux);
    if (state == NET_HOLD_IDLE) {
        hold_gate();
        return;
    }
    dhcp_view_t v = { 0 };
    bool read = state == NET_HOLD_BACK && esp_netif_tcpip_exec(dhcp_view_read, &v) == ESP_OK;
    xSemaphoreTake(s_hold_mux, portMAX_DELAY);
    uint32_t kept = s_hold.kept_ip;
    net_hold_out_t out = net_hold_check(&s_hold, read && v.bound, read ? v.ip : kept, now_ms());
    if (out == NET_HOLD_EXPIRED || out == NET_HOLD_LOST) {
        /* As the default handler would have at the disconnect: the address goes, lwIP aborts the sessions. */
        __real_esp_netif_action_disconnected(s_sta, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    }
    if (out == NET_HOLD_LOST) {
        /* Associated: up again with a fresh DHCP lease, as at an ordinary association. */
        __real_esp_netif_action_connected(s_sta, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
    }
    xSemaphoreGive(s_hold_mux);
    char was[16];
    char now[16];
    ip_text(kept, was, sizeof(was));
    ip_text(v.ip, now, sizeof(now));
    switch (out) {
    case NET_HOLD_EXPIRED:
        s_hold_on = false;
        ESP_LOGW(TAG, "no access point within %u s: the address is given up, the connections close",
                 (unsigned)NET_HOLD_AWAY_S);
        ip_lost();
        break;
    case NET_HOLD_LOST:
        s_hold_on = false;
        ESP_LOGW(TAG, "the DHCP server does not keep %s on the new access point (%s): the connections close, a new "
                      "address follows",
                 was, v.ip != 0 ? now : "no address");
        ip_lost();
        break;
    case NET_HOLD_UNCONFIRMED:
    case NET_HOLD_CONFIRMED:
        if (out == NET_HOLD_UNCONFIRMED) {
            ESP_LOGW(TAG, "no answer from the DHCP server in %u s: %s is kept for the rest of its lease",
                     (unsigned)NET_HOLD_VERIFY_S, was);
        }
        s_hold_on = false;
        move_done(true);
        lease_refresh();
        break;
    default:
        break;
    }
}

/*
 * Something else ends a hold (new credentials, a recovery step, Wi-Fi
 * stopping, lost events): the interface as the default handlers would have
 * left it: down, and up again with a fresh DHCP lease while associated.
 */
static void hold_cut(const char *why)
{
    xSemaphoreTake(s_hold_mux, portMAX_DELAY);
    bool running = s_hold.state != NET_HOLD_IDLE;
    if (!running) {
        (void)net_hold_end(&s_hold);   /* none runs: the gate closes and an arming goes */
    }
    xSemaphoreGive(s_hold_mux);
    if (!running) {
        return;
    }
    /* Read before s_hold_mux is taken: the Wi-Fi task may be waiting for the event task, which may wait for it.
     * Should the link change in between, its event follows and the interface follows it (the hold is over). */
    wifi_ap_record_t ap;
    bool linked = s_wifi_started && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    xSemaphoreTake(s_hold_mux, portMAX_DELAY);
    net_hold_state_t was = net_hold_end(&s_hold);
    if (was != NET_HOLD_IDLE) {
        __real_esp_netif_action_disconnected(s_sta, WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
        if (linked) {
            __real_esp_netif_action_connected(s_sta, WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, NULL);
        }
    }
    xSemaphoreGive(s_hold_mux);
    if (was == NET_HOLD_IDLE) {
        return;
    }
    s_hold_on = false;
    ESP_LOGW(TAG, "the move to another access point is cut short (%s): the address is given up", why);
    ip_lost();
}

/* The station's own move: a deliberate reconnect, whose connect scan joins the strongest access point. */
static void own_move(net_roam_by_t by)
{
    uint8_t bssid[6];
    taskENTER_CRITICAL(&s_lock);
    bool known = s_st.bssid_known;
    memcpy(bssid, s_st.bssid, sizeof(bssid));
    uint8_t channel = s_st.channel;
    int8_t rssi = s_st.rssi;
    taskEXIT_CRITICAL(&s_lock);
    move_begin(by, known, bssid, channel, rssi);
    if (by == NET_ROAM_BY_REQUEST) {
        ESP_LOGW(TAG, "moving on request: reconnecting to the strongest access point of the network (%d dBm here, "
                      "channel %u)",
                 rssi, channel);
    } else {
        ESP_LOGW(TAG, "moving to a stronger access point: %d dBm on channel %u found twice, %d dBm here (channel %u)",
                 s_roam.cand.rssi, s_roam.cand.channel, net_roam_avg_dbm(&s_roam), channel);
    }
    s_attempt = 0;
    hold_arm();
    disconnect_then_connect();
}

/* network_move_now: the station's own move at once, under the same holds as any move. */
static void move_now(void)
{
    roam_hold_t h = roam_hold();
    const char *why = h != ROAM_HOLD_NONE                              ? roam_hold_text(h)
                      : s_move_on                                       ? "a move is under way"
                      : (!s_has_ip || s_sta_state != STA_CONNECTED)    ? "not online"
                                                                        : NULL;
    if (why != NULL) {
        ESP_LOGW(TAG, "move on request refused: %s", why);
        return;
    }
    net_roam_moved(&s_roam, now_s());
    own_move(NET_ROAM_BY_REQUEST);
}

/* Once a second: the gate for the access point's requests, then the station's own check. */
static void roam_tick(uint32_t now)
{
    roam_hold_t h = roam_hold();
    roam_hold_t gate = (h == ROAM_HOLD_NONE && !net_roam_btm_open(&s_roam, false, now)) ? ROAM_HOLD_GAP : h;
    __atomic_store_n(&s_btm_hold, (uint8_t)gate, __ATOMIC_RELAXED);
    taskENTER_CRITICAL(&s_lock);
    bool user_scan = s_scanning;
    bool ladder = s_st.ladder_active;
    taskEXIT_CRITICAL(&s_lock);
    if (user_scan) {
        return;   /* no roaming scan beside the user's */
    }
    bool held = h != ROAM_HOLD_NONE || s_leaving || ladder || s_move_on || !s_has_ip || s_sta_state != STA_CONNECTED;
    switch (net_roam_tick(&s_roam, held, now)) {
    case NET_ROAM_SCAN:
        roam_scan_start(now);
        break;
    case NET_ROAM_GO:
        own_move(NET_ROAM_BY_STATION);
        break;
    default:
        break;
    }
}

/* The station's own configuration again (no access point named), after a move the supplicant pointed at one failed. */
static void restore_sta_config(void)
{
    wifi_config_t cfg;
    build_sta_config(&cfg);
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    memset(&cfg, 0, sizeof(cfg));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "station config: %s", esp_err_to_name(err));
    }
}
#endif /* CONFIG_NETWORK_ROAM */

#if CONFIG_ESP_WIFI_WNM_SUPPORT
#define WNM_ACTION_BSS_TM_REQUEST 7   /* IEEE 802.11 WNM action: BSS transition management request */

struct wpa_supplicant;
void __real_ieee802_11_rx_wnm_action(struct wpa_supplicant *wpa_s, uint8_t *sender, uint8_t *payload, size_t len);
void __wrap_ieee802_11_rx_wnm_action(struct wpa_supplicant *wpa_s, uint8_t *sender, uint8_t *payload, size_t len);

/*
 * The supplicant's handler of the access point's 802.11v action frames,
 * wrapped at link time (CMakeLists.txt); it runs on the Wi-Fi driver's task.
 * payload: the action, then for a BSS transition request its dialog token
 * and request mode. A request the gate keeps out never reaches the
 * supplicant: no answer, no scan, no move. An update is checked here too, so
 * one that starts between two ticks of the net task is covered at once.
 * Without CONFIG_NETWORK_ROAM the station does not take such requests
 * (btm_enabled is off) and this only passes frames on.
 */
void __wrap_ieee802_11_rx_wnm_action(struct wpa_supplicant *wpa_s, uint8_t *sender, uint8_t *payload, size_t len)
{
#if CONFIG_NETWORK_ROAM
    if (payload != NULL && len >= 3 && payload[0] == WNM_ACTION_BSS_TM_REQUEST) {
        uint8_t mode = payload[2];
        const char *why = NULL;
        if (lifecycle_update_active() != LIFECYCLE_UPDATE_NONE) {
            why = roam_hold_text(ROAM_HOLD_UPDATE);
        } else if (!lifecycle_reboot_inhibited(&why)) {
            roam_hold_t h = (roam_hold_t)__atomic_load_n(&s_btm_hold, __ATOMIC_RELAXED);
            why = h != ROAM_HOLD_NONE ? roam_hold_text(h) : NULL;
        }
        if (!net_roam_btm_pass(why == NULL, mode)) {
            ESP_LOGW(TAG, "the access point asks the station to move to another one (request mode 0x%02x): ignored, %s",
                     mode, why);
            return;
        }
        ESP_LOGI(TAG, "the access point asks the station to move to another one (request mode 0x%02x)%s", mode,
                 why != NULL ? ": taken, it disconnects the station anyway" : "");
    }
#endif
    __real_ieee802_11_rx_wnm_action(wpa_s, sender, payload, len);
}
#endif /* CONFIG_ESP_WIFI_WNM_SUPPORT */

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
    taskENTER_CRITICAL(&s_lock);
    bool user = s_scanning;
#if CONFIG_NETWORK_ROAM
    bool roam = s_roam_scanning;
#endif
    taskEXIT_CRITICAL(&s_lock);
#if CONFIG_NETWORK_ROAM
    if (roam) {
        roam_scan_done();
        return;
    }
#endif
    if (!user) {
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

static void trial_revert(network_cred_revert_t why);

/* held: the interface kept its address and sessions (a move, net_hold_t). */
static void handle_disconnected(uint8_t reason, int8_t rssi, bool held)
{
    bool was_linked = s_associated;
    s_associated = false;
    s_trial_assoc = false;   /* only the next association can confirm trial credentials */
#if CONFIG_NETWORK_ROAM
    uint8_t from_bssid[6];   /* the link that ended: where a move the access point asked for starts */
    net_roam_disassociated(&s_roam);
    s_steer_read = false;
#endif
    taskENTER_CRITICAL(&s_lock);
#if CONFIG_NETWORK_ROAM
    bool from_known = s_st.bssid_known;
    memcpy(from_bssid, s_st.bssid, sizeof(from_bssid));
    uint8_t from_channel = s_st.channel;
    int8_t from_rssi = s_st.rssi;
    s_st.ap_steering = -1;
#endif
    s_st.associated = false;
    s_st.bssid_known = false;
    s_st.tx_power_known = false;
    s_st.last_disconnect_reason = reason;
    taskEXIT_CRITICAL(&s_lock);
    if (!held) {
        ip_lost();   /* IDF's default handler has already cleared the address */
    }
#if CONFIG_NETWORK_ROAM
    /* A move that kept the address failed: this disconnect was not kept, so the interface went down with it. */
    bool hold_failed = !held && s_hold_on;
    s_hold_on = false;
#endif

    if (s_leaving) {
        /* Our own Wi-Fi stop on the way to a planned restart: the journal's
         * restart record covers it, and nothing reconnects any more. */
#if CONFIG_NETWORK_ROAM
        if (held) {
            hold_cut("a restart is under way");
        }
#endif
        s_sta_state = STA_IDLE;
        return;
    }

    if (s_sta_state == STA_DISCONNECTING) {
#if CONFIG_NETWORK_ROAM
        if (held) {
            hold_note_kept();   /* the station's own move */
        }
#endif
        connect_now();   /* deliberate: no backoff */
        return;
    }
    if (s_sta_state == STA_IDLE || !s_sta_configured) {
#if CONFIG_NETWORK_ROAM
        if (held) {
            hold_cut("the station stops");
        }
#endif
        return;          /* stopping, restarting or unconfigured */
    }
#if CONFIG_NETWORK_ROAM
    if (reason == WIFI_REASON_ROAMING) {
        /* The supplicant moves the station to the access point the access
         * point named (802.11v): the driver connects by itself, and the net
         * task waits for it as for its own connect. */
        if (!s_move_on) {
            move_begin(NET_ROAM_BY_AP, from_known && was_linked, from_bssid, was_linked ? from_channel : 0,
                       net_rssi_known(rssi) ? rssi : (was_linked ? from_rssi : 0));
            net_roam_moved(&s_roam, now_s());
            ESP_LOGI(TAG, "moving to another access point at the access point's request (from channel %u, %d dBm)",
                     s_move_ch_from, s_move_rssi_from);
        }
        if (held) {
            hold_note_kept();
        }
        s_sta_state = STA_CONNECTING;
        s_deadline_us = esp_timer_get_time() + (int64_t)CONNECT_WATCHDOG_MS * 1000;
        return;
    }
    if (held) {
        hold_cut("not a move");   /* only a move's disconnect is kept: never expected */
    }
    if (hold_failed && !s_move_on) {
        ESP_LOGW(TAG, "the move to another access point failed (reason %u): the address is given up", reason);
    }
    if (s_move_on) {
        /* The move did not reach an address: an outage from here on, recorded like any other. */
        ESP_LOGW(TAG, "the move to another access point did not complete (reason %u): connecting again%s", reason,
                 hold_failed ? ", the address is given up" : "");
        bool by_ap = s_move_by == NET_ROAM_BY_AP;
        s_move_on = false;
        if (by_ap) {
            restore_sta_config();   /* the supplicant named one access point: any of the network again */
        }
    }
#endif
    if (!was_linked) {
        taskENTER_CRITICAL(&s_lock);
        s_st.connect_failures++;
        taskEXIT_CRITICAL(&s_lock);
    }
    /* The driver reports -128 when it has no reading: recorded as 0 (unknown). */
    bool rssi_known = net_rssi_known(rssi);
    if (net_drop_log_should(&s_drop, was_linked, reason, now_s())) {
        if (was_linked) {
            if (rssi_known) {
                ESP_LOGW(TAG, "link lost: reason %u, RSSI %d dBm", reason, rssi);
            } else {
                ESP_LOGW(TAG, "link lost: reason %u, RSSI unknown", reason);
            }
        } else {
            ESP_LOGI(TAG, "connect failed: reason %u", reason);
        }
        if (!s_drop_pending) {
            s_drop_pending = true;
            s_drop_reason = reason;
            s_drop_rssi = rssi_known ? rssi : 0;
        }
        flush_drop_record();
    }
    /* A refused attempt of trial credentials that never associated is
     * retried at once (the connect scans first): the refusals that end the
     * trial then come back to back. Decided before this one is counted. */
    schedule_retry_ms(net_trial_retry_ms(&s_trial, reason, s_attempt));
    /* A network that refuses the new password ends their trial now, not
     * after its 90 s (apply_credentials connects again at once). */
    if (net_cred_trial_disconnect(&s_trial, reason) == NET_TRIAL_REVERT_AUTH) {
        trial_revert(NETWORK_CRED_REVERT_AUTH);
    }
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
 * only tries them first once more. Not while a newer change waits to be
 * applied (it replaces this trial), and the pending pair is cleared only
 * when it is the one confirmed, so a correction made meanwhile survives. */
static void trial_confirmed(void)
{
    xSemaphoreTake(s_cred_mux, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    bool newer = s_new_pending;
    bool pend_same = s_pend_valid && strcmp(s_pend_ssid, s_ssid) == 0 && strcmp(s_pend_pass, s_pass) == 0;
    s_trial_pub = s_trial;   /* over (confirmed): published with the pending pair it cleared */
    if (!newer) {
        strlcpy(s_act_ssid, s_ssid, sizeof(s_act_ssid));
        strlcpy(s_act_pass, s_pass, sizeof(s_act_pass));
        if (pend_same) {
            memset(s_pend_pass, 0, sizeof(s_pend_pass));
            s_pend_ssid[0] = '\0';
            s_pend_valid = false;
            s_cred_reverted = false;
            s_cred_reverted_why = NETWORK_CRED_REVERT_NONE;
        }
    }
    bool other_pending = s_pend_valid;
    publish_cred_locked();
    taskEXIT_CRITICAL(&s_lock);
    if (newer) {
        xSemaphoreGive(s_cred_mux);
        ESP_LOGI(TAG, "Wi-Fi credentials for \"%s\" got an IP, but newer ones are about to be tried", s_ssid);
        return;
    }
    ESP_LOGI(TAG, "new Wi-Fi credentials for \"%s\" confirmed", s_ssid);
    if (settings_is_available()) {
        esp_err_t err = store_pair(SETTINGS_KEY_WIFI_SSID, SETTINGS_KEY_WIFI_PASS, s_ssid, s_pass);
        if (err == ESP_OK && !other_pending) {
            err = erase_pending_keys();
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "confirmed Wi-Fi credentials not stored: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_cred_mux);
}

/* No IP in time with the pending credentials, or the network refused them:
 * back to the stored ones. */
static void trial_revert(network_cred_revert_t why)
{
    taskENTER_CRITICAL(&s_lock);
    bool newer = s_new_pending;
    if (newer || !s_trial_revert_ok) {
        s_trial_pub = s_trial;   /* this trial is over either way */
        publish_cred_locked();
    }
    taskEXIT_CRITICAL(&s_lock);
    if (newer) {
        return;   /* a newer change replaces this trial within a second */
    }
    bool refused = (why == NETWORK_CRED_REVERT_AUTH);
    if (!s_trial_revert_ok) {
        if (refused) {
            ESP_LOGW(TAG, "\"%s\" refused the Wi-Fi password %u times in a row; no previous credentials, still trying",
                     s_ssid, (unsigned)NET_CRED_TRIAL_REFUSALS);
        } else {
            ESP_LOGW(TAG, "Wi-Fi credentials for \"%s\" got no IP in %u s; no previous credentials, still trying",
                     s_ssid, (unsigned)NET_CRED_TRIAL_S);
        }
        return;
    }
    char tried[33];
    strlcpy(tried, s_ssid, sizeof(tried));
    taskENTER_CRITICAL(&s_lock);
    strlcpy(s_ssid, s_act_ssid, sizeof(s_ssid));
    strlcpy(s_pass, s_act_pass, sizeof(s_pass));
    s_cred_reverted = true;
    s_cred_reverted_why = (uint8_t)why;
    s_trial_pub = s_trial;
    publish_cred_locked();
    taskEXIT_CRITICAL(&s_lock);
    if (refused) {
        ESP_LOGW(TAG, "\"%s\" refused the new Wi-Fi password (%u attempts in a row): back to \"%s\" (the new "
                      "credentials stay pending)",
                 tried, (unsigned)NET_CRED_TRIAL_REFUSALS, s_ssid);
    } else {
        ESP_LOGW(TAG, "Wi-Fi credentials for \"%s\" got no IP in %u s: back to \"%s\" (the new ones stay pending)",
                 tried, (unsigned)NET_CRED_TRIAL_S, s_ssid);
    }
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
#if CONFIG_NETWORK_ROAM
    if (!was) {
        move_done(false);   /* a move's own record instead of the return after an outage */
    }
    lease_refresh();   /* a new lease: how long a move may keep it */
#endif
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
     * timeout is left to housekeeping. A newer change waiting to be applied
     * replaces the trial instead (trial_confirmed checks again under the
     * credential lock). */
    taskENTER_CRITICAL(&s_lock);
    bool newer = s_new_pending;
    taskEXIT_CRITICAL(&s_lock);
    if (s_trial.on && s_trial_assoc && !newer &&
        net_cred_trial_tick(&s_trial, true, s_ip_since_s) == NET_TRIAL_CONFIRMED) {
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
#if CONFIG_NETWORK_QEMU_ETHERNET
    ESP_LOGW(TAG, "QEMU: recovery %d skipped (emulated Ethernet, no Wi-Fi)", (int)action);
    return;
#endif
    /* A restart also helps when Wi-Fi did not start: it tries once more now. */
    if (!s_sta_configured || (!s_wifi_started && action != NETWORK_RECOVER_RESTART_WIFI)) {
        ESP_LOGW(TAG, "recovery %d skipped: station not running", (int)action);
        return;
    }
#if CONFIG_NETWORK_ROAM
    hold_cut("a network recovery step");
#endif
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
#if CONFIG_NETWORK_QEMU_ETHERNET
    /* The emulated port is the configured station; stored Wi-Fi credentials are not used. */
    strlcpy(s_ssid, QEMU_ETH_LABEL, sizeof(s_ssid));
    s_sta_configured = true;
    taskENTER_CRITICAL(&s_lock);
    s_st.sta_configured = true;
    strlcpy(s_st.ssid, s_ssid, sizeof(s_st.ssid));
    taskEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "QEMU development build: emulated Ethernet instead of Wi-Fi, no setup AP");
    return esp_eth_start(s_eth);   /* the link event connects, DHCP gives the address */
#endif
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
            trial_publish();
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
        /* held: the association after a move that kept the address (net_hold_t); the interface stayed up with
         * its addresses, IPv6 link-local included, so no new one is made (its duplicate check would hold it back). */
        bool held = m->held != 0;
        s_associated = true;
        s_sta_state = STA_CONNECTED;
        if (!held) {
            ip6_link_up(s_sta, "station");
        }
        if (!s_has_ip) {
            s_noip_since_s = now_s();
        }
        if (s_trial.on) {
            s_trial_assoc = true;   /* a link the trial credentials made */
            net_cred_trial_associated(&s_trial);
        }
        bool moving = false;   /* a move between access points is no drop to count */
#if CONFIG_NETWORK_ROAM
        moving = s_move_on;
#endif
        if (s_had_link && !moving) {
            taskENTER_CRITICAL(&s_lock);
            s_st.reconnects++;
            taskEXIT_CRITICAL(&s_lock);
        }
        s_had_link = true;
        wifi_ap_record_t ap;
        int8_t power = 0;
#if CONFIG_NETWORK_QEMU_ETHERNET
        memset(&ap, 0, sizeof(ap));
        bool have = false;   /* no access point behind the emulated link */
        bool have_power = false;
#else
        bool have = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);
        bool have_power = have && esp_wifi_get_max_tx_power(&power) == ESP_OK;
#endif
#if CONFIG_NETWORK_ROAM
        if (!have) {
            memset(ap.bssid, 0, sizeof(ap.bssid));   /* no access point known: never the "same one" */
        }
        net_roam_associated(&s_roam, ap.bssid, now_s());
        if (have) {
            net_roam_rssi(&s_roam, ap.rssi);
        }
        s_steer_read = false;
        move_associated(have, &ap);
#endif
        taskENTER_CRITICAL(&s_lock);
        s_st.associated = true;
        s_st.bssid_known = have;
        s_st.tx_power_known = have_power;
        if (have) {
            s_st.rssi = ap.rssi;
            s_st.channel = ap.primary;
            memcpy(s_st.bssid, ap.bssid, sizeof(s_st.bssid));
        }
        if (have_power) {
            s_st.tx_power_qdbm = power;
        }
        taskEXIT_CRITICAL(&s_lock);
#if CONFIG_NETWORK_ROAM
        if (held) {
            hold_back(have ? ap.primary : 0);
        }
#endif
        break;
    }
    case MSG_STA_DISCONNECTED:
        handle_disconnected(m->reason, m->rssi, m->held != 0);
        break;
    case MSG_GOT_IP:
        handle_got_ip();
        break;
    case MSG_LOST_IP:
        ip_lost();
        break;
    case MSG_AP_START:
#if !CONFIG_NETWORK_QEMU_ETHERNET
        ip6_link_up(s_ap, "access point");
#endif
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
    case MSG_GOT_IP6:
        ip6_note(m->reason);
        break;
    case MSG_MOVE_NOW:
#if CONFIG_NETWORK_ROAM
        move_now();
#endif
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
#if CONFIG_NETWORK_ROAM
    hold_cut("events were lost");   /* the interface follows the link again; what follows reads the driver */
#endif
#if CONFIG_NETWORK_QEMU_ETHERNET
    bool linked = s_eth_link;
#else
    wifi_ap_record_t ap;
    bool linked = s_wifi_started && esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
#endif
    esp_netif_ip_info_t info = { 0 };
    bool ip = linked && esp_netif_get_ip_info(s_sta, &info) == ESP_OK && info.ip.addr != 0;
    if (!linked && s_associated) {
        handle_disconnected(0, 0, false);
    } else if (linked && !s_associated) {
        s_associated = true;
        s_sta_state = STA_CONNECTED;
        ip6_link_up(s_sta, "station");   /* its link-up message was lost */
#if CONFIG_NETWORK_ROAM
        net_roam_associated(&s_roam, ap.bssid, now_s());
#endif
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
    uint32_t apply_s = now_s();
    taskENTER_CRITICAL(&s_lock);
    if (s_new_pending && now_us >= s_apply_at_us) {
        strlcpy(s_ssid, s_new_ssid, sizeof(s_ssid));
        strlcpy(s_pass, s_new_pass, sizeof(s_pass));
        memset(s_new_pass, 0, sizeof(s_new_pass));
        s_new_pending = false;
        if (s_new_trial) {
            s_cred_reverted = false;   /* a new trial: an earlier revert no longer applies */
            s_cred_reverted_why = NETWORK_CRED_REVERT_NONE;
            net_cred_trial_start(&s_trial, apply_s);
            s_trial_assoc = false;
        } else {
            s_trial.on = false;
        }
        /* The queued change becomes the running trial (or ends one) in one step for readers. */
        s_trial_pub = s_trial;
        publish_cred_locked();
        s_trial_revert_ok = net_ssid_valid(s_act_ssid);
        apply = true;
    }
    taskEXIT_CRITICAL(&s_lock);
    if (apply) {
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
#if CONFIG_NETWORK_ROAM
    taskENTER_CRITICAL(&s_lock);
    bool roam_stuck = s_roam_scanning && now_us - s_roam_scan_started_us > (int64_t)SCAN_WATCHDOG_MS * 1000;
    taskEXIT_CRITICAL(&s_lock);
    if (roam_stuck) {
        ESP_LOGW(TAG, "roaming scan did not finish: abandoned");
        (void)esp_wifi_scan_stop();
        (void)esp_wifi_clear_ap_list();
        roam_scan_end();
        net_roam_scan_failed(&s_roam, now_s());
    }
#endif

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
#if CONFIG_NETWORK_ROAM
            if (s_move_on) {
                bool by_ap = s_move_by == NET_ROAM_BY_AP;
                s_move_on = false;   /* an outage from here on */
                if (by_ap) {
                    restore_sta_config();
                }
            }
#endif
            schedule_retry();
        }
        break;
    default:
        break;
    }
#if CONFIG_NETWORK_ROAM
    hold_tick();   /* every pass: a move that keeps the address waits seconds, not ticks */
#endif

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
#if !CONFIG_NETWORK_QEMU_ETHERNET   /* the emulated link has no signal to read */
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            int8_t power = 0;
            bool have_power = esp_wifi_get_max_tx_power(&power) == ESP_OK;
#if CONFIG_NETWORK_ROAM
            net_roam_rssi(&s_roam, ap.rssi);
            lease_refresh();
            /* Once per association, a few seconds in (the supplicant has the
             * access point's capabilities by then): whether it offers 802.11v
             * BSS transition management, and so whether its requests can come. */
            int8_t steer = -1;
            if (!s_steer_read && (uint32_t)(now - s_roam.assoc_s) >= ROAM_STEER_READ_S) {
                s_steer_read = true;
#if CONFIG_ESP_WIFI_WNM_SUPPORT
                steer = esp_wnm_is_btm_supported_connection() ? 1 : 0;
#endif
            }
#endif
            taskENTER_CRITICAL(&s_lock);
            s_st.rssi = ap.rssi;
            s_st.channel = ap.primary;
            s_st.bssid_known = true;
            memcpy(s_st.bssid, ap.bssid, sizeof(s_st.bssid));
            if (have_power) {
                s_st.tx_power_known = true;
                s_st.tx_power_qdbm = power;
            }
#if CONFIG_NETWORK_ROAM
            if (steer >= 0) {
                s_st.ap_steering = steer;
            }
#endif
            taskEXIT_CRITICAL(&s_lock);
        }
#endif
    }
#if CONFIG_NETWORK_ROAM
    roam_tick(now);
    flush_move_record();
#endif
    if (now - s_last_check_s >= CHECK_INTERVAL_S) {
        s_last_check_s = now;
        ensure_fallback_dns();
        ensure_sntp_servers();
    }
    flush_drop_record();
    flush_up_record();
    if (net_cred_trial_tick(&s_trial, false, now) == NET_TRIAL_REVERT) {
        trial_revert(NETWORK_CRED_REVERT_NO_IP);
    }
    mdns_note_rename();

    uint32_t retry_in = 0;
    if (s_sta_state == STA_WAIT_RETRY && s_retry_at_us > now_us) {
        retry_in = (uint32_t)((s_retry_at_us - now_us + 999999) / 1000000);
    }
    taskENTER_CRITICAL(&s_lock);
    s_st.offline_s = (s_sta_configured && !s_has_ip) ? now - s_offline_since_s : 0;
    s_st.assoc_no_ip_s = (s_associated && !s_has_ip) ? now - s_noip_since_s : 0;
    s_st.retry_in_s = retry_in;
#if CONFIG_NETWORK_ROAM
    s_st.moving = s_move_on;
#endif
    s_trial_pub = s_trial;
    publish_cred_locked();
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
 * be started again by the AP start. (No AP interface in the QEMU build.) */
#if !CONFIG_NETWORK_QEMU_ETHERNET
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
#endif

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
    s_st.ap_close_in_s = UINT32_MAX;
    s_st.ap_steering = -1;
    taskEXIT_CRITICAL(&s_lock);
#if CONFIG_NETWORK_ROAM
    const net_roam_cfg_t roam_cfg = {
        .weak_dbm = CONFIG_NETWORK_ROAM_WEAK_DBM,
        .gain_db = CONFIG_NETWORK_ROAM_MIN_GAIN_DB,
        .min_interval_s = CONFIG_NETWORK_ROAM_MIN_INTERVAL_S,
    };
    net_roam_init(&s_roam, &roam_cfg);
    net_hold_init(&s_hold);
    s_hold_mux = xSemaphoreCreateMutex();
    if (s_hold_mux == NULL) {
        return ESP_ERR_NO_MEM;
    }
#endif

    s_queue = xQueueCreate(NET_QUEUE_LEN, sizeof(net_msg_t));
    s_start_done = xSemaphoreCreateBinary();
    s_dns_done = xSemaphoreCreateBinary();
    s_cred_mux = xSemaphoreCreateMutex();
    s_probe_events = xEventGroupCreate();
    if (s_queue == NULL || s_start_done == NULL || s_dns_done == NULL || s_cred_mux == NULL || s_probe_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    xEventGroupSetBits(s_probe_events, PROBE_IDLE_BIT);

#if CONFIG_NETWORK_QEMU_ETHERNET
    esp_err_t err = qemu_eth_init();   /* no Wi-Fi init, no AP interface */
    if (err != ESP_OK) {
        return err;
    }
    load_hostname();
    load_fallback_dns();
#else
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
#endif
    setup_ap_name();
    mdns_setup();

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL, NULL);
    /* After the default station handlers (registered with the station interface above): see on_sta_link. */
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, on_sta_link, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_sta_link, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_ip_event, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, on_ip_event, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_GOT_IP6, on_ip_event, NULL, NULL);
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
    /* Its stack in PSRAM: the probe only resolves a name and connects (lwIP)
     * and logs; it never reads or writes flash, so it never runs while the
     * cache is off. The task is never deleted. */
    if (xTaskCreatePinnedToCoreWithCaps(probe_task, "netprobe", PROBE_TASK_STACK, NULL, PROBE_TASK_PRIO, &s_probe_task,
                                        tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    net_ladder_init(&s_ladder);
    esp_err_t herr = lifecycle_register_shutdown_hook(on_shutdown, NULL);
    if (herr != ESP_OK) {
        ESP_LOGW(TAG, "no shutdown hook (%s): a planned restart journals its own Wi-Fi stop as a drop",
                 esp_err_to_name(herr));
    }
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
    publish_cred_locked();   /* the trial's countdown to the second */
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

esp_err_t network_set_credentials(const char *ssid, const char *password, network_cred_plan_t *plan_out)
{
    if (ssid == NULL || password == NULL || !net_ssid_valid(ssid) || !net_password_valid(password)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_cred_mux, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    bool have = net_ssid_valid(s_act_ssid);
    bool same = have && strcmp(s_act_ssid, ssid) == 0 && strcmp(s_act_pass, password) == 0;
    taskEXIT_CRITICAL(&s_lock);
#if CONFIG_NETWORK_QEMU_ETHERNET
    const bool wifi_used = false;   /* the emulated Ethernet port is the station interface */
#else
    const bool wifi_used = true;
#endif
    net_cred_plan_t plan = net_cred_plan(have, same, wifi_used);

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
            xSemaphoreGive(s_cred_mux);
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
    s_cred_reverted_why = NETWORK_CRED_REVERT_NONE;
    if (plan != NET_CRED_PLAN_STORE_ONLY) {
        queue_apply_locked(ssid, password, plan == NET_CRED_PLAN_TRIAL);
    }
    publish_cred_locked();
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_cred_mux);
    if (plan_out != NULL) {
        *plan_out = plan == NET_CRED_PLAN_TRIAL        ? NETWORK_CRED_PLAN_TRIAL
                    : plan == NET_CRED_PLAN_STORE_ONLY ? NETWORK_CRED_PLAN_STORE_ONLY
                                                       : NETWORK_CRED_PLAN_APPLY;
    }
    ESP_LOGI(TAG, "Wi-Fi credentials for \"%s\" %s", ssid,
             plan == NET_CRED_PLAN_TRIAL        ? "stored as pending: tried now, kept if they get an IP"
             : plan == NET_CRED_PLAN_STORE_ONLY ? "stored, not used: the emulated Ethernet stays the station's link"
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
        s_cred_reverted_why = NETWORK_CRED_REVERT_NONE;
        queue_apply_locked(s_pend_ssid, s_pend_pass, true);
        publish_cred_locked();
    }
    taskEXIT_CRITICAL(&s_lock);
    return pend ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t network_discard_pending_credentials(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_cred_mux, portMAX_DELAY);
    taskENTER_CRITICAL(&s_lock);
    bool pend = s_pend_valid;
    taskEXIT_CRITICAL(&s_lock);
    if (!pend) {
        xSemaphoreGive(s_cred_mux);
        return ESP_ERR_NOT_FOUND;
    }
    if (settings_is_available()) {
        esp_err_t err = erase_pending_keys();
        if (err != ESP_OK) {
            xSemaphoreGive(s_cred_mux);
            return err;
        }
    }
    taskENTER_CRITICAL(&s_lock);
    bool on_trial = s_trial_pub.on || (s_new_pending && s_new_trial);
    memset(s_pend_pass, 0, sizeof(s_pend_pass));
    s_pend_ssid[0] = '\0';
    s_pend_valid = false;
    s_cred_reverted = false;
    s_cred_reverted_why = NETWORK_CRED_REVERT_NONE;
    if (on_trial && net_ssid_valid(s_act_ssid)) {
        queue_apply_locked(s_act_ssid, s_act_pass, false);   /* back to the station's credentials */
    } else if (s_new_pending && s_new_trial) {
        s_new_pending = false;
    }
    publish_cred_locked();
    taskEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_cred_mux);
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
    bool queued = false;
    if (ok) {
        s_scanning = true;
        s_scan_started_us = esp_timer_get_time();
#if CONFIG_NETWORK_ROAM
        /* A roaming scan runs (a second or two): this one starts when it is done. */
        queued = s_roam_scanning;
        s_user_scan_queued = queued;
#endif
    }
    taskEXIT_CRITICAL(&s_lock);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    if (queued) {
        return ESP_OK;
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

esp_err_t network_move_now(void)
{
#if CONFIG_NETWORK_ROAM
    taskENTER_CRITICAL(&s_lock);
    bool ok = s_inited && s_st.online && s_st.associated;
    taskEXIT_CRITICAL(&s_lock);
    if (!ok) {
        return ESP_ERR_INVALID_STATE;
    }
    net_msg_t m = { .type = MSG_MOVE_NOW };
    return xQueueSend(s_queue, &m, 0) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
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
    lin.has_ip = s_st.online;
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
    case NET_LADDER_PASS: {
        /* rung code 0 = "pass started", the reason byte = the triggers holding now */
        s_ladder_note = 0;
        ladder_journal(LADDER_OUTCOME_STARTED, NET_RUNG_NONE, out.triggers);
        char why[80];
        (void)net_ladder_trigger_text(out.triggers, why, sizeof(why));
        ESP_LOGW(TAG, "network ladder: pass started (%s)", why);
        break;
    }
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
