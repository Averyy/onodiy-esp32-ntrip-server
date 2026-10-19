/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Local NTRIP caster, pure core (no IDF or RTOS calls; clocks and inputs are
 * parameters; host-tested): who may connect, the request parser, the
 * decision for a request, every reply byte for byte, the source table's STR
 * record, chunk framing, Basic credentials and their constant-time compare,
 * the login throttle, the journal gate and its queue, the bit-rate estimate,
 * and the stored config record and its rules. local_caster.c does the I/O
 * around it.
 *
 * Protocol (public NTRIP 1.0 and 2.0): a rover sends
 * "GET /<mountpoint> HTTP/1.x" with Basic authentication; a request without
 * "Ntrip-Version: Ntrip/2.0" is NTRIP 1.0 and gets "ICY 200 OK" and raw
 * bytes, NTRIP 2.0 over HTTP/1.1 gets an HTTP/1.1 reply and chunked data,
 * NTRIP 2.0 over HTTP/1.0 an HTTP/1.0 reply and raw bytes. GET / and any
 * mountpoint that is not ours get the source table.
 *
 * Nothing here logs. The parser keeps the raw request (with its
 * Authorization header) and the decoded credentials only until
 * lcast_req_wipe().
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "local_caster.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- addresses --------------------------------------------------------------------------- */

/** A socket address without the socket headers: IPv4 in b[0..3] (network order), or IPv6. */
typedef struct {
    bool v6;
    uint8_t b[16];
} lcast_addr_t;

/** Who may connect (spec: private, link-local or loopback; not the gateway; not over the hotspot). */
typedef enum {
    LCAST_ADDR_OK = 0,
    LCAST_ADDR_NOT_PRIVATE,     /**< outside the private, link-local and loopback ranges (100.64/10 included) */
    LCAST_ADDR_GATEWAY,         /**< the station's IPv4 gateway: a router masquerading a port forward */
    LCAST_ADDR_HOTSPOT,         /**< it came in over the setup hotspot, or comes from the hotspot's subnet */
} lcast_addr_verdict_t;

/** An IPv4-mapped IPv6 address (::ffff:a.b.c.d) becomes the IPv4 address; anything else stays. */
void lcast_addr_unmap(lcast_addr_t *a);

/**
 * The verdict for a new connection. peer and local are unmapped here (copies).
 * gw_ip4: the station interface's IPv4 gateway, network order as a uint32_t in
 * memory (0 = unknown or no rule). ap_ip4: the AP's IPv4 address, same form (0
 * = none). ap_mask4: the AP's netmask while the AP is up, else 0. ap_ll6: the
 * AP's IPv6 link-local address (NULL = none). Order: the hotspot, then the
 * ranges, then the gateway.
 *
 * The hotspot: the local address is the AP's (IPv4, also v4-mapped, or its
 * link-local address), or, while the AP is up, the peer is an IPv4 address
 * inside the AP's subnet. lwIP accepts an IPv4 packet for any of the
 * station's addresses on any interface (the weak host model), so a phone on
 * the hotspot can reach the station's home-network address: the local
 * address alone does not tell. Its answers go back over the hotspot, so a
 * home-network host cannot pass for one with a forged hotspot source. IPv6:
 * the station has link-local addresses only, and lwIP takes a packet for a
 * link-local address only on that address's own interface, so the local
 * address decides (a peer's link-local address says nothing: every link
 * uses fe80::/64). A home network numbered like the hotspot (192.168.4.0/24)
 * counts as the hotspot while the AP is up.
 */
lcast_addr_verdict_t lcast_addr_verdict(const lcast_addr_t *peer, const lcast_addr_t *local, uint32_t gw_ip4,
                                        uint32_t ap_ip4, uint32_t ap_mask4, const uint8_t *ap_ll6);

/**
 * Address text: dotted IPv4 (a v4-mapped address as IPv4), IPv6 in RFC 5952
 * form (lower case, the longest run of two or more zero groups, the first of
 * equal runs, as "::"). "?" for NULL. Always terminated. @return the length.
 */
size_t lcast_addr_text(const lcast_addr_t *a, char *out, size_t cap);

/**
 * The client a throttle entry belongs to: an IPv4 address (also v4-mapped) as
 * its 32 bits, an IPv6 link-local address as its 128 bits folded to 64, any
 * other IPv6 address as its /64 prefix. 0 for NULL.
 */
uint64_t lcast_client_key(const lcast_addr_t *a);

/** The 4 bytes a journal record carries: the IPv4 address, or an IPv6 address's last 4. */
void lcast_addr_word(const lcast_addr_t *a, uint8_t out[4]);

/* ---- connection slots -------------------------------------------------------------------- */

#define LCAST_FIRST_BYTE_MS  LOCAL_CASTER_FIRST_BYTE_MS  /**< nothing sent this long after the accept: closed */

/** What a connection slot holds, as the slot policy sees it. */
typedef enum {
    LCAST_SLOT_FREE = 0,
    LCAST_SLOT_REQ,             /**< reading a request */
    LCAST_SLOT_LINGER,          /**< its answer is out: draining until the peer closes */
    LCAST_SLOT_BUSY,            /**< writing an answer, or a rover streaming: never taken */
} lcast_slot_kind_t;

typedef struct {
    lcast_slot_kind_t kind;
    uint64_t key;               /**< lcast_client_key() of its peer */
    uint32_t since_ms;          /**< its accept (REQ) or the end of its answer (LINGER) */
    bool got_bytes;             /**< REQ: a byte of its request arrived */
} lcast_slot_view_t;

/** slot -1: refuse the new connection; else the slot it takes (`evict`: close what is there first). */
typedef struct {
    int slot;
    bool evict;
} lcast_slot_pick_t;

/**
 * A slot for a new connection from `key` (nothing read from it yet). A free
 * slot first. With none free, a connection that cannot be a rover's gives
 * its slot up, the oldest of the first of these kinds found: one lingering
 * after its answer; one that has sent nothing yet; one still sending its
 * request from the same client (when slots are scarce a client has one
 * request in the works: the newer replaces it); one still sending its
 * request from a client that holds another slot. Streaming rovers and
 * answers being written are never taken. None of these: refused. While
 * slots are free a client may open several (two rovers behind one address
 * connect at the same moment).
 */
lcast_slot_pick_t lcast_slot_pick(const lcast_slot_view_t *slots, size_t n, uint64_t key, uint32_t now_ms);

/* ---- request ----------------------------------------------------------------------------- */

#define LCAST_MOUNT_MAX        64u    /**< mount text buffer: at most 63 characters are kept */
#define LCAST_CRED_MAX         96u    /**< decoded "user:pass" buffer: at most 95 bytes */
#define LCAST_REQ_MAX_HEADERS  32u    /**< header lines read; later ones are ignored */

typedef enum {
    LCAST_REQ_NEED_MORE = 0,    /**< the header is not complete yet */
    LCAST_REQ_DONE,             /**< parsed: the fields below are set */
    LCAST_REQ_TOO_LARGE,        /**< LOCAL_CASTER_REQUEST_MAX bytes without the end of the header */
    LCAST_REQ_BAD,              /**< not an NTRIP GET (method, target, version, a NUL, two Authorization headers) */
} lcast_req_result_t;

/** One request: the bytes up to the end of the header, then what the parser found in them. */
typedef struct {
    uint8_t buf[LOCAL_CASTER_REQUEST_MAX];
    size_t len;                 /**< bytes in buf */
    size_t scanned;             /**< bytes already searched for the end of the header */
    lcast_req_result_t result;  /**< sticky once not NEED_MORE */
    /* Valid with LCAST_REQ_DONE: */
    char mount[LCAST_MOUNT_MAX];/**< after the first '/', up to '?' or the end; not percent-decoded */
    bool mount_long;            /**< it had more than LCAST_MOUNT_MAX - 1 characters (never ours) */
    uint8_t http_minor;         /**< 1 for HTTP/1.1, else 0 (HTTP/1.0 or no version) */
    bool ntrip2;                /**< Ntrip-Version: Ntrip/2.0 */
    bool auth_present;          /**< an Authorization header was sent */
    bool auth_bad;              /**< ... but not Basic, not base64, over 95 bytes or without ':' */
    char auth[LCAST_CRED_MAX];  /**< decoded "user:pass" (when !auth_bad), terminated */
    size_t auth_len;
    bool browser;               /**< an Origin or Sec-Fetch-* header */
    char agent[LOCAL_CASTER_AGENT_LEN]; /**< User-Agent, printable ASCII, cut; "" = none */
} lcast_req_t;

/** Start over (also wipes). */
void lcast_req_reset(lcast_req_t *r);

/** Overwrite everything the request held (raw bytes, credentials) with zeros. */
void lcast_req_wipe(lcast_req_t *r);

/** Where the next received bytes go, and how many fit (0 once the result is set or the buffer is full). */
uint8_t *lcast_req_room(lcast_req_t *r, size_t *room);

/** n bytes were written at lcast_req_room(): look for the end of the header, parse it when complete. */
lcast_req_result_t lcast_req_added(lcast_req_t *r, size_t n);

/** Copy up to the room left and call lcast_req_added() (bytes beyond the room are dropped). */
lcast_req_result_t lcast_req_feed(lcast_req_t *r, const uint8_t *bytes, size_t n);

/* ---- decision ---------------------------------------------------------------------------- */

typedef enum {
    LCAST_ACT_SOURCETABLE = 0,
    LCAST_ACT_STREAM,
    LCAST_ACT_E400,
    LCAST_ACT_E401,
    LCAST_ACT_E403,
    LCAST_ACT_E429,
    LCAST_ACT_E503,
} lcast_action_t;

/** The reply's form. */
typedef enum {
    LCAST_FLAV_V1 = 0,          /**< NTRIP 1.0: ICY, SOURCETABLE, HTTP/1.0 errors without Ntrip-Version */
    LCAST_FLAV_V2_CHUNKED,      /**< NTRIP 2.0 over HTTP/1.1: chunked data */
    LCAST_FLAV_V2_RAW,          /**< NTRIP 2.0 over HTTP/1.0: raw data, HTTP/1.0 status lines */
} lcast_flavour_t;

/** Expected credentials "user:pass", zero-padded. */
typedef struct {
    char v[LCAST_CRED_MAX];
    size_t len;                 /**< 0 = none (nothing matches) */
} lcast_cred_t;

typedef struct {
    lcast_action_t action;
    lcast_flavour_t flavour;
    bool count_fail;            /**< a wrong login: counts for the throttle */
    bool login_ok;              /**< the credentials matched (resets the throttle; also with E503) */
} lcast_decision_t;

/**
 * The decision for a request (spec order): not DONE (bad, too long, late) ->
 * 400; a browser -> 403; an empty mount or not ours (ignoring case) -> the
 * source table; throttle_wait_s > 0 -> 429 (credentials not checked); no
 * Authorization -> 401; wrong or malformed -> 401 counted; `streaming` >=
 * `max_streaming` -> 503; else stream. `res` is the parser's result (pass
 * LCAST_REQ_NEED_MORE for a request that ran out of time).
 */
lcast_decision_t lcast_decide(const lcast_req_t *req, lcast_req_result_t res, const char *mountpoint,
                              const lcast_cred_t *cred, uint32_t throttle_wait_s, uint32_t streaming,
                              uint32_t max_streaming);

/* ---- replies ----------------------------------------------------------------------------- */

#define LCAST_CLOCK_VALID_UNIX 1704067200   /**< 2024-01-01: an earlier clock is not trusted (no Date header) */

/** Date header value "Mon, 19 Oct 2026 04:30:00 GMT". @return the length, 0 when cap < 30. */
size_t lcast_imf_date(int64_t unix_s, char *out, size_t cap);

/**
 * The success reply of a stream. server_ver: printable ASCII (sanitised by the
 * caller). now_unix: 0, or below LCAST_CLOCK_VALID_UNIX, sends no Date.
 * @return its length, 0 when it does not fit (nothing usable in buf).
 */
size_t lcast_reply_stream(char *buf, size_t cap, lcast_flavour_t f, const char *server_ver, int64_t now_unix);

/**
 * An error reply (action E400 to E503). realm_mount: the configured
 * mountpoint (401's WWW-Authenticate realm "/<mount>"); retry_s: 429's
 * Retry-After. @return its length, 0 when it does not fit or `action` is no error.
 */
size_t lcast_reply_error(char *buf, size_t cap, lcast_action_t action, lcast_flavour_t f, const char *realm_mount,
                         uint32_t retry_s, const char *server_ver, int64_t now_unix);

/** The source table with one STR record (without its CRLF). @return its length, 0 when it does not fit. */
size_t lcast_reply_sourcetable(char *buf, size_t cap, lcast_flavour_t f, const char *str_record,
                               const char *server_ver, int64_t now_unix);

/* ---- STR record -------------------------------------------------------------------------- */

#define LCAST_STR_MSGS_MAX    24u     /**< messages listed in the format details */
#define LCAST_STR_DETAILS_MAX 255u    /**< characters of the format details */
#define LCAST_STR_MAX         512u    /**< a record always fits (fields are bounded) */

typedef struct {
    uint16_t msg;
    uint16_t period_s;
} lcast_str_msg_t;

typedef struct {
    const char *mount;          /**< our mountpoint */
    const char *identifier;     /**< the station's host name */
    const char *generator;      /**< the receiver type (1033), "" or NULL = "onodiy" */
    lcast_str_msg_t msgs[LCAST_STR_MSGS_MAX];
    size_t n;
    uint8_t carrier;            /**< 0, 1 or 2 */
    uint32_t bitrate;           /**< bits/s */
} lcast_str_in_t;

/** The message statistics the station observes (gnss_obs_msg_stat_t's fields that matter). */
typedef struct {
    uint16_t msg;
    uint16_t expected_period_s; /**< in the profile, 0 = not */
    uint64_t count;             /**< 0 = never seen */
    uint32_t period_ms;         /**< UINT32_MAX = fewer than two seen */
    uint32_t age_ms;            /**< since last seen, UINT32_MAX = never */
} lcast_obs_msg_t;

/**
 * The format details' messages: those seen in the last 60 s, else the
 * profile's; 1101-1107 left out; ascending; at most LCAST_STR_MSGS_MAX, and
 * only as many as LCAST_STR_DETAILS_MAX characters hold. Periods in whole
 * seconds (rounded, at least 1; with one sighting only, the profile's period
 * or 1). @return the number written.
 */
size_t lcast_str_msgs_from_obs(const lcast_obs_msg_t *obs, size_t n, lcast_str_msg_t *out, size_t cap);

/** Carrier field: 2 when a constellation shows 2+ bands, 1 with one band, else (nothing observed) 2 with an MSM in the list, 0 without. */
uint8_t lcast_str_carrier(uint8_t max_bands, const lcast_str_msg_t *msgs, size_t n);

/**
 * "STR;<mount>;<identifier>;RTCM 3.3;<details>;<carrier>;<nav>;onodiy;;0.00;0.00;0;0;<generator>;none;B;N;<bitrate>;"
 * (19 fields; every field printable ASCII with ';' as '_'). @return the length, 0 when it does not fit.
 */
size_t lcast_str_build(const lcast_str_in_t *in, char *out, size_t cap);

/** Printable ASCII only (others dropped), ';' as '_' when `str_field`, cut to cap - 1. @return the length. */
size_t lcast_sanitize(const char *in, char *out, size_t cap, bool str_field);

/* ---- chunks ------------------------------------------------------------------------------ */

#define LCAST_CHUNK_HEAD  8u        /**< room before the payload ("1000\r\n" takes 6) */
#define LCAST_CHUNK_DATA  4096u     /**< payload per chunk at most */
#define LCAST_CHUNK_TAIL  2u        /**< "\r\n" after the payload */
#define LCAST_CHUNK_END   "0\r\n\r\n"

/**
 * Frame the len bytes at buf[LCAST_CHUNK_HEAD ..] as one chunk: the size line
 * (lowercase hex, no leading zeros) right before them, CRLF right after.
 * *start = the chunk's first byte. @return the chunk's length, 0 for len 0 or
 * over LCAST_CHUNK_DATA.
 */
size_t lcast_chunk_wrap(uint8_t *buf, size_t len, size_t *start);

/* ---- credentials ------------------------------------------------------------------------- */

/** Overwrite with zeros (not optimised away). */
void lcast_secure_zero(void *p, size_t len);

/**
 * Base64 (RFC 4648 standard alphabet; padding optional; no whitespace inside).
 * @return false for a bad character or length, or more than cap bytes.
 */
bool lcast_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len);

/** "user:pass" (wipes the old value first). @return false (and none) when it would exceed 95 bytes. */
bool lcast_cred_set(lcast_cred_t *c, const char *user, const char *pass);

/** Constant time in the content and the length: both padded into 96-byte windows. */
bool lcast_cred_match(const lcast_cred_t *expected, const char *got, size_t got_len);

/* ---- login throttle (the web Login's rules) ---------------------------------------------- */

#define LCAST_THROTTLE_FREE      5u     /**< free failures before the delays */
#define LCAST_THROTTLE_MAX_S     300u   /**< longest delay */
#define LCAST_THROTTLE_CLIENTS   16u    /**< addresses tracked (the least recently seen is replaced) */
#define LCAST_THROTTLE_BURST     30u    /**< failures in total in a burst */
#define LCAST_THROTTLE_REFILL_S  120u   /**< then one more per this */

typedef struct {
    bool used;
    uint64_t key;               /**< lcast_client_key() */
    uint32_t seen_s;
    uint32_t fails;             /**< consecutive failures */
    uint32_t last_s;            /**< the last counted failure */
} lcast_throttle_client_t;

#define LCAST_THROTTLE_TRUSTED   2u     /**< addresses of the latest logins, exempt from the global bucket */

typedef struct {
    lcast_throttle_client_t c[LCAST_THROTTLE_CLIENTS];
    uint32_t left;              /**< failures left in the global burst */
    uint32_t at_s;              /**< its refill reference */
    uint64_t trusted[LCAST_THROTTLE_TRUSTED];   /**< the latest successful logins' keys, newest first */
    uint32_t trusted_n;
} lcast_throttles_t;

/** Delay after the n-th consecutive failure: 0 below 5, then min(2^(n-4), 300) s. */
uint32_t lcast_throttle_delay_s(uint32_t fails);
void lcast_throttles_init(lcast_throttles_t *t, uint32_t now_s);
/**
 * Seconds until this address may try again (0 = now): its own delay or the
 * global bucket. The addresses of the latest successful logins (RAM, since
 * boot) skip the global bucket, so failures from elsewhere never lock the
 * owner's rover out; their own delay still holds.
 */
uint32_t lcast_throttles_wait_s(lcast_throttles_t *t, uint64_t key, uint32_t now_s);
/** A counted failure (only after a wait of 0). @return its consecutive failures. */
uint32_t lcast_throttles_fail(lcast_throttles_t *t, uint64_t key, uint32_t now_s);
/** A success resets the address and makes it the newest trusted one. */
void lcast_throttles_ok(lcast_throttles_t *t, uint64_t key);

/* ---- journal gate and queue -------------------------------------------------------------- */

#define LCAST_JGATE_AFTER_S   10u      /**< a session is listed once it has streamed this long */
#define LCAST_JGATE_TOKENS    6u       /**< sessions listed per hour (the bucket starts full) */
#define LCAST_JGATE_REFILL_S  600u     /**< one token per this */

typedef struct {
    uint32_t tokens;
    uint32_t at_s;
    uint32_t unlisted;          /**< sessions not listed since the previous listed one */
} lcast_jgate_t;

void lcast_jgate_init(lcast_jgate_t *g, uint32_t now_s);

/**
 * A session that has streamed `age_s` asks to be listed. @return true (a
 * token taken; *unlisted = the count it carries, then reset) when age_s >=
 * LCAST_JGATE_AFTER_S and a token is there.
 */
bool lcast_jgate_try_list(lcast_jgate_t *g, uint32_t age_s, uint32_t now_s, uint32_t *unlisted);

/** A session that was never listed ended. */
void lcast_jgate_unlisted_end(lcast_jgate_t *g);

/** DIAG_EV_ROVER_UP's a and b. */
void lcast_jrec_up(uint32_t conn, uint32_t version, const lcast_addr_t *peer, uint32_t age_s, uint32_t unlisted,
                   int32_t *a, int32_t *b);

/** DIAG_EV_ROVER_DOWN's a and b. */
void lcast_jrec_down(uint32_t conn, local_caster_end_t reason, uint32_t age_s, int32_t *a, int32_t *b);

#define LCAST_JQ_LEN    4u
#define LCAST_JQ_MAX_S  60u      /**< an entry older than this is dropped */

typedef struct {
    uint16_t kind;
    int32_t a;
    int32_t b;
    uint32_t at_s;
} lcast_jq_entry_t;

typedef struct {
    lcast_jq_entry_t e[LCAST_JQ_LEN];
    uint32_t n;
} lcast_jq_t;

/** Queue at the end; when full the oldest goes first. @return true when an entry was dropped for room. */
bool lcast_jq_push(lcast_jq_t *q, uint16_t kind, int32_t a, int32_t b, uint32_t now_s);

/** The head to try now, after dropping entries older than LCAST_JQ_MAX_S (*expired += them). @return false when empty. */
bool lcast_jq_head(lcast_jq_t *q, uint32_t now_s, lcast_jq_entry_t *out, uint32_t *expired);

/** The head is done (written, or dropped for good). */
void lcast_jq_pop(lcast_jq_t *q);

/* ---- bit rate ---------------------------------------------------------------------------- */

#define LCAST_RATE_SAMPLES   6u
#define LCAST_RATE_STEP_S    10u

/** Input bytes sampled every LCAST_RATE_STEP_S into a ring; the rate is over the span the ring holds. */
typedef struct {
    uint64_t bytes[LCAST_RATE_SAMPLES];
    uint32_t at_s[LCAST_RATE_SAMPLES];
    uint32_t n;                 /**< samples held */
    uint32_t head;              /**< the next slot */
} lcast_rate_t;

void lcast_rate_init(lcast_rate_t *r);
/** Take a sample when LCAST_RATE_STEP_S passed since the last one (a gap over twice that starts over). */
void lcast_rate_sample(lcast_rate_t *r, uint32_t now_s, uint64_t bytes_in);
/** Bits per second over the ring, 0 until two samples. */
uint32_t lcast_rate_bps(const lcast_rate_t *r);

/* ---- stored config ----------------------------------------------------------------------- */

#define LCAST_CFG_VERSION   1u
#define LCAST_CFG_BLOB_MAX  146u     /**< 1 + 1 + (1 + 47) + (1 + 31) + (1 + 63) */

/**
 * The record: u8 version, bool enabled, str mountpoint, str username, str
 * password ("" or 8-63; the stored one). c->password is the stored password.
 * @return the length, 0 when it does not fit.
 */
size_t lcast_cfg_encode(const local_caster_cfg_t *c, uint8_t *buf, size_t cap);

/**
 * Read a stored record: version 1 or later (later versions only append),
 * every field within its rule (enabled only with a password). out->password =
 * the stored password, password_set accordingly. Wipes its own copies.
 * @return false (out zeroed) otherwise.
 */
bool lcast_cfg_decode(const uint8_t *buf, size_t len, local_caster_cfg_t *out);

/** Mountpoint and username rule: 1 to size - 1 of [A-Za-z0-9._-]. */
bool lcast_name_ok(const char *s, size_t size);

/** Password rule: 8 to 63 bytes of 0x21-0x7E. */
bool lcast_password_ok(const char *s);

/**
 * local_caster_check_config()'s rules and words (local_caster.h).
 * @return true, or false with *field and *rule set (each optional).
 */
bool lcast_cfg_check(const local_caster_cfg_t *cfg, bool password_stored, const char **field, const char **rule);

/** End-reason words for the log ("closed", "stalled", ...); "none" for an unknown value. */
const char *lcast_end_name(local_caster_end_t reason);

#ifdef __cplusplus
}
#endif
