/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "ntrip_proto.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "settings_codec.h"

/* ---- small helpers ------------------------------------------------------------- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

static bool is_alnum(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool is_hex(char c)
{
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool empty(const char *s)
{
    return s == NULL || s[0] == '\0';
}

/* Control characters (CR, LF, tab, NUL-adjacent bytes, DEL) anywhere? */
static bool has_ctrl(const char *s)
{
    for (; s != NULL && *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20u || c == 0x7Fu) {
            return true;
        }
    }
    return false;
}

static bool starts_with(const char *s, size_t len, const char *lit)
{
    size_t n = strlen(lit);
    return len >= n && memcmp(s, lit, n) == 0;
}

static bool equals(const char *s, size_t len, const char *lit)
{
    return len == strlen(lit) && memcmp(s, lit, len) == 0;
}

static bool due(uint32_t now, uint32_t at)
{
    return (int32_t)(now - at) >= 0;
}

/* ---- field validation ---------------------------------------------------------- */

static bool host_ok(const char *h)
{
    size_t len = strlen(h);
    if (len == 0 || len > 63) {
        return false;
    }
    if (strchr(h, ':') != NULL) {
        /* IPv6 literal (no brackets, no zone). */
        size_t colons = 0;
        for (const char *p = h; *p != '\0'; p++) {
            if (*p == ':') {
                colons++;
            } else if (!is_hex(*p) && *p != '.') {
                return false;
            }
        }
        return colons >= 2;
    }
    /* DNS name or dotted IPv4: labels of [A-Za-z0-9-], 1..63, no '-' at either end. */
    size_t label = 0;
    char prev = '.';
    for (const char *p = h;; p++) {
        char c = *p;
        if (c == '.' || c == '\0') {
            if (label == 0 || prev == '-') {
                return false;
            }
            if (c == '\0') {
                return true;
            }
            label = 0;
        } else if (is_alnum(c) || c == '-') {
            if (label == 0 && c == '-') {
                return false;
            }
            if (++label > 63) {
                return false;
            }
        } else {
            return false;
        }
        prev = c;
    }
}

static bool mount_ok(const char *m)
{
    for (; *m != '\0'; m++) {
        if (!is_alnum(*m) && *m != '.' && *m != '_' && *m != '-') {
            return false;
        }
    }
    return true;
}

/* NTRIP 1.0 passwords end at the first space on the caster side. */
static bool v1_password_ok(const char *p)
{
    for (; *p != '\0'; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x21u || c > 0x7Eu) {
            return false;
        }
    }
    return true;
}

static bool label_ok(const char *l)
{
    for (; *l != '\0'; l++) {
        if (!is_alnum(*l)) {
            return false;
        }
    }
    return true;
}

ntrip_fields_result_t ntrip_validate_fields(const ntrip_fields_t *f)
{
    const char *host = f->host ? f->host : "";
    const char *mount = f->mountpoint ? f->mountpoint : "";
    const char *user = f->username ? f->username : "";
    const char *pass = f->password ? f->password : "";
    const char *label = f->label ? f->label : "";

    if (!empty(host) && (has_ctrl(host) || !host_ok(host))) {
        return NTRIP_FIELDS_BAD_HOST;
    }
    if (has_ctrl(mount) || !mount_ok(mount)) {
        return NTRIP_FIELDS_BAD_MOUNTPOINT;
    }
    if (has_ctrl(user) || strchr(user, ':') != NULL) {
        return NTRIP_FIELDS_BAD_USERNAME;
    }
    if (has_ctrl(pass) || (!f->v2 && !v1_password_ok(pass))) {
        return NTRIP_FIELDS_BAD_PASSWORD;
    }
    if (!label_ok(label)) {
        return NTRIP_FIELDS_BAD_LABEL;
    }
    if (f->enabled) {
        if (f->port == 0) {
            return NTRIP_FIELDS_BAD_PORT;
        }
        if (empty(host) || empty(mount)) {
            return NTRIP_FIELDS_MISSING;
        }
        if (!f->v2 && empty(pass)) {
            return NTRIP_FIELDS_MISSING;
        }
        if (f->v2 && empty(user) && !ntrip_host_is_onocoy(host)) {
            return NTRIP_FIELDS_MISSING;
        }
    }
    return NTRIP_FIELDS_OK;
}

bool ntrip_host_in_domain(const char *host, const char *domain)
{
    if (host == NULL || domain == NULL) {
        return false;
    }
    size_t hl = strlen(host);
    size_t dl = strlen(domain);
    if (dl == 0 || hl < dl) {
        return false;
    }
    const char *tail = host + (hl - dl);
    for (size_t i = 0; i < dl; i++) {
        if (lower(tail[i]) != lower(domain[i])) {
            return false;
        }
    }
    return hl == dl || host[hl - dl - 1] == '.';
}

bool ntrip_host_is_onocoy(const char *host)
{
    return ntrip_host_in_domain(host, "onocoy.com");
}

bool ntrip_host_is_rtk2go(const char *host)
{
    return ntrip_host_in_domain(host, "rtk2go.com");
}

bool ntrip_host_is_open_caster(const char *host)
{
    return ntrip_host_is_rtk2go(host) || ntrip_host_in_domain(host, "crtk.net");
}

ntrip_policy_t ntrip_check_policy(const ntrip_policy_slot_t *slots, size_t count, size_t edited)
{
    if (edited >= count || !slots[edited].enabled) {
        return NTRIP_POLICY_OK;
    }
    const char *mine = slots[edited].host;
    bool other_onocoy = false;
    bool other_rtk2go = false;
    bool other_open = false;
    for (size_t i = 0; i < count; i++) {
        if (i == edited || !slots[i].enabled) {
            continue;
        }
        other_onocoy |= ntrip_host_is_onocoy(slots[i].host);
        other_rtk2go |= ntrip_host_is_rtk2go(slots[i].host);
        other_open |= ntrip_host_is_open_caster(slots[i].host);
    }
    if (ntrip_host_is_onocoy(mine) && other_onocoy) {
        return NTRIP_POLICY_SECOND_ONOCOY;
    }
    if (ntrip_host_is_rtk2go(mine) && other_rtk2go) {
        return NTRIP_POLICY_SECOND_RTK2GO;
    }
    bool pair = (ntrip_host_is_onocoy(mine) && other_open) || (ntrip_host_is_open_caster(mine) && other_onocoy);
    if (pair && !slots[edited].dup_ack) {
        return NTRIP_POLICY_NEED_DUP_ACK;
    }
    return NTRIP_POLICY_OK;
}

/* ---- requests -------------------------------------------------------------------- */

void ntrip_secure_zero(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len-- > 0) {
        *v++ = 0;
    }
}

size_t ntrip_base64(const uint8_t *in, size_t len, char *out, size_t cap)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = ((len + 2) / 3) * 4;
    if (cap < need + 1) {
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < len) {
            v |= in[i + 2];
        }
        out[o++] = alphabet[(v >> 18) & 0x3Fu];
        out[o++] = alphabet[(v >> 12) & 0x3Fu];
        out[o++] = (i + 1 < len) ? alphabet[(v >> 6) & 0x3Fu] : '=';
        out[o++] = (i + 2 < len) ? alphabet[v & 0x3Fu] : '=';
    }
    out[o] = '\0';
    return o;
}

size_t ntrip_build_agent(const char *app_version, char *out, size_t cap)
{
    const char *v = app_version;
    if (v == NULL || v[0] == '\0') {
        v = "unknown";
    } else if ((v[0] == 'v' || v[0] == 'V') && is_digit(v[1])) {
        v++;
    }
    int n = snprintf(out, cap, "NTRIP " NTRIP_AGENT_PRODUCT "/%s", v);
    if (n < 0 || (size_t)n >= cap) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)out[i];
        if (i != 5 && (c < 0x21u || c > 0x7Eu)) {   /* the one space after "NTRIP" stays */
            out[i] = '_';
        }
    }
    return (size_t)n;
}

void ntrip_wire_names(const char *host, bool v2, const char *mountpoint, const char *username,
                      const char *label, ntrip_wire_names_t *out)
{
    out->mount = mountpoint;
    out->user = username;
    if (ntrip_host_is_onocoy(host)) {
        /* The credential username lives in `mountpoint`. */
        if (v2) {
            out->mount = empty(label) ? "onocoydiy" : label;
            out->user = mountpoint;
        } else {
            out->user = "";
        }
    }
}

static size_t finish(int n, size_t cap)
{
    return (n < 0 || (size_t)n >= cap) ? 0 : (size_t)n;
}

size_t ntrip_build_v1_request(char *buf, size_t cap, const char *password, const char *mount, const char *agent)
{
    if (empty(password) || empty(mount) || empty(agent) || !v1_password_ok(password) || !mount_ok(mount) ||
        has_ctrl(agent)) {
        return 0;
    }
    int n = snprintf(buf, cap, "SOURCE %s /%s\r\nSource-Agent: %s\r\n\r\n", password, mount, agent);
    size_t len = finish(n, cap);
    if (len == 0 && cap > 0) {
        ntrip_secure_zero(buf, cap);
    }
    return len;
}

size_t ntrip_build_v2_request(char *buf, size_t cap, const char *host, uint16_t port, bool tls,
                              const char *mount, const char *user, const char *password,
                              const char *agent, bool raw_body)
{
    const char *pass = password ? password : "";
    if (empty(host) || empty(mount) || empty(agent) || user == NULL || has_ctrl(host) || has_ctrl(user) ||
        has_ctrl(pass) || has_ctrl(agent) || !mount_ok(mount) || strchr(user, ':') != NULL) {
        return 0;
    }

    char cred[48 + 1 + 64 + 1];
    char b64[((sizeof(cred) + 2) / 3) * 4 + 1];
    int cl = snprintf(cred, sizeof(cred), "%s:%s", user, pass);
    size_t blen = 0;
    if (cl > 0 && (size_t)cl < sizeof(cred)) {
        blen = ntrip_base64((const uint8_t *)cred, (size_t)cl, b64, sizeof(b64));
    }
    ntrip_secure_zero(cred, sizeof(cred));
    if (blen == 0) {
        ntrip_secure_zero(b64, sizeof(b64));
        return 0;
    }

    char hostport[64 + 2 + 7];
    bool v6 = strchr(host, ':') != NULL;
    bool default_port = (tls && port == 443) || (!tls && port == 80);
    if (default_port) {
        snprintf(hostport, sizeof(hostport), v6 ? "[%s]" : "%s", host);
    } else {
        snprintf(hostport, sizeof(hostport), v6 ? "[%s]:%u" : "%s:%u", host, (unsigned)port);
    }

    int n = snprintf(buf, cap,
                     "POST /%s HTTP/1.1\r\n"
                     "Host: %s\r\n"
                     "Ntrip-Version: Ntrip/2.0\r\n"
                     "User-Agent: %s\r\n"
                     "Authorization: Basic %s\r\n"
                     "Connection: close\r\n"
                     "%s"
                     "\r\n",
                     mount, hostport, agent, b64, raw_body ? "" : "Transfer-Encoding: chunked\r\n");
    ntrip_secure_zero(b64, sizeof(b64));
    size_t len = finish(n, cap);
    if (len == 0 && cap > 0) {
        ntrip_secure_zero(buf, cap);
    }
    return len;
}

/* ---- chunked encoding --------------------------------------------------------------- */

size_t ntrip_chunk_wrap(uint8_t *buf, size_t len, size_t *start)
{
    static const char hex[] = "0123456789abcdef";
    if (len == 0 || len > NTRIP_CHUNK_MAX_PAYLOAD) {
        return 0;
    }
    char digits[8];
    size_t nd = 0;
    for (size_t v = len; v != 0; v >>= 4) {
        digits[nd++] = hex[v & 0xFu];
    }
    size_t head = nd + 2;
    size_t s = NTRIP_CHUNK_HEAD_RESERVE - head;
    for (size_t i = 0; i < nd; i++) {
        buf[s + i] = (uint8_t)digits[nd - 1 - i];
    }
    buf[s + nd] = '\r';
    buf[s + nd + 1] = '\n';
    buf[NTRIP_CHUNK_HEAD_RESERVE + len] = '\r';
    buf[NTRIP_CHUNK_HEAD_RESERVE + len + 1] = '\n';
    *start = s;
    return head + len + NTRIP_CHUNK_TAIL;
}

/* ---- RTCM frames ---------------------------------------------------------------- */

static bool is_msm(uint16_t type)
{
    uint16_t group = type / 10u;
    uint16_t kind = type % 10u;
    /* GPS, GLONASS, Galileo, QZSS, BeiDou, NavIC; 1101-1107 are not MSMs here. */
    bool constellation = group == 107u || group == 108u || group == 109u || group == 111u || group == 112u ||
                         group == 113u;
    return constellation && kind >= 1u && kind <= 7u;
}

void ntrip_count_frames(const uint8_t *buf, size_t len, uint32_t *frames, uint32_t *epochs_ended)
{
    uint32_t nf = 0;
    uint32_t ne = 0;
    size_t off = 0;
    while (off + 6u <= len && buf[off] == 0xD3u) {
        size_t plen = ((size_t)(buf[off + 1] & 0x03u) << 8) | buf[off + 2];
        size_t flen = 3u + plen + 3u;
        if (off + flen > len) {
            break;
        }
        nf++;
        if (plen >= 7u) {
            const uint8_t *p = buf + off + 3;
            uint16_t type = (uint16_t)(((uint16_t)p[0] << 4) | (p[1] >> 4));
            /* Multiple-message bit: payload bit 54 (after type, station id, epoch time). */
            if (is_msm(type) && ((p[6] >> 1) & 1u) == 0u) {
                ne++;
            }
        }
        off += flen;
    }
    *frames = nf;
    *epochs_ended = ne;
}

/* ---- reply classification --------------------------------------------------------- */

bool ntrip_contains_ci(const char *data, size_t len, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || len < n) {
        return n == 0;
    }
    for (size_t i = 0; i + n <= len; i++) {
        size_t k = 0;
        while (k < n && lower(data[i + k]) == needle[k]) {
            k++;
        }
        if (k == n) {
            return true;
        }
    }
    return false;
}

static ntrip_verdict_t fail(ntrip_cls_t cls, ntrip_row_t row, ntrip_hint_t hint, uint8_t rule)
{
    ntrip_verdict_t v = { 0 };
    v.cls = cls;
    v.row = row;
    v.hint = hint;
    v.rule = rule;
    return v;
}

static ntrip_verdict_t ok(int code, uint8_t rule)
{
    ntrip_verdict_t v = { 0 };
    v.success = true;
    v.status_code = code;
    v.rule = rule;
    return v;
}

ntrip_verdict_t ntrip_classify_http(int code)
{
    ntrip_verdict_t v;
    switch (code) {
    case 200:
        return ok(200, 3);
    case 401:
        v = fail(NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, NTRIP_HINT_NONE, 4);
        break;
    case 409:
        v = fail(NTRIP_CLS_MOUNT_BUSY, NTRIP_ROW_MOUNT_BUSY, NTRIP_HINT_NONE, 4);
        break;
    case 500:
    case 503:
        v = fail(NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, NTRIP_HINT_CASTER_BUSY, 4);
        break;
    case 400:
    case 403:
    case 404:
    case 431:
    case 501:
        v = fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_NONE, 4);
        break;
    default:
        v = fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_UNEXPECTED, 4);
        break;
    }
    v.status_code = code;
    return v;
}

/* "HTTP/1.x <3 digits>" followed by a space or the end of the line. */
static int http_code(const char *s, size_t len)
{
    if (len < 12 || !starts_with(s, len, "HTTP/1.") || !is_digit(s[7]) || s[8] != ' ' || !is_digit(s[9]) ||
        !is_digit(s[10]) || !is_digit(s[11]) || (len > 12 && s[12] != ' ')) {
        return -1;
    }
    return (s[9] - '0') * 100 + (s[10] - '0') * 10 + (s[11] - '0');
}

/*
 * The first reply line, matched in this order (first match wins):
 *   1 "ICY 200 OK"                         success
 *   2 "OK"                                 success
 *   3 "HTTP/1.x 200 ..."                   success (headers follow)
 *   4 "HTTP/1.x <code> ..."                by code; 4xx get the body check
 *   5 "SOURCETABLE 200 OK"                 CONFIG: the caster took us for a client
 *   6 "ERROR - Bad Password"               AUTH
 *   7 "ERROR - Mount Point Taken or Invalid"  MOUNT_BUSY (may also mean unknown)
 *   8 "ERROR - Already Connected"          MOUNT_BUSY
 *   9 "ERROR - Bad Mountpoint" / "ERROR - Missing Mountpoint"  CONFIG
 *  10 "ERROR - Too many sources"           TRANSIENT
 *  11 any other "ERROR..."                 CONFIG
 *  12 "Error <digit>..."                   TRANSIENT (plain-text caster errors)
 *  13 "Not authorized (no NTRIP source)"   CONFIG: agent rejected
 *  14 starts with '<' (HTML)               body check: "banned" -> BANNED, else CONFIG
 *  15 contains "banned"                    BANNED
 *  16 contains "sandbox"                   BANNED
 *  17 no line in time / closed before it   TRANSIENT (ntrip_reply_end)
 *  18 anything else                        CONFIG
 * Matching is case-sensitive except rows 15 and 16, so "ERROR" and "Error"
 * are told apart.
 */
ntrip_verdict_t ntrip_classify_line(const char *s, size_t len)
{
    if (equals(s, len, "ICY 200 OK")) {
        return ok(200, 1);
    }
    if (equals(s, len, "OK")) {
        return ok(200, 2);
    }
    int code = http_code(s, len);
    if (code >= 0) {
        return ntrip_classify_http(code);
    }
    if (starts_with(s, len, "SOURCETABLE 200 OK")) {
        return fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_SOURCETABLE, 5);
    }
    if (starts_with(s, len, "ERROR - Bad Password")) {
        return fail(NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, NTRIP_HINT_NONE, 6);
    }
    if (starts_with(s, len, "ERROR - Mount Point Taken or Invalid")) {
        ntrip_verdict_t v = fail(NTRIP_CLS_MOUNT_BUSY, NTRIP_ROW_MOUNT_BUSY, NTRIP_HINT_NONE, 7);
        v.mount_ambiguous = true;
        return v;
    }
    if (starts_with(s, len, "ERROR - Already Connected")) {
        return fail(NTRIP_CLS_MOUNT_BUSY, NTRIP_ROW_MOUNT_BUSY, NTRIP_HINT_NONE, 8);
    }
    if (starts_with(s, len, "ERROR - Bad Mountpoint") || starts_with(s, len, "ERROR - Missing Mountpoint")) {
        return fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_NONE, 9);
    }
    if (starts_with(s, len, "ERROR - Too many sources")) {
        return fail(NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, NTRIP_HINT_CASTER_BUSY, 10);
    }
    if (starts_with(s, len, "ERROR")) {
        return fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_UNEXPECTED, 11);
    }
    if (len > 6 && starts_with(s, len, "Error ") && is_digit(s[6])) {
        return fail(NTRIP_CLS_REMOTE_CLOSE, NTRIP_ROW_TRANSIENT, NTRIP_HINT_CASTER_BUSY, 12);
    }
    if (starts_with(s, len, "Not authorized (no NTRIP source)")) {
        return fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_AGENT, 13);
    }
    if (len > 0 && s[0] == '<') {
        return fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_UNEXPECTED, 14);
    }
    if (ntrip_contains_ci(s, len, "banned")) {
        return fail(NTRIP_CLS_BANNED, NTRIP_ROW_BANNED, NTRIP_HINT_BANNED, 15);
    }
    if (ntrip_contains_ci(s, len, "sandbox")) {
        return fail(NTRIP_CLS_BANNED, NTRIP_ROW_BANNED, NTRIP_HINT_BANNED, 16);
    }
    return fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_UNEXPECTED, 18);
}

void ntrip_reply_init(ntrip_reply_t *r, bool v2_request)
{
    memset(r, 0, sizeof(*r));
    r->phase = NTRIP_RP_LINE;
    r->v2_request = v2_request;
}

uint32_t ntrip_reply_phase_ms(const ntrip_reply_t *r)
{
    switch (r->phase) {
    case NTRIP_RP_LINE:
        return NTRIP_REPLY_LINE_MS;
    case NTRIP_RP_HEADERS:
        return NTRIP_REPLY_HEADERS_MS;
    case NTRIP_RP_BODY:
        return NTRIP_REPLY_BODY_MS;
    default:
        return 0;
    }
}

static bool is4xx(int code)
{
    return code >= 400 && code <= 499;
}

static void finalize(ntrip_reply_t *r)
{
    ntrip_verdict_t *v = &r->verdict;
    if (r->body_check && r->banned) {
        v->success = false;
        v->cls = NTRIP_CLS_BANNED;
        v->row = NTRIP_ROW_BANNED;
        v->hint = NTRIP_HINT_BANNED;
        v->mount_ambiguous = false;
    }
    if (r->v2_request && !v->success && v->rule != 0 && v->rule != 17 && !r->ntrip_v2_header &&
        (v->hint == NTRIP_HINT_NONE || v->hint == NTRIP_HINT_UNEXPECTED)) {
        v->hint = NTRIP_HINT_V1_ONLY;
    }
    r->phase = NTRIP_RP_DONE;
}

static void start_body(ntrip_reply_t *r)
{
    r->phase = NTRIP_RP_BODY;
    r->body_check = true;
    r->banned = ntrip_contains_ci(r->line, r->line_len, "banned");
}

static void on_line(ntrip_reply_t *r)
{
    r->verdict = ntrip_classify_line(r->line, r->line_len);
    switch (r->verdict.rule) {
    case 3:
    case 4:
        r->phase = NTRIP_RP_HEADERS;
        break;
    case 14:
        start_body(r);
        break;
    default:
        finalize(r);
        break;
    }
}

static void after_headers(ntrip_reply_t *r)
{
    if (r->verdict.rule == 4 && is4xx(r->verdict.status_code)) {
        start_body(r);
    } else {
        finalize(r);
    }
}

static void on_header(ntrip_reply_t *r)
{
    const char *h = r->hdr;
    size_t len = r->hdr_len;
    size_t name_len = 0;
    while (name_len < len && h[name_len] != ':') {
        name_len++;
    }
    if (name_len == len) {
        return;
    }
    size_t v = name_len + 1;
    while (v < len && (h[v] == ' ' || h[v] == '\t')) {
        v++;
    }
    size_t vlen = len - v;
    while (vlen > 0 && (h[v + vlen - 1] == ' ' || h[v + vlen - 1] == '\t')) {
        vlen--;
    }
    if (name_len == 6 && ntrip_contains_ci(h, 6, "server")) {
        if (vlen > sizeof(r->server) - 1) {
            vlen = sizeof(r->server) - 1;
        }
        memcpy(r->server, h + v, vlen);
        r->server[vlen] = '\0';
    } else if (name_len == 13 && ntrip_contains_ci(h, 13, "ntrip-version")) {
        r->ntrip_v2_header = ntrip_contains_ci(h + v, vlen, "ntrip/2.0");
    }
}

static void body_byte(ntrip_reply_t *r, char c)
{
    static const char needle[] = "banned";
    const size_t n = sizeof(needle) - 1;
    if (r->tail_len == n) {
        memmove(r->tail, r->tail + 1, n - 1);
        r->tail_len--;
    }
    r->tail[r->tail_len++] = lower(c);
    if (r->tail_len == n && memcmp(r->tail, needle, n) == 0) {
        r->banned = true;
    }
}

bool ntrip_reply_feed(ntrip_reply_t *r, const uint8_t *data, size_t len, size_t *used)
{
    size_t i = 0;
    while (i < len && r->phase != NTRIP_RP_DONE) {
        char c = (char)data[i++];
        switch (r->phase) {
        case NTRIP_RP_LINE:
            if (c == '\n') {
                if (r->line_len > 0 && r->line[r->line_len - 1] == '\r') {
                    r->line_len--;
                }
                r->line[r->line_len] = '\0';
                on_line(r);
            } else if (r->line_len >= NTRIP_REPLY_LINE_MAX) {
                r->line[r->line_len] = '\0';
                r->verdict = fail(NTRIP_CLS_CONFIG, NTRIP_ROW_CONFIG, NTRIP_HINT_UNEXPECTED, 0);
                finalize(r);
            } else {
                r->line[r->line_len++] = c;
            }
            break;
        case NTRIP_RP_HEADERS:
            r->hdr_total++;
            if (c == '\n') {
                if (r->hdr_len > 0 && r->hdr[r->hdr_len - 1] == '\r') {
                    r->hdr_len--;
                }
                if (r->hdr_len == 0) {
                    after_headers(r);
                    break;
                }
                r->hdr[r->hdr_len] = '\0';
                on_header(r);
                r->hdr_len = 0;
            } else if (r->hdr_len < NTRIP_REPLY_HDR_KEEP) {
                r->hdr[r->hdr_len++] = c;
            }
            if (r->phase == NTRIP_RP_HEADERS && r->hdr_total >= NTRIP_REPLY_HEADERS_MAX) {
                after_headers(r);
            }
            break;
        case NTRIP_RP_BODY:
            body_byte(r, c);
            if (++r->body_seen >= NTRIP_REPLY_BODY_MAX) {
                finalize(r);
            }
            break;
        default:
            break;
        }
    }
    *used = i;
    return r->phase == NTRIP_RP_DONE;
}

void ntrip_reply_end(ntrip_reply_t *r, bool eof)
{
    switch (r->phase) {
    case NTRIP_RP_LINE:
        r->line[r->line_len] = '\0';
        r->verdict = fail(eof ? NTRIP_CLS_REMOTE_CLOSE : NTRIP_CLS_TIMEOUT, NTRIP_ROW_TRANSIENT,
                          NTRIP_HINT_NO_REPLY, 17);
        finalize(r);
        break;
    case NTRIP_RP_HEADERS:
        if (eof) {
            r->body_check = is4xx(r->verdict.status_code) && r->verdict.rule == 4;
            if (r->body_check) {
                r->banned = ntrip_contains_ci(r->line, r->line_len, "banned");
            }
        }
        finalize(r);
        break;
    case NTRIP_RP_BODY:
        finalize(r);
        break;
    default:
        break;
    }
}

void ntrip_sanitize(const char *in, size_t len, char *out, size_t cap, const char *const *secrets, size_t nsecrets)
{
    if (cap == 0) {
        return;
    }
    size_t o = 0;
    size_t i = 0;
    while (i < len && o + 1 < cap) {
        size_t hit = 0;
        for (size_t k = 0; k < nsecrets && hit == 0; k++) {
            const char *sec = secrets[k];
            size_t sl = sec ? strlen(sec) : 0;
            if (sl < 3) {
                continue;
            }
            size_t avail = len - i;
            size_t cmp = avail < sl ? avail : sl;
            /* A full match, or the input ends inside the secret. */
            if (cmp >= 3 && memcmp(in + i, sec, cmp) == 0 && (cmp == sl || cmp == avail)) {
                hit = cmp;
            }
        }
        if (hit > 0) {
            for (size_t k = 0; k < hit && o + 1 < cap; k++) {
                out[o++] = '*';
            }
            i += hit;
            continue;
        }
        unsigned char c = (unsigned char)in[i++];
        out[o++] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '.';
    }
    out[o] = '\0';
}

/* ---- backoff ------------------------------------------------------------------------ */

typedef struct {
    uint32_t floor_s;
    uint32_t cap_s;
} transient_row_t;

static const transient_row_t k_transient[3] = {
    { 1, 15 },    /* fast */
    { 2, 60 },    /* standard */
    { 10, 300 },  /* polite */
};

static const uint32_t k_busy[3][4] = {
    { 5, 10, 15, 30 },
    { 10, 20, 30, 60 },
    { 15, 30, 60, 120 },
};

/* AUTH and CONFIG share a row. */
static const uint32_t k_auth[3][5] = {
    { 15, 30, 60, 120, 300 },
    { 60, 300, 900, 1800, 1800 },
    { 300, 900, 1800, 3600, 3600 },
};

static const uint32_t k_banned[4] = { 3u * 3600u, 6u * 3600u, 12u * 3600u, 24u * 3600u };

static size_t prof_index(ntrip_prof_t p)
{
    return (p == NTRIP_PROF_STANDARD) ? 1u : (p == NTRIP_PROF_POLITE) ? 2u : 0u;
}

static uint64_t row_entry_s(ntrip_prof_t prof, ntrip_row_t row, uint32_t n)
{
    size_t p = prof_index(prof);
    switch (row) {
    case NTRIP_ROW_MOUNT_BUSY:
        return k_busy[p][n < 4 ? n : 3];
    case NTRIP_ROW_BANNED:
        if (prof != NTRIP_PROF_FAST) {
            return k_banned[n < 4 ? n : 3];
        }
        /* fast: treated as CONFIG */
        return k_auth[p][n < 5 ? n : 4];
    case NTRIP_ROW_AUTH:
    case NTRIP_ROW_CONFIG:
        return k_auth[p][n < 5 ? n : 4];
    default:
        return 0;
    }
}

uint32_t ntrip_backoff_row_min_ms(ntrip_prof_t prof, ntrip_row_t row)
{
    if (row == NTRIP_ROW_NONE) {
        return 0;
    }
    if (row == NTRIP_ROW_TRANSIENT) {
        return k_transient[prof_index(prof)].floor_s * 1000u;
    }
    return (uint32_t)row_entry_s(prof, row, 0) * 1000u;
}

uint32_t ntrip_backoff_delay_ms(ntrip_prof_t prof, ntrip_row_t row, uint32_t n, bool fast_late,
                                uint32_t late_cap_ms, uint32_t rnd)
{
    uint64_t d;
    if (row == NTRIP_ROW_NONE) {
        return 0;
    }
    if (row == NTRIP_ROW_TRANSIENT) {
        const transient_row_t *t = &k_transient[prof_index(prof)];
        uint64_t cap = (uint64_t)t->cap_s * 1000u;
        if (prof == NTRIP_PROF_FAST && fast_late) {
            cap = late_cap_ms;
        }
        d = (uint64_t)t->floor_s * 1000u;
        for (uint32_t k = 0; k < n && d < cap; k++) {
            d *= 2u;
        }
        if (d > cap) {
            d = cap;
        }
    } else {
        d = row_entry_s(prof, row, n) * 1000u;
    }
    d = d * (800u + (rnd % 401u)) / 1000u;   /* x U(0.8, 1.2) */
    uint64_t lo = ntrip_backoff_row_min_ms(prof, row);
    if (d < lo) {
        d = lo;
    }
    return (uint32_t)d;
}

/* ---- state machine --------------------------------------------------------------------- */

static void set_state(ntrip_sm_t *sm, ntrip_sm_state_t st, uint32_t now)
{
    if (sm->state != st) {
        sm->state = st;
        sm->state_since_ms = now;
    }
}

static ntrip_row_t long_row(ntrip_row_t row)
{
    return (row == NTRIP_ROW_AUTH || row == NTRIP_ROW_CONFIG || row == NTRIP_ROW_BANNED) ? row : NTRIP_ROW_NONE;
}

/* Entries of a long row (its last one repeats). */
static uint32_t row_steps(ntrip_prof_t prof, ntrip_row_t row)
{
    if (row == NTRIP_ROW_BANNED && prof != NTRIP_PROF_FAST) {
        return sizeof(k_banned) / sizeof(k_banned[0]);
    }
    return sizeof(k_auth[0]) / sizeof(k_auth[0][0]);
}

/* Where in its long row a slot stands: changes until the last entry repeats. */
static uint32_t long_step(ntrip_prof_t prof, ntrip_row_t row, uint32_t attempt)
{
    if (long_row(row) == NTRIP_ROW_NONE) {
        return 0;
    }
    uint32_t steps = row_steps(prof, row);
    return attempt < steps ? attempt : steps;
}

/* Polite profile only: caster-reaching failures toward the hourly limit. */
static uint32_t reached_key(const ntrip_sm_t *sm)
{
    if (sm->cfg.profile != NTRIP_PROF_POLITE) {
        return 0;
    }
    return sm->reached_streak < NTRIP_POLITE_HOURLY_AFTER ? sm->reached_streak : NTRIP_POLITE_HOURLY_AFTER;
}

static ntrip_sm_state_t failure_state(ntrip_cls_t cls, ntrip_row_t row)
{
    if (cls == NTRIP_CLS_BANNED || row == NTRIP_ROW_BANNED) {
        return NTRIP_SM_BANNED;
    }
    if (cls == NTRIP_CLS_CONFIG || row == NTRIP_ROW_CONFIG) {
        return NTRIP_SM_CONFIG_ERROR;
    }
    if (cls == NTRIP_CLS_AUTH && row == NTRIP_ROW_AUTH) {
        return NTRIP_SM_AUTH_FAILED;
    }
    return NTRIP_SM_BACKOFF;
}

static void reset_streaks(ntrip_sm_t *sm)
{
    sm->attempt = 0;
    sm->reached_streak = 0;
    sm->hourly = false;
    sm->streak_active = false;
    sm->busy_active = false;
}

static void check_healthy(ntrip_sm_t *sm, uint32_t now)
{
    if (sm->session && !sm->session_healthy && (uint32_t)(now - sm->session_start_ms) >= NTRIP_HEALTHY_SESSION_MS &&
        sm->session_bytes >= NTRIP_HEALTHY_SESSION_B) {
        sm->session_healthy = true;
        reset_streaks(sm);
    }
}

static void end_session(ntrip_sm_t *sm, uint32_t now)
{
    if (sm->session) {
        check_healthy(sm, now);
        sm->session = false;
        sm->window_start_ms = now;
    }
}

void ntrip_sm_init(ntrip_sm_t *sm, const ntrip_sm_cfg_t *cfg, uint32_t cred_hash, uint32_t now_ms)
{
    memset(sm, 0, sizeof(*sm));
    sm->cfg = *cfg;
    sm->state = NTRIP_SM_DISABLED;
    sm->state_since_ms = now_ms;
    sm->window_start_ms = now_ms;   /* boot counts as a disconnect */
    sm->cred_hash = cred_hash;
}

ntrip_sm_act_t ntrip_sm_poll(ntrip_sm_t *sm, const ntrip_sm_in_t *in, uint32_t now_ms, uint32_t rnd,
                             ntrip_close_why_t *why)
{
    *why = NTRIP_WHY_NONE;
    if (in->link_up && !sm->prev_link_up) {
        ntrip_sm_stagger(sm, now_ms, rnd);
    }
    sm->prev_link_up = in->link_up;

    if (!in->enabled) {
        set_state(sm, NTRIP_SM_DISABLED, now_ms);
        *why = NTRIP_WHY_DISABLED;
        return in->session_open ? NTRIP_ACT_CLOSE : NTRIP_ACT_NONE;
    }
    if (!in->have_ip) {
        set_state(sm, NTRIP_SM_WAIT_NETWORK, now_ms);
        *why = NTRIP_WHY_NO_IP;
        return in->session_open ? NTRIP_ACT_CLOSE : NTRIP_ACT_NONE;
    }
    if (in->paused) {
        set_state(sm, NTRIP_SM_PAUSED, now_ms);
        if (in->session_open && in->silent_ms >= sm->cfg.stall_limit_ms) {
            *why = NTRIP_WHY_PAUSE_IDLE;
            return NTRIP_ACT_CLOSE;
        }
        return NTRIP_ACT_NONE;
    }
    if (in->session_open) {
        if (in->silent_ms >= sm->cfg.stall_limit_ms) {
            *why = NTRIP_WHY_SOURCE_STALL;
            return NTRIP_ACT_CLOSE;
        }
        set_state(sm, NTRIP_SM_STREAMING, now_ms);
        return NTRIP_ACT_NONE;
    }
    if (!in->link_up) {
        set_state(sm, NTRIP_SM_WAIT_NETWORK, now_ms);
        return NTRIP_ACT_NONE;
    }
    if (sm->retry_pending) {
        if (!due(now_ms, sm->retry_at_ms)) {
            set_state(sm, failure_state(sm->reason, sm->row), now_ms);
            return NTRIP_ACT_NONE;
        }
        sm->retry_pending = false;
    }
    if (sm->stagger_pending) {
        if (!due(now_ms, sm->stagger_at_ms)) {
            set_state(sm, NTRIP_SM_WAIT_DATA, now_ms);
            return NTRIP_ACT_NONE;
        }
        sm->stagger_pending = false;
    }
    if (!in->fresh_data) {
        set_state(sm, NTRIP_SM_WAIT_DATA, now_ms);
        return NTRIP_ACT_NONE;
    }
    return NTRIP_ACT_CONNECT;
}

void ntrip_sm_connecting(ntrip_sm_t *sm, uint32_t now_ms)
{
    set_state(sm, NTRIP_SM_CONNECTING, now_ms);
}

void ntrip_sm_connected(ntrip_sm_t *sm, uint32_t now_ms)
{
    set_state(sm, NTRIP_SM_STREAMING, now_ms);
    sm->session = true;
    sm->session_start_ms = now_ms;
    sm->session_bytes = 0;
    sm->session_healthy = false;
    sm->row = NTRIP_ROW_NONE;
    sm->hint = NTRIP_HINT_NONE;
    sm->retry_pending = false;
    sm->busy_active = false;
    /* The caster accepted us: the hourly limit ("until a success") and the
     * run of consecutive caster-reaching failures end. */
    sm->hourly = false;
    sm->reached_streak = 0;
    if (!sm->good_valid || sm->good_hash != sm->cred_hash) {
        sm->good_valid = true;
        sm->good_hash = sm->cred_hash;
        sm->saved_dirty = true;
    }
}

void ntrip_sm_sent(ntrip_sm_t *sm, size_t bytes, uint32_t now_ms)
{
    sm->session_bytes += bytes;
    check_healthy(sm, now_ms);
}

uint32_t ntrip_sm_failed(ntrip_sm_t *sm, const ntrip_failure_t *f, uint32_t now_ms, uint32_t rnd)
{
    end_session(sm, now_ms);

    ntrip_row_t row = f->row == NTRIP_ROW_NONE ? NTRIP_ROW_TRANSIENT : f->row;
    ntrip_cls_t cls = f->cls;
    bool cred_ok = sm->good_valid && sm->good_hash == sm->cred_hash;
    if (row == NTRIP_ROW_AUTH && cred_ok && (uint32_t)(now_ms - sm->window_start_ms) < sm->cfg.auth_window_ms) {
        row = NTRIP_ROW_MOUNT_BUSY;   /* the caster may still hold our previous session */
    }

    sm->hint = NTRIP_HINT_NONE;
    if (row == NTRIP_ROW_MOUNT_BUSY) {
        if (!sm->busy_active) {
            sm->busy_active = true;
            sm->busy_start_ms = now_ms;
        }
        uint32_t busy_for = now_ms - sm->busy_start_ms;
        if (busy_for >= NTRIP_BUSY_HINT_MS) {
            sm->hint = NTRIP_HINT_MOUNT_HELD;
        }
        if (busy_for >= NTRIP_BUSY_ESCALATE_MS) {
            if (f->mount_ambiguous) {
                row = NTRIP_ROW_CONFIG;
                cls = NTRIP_CLS_CONFIG;
            } else {
                row = NTRIP_ROW_AUTH;
            }
        }
    } else {
        sm->busy_active = false;
    }

    if (!sm->streak_active) {
        sm->streak_active = true;
        sm->streak_start_ms = now_ms;
    }
    bool late = sm->cfg.profile == NTRIP_PROF_FAST && (uint32_t)(now_ms - sm->streak_start_ms) >= NTRIP_FAST_LATE_MS;
    if (f->reached_caster && sm->reached_streak < UINT32_MAX) {
        sm->reached_streak++;
    }
    if (sm->cfg.profile == NTRIP_PROF_POLITE && sm->reached_streak >= NTRIP_POLITE_HOURLY_AFTER) {
        sm->hourly = true;
    }

    uint32_t d = ntrip_backoff_delay_ms(sm->cfg.profile, row, sm->attempt, late, sm->cfg.fast_late_cap_ms, rnd);
    if (sm->hourly && d < NTRIP_HOURLY_MS) {
        d = NTRIP_HOURLY_MS;
    }
    if (sm->attempt < UINT32_MAX) {
        sm->attempt++;
    }
    sm->retry_pending = true;
    sm->retry_at_ms = now_ms + d;
    sm->row = row;
    sm->reason = cls;
    set_state(sm, failure_state(cls, row), now_ms);
    return d;
}

void ntrip_sm_closed(ntrip_sm_t *sm, uint32_t now_ms)
{
    end_session(sm, now_ms);
}

void ntrip_sm_stagger(ntrip_sm_t *sm, uint32_t now_ms, uint32_t rnd)
{
    if (!sm->cfg.protected_slot) {
        sm->stagger_pending = true;
        sm->stagger_at_ms = now_ms + rnd % (NTRIP_STAGGER_MAX_MS + 1u);
    }
}

void ntrip_sm_user_edit(ntrip_sm_t *sm, const ntrip_sm_cfg_t *cfg, uint32_t cred_hash, uint32_t now_ms)
{
    (void)now_ms;
    sm->cfg = *cfg;
    sm->cred_hash = cred_hash;
    reset_streaks(sm);
    sm->retry_pending = false;
    sm->stagger_pending = false;
    sm->row = NTRIP_ROW_NONE;
    sm->reason = NTRIP_CLS_NONE;
    sm->hint = NTRIP_HINT_NONE;
}

uint32_t ntrip_sm_retry_in_ms(const ntrip_sm_t *sm, uint32_t now_ms)
{
    uint32_t left = 0;
    if (sm->retry_pending && !due(now_ms, sm->retry_at_ms)) {
        left = sm->retry_at_ms - now_ms;
    }
    if (sm->stagger_pending && !due(now_ms, sm->stagger_at_ms)) {
        uint32_t s = sm->stagger_at_ms - now_ms;
        left = s > left ? s : left;
    }
    return left;
}

bool ntrip_sm_persist_due(const ntrip_sm_t *sm)
{
    ntrip_row_t row = long_row(sm->row);
    return row != sm->saved_row || long_step(sm->cfg.profile, row, sm->attempt) != sm->saved_step ||
           sm->hourly != sm->saved_hourly || reached_key(sm) != sm->saved_reached || sm->saved_dirty;
}

void ntrip_sm_persist_get(const ntrip_sm_t *sm, uint32_t now_ms, ntrip_backoff_rec_t *out)
{
    memset(out, 0, sizeof(*out));
    out->row = long_row(sm->row);
    out->cls = out->row != NTRIP_ROW_NONE ? sm->reason : NTRIP_CLS_NONE;
    out->attempt = sm->attempt;
    out->hourly = sm->hourly;
    if (out->row != NTRIP_ROW_NONE || out->hourly) {
        uint32_t left = sm->retry_pending ? ntrip_sm_retry_in_ms(sm, now_ms) : 0;
        out->remaining_s = (left + 999u) / 1000u;
    } else {
        out->attempt = 0;
    }
    out->good_valid = sm->good_valid;
    out->good_hash = sm->good_hash;
    out->reached = (uint8_t)reached_key(sm);
}

void ntrip_sm_persist_done(ntrip_sm_t *sm)
{
    sm->saved_row = long_row(sm->row);
    sm->saved_step = long_step(sm->cfg.profile, sm->saved_row, sm->attempt);
    sm->saved_reached = reached_key(sm);
    sm->saved_hourly = sm->hourly;
    sm->saved_dirty = false;
}

void ntrip_sm_restore(ntrip_sm_t *sm, const ntrip_backoff_rec_t *rec, uint32_t now_ms)
{
    ntrip_row_t row = long_row(rec->row);
    sm->good_valid = rec->good_valid;
    sm->good_hash = rec->good_hash;
    sm->saved_row = row;
    sm->saved_step = long_step(sm->cfg.profile, row, rec->attempt);
    sm->saved_hourly = rec->hourly;
    sm->saved_dirty = false;
    if (sm->cfg.profile == NTRIP_PROF_POLITE) {
        uint32_t reached = rec->reached < NTRIP_POLITE_HOURLY_AFTER ? rec->reached : NTRIP_POLITE_HOURLY_AFTER;
        sm->reached_streak = rec->hourly ? NTRIP_POLITE_HOURLY_AFTER : reached;
    }
    sm->saved_reached = reached_key(sm);
    if (row == NTRIP_ROW_NONE && !rec->hourly) {
        return;
    }
    sm->row = row;
    sm->reason = rec->cls;
    sm->attempt = rec->attempt;
    sm->hourly = rec->hourly;
    if (sm->attempt > 0) {
        sm->streak_active = true;
        sm->streak_start_ms = now_ms;
    }
    /* The remaining delay was taken when the record was written; a reboot
     * loop must not shorten the step, so wait at least its full delay. */
    uint64_t wait_ms = (uint64_t)rec->remaining_s * 1000u;
    if (row != NTRIP_ROW_NONE) {
        uint64_t nominal = row_entry_s(sm->cfg.profile, row, sm->attempt > 0 ? sm->attempt - 1u : 0u) * 1000u;
        wait_ms = nominal > wait_ms ? nominal : wait_ms;
    }
    if (rec->hourly && wait_ms < NTRIP_HOURLY_MS) {
        wait_ms = NTRIP_HOURLY_MS;
    }
    const uint64_t max_ms = 24ull * 3600u * 1200u;   /* longest delay the rows produce (24 h x 1.2) */
    if (wait_ms > max_ms) {
        wait_ms = max_ms;
    }
    if (wait_ms > 0) {
        sm->retry_pending = true;
        sm->retry_at_ms = now_ms + (uint32_t)wait_ms;
    }
}

size_t ntrip_backoff_rec_encode(const ntrip_backoff_rec_t *rec, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, NTRIP_BACKOFF_REC_VERSION);
    blob_put_u8(&w, (uint8_t)rec->cls);
    blob_put_u8(&w, (uint8_t)rec->row);
    blob_put_u32(&w, rec->attempt);
    blob_put_u32(&w, rec->remaining_s);
    blob_put_bool(&w, rec->hourly);
    blob_put_bool(&w, rec->good_valid);
    blob_put_u32(&w, rec->good_hash);
    blob_put_u8(&w, rec->reached);
    return blob_writer_ok(&w) ? w.len : 0;
}

bool ntrip_backoff_rec_decode(const uint8_t *buf, size_t len, ntrip_backoff_rec_t *out)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    memset(out, 0, sizeof(*out));
    uint8_t version = blob_get_u8(&r);
    if (version != 1u && version != NTRIP_BACKOFF_REC_VERSION) {
        return false;
    }
    ntrip_backoff_rec_t rec;
    memset(&rec, 0, sizeof(rec));
    uint8_t cls = blob_get_u8(&r);
    uint8_t row = blob_get_u8(&r);
    rec.attempt = blob_get_u32(&r);
    rec.remaining_s = blob_get_u32(&r);
    rec.hourly = blob_get_bool(&r);
    rec.good_valid = blob_get_bool(&r);
    rec.good_hash = blob_get_u32(&r);
    if (version >= 2u) {
        rec.reached = blob_get_u8(&r);
    }
    if (!blob_reader_ok(&r) || cls >= NTRIP_CLS_COUNT || long_row((ntrip_row_t)row) != (ntrip_row_t)row) {
        return false;
    }
    rec.cls = (ntrip_cls_t)cls;
    rec.row = (ntrip_row_t)row;
    *out = rec;
    return true;
}

static uint32_t fnv_str(uint32_t h, const char *s)
{
    for (; s != NULL && *s != '\0'; s++) {
        h ^= (uint8_t)*s;
        h *= 16777619u;
    }
    h ^= 0u;   /* field separator */
    h *= 16777619u;
    return h;
}

uint32_t ntrip_cred_hash(const char *host, uint16_t port, bool v2, const char *mountpoint,
                         const char *username, const char *password, const char *label)
{
    uint32_t h = 2166136261u;
    char lowhost[64];
    size_t i = 0;
    for (; host != NULL && host[i] != '\0' && i + 1 < sizeof(lowhost); i++) {
        lowhost[i] = lower(host[i]);
    }
    lowhost[i] = '\0';
    h = fnv_str(h, lowhost);
    h ^= (uint8_t)(port & 0xFFu);
    h *= 16777619u;
    h ^= (uint8_t)(port >> 8);
    h *= 16777619u;
    h ^= v2 ? 2u : 1u;
    h *= 16777619u;
    h = fnv_str(h, mountpoint);
    h = fnv_str(h, username);
    h = fnv_str(h, password);
    h = fnv_str(h, label);
    return h;
}

/* ---- Onocoy budget ------------------------------------------------------------------------- */

uint32_t ntrip_offline_budget_s(uint32_t data_rate_permille)
{
    if (data_rate_permille > 1000u) {
        data_rate_permille = 1000u;
    }
    if (data_rate_permille < NTRIP_TARGET_PERMILLE) {
        return 0;
    }
    double rate = (double)data_rate_permille / 1000.0;
    double target = (double)NTRIP_TARGET_PERMILLE / 1000.0;
    double u = 0.8 + 0.2 * sqrt(target / rate);           /* lowest graced uptime that still qualifies */
    double offline = (double)NTRIP_DAY_S + (double)NTRIP_GRACE_S - (double)NTRIP_DAY_S * u;
    if (offline <= 0.0) {
        return 0;
    }
    return (uint32_t)floor(offline + 1e-9);
}
