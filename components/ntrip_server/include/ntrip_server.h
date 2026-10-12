/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Multi-caster NTRIP server (source side: pushes RTCM to casters).
 *
 * One task per caster slot, created once at boot and never deleted (enable,
 * disable and config changes are handled by the slot's state machine, so
 * months of reconfiguring never fragment the heap). Each slot attaches its
 * own rtcm_hub consumer, connects (TCP or TLS), sends the source request
 * (NTRIP 1.0 "SOURCE" or NTRIP 2.0 HTTP POST), waits for the caster's OK and
 * then streams whole RTCM3 frames. Request and transmit buffers are
 * allocated once per slot. Slots are fully independent: one caster's outage,
 * auth failure or slowness never affects another.
 *
 * Bounded blocking: every blocking call has a limit (plain slots: TCP
 * connect CONFIG_NTRIP_TCP_CONNECT_TIMEOUT_MS = 5 s; TLS slots: esp_tls
 * timeout_ms CONFIG_NTRIP_CONNECT_TIMEOUT_MS = 10 s, which bounds the connect
 * and each handshake wait, so the whole handshake can take about twice that;
 * lwIP's resolver gives up after ~7 s per DNS server). After the connect the
 * socket is non-blocking and every wait is a select() of at most 100 ms; a
 * write that gets no byte accepted for CONFIG_NTRIP_SEND_TIMEOUT_MS is a
 * write stall. Keepalive 10/5/3 s and TCP_NODELAY are set on every caster
 * socket. Slot tasks are NOT subscribed to the task watchdog: a
 * slow caster must never reboot the station. Instead each slot stamps its
 * progress; the health monitor restarts only the slot that overruns its
 * per-state budget (CONNECTING <= CONFIG_NTRIP_CONNECT_BUDGET_S, a single
 * send <= the send timeout + 5 s) via ntrip_server_restart_slot().
 *
 * Clean shutdown: a lifecycle shutdown hook closes every session (TLS
 * close_notify / shutdown(SHUT_WR) immediately followed by close(), bounded)
 * before any planned reboot, so the caster does not hold a stale source
 * session. While streaming, the socket is also polled for EOF so a
 * caster-side close is seen at once.
 *
 * Failures and backoff: every failure has a class (ntrip_fail_class_t) and
 * every slot a backoff profile (fast for Onocoy, standard, polite for
 * community casters that ban hammering sources). Transient failures back off
 * exponentially to the profile's cap; the other classes follow the profile's
 * row for their class; every delay gets +-20 % jitter. NTRIP 1.0 has no
 * standard "mountpoint in use" reply, so a rejection right after a reboot or
 * disconnect may just be the caster still holding the previous session: an
 * AUTH reply within CONFIG_NTRIP_AUTH_TRANSIENT_WINDOW_S of a disconnect or
 * boot, on credentials that worked before, follows the profile's MOUNT_BUSY
 * row; after the window AUTH and CONFIG follow the profile's AUTH row, and
 * persistent MOUNT_BUSY escalates to it after 10 min. A polite slot makes at
 * most one attempt per hour after 20 consecutive failures that reached the
 * caster, until the caster accepts it again or the user edits the slot.
 * Entering AUTH, CONFIG, BANNED or the hourly mode persists {class, attempt,
 * remaining delay} (settings cstN/backoff), and so does every further step
 * of such a row until its last entry repeats, and a polite slot's count of
 * caster-reaching failures; after a reboot the slot waits at least the full
 * delay of the stored step, so a reboot loop never shortens a long backoff.
 * Success or a user edit clears it. Network down (from the Wi-Fi disconnect
 * until the next IP), source stalls, pauses and user edits are never
 * failures; a healthy session (>= 60 s and >= 5 KB) resets the attempt count.
 * The protected slot retries a failure that never reached the caster (a
 * failed name lookup, a connect that timed out or failed locally) after a
 * flat 5 s +-20 % without advancing its count; a refused connect and every
 * failure after the TCP connection follow the rows above. Every slot reports
 * how long its current run of such failures lasts (unreached_s; the network
 * recovery ladder's "caster not reached" trigger). The tables live in a pure
 * core with host tests.
 *
 * Connection history (diag journal): DIAG_EV_CASTER_DOWN when a session that
 * reached STREAMING ends, DIAG_EV_CASTER_UP when one starts after failed
 * attempts or a gap (see diag.h for the fields and the flapping limit). A
 * record the journal's rate limit refuses waits in the slot (a few at most)
 * and is written a moment later.
 *
 * Network events: ESP-IDF's default station handlers clear the address at a
 * Wi-Fi disconnect and lwIP aborts every session bound to it, so sessions
 * do not survive a Wi-Fi drop: they close as "no network" (not a failure)
 * and new connections wait for IP_EVENT_STA_GOT_IP (the Onocoy slot at
 * once, the others 0-10 s later at random).
 *
 * Protected slot: the Onocoy slot is "protected": its availability drives
 * the daily downtime budget and the network recovery ladder, and its
 * transient backoff is capped at CONFIG_NTRIP_PROTECTED_MAX_BACKOFF_S.
 *
 * Source stall and pause: a slot with no fresh RTCM closes its session
 * gracefully after CONFIG_NTRIP_SOURCE_STALL_S (casters drop or count silent
 * sources), the protected slot after CONFIG_NTRIP_PROTECTED_SOURCE_STALL_S.
 * A pause for receiver work (configuration or a flash) is a planned source
 * stall with the same limits: paused slots stay connected and idle up to
 * their limit, then close cleanly and wait in PAUSED until the station
 * resumes them; never counted as a failure.
 *
 * Logging: request lines carry the password (NTRIP 1.0) and the Onocoy
 * mountpoint is the credential username, so requests and replies are only
 * ever logged redacted (host and slot name; never mountpoint, user or
 * password).
 *
 * Onocoy: servers.onocoy.com:2101, NTRIP 1.0 by default; the slot holds
 * what the Onocoy console gives (mountpoint, username, password). NTRIP 2.0
 * posts to /<mountpoint> with Basic <username>:<password>; NTRIP 1.0 sends
 * "SOURCE <password> /<username>" (Onocoy's instruction for 1.0-only
 * sources). A slot stored in the older layout (the credential username in
 * `mountpoint`; its stored record says so, and a username a previous
 * caster left in it is dropped on load) keeps working with the old
 * mapping, but does not count as set up (ntrip_server_onocoy_ready) until
 * the console's values are saved.
 *
 * Safe mode skips ntrip_server_init() and _start(): no slot runs. Then
 * ntrip_server_get_status() still works (every slot DISABLED) and
 * ntrip_server_slot_count() is valid, but the config calls return
 * ESP_ERR_INVALID_STATE, so nothing is written over stored slots that were
 * never loaded.
 *
 * Status: implemented; PROVISIONAL, to be confirmed on hardware and against
 * real casters: request formats, reply parsing, timeouts and backoff values.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "ntrip_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(NTRIP_EVENT);

/** NTRIP_EVENT ids; event data is the slot index (size_t). */
typedef enum {
    NTRIP_EVENT_STATE_CHANGED,
} ntrip_event_t;

typedef enum {
    NTRIP_PROTO_V1 = 1,   /**< NTRIP 1.0: SOURCE <password> /<mount> */
    NTRIP_PROTO_V2 = 2,   /**< NTRIP 2.0: HTTP/1.1 POST with Basic auth */
} ntrip_proto_t;

/** Reconnect backoff profile of a slot. */
typedef enum {
    NTRIP_BACKOFF_FAST = 0,     /**< Onocoy: short delays, the daily budget is tight */
    NTRIP_BACKOFF_STANDARD,
    NTRIP_BACKOFF_POLITE,       /**< community casters that ban hammering sources */
} ntrip_backoff_profile_t;

/** TLS certificate check of a slot. */
typedef enum {
    NTRIP_TRUST_BUNDLE = 0,     /**< the built-in CA bundle (default) */
    NTRIP_TRUST_CUSTOM_CA,      /**< the slot's own CA PEM (settings cstN/ca_pem), e.g. a self-signed caster */
    NTRIP_TRUST_INSECURE,       /**< no check: explicit opt-in, shown as a warning */
} ntrip_trust_t;

/** Failure classes (status, counters, backoff rows). */
typedef enum {
    NTRIP_FAIL_NONE = 0,
    NTRIP_FAIL_DNS,
    NTRIP_FAIL_CONNECT,
    NTRIP_FAIL_TLS,
    NTRIP_FAIL_TIMEOUT,
    NTRIP_FAIL_REMOTE_CLOSE,
    NTRIP_FAIL_WRITE_STALL,
    NTRIP_FAIL_KEEPALIVE,
    NTRIP_FAIL_AUTH,
    NTRIP_FAIL_MOUNT_BUSY,
    NTRIP_FAIL_CONFIG,
    NTRIP_FAIL_BANNED,
    NTRIP_FAIL_CLASS_COUNT,
} ntrip_fail_class_t;

/** One caster slot (persisted as one blob in settings namespace "cst<N>"). */
typedef struct {
    bool enabled;
    bool protected_slot;   /**< counts toward the daily availability budget (Onocoy) */
    char name[24];         /**< UI label */
    char host[64];
    uint16_t port;
    char mountpoint[48];   /**< Onocoy slots: the console's mountpoint (the NTRIP 2.0 POST target) */
    char username[48];     /**< NTRIP 2.0 Basic-auth user; Onocoy slots: the console's username, [A-Za-z0-9._-]
                                (NTRIP 1.0 sends it as the mountpoint) */
    char password[64];     /**< secret: never returned to the web UI */
    bool password_set;     /**< output only: a password is stored */
    bool clear_password;   /**< input only (set_config): remove the stored password */
    ntrip_proto_t proto;
    bool tls;
    ntrip_trust_t trust_mode;      /**< TLS slots only */
    bool ca_pem_set;               /**< output only: a custom CA PEM is stored for this slot */
    ntrip_backoff_profile_t backoff_profile; /**< presets set it (Onocoy: fast) */
    char label[48];                /**< [A-Za-z0-9]; only an Onocoy slot stored in the older layout uses it
                                        (its NTRIP 2.0 mountpoint, "onocoydiy" when empty) */
    bool v2_raw_body;              /**< NTRIP 2.0 quirk: send the body unchunked */
    bool dup_stream_ack;           /**< user acknowledged streaming the same data to Onocoy and an open caster */
} ntrip_caster_cfg_t;

typedef enum {
    NTRIP_STATE_DISABLED = 0,
    NTRIP_STATE_WAIT_NETWORK,   /**< no IP yet */
    NTRIP_STATE_WAIT_DATA,      /**< connected network, but no RTCM to send */
    NTRIP_STATE_CONNECTING,     /**< DNS, TCP/TLS connect, request, reply */
    NTRIP_STATE_STREAMING,
    NTRIP_STATE_BACKOFF,        /**< waiting before the next attempt (transient or mountpoint busy) */
    NTRIP_STATE_AUTH_FAILED,    /**< credentials rejected past the transient window; still retried per profile */
    NTRIP_STATE_PAUSED,         /**< station paused output (receiver config/flash) */
    NTRIP_STATE_CONFIG_ERROR,   /**< caster refused the request itself (unknown mountpoint, bad request, certificate); retried per profile */
    NTRIP_STATE_BANNED,         /**< caster says this source is banned; retried after hours */
} ntrip_state_t;

/** Live status of one slot. "today" = the current UTC day. */
typedef struct {
    ntrip_state_t state;
    ntrip_fail_class_t reason;  /**< class of the last failure */
    uint32_t state_age_s;       /**< seconds in the current state */
    uint32_t progress_age_ms;   /**< since the slot task last made progress (liveness) */
    uint32_t connects;          /**< successful connections */
    uint32_t failures;          /**< failed attempts */
    uint32_t failures_by_class[NTRIP_FAIL_CLASS_COUNT];
    uint32_t disconnects;       /**< drops after streaming */
    uint32_t restarts;          /**< forced by the health monitor */
    int64_t connected_since;    /**< unix time the session started, 0 = none or no clock */
    uint32_t connected_s;       /**< seconds in the current session */
    uint32_t today_online_s;    /**< the availability ledger's today: streamed with fresh RTCM (kept across restarts) */
    uint32_t today_offline_s;   /**< the availability ledger's today: expected to stream but did not, planned included */
    bool day_metrics_valid;     /**< protected slot, enabled and set up (ntrip_server_onocoy_ready): the
                                     figures below mean something; false = unknown (the web shows null) */
    uint16_t graced_uptime_permille; /**< protected slot: min(1, (online today + grant) / 86400), the share of the
                                     whole day covered so far */
    bool streak_ok;             /**< protected slot: today's offline time is within the day's budget */
    uint16_t data_rate_permille;/**< protected slot: epochs sent / epochs expected while connected today in this
                                     boot (estimate) */
    bool data_rate_valid;       /**< ... and something was sent today (else the rate is unknown) */
    uint32_t budget_s;          /**< protected slot: offline seconds the day allows at that data rate (estimate) */
    uint32_t budget_left_s;     /**< protected slot: budget_s minus today's offline seconds, 0 once used up */
    uint16_t graced_uptime_so_far_permille; /**< protected slot: 1 - max(0, unplanned offline today - grant) /
                                     time elapsed today (from 00:00 UTC or from when the slot first became
                                     expected today); valid with so_far_valid */
    bool so_far_valid;          /**< day_metrics_valid and a trusted clock */
    uint64_t bytes_sent;
    uint64_t frames_sent;
    uint64_t epochs_sent;       /**< ended by their closing MSM, or by the next epoch time when it never came */
    uint32_t epochs_sent_today;
    uint64_t frames_dropped;    /**< lost while streaming (hub slow-consumer policy) */
    uint64_t bytes_dropped;
    uint64_t epochs_dropped;
    uint32_t epochs_dropped_today;
    uint64_t frames_skipped_idle; /**< expected skips (attach, resync after an outage or pause) */
    uint32_t lag_drops;
    uint32_t laps;
    uint32_t lag_ms_now;        /**< age of the oldest unsent frame */
    uint32_t max_lag_ms;
    uint32_t backoff_attempt;   /**< consecutive failures counted for the backoff */
    uint32_t next_retry_s;      /**< seconds until the next attempt */
    int last_status_code;       /**< last caster HTTP/ICY status, 0 = none */
    char last_status_line[97];  /**< raw first reply line (never contains credentials) */
    char server_header[65];     /**< caster's Server: header */
    char resolved_ip[46];
    uint32_t dns_cached_used;   /**< attempts that used the cached address after a DNS failure */
    uint32_t unreached_s;       /**< since the first of the current run of failures that never reached the caster (DNS, connect timed out or failed locally); 0 = no such run */
    char tls_version[16];
    char tls_cipher[48];
    char hint[64];              /**< e.g. "mountpoint held by another source" */
    esp_err_t last_err;
} ntrip_caster_status_t;

/** Load slot configs from settings, create the slot tasks (idle), register the shutdown hook. */
esp_err_t ntrip_server_init(void);

/** Let enabled slots connect. */
esp_err_t ntrip_server_start(void);

/** Close all sessions (slots stay created, idle). */
esp_err_t ntrip_server_stop(void);

/** Number of slots (CONFIG_NTRIP_MAX_CASTERS). */
size_t ntrip_server_slot_count(void);

/**
 * Read a slot's config.
 * @param with_secret false blanks the password (for anything shown to users);
 *                    password_set still tells whether one is stored.
 * @return ESP_ERR_INVALID_STATE before ntrip_server_init() (safe mode).
 */
esp_err_t ntrip_server_get_config(size_t slot, ntrip_caster_cfg_t *out, bool with_secret);

/**
 * Validate, persist (one blob, replaced as a unit) and apply a slot config;
 * only that slot reconnects.
 *
 * Password: a non-empty one replaces the stored one; clear_password removes
 * it. An empty password keeps the stored one (the UI never sees it, so it
 * cannot send it back) only while the host stays the same: pointed at
 * another host, the slot never sends the old caster's password there.
 *
 * Fields are checked before saving: no control characters; host a DNS name
 * or IP literal; mountpoint [A-Za-z0-9._-]; NTRIP 1.0 password without
 * spaces; NTRIP 2.0 username without ':'; an enabled slot needs host, port,
 * mountpoint, a password (1.0) and a username (2.0, except Onocoy). The
 * host rules (ntrip_server_apply_host_rules(), with no requested profile)
 * then set protected_slot and the backoff profile.
 *
 * Cross-slot rules (this component owns them; callers only name the
 * reason): enabling a slot must not stream to Onocoy or RTK2go from a second
 * slot, and pairing Onocoy with an open caster (RTK2go, Centipede) needs
 * dup_stream_ack on the slot being saved. They are checked against the
 * stored configs of the other slots under the same lock as the save.
 * @param policy optional (NULL = not wanted): the rule the save breaks when
 *        the call returns ESP_ERR_NOT_ALLOWED, NTRIP_POLICY_OK otherwise.
 * @return ESP_ERR_INVALID_ARG for a field that fails these checks,
 *         ESP_ERR_NOT_ALLOWED when a cross-slot rule refuses it (*policy
 *         names which), ESP_ERR_INVALID_STATE before ntrip_server_init()
 *         (safe mode).
 */
esp_err_t ntrip_server_set_config(size_t slot, const ntrip_caster_cfg_t *cfg, ntrip_policy_t *policy);

/** Live status of a slot. Works before ntrip_server_init() (every slot DISABLED). */
esp_err_t ntrip_server_get_status(size_t slot, ntrip_caster_status_t *out);

/** Drop and reconnect one slot (health monitor, after a budget overrun). */
esp_err_t ntrip_server_restart_slot(size_t slot);

/**
 * Pause/resume all slots for receiver work (a planned source stall). Paused
 * slots stay connected and idle up to their source-stall limit
 * (CONFIG_NTRIP_SOURCE_STALL_S, the protected slot
 * CONFIG_NTRIP_PROTECTED_SOURCE_STALL_S), then close cleanly and wait in
 * PAUSED; never a failure. The ledger counts the time as planned downtime.
 */
esp_err_t ntrip_server_set_paused(bool paused);

/** Fill a slot config with the Onocoy endpoint (credentials left empty, protected, fast backoff). */
void ntrip_server_onocoy_defaults(ntrip_caster_cfg_t *out);

/**
 * An Onocoy slot that is set up: Onocoy host, mountpoint, username and a
 * password (stored, password_set, or in the struct). Pure; no init needed.
 */
bool ntrip_server_onocoy_ready(const ntrip_caster_cfg_t *cfg);

/**
 * The host rules of a slot (this component owns them, like the cross-slot
 * rules): `protected_slot` is set exactly for a host in the onocoy.com
 * domain; the backoff profile is fast for that host, polite for rtk2go.com,
 * and elsewhere `requested` (standard or polite; fast is for Onocoy only)
 * or, with `requested` NULL, the profile cfg has, a fast one left from an
 * Onocoy host becoming standard. Hosts match by domain, case-insensitively.
 * The web API applies it to a request (to name a refused field);
 * ntrip_server_set_config() applies it to every save (with NULL), so a
 * stored slot always follows the rules. Pure; no init needed.
 * @return false (cfg unchanged) if `requested` is fast for a host that is
 *         not Onocoy.
 */
bool ntrip_server_apply_host_rules(ntrip_caster_cfg_t *cfg, const ntrip_backoff_profile_t *requested);

#ifdef __cplusplus
}
#endif
