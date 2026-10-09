/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Network: Wi-Fi station with a captive-portal setup AP as fallback, SNTP.
 *
 * Behaviour:
 *  - No stored credentials: start the setup AP (open or password from
 *    settings) with a DNS responder that answers every name with the AP's own
 *    address, so phones open the setup page automatically.
 *  - Credentials stored: connect as a station, reconnect forever with capped
 *    exponential backoff. If offline for CONFIG_NETWORK_STA_FALLBACK_S, also
 *    raise the setup AP (AP+STA) until the station is back online.
 *  - Settings unavailable (degraded boot, NVS unreadable): no AP comes up on
 *    its own. Only a BOOT-button long press starts the recovery AP, so an
 *    unconfigured-looking station on a roof is never open to passers-by.
 *  - Once online: start SNTP (fallback clock for logs and update checks;
 *    the availability ledger prefers GNSS time).
 *
 * Radio settings for a mains-powered streaming device: power save off
 * (esp_wifi_set_ps(WIFI_PS_NONE): no DTIM sleep latency or stalls), full
 * channel scan and strongest-AP selection (scan_method = WIFI_ALL_CHANNEL_SCAN,
 * sort_method = WIFI_CONNECT_AP_BY_SIGNAL, so mesh homes pick the best node),
 * country code from CONFIG_NETWORK_WIFI_COUNTRY. RSSI and disconnect reasons
 * go to the boot/event history.
 *
 * Scanning while connected hops channels and stalls caster traffic for
 * seconds, so the UI shows cached results (network_get_scan_results) and
 * warns before a fresh scan.
 *
 * Recovery ladder (driven by the health monitor while the protected caster
 * is down but RTCM is healthy): reconnect -> renew DHCP and switch to the
 * fallback DNS servers -> restart Wi-Fi -> (health) reboot. "IP but no
 * upstream" is told apart from "no IP" with a DNS lookup / TCP probe.
 *
 * Wi-Fi credentials live in the settings component; the Wi-Fi driver's own
 * NVS storage is disabled (single source of truth).
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
    NETWORK_EVENT_ONLINE,          /**< station has an IP */
    NETWORK_EVENT_OFFLINE,         /**< station lost its IP */
    NETWORK_EVENT_SETUP_AP_UP,
    NETWORK_EVENT_SETUP_AP_DOWN,
    NETWORK_EVENT_TIME_SYNCED,     /**< SNTP set the clock */
    NETWORK_EVENT_SCAN_DONE,       /**< cached scan results updated */
} network_event_t;

/** Snapshot for status pages and health. */
typedef struct {
    bool sta_configured;
    bool online;                   /**< station has an IP */
    bool upstream_ok;              /**< last DNS/TCP probe reached the internet */
    bool setup_ap_active;
    bool time_synced;
    char ssid[33];
    char ip[16];
    int8_t rssi;
    uint8_t channel;
    uint32_t reconnects;           /**< station reconnects since boot */
    uint32_t offline_s;            /**< seconds offline in the current outage, 0 if online */
    uint16_t last_disconnect_reason; /**< wifi_err_reason_t of the last drop */
    bool fallback_dns;             /**< fallback DNS servers in use */
} network_status_t;

/** One scan result. */
typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secured;
} network_scan_entry_t;

/** Recovery actions, mildest first. */
typedef enum {
    NETWORK_RECOVER_RECONNECT = 0,   /**< esp_wifi_disconnect() + connect() */
    NETWORK_RECOVER_RENEW_DHCP,      /**< restart DHCP on the STA netif, fall back to public DNS */
    NETWORK_RECOVER_RESTART_WIFI,    /**< esp_wifi_stop() + start() */
} network_recovery_t;

/** Create netifs, init Wi-Fi (RAM storage), register handlers. */
esp_err_t network_init(void);

/** Start station or setup AP depending on stored credentials (see degraded rule above). */
esp_err_t network_start(void);

/** Status snapshot. */
esp_err_t network_get_status(network_status_t *out);

/** True when the station has an IP address. */
bool network_is_online(void);

/**
 * Store new station credentials and reconnect. Takes effect immediately; the
 * setup AP stays up until the station gets an IP so the browser gets an answer.
 */
esp_err_t network_set_credentials(const char *ssid, const char *password);

/** Start a scan in the background; NETWORK_EVENT_SCAN_DONE when finished. */
esp_err_t network_scan_start(void);

/** Last scan results (cached; no radio activity). */
esp_err_t network_get_scan_results(network_scan_entry_t *entries, size_t max_entries, size_t *out_count);

/**
 * Bring the setup AP up. `recovery` = started by a BOOT-button press: also
 * allowed when settings are unavailable, and taken down again after
 * CONFIG_NETWORK_RECOVERY_AP_S unless someone is connected.
 */
esp_err_t network_start_setup_ap(bool recovery);

/** Run one recovery action (network ladder). */
esp_err_t network_request_recovery(network_recovery_t action);

/** Probe the internet (DNS lookup + TCP connect, bounded); updates upstream_ok. */
esp_err_t network_probe_upstream(uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
