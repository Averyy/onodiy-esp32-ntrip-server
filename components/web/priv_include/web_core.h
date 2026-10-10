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

/* ---- request checks ------------------------------------------------------------------ */

/** Search-domain suffixes a browser may append to the bare hostname. */
#define WEB_HOST_SUFFIXES "lan", "home", "home.arpa", "localdomain", "internal", "fritz.box"

/** The device's names, for the Host check. Empty strings are skipped. */
typedef struct {
    const char *sta_ip;      /**< station IPv4 address ("" while offline) */
    const char *ap_ip;       /**< AP address ("192.168.4.1") */
    const char *hostname;    /**< hostname in use */
} web_host_names_t;

/**
 * Host header check (DNS rebinding): the host part (port ignored) must be
 * one of the device's names; names compare without case. A bracketed IPv6
 * literal, an empty host or a malformed port is refused.
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

/** True if host equals `domain` or ends in "." + domain (case-insensitive). */
bool web_host_in_domain(const char *host, const char *domain);

/**
 * Receiver firmware string the user expects: <= 47 characters, no control
 * characters, containing "Build" followed by digits. @return the build
 * number, 0 if the string is not usable.
 */
uint32_t web_expected_build(const char *fw);

/* ---- caster policy ------------------------------------------------------------------------ */

typedef enum {
    WEB_POLICY_OK = 0,
    WEB_POLICY_SECOND_ONOCOY,
    WEB_POLICY_SECOND_RTK2GO,
    WEB_POLICY_DUP_ACK_REQUIRED,
} web_policy_t;

typedef struct {
    bool enabled;
    const char *host;
    bool dup_ack;
} web_policy_slot_t;

/**
 * Which cross-slot rule saving slot `edited` breaks (slots[] holds the
 * result of the save): a second enabled Onocoy slot, a second RTK2go slot,
 * or Onocoy together with an open caster (RTK2go, Centipede) without the
 * acknowledgement. Used to name the reason when the caster component refuses.
 */
web_policy_t web_caster_policy(const web_policy_slot_t *slots, size_t count, size_t edited);

/** "second_onocoy", "second_rtk2go", "dup_stream_ack_required", NULL for OK. */
const char *web_policy_reason(web_policy_t p);

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
#define WEB_IMAGE_HEAD_MIN       (WEB_IMAGE_DESC_OFS + WEB_DESC_PROJECT_OFS + WEB_DESC_FIELD_LEN)

typedef struct {
    char version[33];
    char project[33];
} web_image_info_t;

/**
 * Check the first bytes of an uploaded app image: image magic, chip id,
 * app description magic, and (unless `project` is NULL) the project name.
 */
web_image_check_t web_image_check(const uint8_t *head, size_t len, uint16_t chip_id, const char *project,
                                  web_image_info_t *out);

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

#ifdef __cplusplus
}
#endif
