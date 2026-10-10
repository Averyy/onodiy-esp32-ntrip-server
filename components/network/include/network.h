/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Network: Wi-Fi station with a captive-portal setup AP as fallback, SNTP,
 * an upstream probe and the network recovery ladder.
 *
 * Behaviour:
 *  - No stored credentials: start the setup AP (open, or WPA2 with the
 *    wifi/ap_pass password from settings) with a DNS responder that answers
 *    every name with the AP's own address and DHCP option 114 pointing at the
 *    setup page, so phones open it automatically. The station interface stays
 *    up beside the AP (AP+STA), unconnected, so the setup page can scan.
 *  - Credentials stored: connect as a station and reconnect forever after
 *    every drop, waiting 1 s, 2 s, 4 s, 8 s, 16 s, then 30 s between
 *    attempts. A Wi-Fi outage never reboots the station and never switches it
 *    to the setup AP: after CONFIG_NETWORK_STA_FALLBACK_S without an IP the
 *    setup AP comes up *beside* the station (AP+STA, on the station's channel
 *    once it connects), and goes down again a minute after the station has an
 *    IP. The station keeps retrying all along.
 *  - Settings unavailable (degraded boot, NVS unreadable): no AP comes up on
 *    its own. Only a BOOT-button long press starts the recovery AP, so an
 *    unconfigured-looking station on a roof is never open to passers-by.
 *  - Recovery AP (network_start_setup_ap(true), BOOT button): wifi/ap_pass if
 *    set and readable, else open; down again after CONFIG_NETWORK_RECOVERY_AP_S
 *    unless a client is still connected (then when the last one leaves).
 *  - An AP that is up for setup or fallback stays up while the station tries
 *    new credentials and until a minute after it gets an IP, so the browser
 *    on the setup page receives an answer (and the new address).
 *  - Once online: SNTP (pool.ntp.org plus a DHCP-provided server; the
 *    fallback clock for logs and update checks, the availability ledger
 *    prefers GNSS time) and one upstream probe.
 *
 * Radio settings for a mains-powered streaming device: power save off
 * (esp_wifi_set_ps(WIFI_PS_NONE) after every start: no DTIM sleep latency or
 * stalls), full channel scan and strongest-AP selection (scan_method =
 * WIFI_ALL_CHANNEL_SCAN, sort_method = WIFI_CONNECT_AP_BY_SIGNAL, so mesh
 * homes pick the best node), country code from CONFIG_NETWORK_WIFI_COUNTRY
 * with 802.11d. Disconnect reasons and the RSSI at the drop go to the event
 * journal (DIAG_EV_WIFI_DISCONNECT): every drop of an established link, and
 * during an outage only changed reasons (at most one a minute) plus one
 * record every 10 min, so a long outage cannot flood the journal.
 *
 * Link drops: with ESP-IDF's default station handlers a disconnect takes the
 * interface down at once (the IP is cleared and TCP sessions bound to it
 * end), so `online` turns false and NETWORK_EVENT_OFFLINE is posted at the
 * drop, not only when IP_EVENT_STA_LOST_IP fires later.
 *
 * Scanning while connected hops channels and stalls caster traffic for
 * seconds, so the UI shows cached results (network_get_scan_results) and
 * warns before a fresh scan.
 *
 * Recovery ladder (network_ladder_tick, driven by the health monitor once a
 * second while the protected caster is not streaming but RTCM is healthy):
 * +60 s reconnect, +3 min renew DHCP and switch to the fallback DNS servers
 * (CONFIG_NETWORK_FALLBACK_DNS1/2, kept for the rest of the boot), +6 min
 * restart Wi-Fi, +10 min reboot (lifecycle_request_reboot(NETWORK_LADDER)).
 * Each rung first probes upstream: reachable -> the fault is remote (a caster)
 * and the rung is skipped; an IP but nothing beyond the router -> no reboot.
 * The reboot is also withheld while a receiver-flash journal is open (not
 * when its state is only unknown because the settings are unavailable),
 * within the minimum interval since the last network-ladder reboot (counted
 * across resets; a power cycle, or RTC state from before another app image's
 * run, starts afresh), while a reboot inhibit is held
 * and while someone is connected to the station's AP. The reconnect, renew
 * and restart rungs run regardless of the journal, so the user can still
 * reach the UI. After the reboot rung the ladder holds 10 min, then starts a
 * new pass; it resets after 60 s without the condition. Ladder timings are
 * PROVISIONAL, to be confirmed on hardware.
 * Journal: DIAG_EV_LADDER_STEP with a = 1 and b = (reason << 16) |
 * (outcome << 8) | code. Codes: 1 reconnect, 2 renew DHCP + fallback DNS,
 * 3 restart Wi-Fi, 4 reboot. Outcomes: 0 started, 1 healthy again (code =
 * the last rung started), 2 skipped or withheld with the reason
 * (network_ladder_why_t).
 *
 * Wi-Fi credentials live in the settings component; the Wi-Fi driver's own
 * NVS storage is disabled (single source of truth).
 *
 * Thread safety: every function may be called from any task. None blocks for
 * long except network_probe_upstream() with a non-zero timeout.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(NETWORK_EVENT);

/** NETWORK_EVENT ids (no event data). */
typedef enum {
    NETWORK_EVENT_ONLINE,          /**< station has an IP (and its link is up) */
    NETWORK_EVENT_OFFLINE,         /**< station lost its IP (link drop, DHCP restart, Wi-Fi restart, IP lost) */
    NETWORK_EVENT_SETUP_AP_UP,
    NETWORK_EVENT_SETUP_AP_DOWN,
    NETWORK_EVENT_TIME_SYNCED,     /**< SNTP set the clock (every sync) */
    NETWORK_EVENT_SCAN_DONE,       /**< cached scan results updated */
} network_event_t;

/** Result of the last upstream probe. */
typedef enum {
    NETWORK_PROBE_NONE = 0,        /**< not probed yet */
    NETWORK_PROBE_OK,              /**< name lookup and TCP connect went through */
    NETWORK_PROBE_NO_IP,           /**< the station has no IP */
    NETWORK_PROBE_DNS_FAILED,      /**< IP; name lookup failed, a TCP connect to a public resolver worked */
    NETWORK_PROBE_NO_UPSTREAM,     /**< IP; nothing beyond the router answered */
} network_probe_t;

/** Why the AP is up (bit flags in network_status_t.ap_reasons). */
typedef enum {
    NETWORK_AP_SETUP    = 1u << 0, /**< no station credentials */
    NETWORK_AP_FALLBACK = 1u << 1, /**< configured but offline (or waiting for the station's first IP) */
    NETWORK_AP_LINGER   = 1u << 2, /**< station just got its IP; AP closes shortly */
    NETWORK_AP_RECOVERY = 1u << 3, /**< BOOT button (or manual request), time-limited */
} network_ap_reason_t;

/** Why a ladder rung was skipped or withheld (journal reason codes). */
typedef enum {
    NETWORK_LADDER_WHY_NONE = 0,
    NETWORK_LADDER_WHY_UPSTREAM_OK,  /**< probe reached the internet: the fault is remote */
    NETWORK_LADDER_WHY_NO_UPSTREAM,  /**< IP but nothing beyond the router: a reboot cannot help */
    NETWORK_LADDER_WHY_JOURNAL_OPEN, /**< receiver-flash journal open: no reboot */
    NETWORK_LADDER_WHY_INTERVAL,     /**< last network-ladder reboot too recent */
    NETWORK_LADDER_WHY_INHIBITED,    /**< a reboot inhibit is held */
    NETWORK_LADDER_WHY_AP_IN_USE,    /**< a client is connected to the station's AP */
} network_ladder_why_t;

/** Snapshot for status pages and health. */
typedef struct {
    bool sta_configured;
    bool sta_credentials_unreadable; /**< SSID stored, password unreadable: station not started (setup AP) */
    bool online;                   /**< station has an IP */
    bool associated;               /**< station link to the access point is up */
    bool upstream_ok;              /**< last probe reached the internet */
    network_probe_t probe;         /**< last probe result */
    uint32_t probe_age_s;          /**< since the last probe finished, UINT32_MAX = never */
    bool setup_ap_active;
    uint8_t ap_reasons;            /**< network_ap_reason_t flags while the AP is up */
    uint8_t ap_clients;            /**< stations connected to the AP */
    uint32_t recovery_ap_left_s;   /**< recovery AP lifetime left (0 = none, or kept up for a connected client) */
    char ap_ssid[33];              /**< setup AP name (set once Wi-Fi is initialised) */
    bool time_synced;
    char ssid[33];
    char ip[16];
    int8_t rssi;
    uint8_t channel;
    uint32_t reconnects;           /**< station links re-established after a drop, since boot */
    uint32_t connect_failures;     /**< connect attempts that ended without a link, since boot */
    uint32_t offline_s;            /**< seconds offline in the current outage, 0 if online */
    uint32_t retry_in_s;           /**< seconds to the next connect attempt (0 = connecting or connected) */
    uint16_t last_disconnect_reason; /**< wifi_err_reason_t of the last drop */
    bool fallback_dns;             /**< fallback DNS servers in use */
    bool ladder_active;            /**< the recovery ladder runs (or holds between passes) */
    uint8_t ladder_rung;           /**< last rung started: 1 reconnect, 2 renew DHCP, 3 restart Wi-Fi, 4 reboot */
    uint8_t ladder_withheld;       /**< network_ladder_why_t of the last skipped or withheld rung */
    uint32_t ladder_actions;       /**< recovery actions started by the ladder since boot */
} network_status_t;

/** One scan result. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secured;
} network_scan_entry_t;

/** Most scan results kept (one per SSID, strongest first). */
#define NETWORK_SCAN_MAX 24

/** Recovery actions, mildest first. */
typedef enum {
    NETWORK_RECOVER_RECONNECT = 0,   /**< esp_wifi_disconnect() + connect() (or connect now when waiting to retry) */
    NETWORK_RECOVER_RENEW_DHCP,      /**< restart DHCP on the STA netif, switch to the fallback DNS servers */
    NETWORK_RECOVER_RESTART_WIFI,    /**< esp_wifi_stop() + start() */
} network_recovery_t;

/** What health knows and the network ladder needs, once a second. */
typedef struct {
    bool caster_down;              /**< the protected caster slot is configured but not STREAMING */
    bool rtcm_healthy;             /**< the GNSS path delivers RTCM (so the network is the suspect) */
    bool flash_journal_open;       /**< a receiver-flash journal is open, any state, safe mode included: no reboot */
    bool flash_journal_unknown;    /**< journal state unknown because the settings are unavailable: does not withhold the reboot */
    uint32_t min_reboot_interval_s;/**< at most one network-ladder reboot per this many seconds (counted across resets) */
} network_ladder_input_t;

/** Create netifs, init Wi-Fi (RAM storage), register handlers, start the network task. */
esp_err_t network_init(void);

/**
 * Start station or setup AP depending on stored credentials. With the
 * settings unavailable nothing starts (no AP on its own); the BOOT button's
 * network_start_setup_ap(true) is then the only way in.
 */
esp_err_t network_start(void);

/** Status snapshot. */
esp_err_t network_get_status(network_status_t *out);

/** True when the station has an IP address (and its link is up). */
bool network_is_online(void);

/**
 * Store new station credentials and reconnect with them (applied about a
 * second later, so an HTTP reply can go out first). An AP that is up stays
 * up until the station gets an IP and a minute after, so the browser gets an
 * answer. ssid: 1-32 bytes; password: "" (open), 8-63 characters or 64 hex
 * digits. With the settings unavailable they are used for this boot only.
 * @return ESP_ERR_INVALID_ARG for invalid values, ESP_ERR_INVALID_STATE before
 *         network_init(), or the settings error if they could not be stored.
 */
esp_err_t network_set_credentials(const char *ssid, const char *password);

/**
 * Start a scan in the background; NETWORK_EVENT_SCAN_DONE when finished.
 * @return ESP_ERR_INVALID_STATE while Wi-Fi is not running, a scan runs or
 *         the station is in the middle of connecting.
 */
esp_err_t network_scan_start(void);

/** Last scan results (cached; no radio activity), strongest first. */
esp_err_t network_get_scan_results(network_scan_entry_t *entries, size_t max_entries, size_t *out_count);

/**
 * Bring the AP up for CONFIG_NETWORK_RECOVERY_AP_S (longer while a client
 * stays connected; a repeated call restarts the lifetime).
 * `recovery` = started by a BOOT-button press: also allowed when the settings
 * are unavailable. Without it (a manual request) the call is refused then
 * (ESP_ERR_INVALID_STATE): no AP opens on its own in degraded mode.
 */
esp_err_t network_start_setup_ap(bool recovery);

/**
 * Run one recovery action (in the network task; returns at once).
 * @return ESP_ERR_INVALID_STATE before network_start() or without station
 *         credentials, ESP_ERR_INVALID_ARG for an unknown action.
 */
esp_err_t network_request_recovery(network_recovery_t action);

/**
 * Probe the internet: a name lookup of CONFIG_NETWORK_PROBE_HOST plus a TCP
 * connect to it, and when the lookup fails a TCP connect to the fallback DNS
 * servers (port 53), which tells a DNS failure from no upstream at all. The
 * probe runs in its own task and takes at most ~35 s; the result also lands
 * in the status (probe, probe_age_s, upstream_ok).
 * @param timeout_ms how long to wait for the result; 0 starts the probe and
 *        returns at once (for callers that must not block). A probe already
 *        running is waited for instead of starting another.
 * @return ESP_OK upstream reachable; ESP_ERR_INVALID_STATE no IP (or before
 *         network_init()); ESP_ERR_NOT_FOUND name lookup failed while a public
 *         resolver answered; ESP_FAIL nothing beyond the router;
 *         ESP_ERR_TIMEOUT not finished within timeout_ms.
 */
esp_err_t network_probe_upstream(uint32_t timeout_ms);

/**
 * Network recovery ladder, once a second from the health monitor. Never
 * blocks: probes and recovery actions run in the network tasks, the reboot
 * is requested from lifecycle.
 * @return ESP_ERR_INVALID_ARG for NULL, ESP_ERR_INVALID_STATE before network_init().
 */
esp_err_t network_ladder_tick(const network_ladder_input_t *in);

#ifdef __cplusplus
}
#endif
