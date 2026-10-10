/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * NTRIP source protocol core (pure logic, no IDF or RTOS includes, injected
 * clock and random numbers, host-tested).
 *
 *   - field validation for a caster slot and the cross-slot rules (one Onocoy
 *     slot, one RTK2go slot, acknowledged duplicate streams);
 *   - request builders: NTRIP 1.0 "SOURCE" and NTRIP 2.0 HTTP POST with Basic
 *     auth, both with the "NTRIP <product>/<version>" agent;
 *   - the chunked transfer encoder (one chunk per hub read);
 *   - the reply parser and classifier: the first reply line is matched
 *     against an ordered table (first match wins), HTTP headers are read for
 *     Server: and Ntrip-Version:, and HTML or 4xx replies get a body check
 *     for "banned";
 *   - the backoff calculator (fast, standard and polite profiles, +-20 %
 *     jitter, MOUNT_BUSY escalation, the AUTH transient window, the polite
 *     hourly limit) and the per-slot state machine that uses it, including
 *     the run of failures that never reached the caster (the network
 *     ladder's trigger S, and the protected slot's flat retry);
 *   - the persisted long-backoff record and the Onocoy daily budget estimate;
 *   - the connection history's journal records and their flapping limit.
 *
 * Times are wrapping u32 milliseconds, only compared as (uint32_t)(now -
 * then); no single delay or window here comes near 2^31 ms.
 *
 * The enums below mirror the public ones in ntrip_server.h value for value
 * (ntrip_server.c static-asserts that), so the core needs no IDF header. The
 * cross-slot policy result is the public type itself (ntrip_policy.h holds
 * only types).
 *
 * Status: PROVISIONAL, to be confirmed against real casters: reply texts,
 * timeouts and backoff values.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ntrip_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Protocol limits and timeouts ----------------------------------------- */

#define NTRIP_AGENT_PRODUCT       "onocoydiy-esp-um982"
#define NTRIP_AGENT_MAX           80u     /**< "NTRIP " + product + "/" + version */
#define NTRIP_REQUEST_MAX         640u    /**< largest v2 request with maximum field lengths */

#define NTRIP_REQUEST_WRITE_MS    5000u   /**< whole request written within this */
#define NTRIP_REPLY_LINE_MS       10000u  /**< first reply line, from the end of the request */
#define NTRIP_REPLY_HEADERS_MS    2000u   /**< HTTP header remainder */
#define NTRIP_REPLY_BODY_MS       2000u   /**< body check */
#define NTRIP_TERMINATOR_MS       1000u   /**< NTRIP 2.0 final "0" chunk on a clean close */
#define NTRIP_FRESH_DATA_MS       2000u   /**< receiver RTCM at most this old before connecting */

#define NTRIP_REPLY_LINE_MAX      1024u   /**< first line longer than this: CONFIG */
#define NTRIP_REPLY_HEADERS_MAX   4096u   /**< header bytes read after the status line */
#define NTRIP_REPLY_BODY_MAX      2048u   /**< bytes searched for "banned" */
#define NTRIP_REPLY_HDR_KEEP      160u    /**< longest header line kept for matching */

#define NTRIP_KEEPALIVE_IDLE_S    10
#define NTRIP_KEEPALIVE_INTVL_S   5
#define NTRIP_KEEPALIVE_COUNT     3

#define NTRIP_HEALTHY_SESSION_MS  60000u  /**< a session this long ... */
#define NTRIP_HEALTHY_SESSION_B   5120u   /**< ... with this much sent resets the backoff */
#define NTRIP_BUSY_HINT_MS        (3u * 60u * 1000u)   /**< MOUNT_BUSY this long: hint */
#define NTRIP_BUSY_ESCALATE_MS    (10u * 60u * 1000u)  /**< MOUNT_BUSY this long: AUTH row */
#define NTRIP_FAST_LATE_MS        (10u * 60u * 1000u)  /**< fast profile: raised TRANSIENT cap after this */
#define NTRIP_POLITE_HOURLY_AFTER 20u     /**< caster-reaching failures before the hourly limit */
#define NTRIP_HOURLY_MS           (3600u * 1000u)
#define NTRIP_STAGGER_MAX_MS      10000u  /**< non-protected slots after the network returns */
#define NTRIP_TLS_PLAIN_HINT_AFTER 10u    /**< consecutive TLS failures on an Onocoy slot */
/**
 * Protected slot: a failure that never reached the caster (a failed name
 * lookup, a connect that timed out or failed locally) retries after this x
 * U(0.8, 1.2), without advancing the backoff count: the TRANSIENT row's
 * rationale (do not hammer the caster) does not cover a caster that never
 * saw the attempt. PROVISIONAL, to be confirmed on hardware.
 */
#define NTRIP_UNREACHED_RETRY_MS  5000u

/** Daily availability rule used for the Onocoy budget estimate. */
#define NTRIP_DAY_S               86400u
#define NTRIP_GRACE_S             300u
#define NTRIP_TARGET_PERMILLE     980u    /**< availability (uptime score x data rate) to keep the streak */

/** Chunk framing: room reserved before the payload for the size line. */
#define NTRIP_CHUNK_HEAD_RESERVE  6u      /**< "1000\r\n" for a 4096-byte chunk */
#define NTRIP_CHUNK_TAIL          2u      /**< "\r\n" after the payload */
#define NTRIP_CHUNK_MAX_PAYLOAD   4096u
#define NTRIP_CHUNK_TERMINATOR    "0\r\n\r\n"

/* ---- Enums (mirror ntrip_server.h) ------------------------------------------ */

/** Failure classes, same values as ntrip_fail_class_t. */
typedef enum {
    NTRIP_CLS_NONE = 0,
    NTRIP_CLS_DNS,
    NTRIP_CLS_CONNECT,
    NTRIP_CLS_TLS,
    NTRIP_CLS_TIMEOUT,
    NTRIP_CLS_REMOTE_CLOSE,
    NTRIP_CLS_WRITE_STALL,
    NTRIP_CLS_KEEPALIVE,
    NTRIP_CLS_AUTH,
    NTRIP_CLS_MOUNT_BUSY,
    NTRIP_CLS_CONFIG,
    NTRIP_CLS_BANNED,
    NTRIP_CLS_COUNT,
} ntrip_cls_t;

/** Backoff profiles, same values as ntrip_backoff_profile_t. */
typedef enum {
    NTRIP_PROF_FAST = 0,
    NTRIP_PROF_STANDARD,
    NTRIP_PROF_POLITE,
} ntrip_prof_t;

/** Slot states, same values as ntrip_state_t. */
typedef enum {
    NTRIP_SM_DISABLED = 0,
    NTRIP_SM_WAIT_NETWORK,
    NTRIP_SM_WAIT_DATA,
    NTRIP_SM_CONNECTING,
    NTRIP_SM_STREAMING,
    NTRIP_SM_BACKOFF,
    NTRIP_SM_AUTH_FAILED,
    NTRIP_SM_PAUSED,
    NTRIP_SM_CONFIG_ERROR,
    NTRIP_SM_BANNED,
} ntrip_sm_state_t;

/** Backoff row a failure follows. */
typedef enum {
    NTRIP_ROW_NONE = 0,     /**< success */
    NTRIP_ROW_TRANSIENT,
    NTRIP_ROW_MOUNT_BUSY,
    NTRIP_ROW_AUTH,
    NTRIP_ROW_CONFIG,
    NTRIP_ROW_BANNED,
} ntrip_row_t;

/** Status hints (the server maps them to UI text). */
typedef enum {
    NTRIP_HINT_NONE = 0,
    NTRIP_HINT_V1_ONLY,          /**< v2 rejected without an Ntrip-Version header */
    NTRIP_HINT_SOURCETABLE,      /**< caster sent its sourcetable: wrong host or protocol */
    NTRIP_HINT_MOUNT_HELD,       /**< MOUNT_BUSY for minutes: another source holds it */
    NTRIP_HINT_TLS_CERT,         /**< certificate check failed */
    NTRIP_HINT_TLS_TRY_PLAIN,    /**< TLS keeps failing on an Onocoy slot */
    NTRIP_HINT_CASTER_BUSY,      /**< transient rejection (server full, restarting) */
    NTRIP_HINT_BANNED,
    NTRIP_HINT_AGENT,            /**< caster rejected the agent string */
    NTRIP_HINT_UNEXPECTED,       /**< reply matched no known form */
    NTRIP_HINT_NO_REPLY,
    NTRIP_HINT_COUNT,
} ntrip_hint_t;

/* ---- Field validation -------------------------------------------------------- */

typedef struct {
    const char *host;
    uint16_t port;
    const char *mountpoint;
    const char *username;
    const char *password;
    const char *label;
    bool v2;
    bool enabled;
} ntrip_fields_t;

typedef enum {
    NTRIP_FIELDS_OK = 0,
    NTRIP_FIELDS_BAD_HOST,
    NTRIP_FIELDS_BAD_PORT,
    NTRIP_FIELDS_BAD_MOUNTPOINT,
    NTRIP_FIELDS_BAD_USERNAME,
    NTRIP_FIELDS_BAD_PASSWORD,
    NTRIP_FIELDS_BAD_LABEL,
    NTRIP_FIELDS_MISSING,        /**< an enabled slot lacks a required field */
} ntrip_fields_result_t;

/**
 * Check one slot's fields (header injection and caster syntax rules):
 * no control characters anywhere; host = DNS name (labels [A-Za-z0-9-],
 * <= 63 each) or IP literal, <= 63 characters; mountpoint [A-Za-z0-9._-];
 * v1 password printable ASCII without spaces; v2 username without ':';
 * label [A-Za-z0-9]. An enabled slot needs host, port and mountpoint, a v1
 * slot a password, and a v2 slot that is not an Onocoy slot a username
 * (an Onocoy slot authenticates with its mountpoint, the credential username).
 */
ntrip_fields_result_t ntrip_validate_fields(const ntrip_fields_t *f);

/** True if host equals `domain` or ends in "." + domain (case-insensitive). */
bool ntrip_host_in_domain(const char *host, const char *domain);

/** Onocoy slot: host in onocoy.com. */
bool ntrip_host_is_onocoy(const char *host);

/** RTK2go: host in rtk2go.com (a community caster: polite backoff, one slot). */
bool ntrip_host_is_rtk2go(const char *host);

/** Caster whose streams anyone can use without credentials (RTK2go, Centipede). */
bool ntrip_host_is_open_caster(const char *host);

typedef struct {
    bool enabled;
    const char *host;
    bool dup_ack;
} ntrip_policy_slot_t;

/**
 * Cross-slot rules for saving slot `edited` (slots[] holds the result of the
 * save): a second enabled Onocoy slot, a second RTK2go slot, or Onocoy
 * together with an open caster (RTK2go, Centipede) without the edited slot's
 * acknowledgement. Hosts match by domain, case-insensitively; a disabled
 * edited slot breaks nothing. The only place these rules live: the web API
 * names the reason from what ntrip_server_set_config() reports.
 */
ntrip_policy_t ntrip_check_policy(const ntrip_policy_slot_t *slots, size_t count, size_t edited);

/* ---- Requests ---------------------------------------------------------------- */

/** Overwrite a buffer in a way the compiler does not drop. */
void ntrip_secure_zero(void *p, size_t len);

/** Standard base64 with padding; returns the length (excluding NUL) or 0 if cap is too small. */
size_t ntrip_base64(const uint8_t *in, size_t len, char *out, size_t cap);

/**
 * Agent string "NTRIP onocoydiy-esp-um982/<version>": a leading 'v' is
 * dropped and anything but printable non-space ASCII becomes '_'.
 */
size_t ntrip_build_agent(const char *app_version, char *out, size_t cap);

/**
 * What goes on the wire for a slot. Onocoy slots carry the credential
 * username in `mountpoint`: NTRIP 1.0 sends it as the mountpoint; NTRIP 2.0
 * sends it as the Basic-auth user and the dashboard label (or "onocoydiy")
 * as the mountpoint.
 */
typedef struct {
    const char *mount;
    const char *user;    /**< v2 only */
} ntrip_wire_names_t;

void ntrip_wire_names(const char *host, bool v2, const char *mountpoint, const char *username,
                      const char *label, ntrip_wire_names_t *out);

/** "SOURCE <password> /<mount>\r\nSource-Agent: <agent>\r\n\r\n". @return length or 0. */
size_t ntrip_build_v1_request(char *buf, size_t cap, const char *password, const char *mount, const char *agent);

/**
 * NTRIP 2.0 POST with Host (port unless the scheme default: 80 plain, 443
 * TLS), Ntrip-Version, User-Agent, Basic Authorization, Connection: close
 * and, unless raw_body, Transfer-Encoding: chunked. The "user:password"
 * scratch is zeroed before returning; the caller zeroes buf after sending.
 * @return length or 0 (overflow or a field with CR/LF).
 */
size_t ntrip_build_v2_request(char *buf, size_t cap, const char *host, uint16_t port, bool tls,
                              const char *mount, const char *user, const char *password,
                              const char *agent, bool raw_body);

/* ---- Chunked transfer encoding ------------------------------------------------ */

/**
 * Wrap the payload at buf[NTRIP_CHUNK_HEAD_RESERVE .. + len) as one chunk:
 * lowercase hex size line (no leading zeros) right before it, CRLF after it.
 * buf must hold NTRIP_CHUNK_HEAD_RESERVE + len + NTRIP_CHUNK_TAIL bytes.
 * @param start offset of the first byte to send.
 * @return bytes to send from buf + *start; 0 for an empty or oversized
 *         payload (a zero-length chunk would end the body).
 */
size_t ntrip_chunk_wrap(uint8_t *buf, size_t len, size_t *start);

/* ---- RTCM frames in a hub read ---------------------------------------------- */

/**
 * Count whole RTCM3 frames in buf and the MSM frames that end an epoch
 * (multiple-message bit 0). Stops at the first byte that is not a frame.
 */
void ntrip_count_frames(const uint8_t *buf, size_t len, uint32_t *frames, uint32_t *epochs_ended);

/* ---- Reply parser and classifier ------------------------------------------------ */

typedef struct {
    bool success;
    ntrip_cls_t cls;          /**< failure class (NONE on success) */
    ntrip_row_t row;          /**< backoff row (NONE on success) */
    int status_code;          /**< HTTP code, 200 for ICY/OK successes, 0 otherwise */
    ntrip_hint_t hint;
    bool mount_ambiguous;     /**< the reply also means "unknown mountpoint" */
    uint8_t rule;             /**< matched row of the ordered table (1-18), 0 = cap exceeded */
} ntrip_verdict_t;

typedef enum {
    NTRIP_RP_LINE = 0,        /**< collecting the first line */
    NTRIP_RP_HEADERS,         /**< HTTP reply: header lines */
    NTRIP_RP_BODY,            /**< body check */
    NTRIP_RP_DONE,
} ntrip_rp_phase_t;

typedef struct {
    ntrip_rp_phase_t phase;
    bool v2_request;
    char line[NTRIP_REPLY_LINE_MAX + 1];
    size_t line_len;
    char hdr[NTRIP_REPLY_HDR_KEEP + 1];
    size_t hdr_len;
    size_t hdr_total;
    char server[65];
    bool ntrip_v2_header;
    size_t body_seen;
    char tail[8];             /**< last bytes of the body, for matches across reads */
    size_t tail_len;
    bool banned;
    bool body_check;          /**< finished by the body check */
    ntrip_verdict_t verdict;
} ntrip_reply_t;

void ntrip_reply_init(ntrip_reply_t *r, bool v2_request);

/**
 * Feed received bytes. @param used bytes consumed (the rest belong to the
 * stream after the reply and are ignored). @return true once the verdict is
 * final (phase DONE).
 */
bool ntrip_reply_feed(ntrip_reply_t *r, const uint8_t *data, size_t len, size_t *used);

/** The current phase's time ran out (eof = false) or the caster closed (eof = true). */
void ntrip_reply_end(ntrip_reply_t *r, bool eof);

/** Time allowed for the current phase, from its start. */
uint32_t ntrip_reply_phase_ms(const ntrip_reply_t *r);

/** Classify one first line (rows in table order); the parser uses it. */
ntrip_verdict_t ntrip_classify_line(const char *line, size_t len);

/** Map an HTTP status code to a verdict (no body check). */
ntrip_verdict_t ntrip_classify_http(int code);

/** Case-insensitive search of `needle` (lowercase) in data. */
bool ntrip_contains_ci(const char *data, size_t len, const char *needle);

/**
 * Copy text for logs and status: non-printable bytes become '.', at most
 * cap - 1 bytes, and every occurrence of a secret (>= 3 characters) is
 * replaced by '*'. secrets may contain NULLs.
 */
void ntrip_sanitize(const char *in, size_t len, char *out, size_t cap, const char *const *secrets, size_t nsecrets);

/* ---- Backoff ------------------------------------------------------------------ */

/**
 * Delay before the next attempt. TRANSIENT: min(cap, floor x 2^n); other
 * rows: the n-th entry, the last one repeating. Then x U(0.8, 1.2) from rnd
 * and at least the row's first value.
 * @param fast_late fast profile, 10 min into a failure streak: TRANSIENT cap late_cap_ms.
 */
uint32_t ntrip_backoff_delay_ms(ntrip_prof_t prof, ntrip_row_t row, uint32_t n, bool fast_late,
                                uint32_t late_cap_ms, uint32_t rnd);

/** First value of a row (the floor after jitter). */
uint32_t ntrip_backoff_row_min_ms(ntrip_prof_t prof, ntrip_row_t row);

/* ---- Per-slot state machine --------------------------------------------------------- */

typedef struct {
    ntrip_prof_t profile;
    bool protected_slot;
    uint32_t stall_limit_ms;     /**< silent session limit: source stall and pause idle */
    uint32_t auth_window_ms;     /**< AUTH after a disconnect or boot follows the MOUNT_BUSY row */
    uint32_t fast_late_cap_ms;   /**< fast TRANSIENT cap after 10 min of failure */
} ntrip_sm_cfg_t;

/*
 * have_ip and link_up: open sessions stay while the station holds its
 * address; new connections need the link as well. With ESP-IDF's default
 * Wi-Fi handlers the address is cleared (and lwIP aborts every session on
 * it) at the disconnect itself, so the server reports both together; only a
 * network layer that held the address through a short drop would ever give
 * have_ip without link_up.
 */
typedef struct {
    bool enabled;                /**< slot enabled and the server running */
    bool have_ip;                /**< the station holds an IP: open sessions stay */
    bool link_up;                /**< the station holds an IP and its link is up: new connections allowed */
    bool paused;                 /**< receiver work in progress */
    bool fresh_data;             /**< receiver RTCM within NTRIP_FRESH_DATA_MS */
    bool session_open;
    uint32_t silent_ms;          /**< session open: since the last byte was sent */
} ntrip_sm_in_t;

typedef enum {
    NTRIP_ACT_NONE = 0,
    NTRIP_ACT_CONNECT,
    NTRIP_ACT_CLOSE,             /**< close the session cleanly; not a failure */
} ntrip_sm_act_t;

typedef enum {
    NTRIP_WHY_NONE = 0,
    NTRIP_WHY_DISABLED,
    NTRIP_WHY_NO_IP,
    NTRIP_WHY_PAUSE_IDLE,
    NTRIP_WHY_SOURCE_STALL,
} ntrip_close_why_t;

typedef struct {
    ntrip_cls_t cls;
    ntrip_row_t row;
    bool reached_caster;         /**< a TCP connection was made */
    bool mount_ambiguous;
    bool refused;                /**< the caster's host refused the TCP connect (a reset): it was reached, no connection */
} ntrip_failure_t;

/**
 * The failure never reached the caster: a failed name lookup (no cached
 * address either), or a TCP connect that timed out or failed locally (no
 * route, socket error). A refused connect reached the caster's host.
 */
bool ntrip_failure_unreached(const ntrip_failure_t *f);

typedef struct {
    ntrip_sm_cfg_t cfg;
    ntrip_sm_state_t state;
    uint32_t state_since_ms;
    ntrip_cls_t reason;          /**< class of the last failure */
    ntrip_row_t row;             /**< effective row of the last failure, NONE after a success */
    ntrip_hint_t hint;           /**< backoff hint (MOUNT_HELD), NONE otherwise */
    uint32_t attempt;            /**< n: consecutive failures */
    uint32_t reached_streak;     /**< consecutive failures that reached the caster */
    bool hourly;                 /**< polite hourly limit active */
    bool retry_pending;
    uint32_t retry_at_ms;
    bool stagger_pending;
    uint32_t stagger_at_ms;
    bool streak_active;          /**< failures since the last healthy session */
    uint32_t streak_start_ms;
    bool busy_active;            /**< consecutive MOUNT_BUSY-row failures */
    uint32_t busy_start_ms;
    bool unreached_active;       /**< consecutive failures that never reached the caster */
    uint32_t unreached_start_ms; /**< ... since the first of them */
    uint32_t window_start_ms;    /**< last disconnect or boot */
    bool session;
    uint32_t session_start_ms;
    uint64_t session_bytes;
    bool session_healthy;
    bool prev_link_up;
    uint32_t cred_hash;          /**< credentials in use */
    uint32_t good_hash;          /**< credentials that last succeeded */
    bool good_valid;
    ntrip_row_t saved_row;       /**< long state as last persisted */
    uint32_t saved_step;         /**< step of that row as last persisted */
    uint32_t saved_reached;      /**< polite: caster-reaching failures as last persisted */
    bool saved_hourly;
    bool saved_dirty;            /**< good_hash changed since the last write */
} ntrip_sm_t;

/** Persisted long-backoff record. */
typedef struct {
    ntrip_cls_t cls;
    ntrip_row_t row;             /**< AUTH, CONFIG, BANNED or NONE */
    uint32_t attempt;
    uint32_t remaining_s;
    bool hourly;
    bool good_valid;
    uint32_t good_hash;
    uint8_t reached;             /**< polite profile: consecutive caster-reaching failures (at most the hourly limit) */
} ntrip_backoff_rec_t;

#define NTRIP_BACKOFF_REC_VERSION 2u   /**< version 1 (no `reached`) is still read */
#define NTRIP_BACKOFF_REC_MAX     32u

void ntrip_sm_init(ntrip_sm_t *sm, const ntrip_sm_cfg_t *cfg, uint32_t cred_hash, uint32_t now_ms);

/**
 * One step: given the inputs, set the state and say what to do. On
 * NTRIP_ACT_CLOSE, *why says why (never a failure).
 */
ntrip_sm_act_t ntrip_sm_poll(ntrip_sm_t *sm, const ntrip_sm_in_t *in, uint32_t now_ms, uint32_t rnd,
                             ntrip_close_why_t *why);

/** A connection attempt starts (state CONNECTING). */
void ntrip_sm_connecting(ntrip_sm_t *sm, uint32_t now_ms);

/**
 * The caster accepted the request (state STREAMING). A success: the polite
 * hourly limit and its count of caster-reaching failures end here; the
 * attempt count itself waits for a healthy session.
 */
void ntrip_sm_connected(ntrip_sm_t *sm, uint32_t now_ms);

/** Bytes accepted by the socket; a session that becomes healthy resets the backoff. */
void ntrip_sm_sent(ntrip_sm_t *sm, size_t bytes, uint32_t now_ms);

/**
 * A connection attempt or a session failed: apply the AUTH window and the
 * MOUNT_BUSY escalation, compute the delay, enter the row's state.
 * @return the delay in ms.
 */
uint32_t ntrip_sm_failed(ntrip_sm_t *sm, const ntrip_failure_t *f, uint32_t now_ms, uint32_t rnd);

/** A session or attempt ended without a failure (planned close, abort). */
void ntrip_sm_closed(ntrip_sm_t *sm, uint32_t now_ms);

/** The network came back with a new address: non-protected slots wait 0-10 s at random. */
void ntrip_sm_stagger(ntrip_sm_t *sm, uint32_t now_ms, uint32_t rnd);

/** The user saved the slot: new settings, counters and delays reset, retry at once. */
void ntrip_sm_user_edit(ntrip_sm_t *sm, const ntrip_sm_cfg_t *cfg, uint32_t cred_hash, uint32_t now_ms);

/** ms until the next attempt may start (0 = none pending). */
uint32_t ntrip_sm_retry_in_ms(const ntrip_sm_t *sm, uint32_t now_ms);

/**
 * Since the first failure of the slot's current run of consecutive failures
 * that never reached the caster (ntrip_failure_unreached), in ms; 0 when
 * there is no such run. A refused connect, any failure after the TCP
 * connection was made, a successful connect, a user edit and the slot
 * leaving its connect/backoff cycle (disabled, waiting for the network or
 * for data, paused) end the run.
 */
uint32_t ntrip_sm_unreached_ms(const ntrip_sm_t *sm, uint32_t now_ms);

/**
 * True if the record changed since the last write: the long row (AUTH,
 * CONFIG, BANNED) or its step (bounded: once the row's last entry repeats,
 * further failures change nothing), the hourly limit, a polite slot's count
 * of caster-reaching failures (up to the hourly limit) or the last-good
 * credentials. So a reboot never puts a slot back to an earlier step.
 */
bool ntrip_sm_persist_due(const ntrip_sm_t *sm);

/** Snapshot for persisting; then call ntrip_sm_persist_done(). */
void ntrip_sm_persist_get(const ntrip_sm_t *sm, uint32_t now_ms, ntrip_backoff_rec_t *out);
void ntrip_sm_persist_done(ntrip_sm_t *sm);

/**
 * Restore a record at boot. A long row or the hourly limit waits the larger
 * of the stored remaining delay and the full nominal delay of the stored
 * step (one hour in hourly mode) before the first attempt, so a reboot loop
 * can never shorten the escalation; the stored count of caster-reaching
 * failures carries on toward the hourly limit.
 */
void ntrip_sm_restore(ntrip_sm_t *sm, const ntrip_backoff_rec_t *rec, uint32_t now_ms);

/** Encode / decode the record (settings blob). */
size_t ntrip_backoff_rec_encode(const ntrip_backoff_rec_t *rec, uint8_t *buf, size_t cap);
bool ntrip_backoff_rec_decode(const uint8_t *buf, size_t len, ntrip_backoff_rec_t *out);

/** Hash of the credentials a slot authenticates with (host, port, protocol, names, password). */
uint32_t ntrip_cred_hash(const char *host, uint16_t port, bool v2, const char *mountpoint,
                         const char *username, const char *password, const char *label);

/* ---- Connection history (journal records) ------------------------------------------- */

#define NTRIP_HIST_SHORT_S   60u      /**< a session shorter than this is "short" (the healthy-session rule) */
#define NTRIP_HIST_LIMIT_S   600u     /**< short sessions: one record of a kind per slot in this long */
#define NTRIP_HIST_UP_GAP_S  10u      /**< an UP is written after this long without a session, failed attempts or not */
#define NTRIP_HIST_COUNT_MAX 32767u   /**< counts in a record saturate here (bits 16-30) */

/**
 * Per slot. DOWN when a session that reached STREAMING ends (end class, the
 * short sessions not journalled since the last DOWN, its length); UP when a
 * session reaches STREAMING after at least one failed attempt or at least
 * NTRIP_HIST_UP_GAP_S after the previous session ended (failed attempts
 * since that session, the outage: from that session's end, or from the
 * first failed attempt when the slot had not streamed this boot). A DOWN
 * for a session shorter than NTRIP_HIST_SHORT_S, and the UP after such a
 * session, are written only when the slot wrote no record of that kind in
 * the last NTRIP_HIST_LIMIT_S; a skipped DOWN is counted into the next DOWN
 * written. Failed attempts are never recorded one by one. Times are seconds
 * of a monotonic clock (uptime).
 */
typedef struct {
    bool streamed;               /**< a session reached STREAMING this boot */
    bool in_session;
    uint32_t session_start_s;
    uint32_t prev_end_s;         /**< the previous session's end (valid once streamed) */
    bool prev_short;             /**< the previous session was short */
    uint32_t fails;              /**< failed attempts since the previous session (or since boot) */
    bool first_fail_valid;
    uint32_t first_fail_s;       /**< first failed attempt of this boot */
    bool down_written;
    uint32_t down_at_s;
    bool up_written;
    uint32_t up_at_s;
    uint32_t skipped;            /**< short-session DOWNs not written since the last DOWN written */
} ntrip_hist_t;

void ntrip_hist_init(ntrip_hist_t *h);

/** A connection attempt failed (a caster failure; not an attempt the network loss ended). */
void ntrip_hist_failed(ntrip_hist_t *h, uint32_t now_s);

/**
 * The session reached STREAMING. @return true with *a, *b set when an UP
 * record is due (a = slot | failed attempts << 16, b = outage s).
 */
bool ntrip_hist_up(ntrip_hist_t *h, uint32_t slot, uint32_t now_s, int32_t *a, int32_t *b);

/**
 * The session ended. `cls` = the failure class that ended it, 0 for an end
 * that was not a caster failure. @return true with *a, *b set when a DOWN
 * record is due (a = slot | cls << 8 | skipped << 16, b = length s).
 */
bool ntrip_hist_down(ntrip_hist_t *h, uint32_t slot, uint8_t cls, uint32_t now_s, int32_t *a, int32_t *b);

/** `a` of a history record: slot bits 0-7, class bits 8-15, count bits 16-30 (saturating). */
int32_t ntrip_hist_pack(uint32_t slot, uint32_t cls, uint32_t count);

/* ---- Onocoy daily budget ----------------------------------------------------------- */

/**
 * Offline seconds a UTC day may hold and still keep the streak, for a data
 * rate in per mille: availability = ((u - 0.8) / 0.2)^2 x rate >= 0.98 with
 * graced uptime u = (online + 300) / 86400. Rounded down; 0 below 980 per mille.
 */
uint32_t ntrip_offline_budget_s(uint32_t data_rate_permille);

#ifdef __cplusplus
}
#endif
