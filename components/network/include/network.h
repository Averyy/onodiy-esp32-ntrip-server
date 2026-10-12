/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Network: Wi-Fi station with a captive-portal setup AP as fallback, SNTP,
 * an mDNS responder, an upstream probe and the network recovery ladder.
 *
 * Behaviour:
 *  - No stored credentials: start the setup AP (open, or WPA2 with the
 *    wifi/ap_pass password from settings) with a DNS responder that answers
 *    every name with the AP's own address and DHCP option 114 pointing at the
 *    setup page, so phones open it automatically. The station interface stays
 *    up beside the AP (AP+STA), unconnected, so the setup page can scan.
 *  - Credentials stored: connect as a station and reconnect forever after
 *    every drop, waiting 1 s, 2 s, 4 s, 8 s, 16 s, then 30 s between
 *    attempts. A Wi-Fi outage never switches the station to setup-AP-only
 *    mode: after CONFIG_NETWORK_STA_FALLBACK_S without an IP the setup AP
 *    comes up *beside* the station (AP+STA, on the station's channel once it
 *    connects), and goes down again CONFIG_NETWORK_AP_LINGER_S (15 s) after
 *    the station has an IP.
 *    The station keeps retrying all along. A Wi-Fi outage can reboot the
 *    ESP32 only through the network ladder's last rung (upstream probe
 *    NO_IP or DNS_FAILED, at most once per
 *    CONFIG_HEALTH_NET_REBOOT_MIN_INTERVAL_H, never while a receiver-flash
 *    journal is open); the receiver is untouched by it.
 *  - Changed credentials are tried first (network_set_credentials): stored
 *    as pending (wifi/pend_ssid, wifi/pend_pass) beside the station's
 *    current ones and used at once; when they get an IP within 90 s they
 *    replace the current ones, otherwise the station goes back to its
 *    previous credentials and keeps the new ones pending (shown in the
 *    status, retried with network_retry_pending_credentials() and first
 *    thing at the next boot, dropped with
 *    network_discard_pending_credentials()). So a typo never strands a
 *    remote station. A network that refuses the new password outright
 *    (three refused attempts in a row before any association: wrong
 *    passphrase, handshake timeout, an access point of that name only with
 *    another security setting) ends the trial at once instead of after
 *    90 s: during such a trial a refused attempt is retried without the
 *    reconnect backoff (the scan before each attempt stays), so the third
 *    refusal comes about 18 s after the change; the status says why the
 *    station went back (cred_reverted_why). The status shows a change at
 *    once: every call that changes the credentials publishes cred_trial,
 *    cred_trial_left_s (90 s plus the second until the change is applied),
 *    cred_pending, pending_ssid and cred_reverted under the status lock,
 *    so the next network_get_status() agrees with what the call decided.
 *    Without previous credentials (first setup) the new ones
 *    are stored as the station's at once. A change made while a trial runs
 *    replaces that trial: an IP for the earlier pair that arrives before the
 *    change is applied confirms nothing, and a pending pair is cleared only
 *    when it is the pair confirmed.
 *  - Settings unavailable (degraded boot, NVS unreadable): no AP comes up on
 *    its own. Only a BOOT-button long press starts the recovery AP, so an
 *    unconfigured-looking station on a roof is never open to passers-by.
 *  - Recovery AP (network_start_setup_ap(true), BOOT button): wifi/ap_pass if
 *    set and readable, else open; down again after CONFIG_NETWORK_RECOVERY_AP_S
 *    unless a client is still connected (then when the last one leaves, or
 *    30 min after the lifetime at the latest).
 *  - An AP that is up for setup or fallback stays up while the station tries
 *    new credentials and until CONFIG_NETWORK_AP_LINGER_S after it gets an
 *    IP, so the browser on the setup page receives an answer (the new
 *    address, and the countdown to the AP closing: ap_close_in_s).
 *  - mDNS (Espressif's mdns component): the station answers
 *    <hostname>.local (the name in use, the stored sys/hostname or the
 *    default) on the home network and on its own AP, and advertises its web
 *    page as an _http._tcp service. When another device holds that name
 *    (a lost probe or an address conflict), the responder renames itself
 *    (<name>-2, then -3, ...); the status shows the name it answers
 *    (mdns_host), the log says so once, and the next boot probes the
 *    configured name again (the renamed one is never stored). The service
 *    instance carries no name of its own: it is named after the host name
 *    in use, so it follows a rename. Before a planned restart the services
 *    send their goodbye (TTL 0) while the network is still up. Every
 *    interface gets an IPv6 link-local address when its link comes up (and
 *    no other address), so AAAA queries for the name are answered at once.
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
 * second) while the station has credentials and any of these triggers holds:
 * P, the protected caster is enabled and not streaming (nor paused) while
 * RTCM is healthy; A, the station has been associated for 60 s without an
 * IP (assoc_no_ip_s); S, an enabled slot of any caster has failed for 180 s
 * in a row without reaching its caster (DNS failures, connects that timed
 * out or failed locally; network_ladder_input_t.slot_unreached_s).
 * +60 s reconnect, +3 min renew DHCP and switch to the fallback DNS servers
 * (CONFIG_NETWORK_FALLBACK_DNS1/2, kept for the rest of the boot), +6 min
 * restart Wi-Fi, +10 min reboot (lifecycle_request_reboot(NETWORK_LADDER)).
 * Each rung first probes upstream: reachable -> the fault is remote (a caster)
 * and the rung is skipped; an IP but nothing beyond the router -> no reboot.
 * Only trigger P can reboot: without it at the reboot rung the reboot is
 * withheld (NO_REBOOT_TRIGGER), so A and S never reboot the ESP32.
 * The reboot is also withheld while a receiver-flash journal is open (not
 * when its state is only unknown because the settings are unavailable),
 * within the minimum interval since the last network-ladder reboot (counted
 * across resets; a power cycle, or RTC state from before another app image's
 * run, starts afresh), while a reboot inhibit is held
 * and while someone is connected to the station's AP. A connected AP client
 * holds the Wi-Fi restart and the reboot back for at most 30 min per ladder
 * episode; after that they run anyway (reason AP_OVERRIDDEN). The
 * reconnect, renew and restart rungs run regardless of the journal, so the
 * user can still reach the UI. After the reboot rung the ladder holds 10
 * min, then starts a new pass; with S as the only trigger and the internet
 * answering a fresh probe (a slot's remote fault: a host name that does not
 * resolve, a host that drops connects) it holds again instead, without
 * journalling another pass. It resets after 60 s without a trigger; for an
 * episode A or S started or kept alive, only with an IP and, after S, no
 * slot in an unreached run (the ladder's own reconnect clears A and S for
 * longer than that, which would end every episode at its first rung).
 * Ladder timings are PROVISIONAL, to be confirmed on hardware.
 * Journal: DIAG_EV_LADDER_STEP with a = 1 and b = (reason << 16) |
 * (outcome << 8) | code. Codes: 0 pass started (reason = the triggers
 * holding then: bit 0 P, bit 1 A, bit 2 S), 1 reconnect, 2 renew DHCP +
 * fallback DNS, 3 restart Wi-Fi, 4 reboot. Outcomes: 0 started (reason
 * AP_OVERRIDDEN when it runs with an AP client connected), 1 healthy again
 * (code = the last rung started), 2 skipped or withheld with the reason
 * (network_ladder_why_t).
 * Connection history: DIAG_EV_WIFI_UP when the station gets its IP back
 * after losing it (not the first IP of a boot): a = connect attempts during
 * the outage, b = outage in seconds.
 *
 * Wi-Fi credentials live in the settings component; the Wi-Fi driver's own
 * NVS storage is disabled (single source of truth).
 *
 * A planned restart (lifecycle_reboot / lifecycle_request_reboot) ends the
 * Wi-Fi link on the way down; that disconnect is neither logged as a link
 * loss nor journalled (the journal's restart record covers it).
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
    NETWORK_LADDER_WHY_NO_REBOOT_TRIGGER, /**< reboot rung: the pass runs for triggers A or S only (never reboot) */
    NETWORK_LADDER_WHY_AP_OVERRIDDEN,/**< the rung ran although an AP client is connected (30 min limit over) */
} network_ladder_why_t;

/** What network_set_credentials() does with the credentials it was given. */
typedef enum {
    NETWORK_CRED_PLAN_STORE_ONLY = 0, /**< stored as the station's, not used: the station is not on Wi-Fi (the QEMU build) */
    NETWORK_CRED_PLAN_APPLY,          /**< stored as the station's and used about a second later (none stored before, or the same ones) */
    NETWORK_CRED_PLAN_TRIAL,          /**< stored as pending and tried about a second later; the previous ones stay */
} network_cred_plan_t;

/** Why the station went back to its previous credentials (network_status_t.cred_reverted_why). */
typedef enum {
    NETWORK_CRED_REVERT_NONE = 0,
    NETWORK_CRED_REVERT_NO_IP = 1,    /**< no IP within the 90 s trial */
    NETWORK_CRED_REVERT_AUTH = 2,     /**< the network refused the new password (three attempts in a row) */
} network_cred_revert_t;

/** Network ladder triggers (network_status_t.ladder_triggers, pass-start record). */
typedef enum {
    NETWORK_LADDER_TRIG_PROTECTED  = 1u << 0, /**< P: protected caster down while RTCM is healthy */
    NETWORK_LADDER_TRIG_ASSOC_NO_IP = 1u << 1, /**< A: associated 60 s without an IP */
    NETWORK_LADDER_TRIG_UNREACHED  = 1u << 2, /**< S: a slot's failures have not reached its caster for 180 s */
} network_ladder_trigger_t;

/** Snapshot for status pages and health. */
typedef struct {
    bool sta_configured;
    bool sta_credentials_unreadable; /**< SSID stored, password unreadable: station not started (setup AP) */
    bool online;                   /**< station has an IP */
    bool associated;               /**< station link to the access point is up */
    uint32_t assoc_no_ip_s;        /**< associated without an IP for this long (since the association or the loss of the address); 0 = not associated, or has an IP */
    bool upstream_ok;              /**< last probe reached the internet */
    network_probe_t probe;         /**< last probe result */
    uint32_t probe_age_s;          /**< since the last probe finished, UINT32_MAX = never */
    bool setup_ap_active;
    uint8_t ap_reasons;            /**< network_ap_reason_t flags while the AP is up */
    uint8_t ap_clients;            /**< stations connected to the AP */
    uint32_t recovery_ap_left_s;   /**< recovery AP lifetime left (0 = none, or kept up for a connected client) */
    uint32_t ap_close_in_s;        /**< until the station closes its AP: the linger after the station got its IP, or the recovery AP's lifetime left; UINT32_MAX = AP down, or nothing scheduled (setup without credentials, offline fallback, a recovery AP held by a client) */
    char ap_ssid[33];              /**< setup AP name (set once Wi-Fi is initialised) */
    char mdns_host[33];            /**< host label the mDNS responder answers, without ".local" (the hostname, or <hostname>-N after a name conflict); "" = not running */
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
    uint8_t ladder_triggers;       /**< network_ladder_trigger_t flags holding at the last tick */
    uint32_t ladder_actions;       /**< recovery actions started by the ladder since boot */
    bool cred_trial;               /**< changed credentials are being tried now */
    uint32_t cred_trial_left_s;    /**< until the trial gives up (0 = none) */
    bool cred_pending;             /**< credentials wait to be confirmed (pending_ssid) */
    char pending_ssid[33];         /**< SSID of the pending credentials ("" = none) */
    bool cred_reverted;            /**< the pending credentials got no IP in time (or were refused): back on the previous ones */
    uint8_t cred_reverted_why;     /**< network_cred_revert_t while cred_reverted */
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
    uint32_t slot_unreached_s;     /**< longest run of an enabled slot's failures that never reached its caster (ntrip_caster_status_t.unreached_s) */
    bool flash_journal_open;       /**< a receiver-flash journal is open, any state, safe mode included: no reboot */
    bool flash_journal_unknown;    /**< journal state unknown because the settings are unavailable: does not withhold the reboot */
    uint32_t min_reboot_interval_s;/**< at most one network-ladder reboot per this many seconds (counted across resets) */
} network_ladder_input_t;

/**
 * Create netifs, init Wi-Fi (RAM storage), start the mDNS responder (never
 * fatal: without it the station is reached by its IP), register handlers,
 * start the network task.
 */
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
 * New station credentials, applied about a second later (so an HTTP reply
 * can go out first). With credentials already stored (and different) they
 * are stored as pending and tried: an IP within 90 s makes them the
 * station's credentials, otherwise the station goes back to the previous
 * ones and keeps these pending (see the header). Without stored credentials
 * they are stored as the station's at once. An AP that is up stays up until
 * the station gets an IP and CONFIG_NETWORK_AP_LINGER_S after, so the
 * browser gets an answer.
 * ssid: 1-32 bytes; password: "" (open), 8-63 characters or 64 hex digits.
 * With the settings unavailable they are kept for this boot only.
 * The status reflects the change before this returns (cred_trial true with
 * cred_trial_left_s 91 for a trial).
 * @param plan what was done with them (NULL: not wanted); set on ESP_OK only.
 * @return ESP_ERR_INVALID_ARG for invalid values, ESP_ERR_INVALID_STATE before
 *         network_init(), or the settings error if they could not be stored.
 */
esp_err_t network_set_credentials(const char *ssid, const char *password, network_cred_plan_t *plan);

/**
 * Try the pending credentials again (90 s, then back to the current ones
 * unless they got an IP). The status shows the trial at once.
 * @return ESP_ERR_NOT_FOUND when none are pending, ESP_ERR_INVALID_STATE
 *         before network_init().
 */
esp_err_t network_retry_pending_credentials(void);

/**
 * Forget the pending credentials (a trial in progress ends: the station goes
 * back to its current credentials). The status shows none pending, no
 * trial and no revert at once.
 * @return ESP_ERR_NOT_FOUND when none are pending, ESP_ERR_INVALID_STATE
 *         before network_init(), or the settings error.
 */
esp_err_t network_discard_pending_credentials(void);

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
