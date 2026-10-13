/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Network decisions that need no radio (pure logic, no IDF or RTOS includes,
 * host-testable): reconnect backoff, input validation, the setup AP's name,
 * when the setup/recovery AP is up, which disconnects go to the event
 * journal, the cached scan list, the age of the last network-ladder reboot
 * across resets, and the trial of changed station credentials. Times are seconds of an injected monotonic clock,
 * compared only as (uint32_t)(now - then), so they survive the clock wrap.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- reconnect backoff --------------------------------------------------------- */

/** Longest wait between two station connect attempts. */
#define NET_BACKOFF_CAP_MS 30000u

/**
 * Wait before connect attempt `attempt` (0 = the first retry after a drop):
 * 1 s, 2 s, 4 s, 8 s, 16 s, then 30 s for every further attempt, forever.
 */
uint32_t net_backoff_ms(uint32_t attempt);

/* ---- validation ------------------------------------------------------------------ */

/** SSID: 1-32 bytes (any bytes except NUL). */
bool net_ssid_valid(const char *ssid);

/**
 * Station password: "" (open network), 8-63 printable ASCII characters
 * (WPA passphrase) or exactly 64 hex digits (raw PSK).
 */
bool net_password_valid(const char *pass);

/** Setup-AP password: 8-63 printable ASCII characters (WPA2 passphrase). */
bool net_ap_password_valid(const char *pass);

/** Stored station credentials, as read at start. */
typedef enum {
    NET_CRED_NONE = 0,       /**< no valid SSID stored: not configured (setup) */
    NET_CRED_OK,             /**< SSID and password read ("" = an open network, stored as such) */
    NET_CRED_UNREADABLE,     /**< an SSID is stored but its password could not be read (or is not valid) */
} net_cred_t;

/**
 * What the station may use. Both values are always written together (an
 * open network stores ""), and a damaged entry reads as "not found", so any
 * failure to read the password of a stored SSID fails closed: the station
 * stays unconfigured for this boot (the setup AP takes the credentials
 * again) instead of dropping its security floor to an open network that
 * carries the same name.
 */
net_cred_t net_sta_credentials(bool ssid_read, const char *ssid, bool pass_read, const char *pass);

/** DHCP hostname: 1-32 characters [A-Za-z0-9-], not starting or ending with '-'. */
bool net_hostname_valid(const char *name);

/**
 * Setup-AP SSID: `prefix` followed by the last two MAC bytes as four
 * upper-case hex digits ("onocoydiy-" + "3A7F"). The prefix is cut so the
 * whole name fits the 32-byte SSID limit and the digits always survive.
 * @return length written (excluding the terminator), 0 if out is too small
 *         for the digits.
 */
size_t net_ap_ssid(const char *prefix, const uint8_t mac[6], char *out, size_t out_size);

/* ---- when the AP is up --------------------------------------------------------------- */

/** Why the AP is up (bit flags; 0 = down). */
typedef enum {
    NET_AP_SETUP    = 1u << 0, /**< no station credentials (first boot, after a factory reset) */
    NET_AP_FALLBACK = 1u << 1, /**< configured but offline for the fallback time, or held until the station has an IP */
    NET_AP_LINGER   = 1u << 2, /**< station just got its IP: setup page can still show the new address */
    NET_AP_RECOVERY = 1u << 3, /**< BOOT button (or a manual request): fixed lifetime, longer while a client is connected */
} net_ap_reason_t;

/** A connected client keeps the recovery AP up at most this long past its lifetime. */
#define NET_AP_CLIENT_HOLD_MAX_S 1800u

typedef struct {
    bool settings_available;     /**< settings readable this boot; without them no AP opens on its own */
    bool sta_configured;         /**< station credentials present */
    bool has_ip;                 /**< station holds an IP */
    uint32_t offline_s;          /**< since the station last had an IP (or since start); ignored while has_ip */
    uint32_t since_ip_s;         /**< since the station got its current IP; ignored unless has_ip */
    uint32_t reasons_now;        /**< what the AP is up for right now (net_ap_reason_t, 0 = down) */
    bool recovery_requested;     /**< a recovery (or manual) AP was requested and has not expired */
    uint32_t recovery_age_s;     /**< since that request */
    uint32_t ap_clients;         /**< stations connected to the AP */
    uint32_t fallback_after_s;   /**< offline time before the fallback AP opens */
    uint32_t recovery_lifetime_s;/**< recovery AP lifetime when nobody is connected */
    uint32_t linger_s;           /**< how long the AP stays after the station got an IP */
} net_ap_input_t;

/**
 * Reasons the AP should be up now (net_ap_reason_t flags, 0 = down):
 *  - RECOVERY while the request is younger than its lifetime, and after that
 *    while a client stays connected, for at most NET_AP_CLIENT_HOLD_MAX_S
 *    beyond the lifetime; then *recovery_expired is set (the caller clears
 *    the request).
 *  - Only with settings available (degraded mode opens nothing on its own):
 *    SETUP while no credentials are stored; FALLBACK once the configured
 *    station has been offline for fallback_after_s, and also for as long as
 *    an AP that is up for SETUP/FALLBACK/LINGER waits for the station's IP
 *    (new credentials being tried, or failing: the browser keeps its link);
 *    LINGER for linger_s after the station got its IP while such an AP is up.
 */
uint32_t net_ap_reasons(const net_ap_input_t *in, bool *recovery_expired);

/**
 * Seconds until the station closes an AP that is up for `reasons` (the
 * result of net_ap_reasons() for the same input), UINT32_MAX when nothing
 * is scheduled:
 *  - the AP is down (reasons 0), or up for SETUP or FALLBACK (no deadline:
 *    it stays until the station has an IP);
 *  - LINGER: linger_s - since_ip_s;
 *  - RECOVERY: the lifetime left, or UINT32_MAX while a client holds it past
 *    its lifetime (it closes when the last one leaves, or at the 30 min
 *    limit);
 *  - LINGER and RECOVERY together: the later of the two.
 */
uint32_t net_ap_close_in_s(const net_ap_input_t *in, uint32_t reasons);

/* ---- disconnect journal filter -------------------------------------------------------- */

/**
 * Which station disconnects are journalled. A long outage retries every 30 s
 * and must not flood the event journal (fixed-size ring) with identical
 * records, so within one outage:
 *  - the drop of an established connection is always recorded;
 *  - a failed attempt is recorded when its reason differs from the last
 *    recorded one and the last record is at least NET_DROP_LOG_CHANGE_S old,
 *    or when the last record is at least NET_DROP_LOG_REPEAT_S old.
 */
#define NET_DROP_LOG_CHANGE_S 60u
#define NET_DROP_LOG_REPEAT_S 600u

typedef struct {
    bool any;              /**< something was recorded in this outage */
    uint8_t last_reason;
    uint32_t last_s;
} net_drop_log_t;

/** Start state (also after a successful connection). */
void net_drop_log_reset(net_drop_log_t *s);

/**
 * Should this disconnect be recorded? `was_connected` = the station was
 * associated before this event (an established link dropped). Updates the
 * state when it answers true.
 */
bool net_drop_log_should(net_drop_log_t *s, bool was_connected, uint8_t reason, uint32_t now_s);

/**
 * Is a signal reading from the driver a real one? An integer from -127 to
 * -1 dBm; anything else (the driver's -128 for "no signal", 0, positive
 * values) means unknown. A drop records 0 for unknown in its journal entry.
 */
bool net_rssi_known(int8_t rssi);

/* ---- IPv6 ---------------------------------------------------------------------------- */

/** True for an IPv6 link-local unicast address (fe80::/10); `addr` is 16 bytes in network order. */
bool net_ip6_is_link_local(const uint8_t addr[16]);

/* ---- cached scan list ----------------------------------------------------------------- */

typedef struct {
    char ssid[33];
    int8_t rssi;
    uint8_t channel;
    bool secured;
} net_scan_entry_t;

/**
 * Add one scan record to a list kept sorted by signal, strongest first. One
 * entry per SSID (the strongest access point of a mesh or repeater set);
 * hidden networks (empty SSID) are skipped; a full list drops its weakest
 * entry for a stronger one.
 * @return the new number of entries.
 */
size_t net_scan_insert(net_scan_entry_t *list, size_t count, size_t cap, const net_scan_entry_t *e);

/* ---- changed station credentials on trial -------------------------------------------- */

/**
 * Changed station credentials are tried before they replace the ones the
 * station has: stored as "pending" (settings wifi/pend_*), used at once,
 * and only when they get an IP within NET_CRED_TRIAL_S do they become the
 * station's credentials. Otherwise the station goes back to its previous
 * credentials and keeps the new ones pending: shown in the UI, retried on
 * request and first thing at the next boot (a user who enters the next
 * site's network before moving the station). So a typo never strands a
 * remote station; a password the network refuses outright ends the trial
 * sooner (NET_CRED_TRIAL_REFUSALS). Without previous credentials (first setup) there is
 * nothing to go back to: the new ones are stored as the station's at once.
 */
#define NET_CRED_TRIAL_S 90u

typedef enum {
    NET_CRED_PLAN_ACTIVE = 0,   /**< store and use as the station's credentials (none to go back to, or the same ones) */
    NET_CRED_PLAN_TRIAL,        /**< store as pending and try them; the previous ones stay */
    NET_CRED_PLAN_STORE_ONLY,   /**< store as the station's credentials, not used: the station is not on Wi-Fi (no apply, no trial) */
} net_cred_plan_t;

/**
 * What to do with credentials the user entered. wifi_used: the station
 * connects over Wi-Fi (false in the QEMU development build, which runs on an
 * emulated Ethernet port: there is nothing to try them on, so a trial could
 * only ever end in a revert).
 */
net_cred_plan_t net_cred_plan(bool have_active, bool same_as_active, bool wifi_used);

/**
 * A trial ends early when the network refuses the new credentials: this
 * many disconnects in a row, all for a reason that says the password or the
 * security setting was refused (net_trial_reason_refused), while the trial
 * credentials never got an association. Other reasons (no access point
 * found, a weak signal) keep the NET_CRED_TRIAL_S rule: the network may
 * simply be out of reach for a moment.
 */
#define NET_CRED_TRIAL_REFUSALS 3u

/* Disconnect reasons (wifi_err_reason_t values; network.c checks them). */
#define NET_REASON_4WAY_HANDSHAKE_TIMEOUT 15u
#define NET_REASON_NO_AP_FOUND 201u
#define NET_REASON_AUTH_FAIL 202u
#define NET_REASON_HANDSHAKE_TIMEOUT 204u
#define NET_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY 210u
#define NET_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD 211u

/**
 * A disconnect reason that means the network refused the credentials: a
 * failed authentication, a handshake that timed out (wrong passphrase), or
 * an access point of that name only with another security setting.
 */
bool net_trial_reason_refused(uint16_t reason);

typedef enum {
    NET_TRIAL_NONE = 0,         /**< no trial, or still waiting */
    NET_TRIAL_CONFIRMED,        /**< an IP with the new credentials: they become the station's */
    NET_TRIAL_REVERT,           /**< no IP in time: back to the previous credentials (if any) */
    NET_TRIAL_REVERT_AUTH,      /**< the network refused them NET_CRED_TRIAL_REFUSALS times: back now */
} net_trial_act_t;

typedef struct {
    bool on;
    uint32_t since_s;
    bool assoc;                 /**< the trial credentials got an association since the trial started */
    uint8_t refusals;           /**< refused attempts in a row, while never associated */
} net_cred_trial_t;

void net_cred_trial_start(net_cred_trial_t *t, uint32_t now_s);

/**
 * One check: `got_ip` = an IP arrived on a link the trial credentials made
 * (associated after the trial started). Ends the trial on CONFIRMED or REVERT.
 */
net_trial_act_t net_cred_trial_tick(net_cred_trial_t *t, bool got_ip, uint32_t now_s);

/** The station associated with the trial credentials: no early revert for this trial any more. */
void net_cred_trial_associated(net_cred_trial_t *t);

/**
 * A disconnect while the trial runs (`reason` = wifi_err_reason_t). Counts
 * refused attempts in a row (net_trial_reason_refused; any other reason
 * starts the count again) until the trial credentials associate once. At
 * NET_CRED_TRIAL_REFUSALS it ends the trial: NET_TRIAL_REVERT_AUTH.
 */
net_trial_act_t net_cred_trial_disconnect(net_cred_trial_t *t, uint16_t reason);

/** Seconds the trial still has (0 = none running). */
uint32_t net_cred_trial_left_s(const net_cred_trial_t *t, uint32_t now_s);

/**
 * Wait before the next connect attempt after a disconnect with `reason`
 * (`attempt` as for net_backoff_ms). A refused attempt (net_trial_reason_refused)
 * while a trial runs whose credentials never associated is retried at once
 * (0 ms; the connect still scans first), so the NET_CRED_TRIAL_REFUSALS
 * refusals that end the trial come back to back: one refusal is already one
 * driver event covering several authentication tries, and waiting between
 * them only delays the way back. Everything else, an out-of-reach reason
 * during a trial included, waits net_backoff_ms(attempt).
 */
uint32_t net_trial_retry_ms(const net_cred_trial_t *t, uint16_t reason, uint32_t attempt);

/**
 * What the status shows about changed credentials, from the state the
 * network keeps under its lock: the pending pair, a revert, the trial the
 * net task runs, and credentials queued to be applied (they are applied
 * NET_CRED_APPLY_DELAY_MS after the request, so an HTTP reply goes out
 * first). Every request that changes them publishes this view under the
 * same lock, so the next status read is coherent at once.
 */
typedef struct {
    bool pend_valid;           /**< pending credentials are stored */
    const char *pend_ssid;     /**< their SSID (read only while pend_valid) */
    bool reverted;             /**< the station went back to its previous credentials */
    uint8_t reverted_why;      /**< why (network_cred_revert_t) */
    bool trial_running;        /**< the net task runs a trial now */
    uint32_t trial_left_s;     /**< its time left (net_cred_trial_left_s) */
    bool trial_queued;         /**< credentials wait to be applied as a trial (a new one, or a retry) */
    bool other_queued;         /**< credentials wait to be applied as the station's: a running trial ends then */
    uint32_t apply_left_ms;    /**< until the queued credentials are applied */
} net_cred_in_t;

typedef struct {
    bool cred_trial;           /**< a trial runs, or one is about to start */
    uint32_t cred_trial_left_s;/**< until it gives up: a queued trial counts NET_CRED_TRIAL_S from its start */
    bool cred_pending;
    char pending_ssid[33];     /**< "" = none */
    bool cred_reverted;        /**< only while credentials are pending and no trial of them runs */
    uint8_t cred_reverted_why; /**< 0 unless cred_reverted */
} net_cred_view_t;

/** Delay between a credential request and its use (the reply goes out first). */
#define NET_CRED_APPLY_DELAY_MS 1000u

void net_cred_view(const net_cred_in_t *in, net_cred_view_t *out);

/* ---- network-ladder reboot age across resets ---------------------------------------- */

/** Age not known (no network-ladder reboot since power-on, or state lost). */
#define NET_AGE_UNKNOWN UINT32_MAX

#define NET_RTC_MAGIC 0x4E455452u  /* "NETR" */

/**
 * Kept in RTC memory (survives software, panic and watchdog resets, not
 * power loss), validated by magic and checksum, and by its stamp (the image
 * generation of the run that wrote it, lifecycle_rtc_stamp_ok()). An invalid
 * block, or one that predates another app image's run, is a fresh start,
 * like a power cycle.
 */
typedef struct {
    uint32_t magic;
    uint32_t age_at_boot_s;  /**< seconds from the last network-ladder reboot to this boot's start, NET_AGE_UNKNOWN if none */
    uint32_t stamp;          /**< image generation of the run that wrote it */
    uint32_t check;
} net_rtc_t;

bool net_rtc_valid(const net_rtc_t *r);
void net_rtc_seal(net_rtc_t *r);

/**
 * Account for this boot:
 *  - the previous boot ended in a network-ladder reboot: age 0;
 *  - else, with a valid block whose stamp lifecycle accepted (`stamp_ok`)
 *    and a known previous session length (software, panic and watchdog
 *    resets): the stored age plus that session;
 *  - else (power-on, brownout, lost block, a block from before another
 *    image's run): unknown.
 * Stamps the block with `stamp` (this boot's generation) and seals it.
 */
void net_rtc_on_boot(net_rtc_t *r, bool stamp_ok, uint32_t stamp, bool prev_was_ladder_reboot,
                     uint32_t prev_session_s);

/** Seconds since the last network-ladder reboot now (saturating), NET_AGE_UNKNOWN if none. */
uint32_t net_rtc_age_now(const net_rtc_t *r, uint32_t uptime_s);

#ifdef __cplusplus
}
#endif
