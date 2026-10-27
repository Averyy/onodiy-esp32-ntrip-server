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
 * Planned closes end on a whole epoch: before the shutdown's close, and
 * before the close for new settings (ntrip_server_set_config) or a restart
 * by the health monitor (ntrip_server_restart_slot), a session that is
 * inside an epoch (it sent an MSM that announced more) sends the rest of it
 * and nothing of the next (rtcm_hub_finish_epoch): at most 1 s, on a reboot
 * at most half the shutdown budget left. Only terminated epochs count for
 * Onocoy, and a close in the receiver's MSM burst would otherwise cut one.
 * A close for a lost network or a failure is at once.
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
 * Onocoy login guard (slots that count as Onocoy, ntrip_slot_is_onocoy;
 * the rules and their values are in ntrip_proto.h, all PROVISIONAL): a
 * failing login is never looped, because Onocoy may block logins from the
 * station's whole public IP and two devices on one credential knock each
 * other off. Onocoy's reason texts are matched in the reply line, the reply
 * body (read for every non-success HTTP reply on these slots) and every
 * line the caster sends during a session (status last_session_text). A
 * refused login waits 300 s (30 s once right after a session on
 * credentials that worked), then 1800 s, and the third stops the slot;
 * "too many unsuccessful logins" waits 1 h, then 2 h, then stops; "in use"
 * stops at once unless it comes within 300 s of this station's own session
 * on credentials that worked (then 15, 30, 60, 120 s, then stop); sessions
 * the caster keeps ending soon after login (a take-over by another device)
 * stop it after a score of 3; a duplicate-stream or banned report stops it
 * at once. A stopped slot (NTRIP_STATE_STOPPED, status stop_reason) makes
 * no attempt at all, across restarts (stored with its backoff record),
 * until the user saves the slot, retries it (ntrip_server_retry_slot) or
 * disables it; each stop is logged once and journalled (DIAG_EV_CASTER_STOP).
 * It still counts as downtime and as a slot that is down, but the network
 * ladder does not act on it (a network action cannot fix a credential).
 *
 * Test Onocoy slots (CONFIG_NTRIP_TEST_ONOCOY_SLOT, bench and QEMU builds
 * only): a slot marked test_onocoy counts as Onocoy in every respect
 * whatever its host (protected, fast profile, the Onocoy fields and wire
 * names, the one-Onocoy rule, the guard), so a local test caster sees what
 * Onocoy would. A build without the option ignores the stored mark (the
 * slot is judged by its host) and drops it at the next save.
 *
 * RTK2go mountpoints: a save with a new RTK2go host or a changed
 * mountpoint must follow RTK2go's rules for new reservations
 * (ntrip_server_rtk2go_check); a slot stored before keeps its name.
 *
 * Source stall and pause: a slot with no fresh RTCM closes its session
 * gracefully after CONFIG_NTRIP_SOURCE_STALL_S (casters drop or count silent
 * sources), the protected slot after CONFIG_NTRIP_PROTECTED_SOURCE_STALL_S.
 * A pause for receiver work (configuration or a flash) is a planned source
 * stall with the same limits: paused slots stay connected and idle up to
 * their limit, then close cleanly and wait in PAUSED until the station
 * resumes them; never counted as a failure. The station holds the RTCM hub
 * at an epoch boundary first (rtcm_hub_hold): a slot still behind it sends
 * the rest up to that point before it idles, and after the release the hub
 * starts it over at a whole epoch (sticky copies first).
 *
 * Logging: request lines carry the password (NTRIP 1.0) and an Onocoy
 * slot's 1.0 mountpoint is the credential username, so requests and replies
 * are only ever logged redacted (host and slot name; never mountpoint, user
 * or password).
 *
 * Onocoy: servers.onocoy.com:2101, NTRIP 2.0 by default (Onocoy's guide);
 * the slot holds the console's credential (username and password) and the
 * mountpoint the user chose (any name: it only labels the station in the
 * Onocoy dashboard). NTRIP 2.0 posts to /<mountpoint> with Basic
 * <username>:<password>; NTRIP 1.0, the fallback, sends "SOURCE <password>
 * /<username>" (Onocoy's instruction for 1.0-only sources; the mountpoint is
 * then not sent). An Onocoy slot without any credential takes the default
 * version when loaded (ntrip_server_onocoy_blank_proto); a set-up slot keeps
 * the version it was saved with. A slot stored in the older layout (the
 * credential username in `mountpoint`; its stored record says so, and a
 * username a previous caster left in it is dropped on load) keeps working
 * with the old mapping and its version, but does not count as set up
 * (ntrip_server_onocoy_ready) until the console's values are saved.
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
#include "ntrip_tx_diag.h"

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
    NTRIP_FAIL_LOGIN_BLOCKED,   /**< Onocoy: too many unsuccessful logins (login guard slots) */
    NTRIP_FAIL_IN_USE,          /**< Onocoy: another stream uses the credential (login guard slots) */
    NTRIP_FAIL_REPLACED,        /**< Onocoy: another device logged in with the credential (login guard slots) */
    NTRIP_FAIL_LOCAL,           /**< a send or receive failed on this station's side (memory, no or stale errno); never a take-over */
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
    char password[64];     /**< secret: only in the web UI's Show answer */
    bool password_set;     /**< output only: a password is stored */
    bool clear_password;   /**< input only (set_config): remove the stored password */
    ntrip_proto_t proto;
    bool tls;
    ntrip_trust_t trust_mode;      /**< TLS slots only */
    bool ca_pem_set;               /**< output only: a custom CA PEM is stored for this slot */
    ntrip_backoff_profile_t backoff_profile; /**< presets set it (Onocoy: fast) */
    char label[48];                /**< [A-Za-z0-9]; only an Onocoy slot stored in the older layout uses it
                                        (its NTRIP 2.0 mountpoint, "onodiy" when empty) */
    bool v2_raw_body;              /**< NTRIP 2.0 quirk: send the body unchunked */
    bool dup_stream_ack;           /**< user acknowledged streaming the same data to Onocoy and an open caster */
    bool onocoy_stats;             /**< explicit opt-in to downloading daily statistics with this credential */
    bool test_onocoy;              /**< bench and QEMU builds only: the slot counts as Onocoy whatever its host
                                        (ignored without CONFIG_NTRIP_TEST_ONOCOY_SLOT) */
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
    NTRIP_STATE_STOPPED,        /**< the Onocoy login guard stopped the slot (stop_reason): no attempt until the user acts */
} ntrip_state_t;

/** Live status of one slot. "today" = the current UTC day. */
typedef struct {
    ntrip_state_t state;
    ntrip_fail_class_t reason;  /**< class of the last failure (kept after a success) */
    ntrip_fail_class_t retry_reason; /**< class of the failure the slot's retries follow now: NONE once it
                                     connected, before any failure and after the user's edit or retry */
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
    uint16_t data_rate_permille;/**< protected slot: epochs sent / epochs expected while online today in this
                                     boot (online as the ledger counts it; estimate) */
    bool data_rate_valid;       /**< ... and something was sent today (else the rate is unknown) */
    uint32_t budget_s;          /**< protected slot: offline seconds the day allows at that data rate (estimate) */
    uint32_t budget_left_s;     /**< protected slot: budget_s minus today's offline seconds, 0 once used up */
    uint16_t graced_uptime_so_far_permille; /**< protected slot: 1 - max(0, unplanned offline today - grant) /
                                     time elapsed today (from 00:00 UTC or from when the slot first became
                                     expected today); valid with so_far_valid */
    bool so_far_valid;          /**< day_metrics_valid and a trusted clock */
    uint64_t bytes_sent;
    uint64_t bytes_sent_today;  /**< this start, current UTC day (RAM, rolled over like epochs_sent_today) */
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
    /* the Onocoy login guard (always NONE / 0 for other slots) */
    ntrip_stop_t stop_reason;   /**< STOPPED: why; NTRIP_STOP_NONE otherwise */
    uint8_t login_rejects;      /**< refused logins since the last healthy session (the 3rd stops) */
    uint8_t login_blocks;       /**< "too many unsuccessful logins" since the last healthy session (the 3rd stops) */
    uint8_t kicked_sessions;    /**< take-over score: sessions ended within 120 s count 1, a "device reconnected"
                                     text 2; 3 stops (reported at most 3); a session of 10 min or more resets it */
    char last_session_text[97]; /**< the newest line the caster sent during a session, sanitised ("" = none) */
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
 * mountpoint, a password (1.0) and a username (2.0, except Onocoy); a new
 * RTK2go host or a changed RTK2go mountpoint follows RTK2go's rules
 * (ntrip_server_rtk2go_check, against the stored slot). The host rules
 * (ntrip_server_apply_host_rules(), with no requested profile) then set
 * protected_slot and the backoff profile. A save clears a stopped slot's
 * stop and its login guard's counts. Another host (case aside) is another
 * server: the slot's figures start over, as a new slot's (status counters,
 * today's data rate and Onocoy day figures, ledger_forget_slot()); the same
 * host keeps them.
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

/**
 * Remove a slot's server: the slot is empty again, as a slot after the first
 * is before it is ever set up (no host, off, no name, no credentials, no
 * duplicate-stream acknowledgement), saved through ntrip_server_set_config(),
 * and its custom CA PEM is erased. Its figures start over (another host,
 * ntrip_server_set_config()). Only that slot disconnects.
 * @return ESP_ERR_INVALID_STATE before ntrip_server_init() (safe mode), or
 *         the settings error of the save or the erase.
 */
esp_err_t ntrip_server_remove_slot(size_t slot);

/** Live status of a slot. Works before ntrip_server_init() (every slot DISABLED). */
esp_err_t ntrip_server_get_status(size_t slot, ntrip_caster_status_t *out);

/** Passive numeric sender diagnostics, including a live pending write.
 * No transport API is called by getters. UTC is trusted whole seconds only;
 * monotonic stamps and generation identify writes without wall-clock jumps. */
esp_err_t ntrip_server_get_tx_diag(size_t slot, ntrip_tx_diag_t *out);

/** Newest min(cap, 8) completed slow/failed writes, oldest first. RAM only.
 * A concurrent replacement retries once, then returns ESP_ERR_TIMEOUT and
 * count=0; individual record copies never hold a lock over the whole export. */
esp_err_t ntrip_server_get_tx_history(size_t slot, ntrip_tx_write_diag_t *out, size_t cap, size_t *count);

/** Drop and reconnect one slot (health monitor, after a budget overrun). */
esp_err_t ntrip_server_restart_slot(size_t slot);

/**
 * The user's retry: a stopped slot (or one waiting out a backoff) may try
 * once at once; the stop, the login guard's counts and the backoff are
 * cleared as by a save with the same settings.
 * @return ESP_ERR_INVALID_STATE when the slot is not enabled, or before
 *         ntrip_server_init() (safe mode).
 */
esp_err_t ntrip_server_retry_slot(size_t slot);

/**
 * Pause/resume all slots for receiver work (a planned source stall). The
 * station holds the hub before the pause and releases it before the resume
 * (rtcm_hub_hold, rtcm_hub_release). Paused slots finish what the hub has
 * before its hold point, then stay connected and idle up to their source-stall limit
 * (CONFIG_NTRIP_SOURCE_STALL_S, the protected slot
 * CONFIG_NTRIP_PROTECTED_SOURCE_STALL_S), then close cleanly and wait in
 * PAUSED; never a failure. The ledger counts the time as planned downtime.
 */
esp_err_t ntrip_server_set_paused(bool paused);

/**
 * Fill a slot config with the Onocoy endpoint: servers.onocoy.com:2101,
 * NTRIP 2.0 (Onocoy's guide for 2.0 sources), credentials left empty,
 * protected, fast backoff. Also the API's onocoy_preset.
 */
void ntrip_server_onocoy_defaults(ntrip_caster_cfg_t *out);

/**
 * An Onocoy slot that holds no credential at all (no mountpoint, username
 * or password: never set up, like the factory slot) takes the preset's NTRIP
 * version, so a slot stored while 1.0 was the default is set up over 2.0
 * like a new one. A slot with any credential is never changed: one that is
 * set up, and one in the older layout that still streams (not set up by
 * ntrip_server_onocoy_ready, but working). The caster component applies it
 * to every slot it loads (in RAM; the next save stores it). Pure.
 * @return true if the version changed.
 */
bool ntrip_server_onocoy_blank_proto(ntrip_caster_cfg_t *cfg);

/**
 * An Onocoy slot that is set up: a slot that counts as Onocoy
 * (ntrip_slot_is_onocoy), mountpoint, username and a password (stored,
 * password_set, or in the struct). Pure; no init needed.
 */
bool ntrip_server_onocoy_ready(const ntrip_caster_cfg_t *cfg);

/**
 * The slot counts as Onocoy: its host is in onocoy.com, or it is marked
 * test_onocoy and the build allows that (ntrip_test_onocoy_allowed). Every
 * "is this slot Onocoy" decision uses this, never the host alone. Pure.
 */
bool ntrip_slot_is_onocoy(const ntrip_caster_cfg_t *cfg);

/** The build allows test Onocoy slots (CONFIG_NTRIP_TEST_ONOCOY_SLOT: bench and QEMU builds). */
bool ntrip_test_onocoy_allowed(void);

/** Host tests only (the linux target; the firmware has no such function): switch the option. */
void ntrip_test_onocoy_set_allowed(bool allowed);

/**
 * Onocoy and an open caster at once (a warning, never a refusal): `cfgs`
 * holds n slot configs; the result has a bit per enabled slot that counts
 * as Onocoy (ntrip_slot_is_onocoy) while at least one other enabled slot
 * streams to an open caster (RTK2go, Centipede), whose streams anyone may
 * take, and *open_mask (optional) a bit per such open-caster slot; both 0
 * without such a pair. Onocoy's FAQ: "multiple submissions of the same
 * streams, e.g. from free or public networks such as IGS or RTK2GO, even
 * when done by separate users in the onocoy platform, will result in
 * banning." Saving such a pair needs dup_stream_ack (above); this tells it
 * while it lasts. At most 32 slots are looked at. Pure; no init needed.
 */
uint32_t ntrip_server_open_caster_pair(const ntrip_caster_cfg_t *cfgs, size_t n, uint32_t *open_mask);

/** The slot streams to an open caster (RTK2go, Centipede): anyone may take its stream. Pure. */
bool ntrip_slot_is_open_caster(const ntrip_caster_cfg_t *cfg);

/** The slot streams to RTK2go (the host is in rtk2go.com). Pure. */
bool ntrip_slot_is_rtk2go(const ntrip_caster_cfg_t *cfg);

/**
 * The host rules of a slot (this component owns them, like the cross-slot
 * rules): `protected_slot` is set exactly for a slot that counts as Onocoy
 * (ntrip_slot_is_onocoy: a host in the onocoy.com domain, or a test Onocoy
 * slot); the backoff profile is fast for such a slot, polite for
 * rtk2go.com, and elsewhere `requested` (standard or polite; fast is for
 * Onocoy only) or, with `requested` NULL, the profile cfg has, a fast one
 * left from an Onocoy host becoming standard. Hosts match by domain,
 * case-insensitively. A test_onocoy mark the build does not allow is
 * cleared. The web API applies it to a request (to name a refused field);
 * ntrip_server_set_config() applies it to every save (with NULL), so a
 * stored slot always follows the rules. Pure; no init needed.
 * @return false (cfg unchanged) if `requested` is fast for a slot that is
 *         not Onocoy.
 */
bool ntrip_server_apply_host_rules(ntrip_caster_cfg_t *cfg, const ntrip_backoff_profile_t *requested);

/**
 * RTK2go's mountpoint rules for a save (`old_*` as stored, "" for a new
 * slot): NTRIP_RTK2GO_OK unless the new host is in rtk2go.com and there was
 * no stored host, the host changed or the mountpoint changed, and the new
 * mountpoint breaks one of them (ntrip_policy.h; an empty mountpoint is left
 * to the field rules). So a slot stored before the rules keeps streaming and
 * can be enabled and disabled unchanged. The web API names the rule;
 * ntrip_server_set_config() refuses it too. Pure; no init needed.
 */
ntrip_rtk2go_mount_t ntrip_server_rtk2go_check(const char *old_host, const char *old_mount, const char *new_host,
                                               const char *new_mount);

#ifdef __cplusplus
}
#endif
