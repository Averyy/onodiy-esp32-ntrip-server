/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Web server decisions that need no socket (pure logic, no IDF driver or
 * RTOS includes, host-testable): the error table, request checks (Host,
 * Origin, content types, cookies), the session table, login throttling,
 * password and field rules, the app image check of an upload, the status
 * section checksum and a few formatting helpers.
 *
 * Times are seconds of an injected monotonic clock, compared only as
 * (uint32_t)(now - then), so they survive the clock wrap.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ntrip_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- errors ------------------------------------------------------------------------ */

/** Every error code of the API, in the order of its table (HTTP status by code). */
typedef enum {
    WEB_ERR_BAD_JSON = 0,
    WEB_ERR_INVALID_FIELD,
    WEB_ERR_CONFIRM_REQUIRED,
    WEB_ERR_BAD_REQUEST,
    WEB_ERR_INVALID_IMAGE,
    WEB_ERR_SIZE_MISMATCH,
    WEB_ERR_UNAUTHORIZED,
    WEB_ERR_FORBIDDEN,
    WEB_ERR_WRONG_PASSWORD,
    WEB_ERR_NOT_ON_SETUP_AP,
    WEB_ERR_NOT_FOUND,
    WEB_ERR_NOTHING_STAGED,
    WEB_ERR_NO_JOURNAL,
    WEB_ERR_NO_COREDUMP,
    WEB_ERR_NOTHING_PENDING,
    WEB_ERR_METHOD_NOT_ALLOWED,
    WEB_ERR_BUSY,
    WEB_ERR_JOURNAL_OPEN,
    WEB_ERR_NOT_WAITING,
    WEB_ERR_APP_TRIAL,
    WEB_ERR_APP_UPDATE,
    WEB_ERR_SAFE_MODE,
    WEB_ERR_NOT_IN_SAFE_MODE,
    WEB_ERR_RECEIVER_NOT_STREAMING,
    WEB_ERR_UNSUPPORTED_MODEL,
    WEB_ERR_PACKAGE_PINNED,
    WEB_ERR_MANIFEST_CHANGED,
    WEB_ERR_NOT_OFFERED,
    WEB_ERR_ROLLED_BACK_ACK_REQUIRED,
    WEB_ERR_REBOOT_INHIBITED,
    WEB_ERR_SETTINGS_UNAVAILABLE,
    WEB_ERR_NO_POSITION_YET,
    WEB_ERR_CASTER_POLICY,
    WEB_ERR_SHA256_MISMATCH,
    WEB_ERR_DOWNGRADE,
    WEB_ERR_NO_ROLLBACK_IMAGE,
    WEB_ERR_ADMIN_ALREADY_SET,
    WEB_ERR_ADMIN_NOT_SET,
    WEB_ERR_RECEIVER_MAINTENANCE,
    WEB_ERR_NOT_IN_MAINTENANCE,
    WEB_ERR_LIVE_VIEW_CLOSED,
    WEB_ERR_LENGTH_REQUIRED,
    WEB_ERR_TOO_LARGE,
    WEB_ERR_UNSUPPORTED_MEDIA_TYPE,
    WEB_ERR_BAD_HOST,
    WEB_ERR_NOT_THIS_PROJECT,
    WEB_ERR_RATE_LIMITED,
    WEB_ERR_INTERNAL,
    WEB_ERR_NOT_AVAILABLE_YET,
    WEB_ERR_CHECKS_DISABLED,
    WEB_ERR_TOO_MANY_CLIENTS,
    WEB_ERR_NOT_NO_RECEIVER,
    WEB_ERR_SAME_IMAGE,
    WEB_ERR_SLOT_DISABLED,
    WEB_ERR_SIGNAL_TEST_RUNNING,
    WEB_ERR_SIGNAL_TEST_IDLE,
    WEB_ERR_SIGNAL_TEST_NOT_DECIDING,
    WEB_ERR_FIRST_APPLY_HELD,
    WEB_ERR_AUTOCFG_OFF,
    WEB_ERR_IDENTITY_UNCONFIRMED,
    WEB_ERR_CONFIG_FAILED,
    WEB_ERR_POSITION_NOT_LOCKED,
    WEB_ERR_NO_RECEIVER,
    WEB_ERR_COUNT,
} web_err_t;

/** Machine-readable code ("journal_open"). */
const char *web_err_code(web_err_t e);
/** English fallback text for logs and the UI. */
const char *web_err_message(web_err_t e);
/** HTTP status (409, ...). */
int web_err_status(web_err_t e);
/** Status line for a status code ("409 Conflict"); "500 Internal Server Error" for unknown ones. */
const char *web_status_line(int status);

/* ---- response headers ------------------------------------------------------------------ */

/** One response header. */
typedef struct {
    const char *name;
    const char *value;
} web_hdr_t;

/**
 * The headers every API and page answer carries, from one table: the i-th
 * of an answer (`api`: an API answer, which adds "Cache-Control: no-store"),
 * NULL past the last. web_set_common_headers() sets them on an answer of the
 * server, web_stream_head() writes them into a head of its own, so the two
 * cannot drift apart.
 */
const web_hdr_t *web_common_header(size_t i, bool api);

/**
 * The head of a streamed answer to an HTTP/1.0 request, which cannot take
 * chunked framing: "HTTP/1.1 <code> <text>" (the server's own status line
 * for every answer), Content-Type, the common headers (web_common_header),
 * Content-Disposition when `disposition` is not NULL, "Connection: close",
 * and Content-Length when `content_length` >= 0 (else the body ends where
 * the connection does), then the empty line. Never Transfer-Encoding.
 * @return its length, 0 when it does not fit in `cap` (buf then holds "").
 */
size_t web_stream_head(char *buf, size_t cap, int status, const char *type, const char *disposition, bool api,
                       int64_t content_length);

/* ---- request checks ------------------------------------------------------------------ */

/**
 * Search-domain suffixes a browser may append to the bare hostname (the
 * DHCP name a router's DNS may resolve): only names that never resolve in
 * public DNS (reserved, or never delegated), so nobody outside the LAN
 * controls what they resolve to. A router's domain that is a publicly
 * delegated zone is left out on purpose: reach the station by its IP
 * address or its mDNS name there.
 */
#define WEB_HOST_SUFFIXES "lan", "home", "home.arpa", "localdomain", "internal"

/** The device's names, for the Host check. Empty strings (and NULL) are skipped. */
typedef struct {
    const char *sta_ip;      /**< station IPv4 address ("" while offline) */
    const char *ap_ip;       /**< AP address ("192.168.4.1") */
    const char *hostname;    /**< hostname in use (the DHCP name) */
    const char *mdns_host;   /**< label the mDNS responder answers ("" = not running): <mdns_host>.local */
} web_host_names_t;

/**
 * Host header check (DNS rebinding): the host part (port ignored) must be
 * one of the device's names: the station's IP, the AP's IP, the hostname
 * alone or with one of WEB_HOST_SUFFIXES (the DHCP name), or
 * <mdns_host>.local (the name the mDNS responder answers, which can differ
 * from the hostname after a name conflict: "onocoydiy-2"). Names compare
 * without case and may carry one trailing dot (the fully qualified form a
 * DNS-SD browser opens); IP addresses may not. A bracketed IPv6 literal, an
 * empty host or a malformed port is refused.
 */
bool web_host_allowed(const char *host_hdr, const web_host_names_t *names);

/** Origin check: "http://" followed by exactly the request's Host (case-insensitive). */
bool web_origin_ok(const char *origin, const char *host_hdr);

/** Content-Type is `type` (case-insensitive), optionally followed by ";" parameters. */
bool web_content_type_is(const char *ct, const char *type);

/**
 * Session id from a Cookie header: the value of `sid` when it is exactly
 * 32 hex digits. @return true and out (lower case, NUL-terminated) on success.
 */
bool web_cookie_sid(const char *cookie, char out[33]);

/** Lower-case hex of 16 bytes. */
void web_hex16(const uint8_t in[16], char out[33]);

/** Parse 64 hex digits (any case) into 32 bytes. */
bool web_parse_sha256_hex(const char *s, uint8_t out[32]);

/** Query parameter value (URL-decoded "%XX" and '+'); false if absent or too long. */
bool web_query_value(const char *query, const char *key, char *out, size_t out_size);

/* ---- sessions ------------------------------------------------------------------------- */

#define WEB_SESSIONS_MAX     4u
#define WEB_SESSION_IDLE_S   1800u          /**< idle timeout, extended by every authenticated request */
#define WEB_SESSION_LIFE_S   86400u         /**< absolute lifetime */

typedef struct {
    bool used;
    uint8_t token[16];
    uint32_t created_s;
    uint32_t last_s;
    uint32_t gen;          /**< changes every time the slot gets a new session (links WebSocket clients) */
} web_session_t;

typedef struct {
    web_session_t s[WEB_SESSIONS_MAX];
    uint32_t next_gen;
} web_sessions_t;

void web_sess_init(web_sessions_t *t);

/** New session with this token; a full table ends the least recently used one. @return its index. */
int web_sess_create(web_sessions_t *t, const uint8_t token[16], uint32_t now_s);

/** Index of the live session with this token (an expired one is ended here), -1 if none. Constant-time compare. */
int web_sess_find(web_sessions_t *t, const uint8_t token[16], uint32_t now_s);

/** Extend the idle timeout. */
void web_sess_touch(web_sessions_t *t, int idx, uint32_t now_s);

/** Seconds until the session ends (idle or absolute, whichever comes first). */
uint32_t web_sess_left_s(const web_sessions_t *t, int idx, uint32_t now_s);

/** End one session. */
void web_sess_end(web_sessions_t *t, int idx);

/** End every session except `keep` (-1 = all). @return bit mask of the ended indexes. */
uint32_t web_sess_end_all_except(web_sessions_t *t, int keep);

/** End expired sessions. @return bit mask of the ended indexes. */
uint32_t web_sess_expire(web_sessions_t *t, uint32_t now_s);

/** True while index idx holds the session generation `gen`. */
bool web_sess_alive(const web_sessions_t *t, int idx, uint32_t gen);

/* ---- login throttling ------------------------------------------------------------------- */

/** Free failures before delays start. */
#define WEB_THROTTLE_FREE   5u
/** Longest delay after a failure, seconds. */
#define WEB_THROTTLE_MAX_S  300u

typedef struct {
    uint32_t fails;      /**< consecutive failures */
    uint32_t last_s;     /**< time of the last counted failure */
} web_throttle_t;

/** Seconds until the next attempt is accepted (0 = now). */
uint32_t web_throttle_wait_s(const web_throttle_t *t, uint32_t now_s);
/** A counted failure (only call when web_throttle_wait_s() was 0). */
void web_throttle_fail(web_throttle_t *t, uint32_t now_s);
/** A success resets the count. */
void web_throttle_ok(web_throttle_t *t);
/** Delay after the n-th consecutive failure: 0 for n < 5, then min(2^(n-4), 300). */
uint32_t web_throttle_delay_s(uint32_t fails);

/*
 * Login throttling per client. Each client (its address, and whether it came
 * over the AP) has its own count and delay, so one client's failures never
 * delay another's login. Distributed guessing from many addresses is bounded
 * by a ceiling per interface (station / AP): at most
 * WEB_THROTTLE_IFACE_BURST failures in a burst, then one more every
 * WEB_THROTTLE_IFACE_REFILL_S. A single client, throttled to one failure
 * per 300 s after its first few, stays well below that ceiling, and a LAN
 * client never touches the AP's, so nobody on the LAN can lock out an owner
 * standing next to the station.
 */
#define WEB_THROTTLE_CLIENTS          16u     /**< clients tracked (the least recently seen one is replaced) */
#define WEB_THROTTLE_IFACE_BURST      30u
#define WEB_THROTTLE_IFACE_REFILL_S   120u

typedef struct {
    bool used;
    bool ap;
    uint64_t key;        /**< IPv4 address, or an IPv6 address's /64 prefix */
    uint32_t seen_s;     /**< last failure or check (replacement order) */
    web_throttle_t t;
} web_throttle_client_t;

typedef struct {
    uint32_t left;       /**< failures left in the burst */
    uint32_t at_s;       /**< refill reference */
} web_throttle_bucket_t;

typedef struct {
    web_throttle_client_t c[WEB_THROTTLE_CLIENTS];
    web_throttle_bucket_t iface[2];   /**< [0] station interface, [1] AP */
} web_throttles_t;

/** Empty table (also: a new admin password makes every earlier failure moot). */
void web_throttles_init(web_throttles_t *t, uint32_t now_s);
/** Seconds until this client's next attempt is accepted (0 = now): its own delay or its interface's ceiling. */
uint32_t web_throttles_wait_s(web_throttles_t *t, uint64_t key, bool ap, uint32_t now_s);
/** A counted failure of this client (only call when web_throttles_wait_s() was 0). @return its consecutive failures. */
uint32_t web_throttles_fail(web_throttles_t *t, uint64_t key, bool ap, uint32_t now_s);
/** This client's success resets its count. */
void web_throttles_ok(web_throttles_t *t, uint64_t key, bool ap);

/* ---- field rules ------------------------------------------------------------------------- */

/** Admin password: 8-64 bytes of valid UTF-8 without control characters. */
bool web_password_valid(const char *pw);

/** Hostname: 1-32 of [A-Za-z0-9-], not starting or ending with '-'. */
bool web_hostname_valid(const char *name);

/** Station SSID: 1-32 bytes. */
bool web_ssid_valid(const char *ssid);

/** Station password: "" (open), 8-63 printable ASCII, or exactly 64 hex digits. */
bool web_wifi_password_valid(const char *pw);

/** AP password: 8-63 printable ASCII. */
bool web_ap_password_valid(const char *pw);

/** No byte below 0x20 and no DEL. */
bool web_no_ctrl(const char *s);

/** Every character in [A-Za-z0-9] plus the characters of `extra` ("" for none). */
bool web_chars_ok(const char *s, const char *extra);

/** A DNS name (labels of [A-Za-z0-9-], 1-63 each, not starting or ending with '-') or an IPv4 literal; <= 63 characters. */
bool web_host_name_ok(const char *host);

/**
 * Receiver firmware string the user expects: <= 47 characters, no control
 * characters, containing "Build" followed by digits. @return the build
 * number, 0 if the string is not usable.
 */
uint32_t web_expected_build(const char *fw);

/**
 * Receiver maintenance durations the station accepts (s; station.h), the
 * default (a request without a duration) and the most time left after an
 * extension.
 */
#define WEB_MAINT_CHOICES_S  { 900u, 1800u, 3600u, 7200u, 14400u }
#define WEB_MAINT_DEFAULT_S  1800u
#define WEB_MAINT_MAX_LEFT_S 14400u

/** One of WEB_MAINT_CHOICES_S. */
bool web_maint_duration_ok(uint32_t s);

/* ---- caster policy ------------------------------------------------------------------------ */

/**
 * API name of the cross-slot rule ntrip_server_set_config() reported (the
 * `reason` of `409 caster_policy`): "second_onocoy", "second_rtk2go",
 * "dup_stream_ack_required"; NULL for NTRIP_POLICY_OK and unknown values.
 * The rules themselves live in ntrip_server only.
 */
const char *web_policy_reason(ntrip_policy_t p);

/* ---- app image check ------------------------------------------------------------------------- */

typedef enum {
    WEB_IMAGE_OK = 0,
    WEB_IMAGE_NOT_APP,          /**< not an app image for this chip */
    WEB_IMAGE_OTHER_PROJECT,    /**< an app, but its description names another project */
} web_image_check_t;

/** Fixed offsets of an app image's first bytes (checked against the IDF types where they are compiled). */
#define WEB_IMAGE_MAGIC          0xE9u
#define WEB_IMAGE_CHIP_ID_OFS    12u
#define WEB_IMAGE_DESC_OFS       32u      /**< image header (24) + first segment header (8) */
#define WEB_DESC_MAGIC           0xABCD5432u
#define WEB_DESC_VERSION_OFS     16u
#define WEB_DESC_PROJECT_OFS     48u
#define WEB_DESC_FIELD_LEN       32u
#define WEB_DESC_ELF_SHA_OFS     144u     /**< app_elf_sha256[32] */
#define WEB_IMAGE_HEAD_MIN       (WEB_IMAGE_DESC_OFS + WEB_DESC_PROJECT_OFS + WEB_DESC_FIELD_LEN)
#define WEB_IMAGE_HEAD_SHA       (WEB_IMAGE_DESC_OFS + WEB_DESC_ELF_SHA_OFS + 32u)

typedef struct {
    char version[33];
    char project[33];
    bool has_elf_sha;            /**< the head reached the app description's ELF SHA-256 */
    uint8_t elf_sha256[32];      /**< SHA-256 of the app's ELF file (identical code has the same) */
} web_image_info_t;

/**
 * Check the first bytes of an uploaded app image: image magic, chip id,
 * app description magic, and (unless `project` is NULL) the project name.
 */
web_image_check_t web_image_check(const uint8_t *head, size_t len, uint16_t chip_id, const char *project,
                                  web_image_info_t *out);

/**
 * What an app upload gets after its first bytes were checked: WEB_ERR_COUNT
 * = go on and install; WEB_ERR_INVALID_IMAGE, WEB_ERR_NOT_THIS_PROJECT;
 * WEB_ERR_SAME_IMAGE for the image already running (`same_image`: its ELF
 * SHA-256 equals the running one) unless the user asked to install it again
 * (`reinstall`, header X-Reinstall: 1); or WEB_ERR_ROLLED_BACK_ACK_REQUIRED
 * for a version that was rolled back on this station (`rolled_back`) unless
 * the user acknowledged it (`?rolled_back_ack=1`), the same warning an
 * install from the banner gives.
 */
web_err_t web_app_upload_verdict(web_image_check_t chk, bool rolled_back, bool rolled_back_ack, bool same_image,
                                 bool reinstall);

/**
 * The result an app upload refused by its verdict (not WEB_ERR_COUNT) is
 * noted with (last_result, the final progress message): "same_image" and
 * "rolled_back_ack_required" wait for the user's answer and say so (nothing
 * failed and nothing changed); anything else is "invalid_image".
 */
const char *web_app_refusal_result(web_err_t verdict);

/* ---- status deltas, dates, names ------------------------------------------------------------- */

/** CRC-32 (IEEE 802.3, reflected), continuing from `crc` (start with 0). */
uint32_t web_crc32(uint32_t crc, const void *data, size_t len);

/** "YYYY-MM-DD" of a day count since 1970-01-01 (proleptic Gregorian). */
void web_utc_date(int32_t utc_day, char out[11]);

/**
 * Copy `in` into out keeping only [A-Za-z0-9._+-] (others become '_'), for
 * a file name in a Content-Disposition header.
 */
void web_safe_filename_part(const char *in, char *out, size_t out_size);

/** Kind of a WebSocket message: decides what a full client queue drops. */
typedef enum {
    WEB_MSG_STATUS = 0,
    WEB_MSG_EVENT,
    WEB_MSG_PROGRESS,
    WEB_MSG_LOG,
    WEB_MSG_OTHER,         /**< hello, pong: never dropped while there is room */
} web_msg_kind_t;

/** Per-client queue limits. */
#define WEB_WSQ_MAX_MSGS   8u
#define WEB_WSQ_MAX_BYTES  16384u

/**
 * Admission of one message into a client queue (kinds and byte sizes of
 * what is queued, oldest first). Returns the index of the queued message
 * to drop to make room (or -1 for none), and whether the new message is
 * taken at all:
 *  - status: never evicts; refused when full (the client then needs a full snapshot);
 *  - log: never evicts; refused when full (the next log message says gap);
 *  - progress: replaces a queued progress message (only the newest is kept);
 *  - event: evicts the oldest queued event when full;
 *  - other: refused when full.
 */
typedef struct {
    bool accept;
    int evict;
} web_wsq_decision_t;

web_wsq_decision_t web_wsq_admit(const web_msg_kind_t *kinds, const size_t *sizes, size_t count,
                                 web_msg_kind_t kind, size_t size);

/* ---- client and local addresses ------------------------------------------------------- */

/** A socket address without the socket headers: IPv4, or IPv6 (v4-mapped included). */
typedef struct {
    bool v6;
    uint8_t b[16];       /**< IPv6: the 16 bytes; IPv4: b[0..3], network order */
} web_addr_t;

/**
 * The client a login throttle entry (and a log rate entry) belongs to:
 * an IPv4 address as its 32 bits (also when mapped into IPv6); an IPv6
 * link-local address (fe80::/10) as all 128 bits folded to 64 (every host
 * on a link shares the fe80::/64 prefix, so the prefix alone would put the
 * whole setup AP into one bucket); any other IPv6 address as its /64 prefix
 * (one host can use any address of its prefix). 0 for NULL.
 */
uint64_t web_client_key(const web_addr_t *a);

/**
 * The request came over the AP: its local address is the AP's IPv4 address
 * (`ap_ip4`, network order, 0 = unknown), also v4-mapped, or the AP
 * interface's IPv6 link-local address (`ap_ll6`, NULL = none).
 */
bool web_addr_on_ap(const web_addr_t *local, uint32_t ap_ip4, const uint8_t *ap_ll6);

/**
 * Address text for the log: dotted IPv4 (a v4-mapped IPv6 address as its
 * IPv4 address), IPv6 in the RFC 5952 form (lower case, the longest run of
 * two or more zero groups as "::"). "?" for NULL. @return the length.
 */
size_t web_addr_text(const web_addr_t *a, char *out, size_t cap);

/* ---- requests no route took ------------------------------------------------------------ */

/** What the catch-all answers. */
typedef enum {
    WEB_FB_NOT_FOUND = 0,      /**< 404 not_found */
    WEB_FB_METHOD,             /**< 405 method_not_allowed */
    WEB_FB_ASSET,              /**< serve the UI asset */
    WEB_FB_REDIRECT,           /**< the captive redirect (over the AP) */
} web_fallback_t;

/**
 * Classify a request no route took, before anything of its body is read:
 * OPTIONS is 405 (no CORS, ever); /api, /api/... and /ws are 405 when
 * `api_known` knows the path (query removed; a path of 64 bytes or more is
 * unknown), else 404; a UI asset (`asset`) is served to GET and HEAD, 405
 * otherwise; over the AP everything else is the captive redirect; 404
 * elsewhere.
 */
web_fallback_t web_fallback_classify(const char *uri, bool options, bool get_or_head, bool asset, bool on_ap,
                                     bool (*api_known)(const char *path));

/* ---- connection handling ------------------------------------------------------------------- */

#define WEB_CONN_TOK_CLOSE      (1u << 0)
#define WEB_CONN_TOK_KEEP_ALIVE (1u << 1)

/**
 * Tokens of a Connection header value (a comma-separated list; tokens
 * compared without case, spaces and tabs around them ignored): CLOSE for
 * "close", KEEP_ALIVE for "keep-alive"; others (Upgrade, ...) are ignored.
 * NULL or "" = none.
 */
unsigned web_connection_tokens(const char *value);

/**
 * The connection ends after this response: the request asked for it
 * ("Connection: close"), or it is HTTP/1.0, with or without
 * "Connection: keep-alive". An HTTP/1.0 client keeps a connection only when
 * the answer echoes the keep-alive token, and otherwise reads the answer to
 * the end of the connection; and an answer sent in chunks (a large JSON
 * document, a download) cannot go to an HTTP/1.0 client on a connection that
 * stays open at all. So HTTP/1.0 connections are never persistent here.
 */
bool web_conn_should_close(unsigned tokens, bool http10);

/** A request's body as the HTTP server hands the request to its handler. */
typedef enum {
    WEB_REQ_BODY_NONE = 0,      /**< no body */
    WEB_REQ_BODY_LENGTH,        /**< Content-Length bytes */
    WEB_REQ_BODY_UNFRAMED,      /**< a Transfer-Encoding (chunked): esp_http_server reports a length of 0 and
                                     would parse the chunks as the next request; never read here */
} web_req_body_t;

/** The body of a request with this Content-Length (0 = none) and, or not, a Transfer-Encoding header. */
web_req_body_t web_req_body(size_t content_len, bool transfer_encoding);

/* ---- log lines about clients, rate-limited --------------------------------------------------- */

#define WEB_LOGRATE_CLIENTS  8u
#define WEB_LOGRATE_PERIOD_S 60u

typedef struct {
    bool used;
    uint64_t key;              /**< web_client_key() */
    uint8_t tag;               /**< the caller's reason for the line (0 when it has only one) */
    uint32_t last_s;           /**< when its last line went out */
    uint32_t held;             /**< lines held back since */
} web_lograte_client_t;

/**
 * At most one log line per client and reason (`tag`) per
 * WEB_LOGRATE_PERIOD_S, for the WEB_LOGRATE_CLIENTS (client, reason) pairs
 * seen last (a new pair takes the place of the one whose last line is
 * oldest); the lines held back are counted per pair and the next line of
 * that pair says how many. A client on the network retrying something every
 * second cannot fill the log, its address is still in it, and one reason
 * never hides another (a full slot is still logged while another tab of
 * the same client keeps being refused for want of a session).
 */
typedef struct {
    web_lograte_client_t c[WEB_LOGRATE_CLIENTS];
} web_lograte_t;

void web_lograte_init(web_lograte_t *r);

/**
 * May a line about this client, for this reason, go out now? true: *held
 * (if not NULL) gets how many of that pair were held back since its last
 * one, and the count restarts. false: this one is counted.
 */
bool web_lograte_allow(web_lograte_t *r, uint64_t key, uint8_t tag, uint32_t now_s, uint32_t *held);

/** "none", "error", "warn", "info", "debug", "verbose" for esp_log_level_t values 0-5; "?" otherwise. */
const char *web_log_level_text(unsigned level);

/**
 * A query parameter that must be a decimal u32 when present (a cursor):
 * absent -> true and *out = 0; 1-10 digits up to 4294967295 -> true and
 * the value; anything else (empty, signs, spaces, other characters, too
 * large) -> false.
 */
bool web_query_u32(const char *query, const char *key, uint32_t *out);

#ifdef __cplusplus
}
#endif
