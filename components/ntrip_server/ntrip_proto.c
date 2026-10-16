/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "ntrip_proto.h"

#include <errno.h>
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
    bool onocoy = f->onocoy || ntrip_host_is_onocoy(host);
    if (has_ctrl(user) || strchr(user, ':') != NULL || (onocoy && !mount_ok(user))) {
        return NTRIP_FIELDS_BAD_USERNAME;   /* Onocoy over 1.0 sends the username as the mountpoint */
    }
    if (has_ctrl(pass) || ((!f->v2 || onocoy) && !v1_password_ok(pass))) {
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
        if ((!f->v2 || onocoy) && empty(pass)) {
            return NTRIP_FIELDS_MISSING;
        }
        if (f->v2 && empty(user) && !onocoy) {
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

static bool slot_onocoy(const ntrip_policy_slot_t *s)
{
    return s->onocoy || ntrip_host_is_onocoy(s->host);
}

ntrip_policy_t ntrip_check_policy(const ntrip_policy_slot_t *slots, size_t count, size_t edited)
{
    if (edited >= count || !slots[edited].enabled) {
        return NTRIP_POLICY_OK;
    }
    const char *mine = slots[edited].host;
    bool mine_onocoy = slot_onocoy(&slots[edited]);
    bool other_onocoy = false;
    bool other_rtk2go = false;
    bool other_open = false;
    for (size_t i = 0; i < count; i++) {
        if (i == edited || !slots[i].enabled) {
            continue;
        }
        other_onocoy |= slot_onocoy(&slots[i]);
        other_rtk2go |= ntrip_host_is_rtk2go(slots[i].host);
        other_open |= ntrip_host_is_open_caster(slots[i].host);
    }
    if (mine_onocoy && other_onocoy) {
        return NTRIP_POLICY_SECOND_ONOCOY;
    }
    if (ntrip_host_is_rtk2go(mine) && other_rtk2go) {
        return NTRIP_POLICY_SECOND_RTK2GO;
    }
    bool pair = (mine_onocoy && other_open) || (ntrip_host_is_open_caster(mine) && other_onocoy);
    if (pair && !slots[edited].dup_ack) {
        return NTRIP_POLICY_NEED_DUP_ACK;
    }
    return NTRIP_POLICY_OK;
}

/* ---- RTK2go mountpoint rules ------------------------------------------------------- */

ntrip_rtk2go_mount_t ntrip_rtk2go_mount_check(const char *mount)
{
    const char *m = mount ? mount : "";
    size_t len = strlen(m);
    for (size_t i = 0; i < len; i++) {
        if (!is_alnum(m[i]) && m[i] != '_' && m[i] != '-') {
            return NTRIP_RTK2GO_BAD_CHARS;   /* '.' included: the form refuses it */
        }
    }
    if (len == 0) {
        return NTRIP_RTK2GO_TOO_SHORT;
    }
    if (!((m[0] >= 'A' && m[0] <= 'Z') || (m[0] >= 'a' && m[0] <= 'z'))) {
        return NTRIP_RTK2GO_FIRST_NOT_LETTER;
    }
    if (m[0] == 'A') {
        return NTRIP_RTK2GO_FIRST_A;
    }
    if (len < 4) {
        return NTRIP_RTK2GO_TOO_SHORT;
    }
    if (len > 20) {
        return NTRIP_RTK2GO_TOO_LONG;
    }
    return NTRIP_RTK2GO_OK;
}

static bool same_ci(const char *a, const char *b)
{
    a = a ? a : "";
    b = b ? b : "";
    while (*a != '\0' && *b != '\0' && lower(*a) == lower(*b)) {
        a++;
        b++;
    }
    return *a == *b;
}

bool ntrip_rtk2go_rule_applies(const char *old_host, const char *old_mount, const char *new_host,
                               const char *new_mount)
{
    if (!ntrip_host_is_rtk2go(new_host)) {
        return false;
    }
    if (empty(old_host) || !same_ci(old_host, new_host)) {
        return true;
    }
    return strcmp(old_mount ? old_mount : "", new_mount ? new_mount : "") != 0;
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

void ntrip_wire_names(bool onocoy, bool v2, const char *mountpoint, const char *username, const char *label,
                      ntrip_wire_names_t *out)
{
    out->mount = mountpoint;
    out->user = username;
    if (!onocoy) {
        return;
    }
    if (!empty(username)) {
        /* What the Onocoy console gives: 2.0 posts to the mountpoint as the
         * username; 1.0-only sources send the username as the mountpoint. */
        if (!v2) {
            out->mount = username;
            out->user = "";
        }
        return;
    }
    /* The older layout (ntrip_cfg_blob.h): the credential username lives in `mountpoint`. */
    if (v2) {
        out->mount = empty(label) ? "onocoydiy" : label;
        out->user = mountpoint;
    } else {
        out->user = "";
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

void ntrip_epoch_track_init(ntrip_epoch_track_t *t)
{
    memset(t, 0, sizeof(*t));
}

void ntrip_count_frames(const uint8_t *buf, size_t len, ntrip_epoch_track_t *track, uint32_t *frames,
                        uint32_t *epochs_ended)
{
    /* GPS, Galileo, QZSS and NavIC epoch times are all GPS time of week. */
    static const uint8_t k_gps_scale = (1u << 0) | (1u << 2) | (1u << 4) | (1u << 6);
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
            if (is_msm(type)) {
                /* Epoch time: payload bits 24..53; multiple-message bit: bit 54. */
                const unsigned slot = (unsigned)(type / 10u - 107u);
                const uint32_t time = ((uint32_t)p[3] << 22) | ((uint32_t)p[4] << 14) | ((uint32_t)p[5] << 6) |
                                      ((uint32_t)p[6] >> 2);
                const uint8_t scale = ((1u << slot) & k_gps_scale) ? k_gps_scale : (uint8_t)(1u << slot);
                for (unsigned s = 0; s < 7; s++) {
                    if ((scale & track->open_mask & (1u << s)) && track->time[s] != time) {
                        ne++;                  /* its closing MSM never came */
                        track->open_mask = 0;
                        break;
                    }
                }
                if (((p[6] >> 1) & 1u) == 0u) {
                    ne++;
                    track->open_mask = 0;
                } else {
                    track->open_mask |= (uint8_t)(1u << slot);
                    track->time[slot] = time;
                }
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

/* ---- Onocoy reason texts ------------------------------------------------------------- */

/* The reasons Onocoy's console shows, lowercase, in priority order (the
 * first that matches wins). PROVISIONAL: the wire form is unknown. */
static const struct {
    const char *text;
    uint8_t reason;
} k_ono_phrases[] = {
    { "too many unsuccessful logins", NTRIP_ONO_LOGIN_BLOCKED },
    { "already in use", NTRIP_ONO_IN_USE },
    { "parallel connections", NTRIP_ONO_IN_USE },
    { "device reconnected", NTRIP_ONO_REPLACED },
    { "duplicate of another stream", NTRIP_ONO_DUPLICATE },
    { "credential is banned", NTRIP_ONO_BANNED },
    { "authentication failed", NTRIP_ONO_AUTH },   /* also "reauthentication failed" */
    { "invalid request in headers", NTRIP_ONO_AUTH },
};
#define ONO_PHRASES (sizeof(k_ono_phrases) / sizeof(k_ono_phrases[0]))

ntrip_onocoy_reason_t ntrip_onocoy_reason(const char *text, size_t len)
{
    for (size_t i = 0; text != NULL && i < ONO_PHRASES; i++) {
        if (ntrip_contains_ci(text, len, k_ono_phrases[i].text)) {
            return (ntrip_onocoy_reason_t)k_ono_phrases[i].reason;
        }
    }
    return NTRIP_ONO_NONE;
}

/* Position of a reason in the priority order (NONE last). */
static size_t ono_rank(ntrip_onocoy_reason_t r)
{
    for (size_t i = 0; i < ONO_PHRASES; i++) {
        if (k_ono_phrases[i].reason == (uint8_t)r) {
            return i;
        }
    }
    return ONO_PHRASES;
}

ntrip_onocoy_reason_t ntrip_onocoy_reason_merge(ntrip_onocoy_reason_t a, ntrip_onocoy_reason_t b)
{
    return ono_rank(b) < ono_rank(a) ? b : a;
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
 * are told apart. The Onocoy reason the line names is set beside the row
 * (ntrip_classify_line); only a guard slot acts on it.
 */
static ntrip_verdict_t classify_rows(const char *s, size_t len)
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

ntrip_verdict_t ntrip_classify_line(const char *s, size_t len)
{
    ntrip_verdict_t v = classify_rows(s, len);
    v.onocoy_reason = (uint8_t)ntrip_onocoy_reason(s, len);
    return v;
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

/* An HTTP reply whose body is checked: 4xx, or any non-success one on a guard slot. */
static bool body_wanted(const ntrip_reply_t *r)
{
    return r->verdict.rule == 4 && (is4xx(r->verdict.status_code) || r->body_always);
}

static void after_headers(ntrip_reply_t *r)
{
    if (body_wanted(r)) {
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
    } else if (name_len == 17 && ntrip_contains_ci(h, 17, "transfer-encoding")) {
        r->chunked = ntrip_contains_ci(h + v, vlen, "chunked");
    }
}

/* Chunked body decoder states (the body check reads the data bytes only). */
enum {
    CK_SIZE = 0,   /* hex digits of the size line */
    CK_EXT,        /* chunk extension up to CR */
    CK_SIZE_LF,
    CK_DATA,
    CK_DATA_CR,
    CK_DATA_LF,
    CK_RAW,        /* framing not understood: plain body bytes from here */
};

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    c = lower(c);
    return (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1;
}

static void body_byte(ntrip_reply_t *r, char c);

/* One body byte through the chunk decoder. @return true at the last chunk. */
static bool chunk_byte(ntrip_reply_t *r, char c)
{
    switch (r->ck_state) {
    case CK_SIZE: {
        int d = hex_val(c);
        if (d >= 0 && r->ck_digits < 7) {
            r->ck_left = r->ck_left * 16u + (uint32_t)d;
            r->ck_digits++;
            return false;
        }
        if (r->ck_digits > 0 && (c == ';' || c == '\r')) {
            r->ck_state = (c == ';') ? CK_EXT : CK_SIZE_LF;
            return false;
        }
        break;
    }
    case CK_EXT:
        if (c == '\r') {
            r->ck_state = CK_SIZE_LF;
        }
        return false;
    case CK_SIZE_LF:
        if (c != '\n') {
            break;
        }
        if (r->ck_left == 0) {
            return true;   /* last chunk: the body is over */
        }
        r->ck_state = CK_DATA;
        return false;
    case CK_DATA:
        body_byte(r, c);
        if (--r->ck_left == 0) {
            r->ck_state = CK_DATA_CR;
        }
        return false;
    case CK_DATA_CR:
        if (c != '\r') {
            break;
        }
        r->ck_state = CK_DATA_LF;
        return false;
    case CK_DATA_LF:
        if (c != '\n') {
            break;
        }
        r->ck_state = CK_SIZE;
        r->ck_left = 0;
        r->ck_digits = 0;
        return false;
    default:
        body_byte(r, c);
        return false;
    }
    r->ck_state = CK_RAW;   /* not chunk framing after all */
    body_byte(r, c);
    return false;
}

static bool tail_ends_with(const ntrip_reply_t *r, const char *needle)
{
    size_t n = strlen(needle);
    return r->tail_len >= n && memcmp(r->tail + (r->tail_len - n), needle, n) == 0;
}

/* One body data byte: "banned" and the Onocoy reasons, across reads and chunk boundaries. */
static void body_byte(ntrip_reply_t *r, char c)
{
    if (r->tail_len == sizeof(r->tail)) {
        memmove(r->tail, r->tail + 1, sizeof(r->tail) - 1);
        r->tail_len--;
    }
    r->tail[r->tail_len++] = lower(c);
    if (tail_ends_with(r, "banned")) {
        r->banned = true;
    }
    for (size_t i = 0; i < ONO_PHRASES; i++) {
        if (tail_ends_with(r, k_ono_phrases[i].text)) {
            r->verdict.onocoy_reason = (uint8_t)ntrip_onocoy_reason_merge(
                (ntrip_onocoy_reason_t)r->verdict.onocoy_reason, (ntrip_onocoy_reason_t)k_ono_phrases[i].reason);
        }
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
                r->verdict.onocoy_reason = (uint8_t)ntrip_onocoy_reason(r->line, r->line_len);
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
        case NTRIP_RP_BODY: {
            bool last = false;
            if (r->chunked) {
                last = chunk_byte(r, c);
            } else {
                body_byte(r, c);
            }
            if (last || ++r->body_seen >= NTRIP_REPLY_BODY_MAX) {
                finalize(r);
            }
            break;
        }
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
        /* A line the caster ended without a line end (or did not finish in
         * time) may still name Onocoy's reason: its wire form is unknown. */
        r->verdict.onocoy_reason = (uint8_t)ntrip_onocoy_reason(r->line, r->line_len);
        finalize(r);
        break;
    case NTRIP_RP_HEADERS:
        if (eof) {
            r->body_check = body_wanted(r);
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

/* ---- text during a session ------------------------------------------------------------- */

void ntrip_sess_text_start(ntrip_sess_text_t *t)
{
    t->len = 0;
    t->seen = 0;
}

static void sess_line(ntrip_sess_text_t *t, const char *const *secrets, size_t nsecrets)
{
    ntrip_onocoy_reason_t r = ntrip_onocoy_reason(t->line, t->len);
    if (r == NTRIP_ONO_NONE && ntrip_contains_ci(t->line, t->len, "banned")) {
        r = NTRIP_ONO_BANNED;
    }
    if (r != NTRIP_ONO_NONE) {
        t->seen |= (uint8_t)(1u << r);
    }
    ntrip_sanitize(t->line, t->len, t->last, sizeof(t->last), secrets, nsecrets);
    t->len = 0;
}

bool ntrip_sess_text_feed(ntrip_sess_text_t *t, const uint8_t *data, size_t len, const char *const *secrets,
                          size_t nsecrets)
{
    bool ended = false;
    for (size_t i = 0; i < len; i++) {
        char c = (char)data[i];
        if (c == '\r' || c == '\n') {
            if (t->len > 0) {
                sess_line(t, secrets, nsecrets);
                ended = true;
            }
            continue;
        }
        t->line[t->len++] = c;
        if (t->len == sizeof(t->line)) {
            sess_line(t, secrets, nsecrets);   /* a long line in pieces */
            ended = true;
        }
    }
    return ended;
}

bool ntrip_sess_text_end(ntrip_sess_text_t *t, const char *const *secrets, size_t nsecrets)
{
    if (t->len == 0) {
        return false;
    }
    sess_line(t, secrets, nsecrets);
    return true;
}

ntrip_onocoy_reason_t ntrip_sess_text_reason(const ntrip_sess_text_t *t)
{
    if (t->seen & (1u << NTRIP_ONO_DUPLICATE)) {
        return NTRIP_ONO_DUPLICATE;
    }
    if (t->seen & (1u << NTRIP_ONO_BANNED)) {
        return NTRIP_ONO_BANNED;
    }
    ntrip_onocoy_reason_t best = NTRIP_ONO_NONE;
    for (unsigned r = NTRIP_ONO_AUTH; r <= NTRIP_ONO_BANNED; r++) {
        if (t->seen & (1u << r)) {
            best = ntrip_onocoy_reason_merge(best, (ntrip_onocoy_reason_t)r);
        }
    }
    return best;
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
    if ((cls == NTRIP_CLS_AUTH || cls == NTRIP_CLS_LOGIN_BLOCKED) && row == NTRIP_ROW_AUTH) {
        return NTRIP_SM_AUTH_FAILED;
    }
    return NTRIP_SM_BACKOFF;
}

bool ntrip_failure_unreached(const ntrip_failure_t *f)
{
    return !f->reached_caster && !f->refused && (f->cls == NTRIP_CLS_DNS || f->cls == NTRIP_CLS_CONNECT);
}

ntrip_cls_t ntrip_guard_class(const ntrip_failure_t *f)
{
    switch ((ntrip_onocoy_reason_t)f->onocoy) {
    case NTRIP_ONO_LOGIN_BLOCKED:
        return NTRIP_CLS_LOGIN_BLOCKED;
    case NTRIP_ONO_IN_USE:
        return NTRIP_CLS_IN_USE;
    case NTRIP_ONO_REPLACED:
        return NTRIP_CLS_REPLACED;
    case NTRIP_ONO_AUTH:
        return NTRIP_CLS_AUTH;
    case NTRIP_ONO_DUPLICATE:
    case NTRIP_ONO_BANNED:
        return NTRIP_CLS_BANNED;
    default:
        return f->cls == NTRIP_CLS_MOUNT_BUSY ? NTRIP_CLS_IN_USE : f->cls;
    }
}

ntrip_failure_t ntrip_reply_failure(const ntrip_verdict_t *v, bool guard)
{
    ntrip_failure_t f;
    memset(&f, 0, sizeof(f));
    f.cls = v->cls;
    f.row = v->row;
    f.reached_caster = true;
    f.mount_ambiguous = v->mount_ambiguous;
    f.onocoy = v->onocoy_reason;
    f.rule = v->rule;
    /* rule 17: no line in time, or closed before it ended; a partial line
     * that names Onocoy's reason is the caster's answer all the same. */
    f.replied = v->rule != 17 || v->onocoy_reason != NTRIP_ONO_NONE;
    if (guard) {
        f.cls = ntrip_guard_class(&f);   /* the class Onocoy's reason names */
    }
    return f;
}

ntrip_cls_t ntrip_errno_class(int err)
{
    switch (err) {
    case ECONNRESET:
    case EPIPE:
    case ENOTCONN:
        return NTRIP_CLS_REMOTE_CLOSE;   /* the caster's side reset or closed it */
    case ECONNABORTED:
    case ETIMEDOUT:
        return NTRIP_CLS_KEEPALIVE;      /* lwIP gave up on the peer (keepalive, retransmissions) */
    default:
        return NTRIP_CLS_LOCAL;          /* this station's own failure, or no errno to tell: never a take-over */
    }
}

static void reset_streaks(ntrip_sm_t *sm)
{
    sm->attempt = 0;
    sm->reached_streak = 0;
    sm->hourly = false;
    sm->streak_active = false;
    sm->busy_active = false;
}

/* A healthy session (or the user): the guard's incident is over (the take-over score stays). */
static void reset_guard_incident(ntrip_sm_t *sm)
{
    sm->rejects = 0;
    sm->blocks = 0;
    sm->inuse = 0;
    sm->line_transient_used = false;
    sm->silent_run = 0;
}

static void check_healthy(ntrip_sm_t *sm, uint32_t now)
{
    if (!sm->session) {
        return;
    }
    uint32_t len = now - sm->session_start_ms;
    if (!sm->session_healthy && len >= NTRIP_HEALTHY_SESSION_MS && sm->session_bytes >= NTRIP_HEALTHY_SESSION_B) {
        sm->session_healthy = true;
        reset_streaks(sm);
        reset_guard_incident(sm);
    }
    if (sm->kick_score != 0 && len >= NTRIP_ONO_KICK_RESET_S * 1000u) {
        sm->kick_score = 0;   /* a long session: whatever ended the earlier ones has stopped */
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
        sm->unreached_active = false;   /* out of the connect/backoff cycle */
        *why = NTRIP_WHY_DISABLED;
        return in->session_open ? NTRIP_ACT_CLOSE : NTRIP_ACT_NONE;
    }
    if (sm->stop != NTRIP_STOP_NONE) {
        /* The login guard stopped the slot: nothing until the user acts. */
        set_state(sm, NTRIP_SM_STOPPED, now_ms);
        sm->unreached_active = false;
        return NTRIP_ACT_NONE;
    }
    if (!in->have_ip) {
        set_state(sm, NTRIP_SM_WAIT_NETWORK, now_ms);
        sm->unreached_active = false;
        *why = NTRIP_WHY_NO_IP;
        return in->session_open ? NTRIP_ACT_CLOSE : NTRIP_ACT_NONE;
    }
    if (in->paused) {
        set_state(sm, NTRIP_SM_PAUSED, now_ms);
        sm->unreached_active = false;
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
        sm->unreached_active = false;
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
            sm->unreached_active = false;
            return NTRIP_ACT_NONE;
        }
        sm->stagger_pending = false;
    }
    if (!in->fresh_data) {
        set_state(sm, NTRIP_SM_WAIT_DATA, now_ms);
        sm->unreached_active = false;
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
    sm->unreached_active = false;
    /* The caster accepted us: the hourly limit ("until a success") and the
     * run of consecutive caster-reaching failures end. The login guard's
     * incident does not: Onocoy may accept a login and end the session at
     * once with a reason, so only a healthy session ends it (check_healthy). */
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

/* ---- the Onocoy login guard ------------------------------------------------------------- */

static const uint32_t k_ono_reject_s[] = NTRIP_ONO_REJECT_S;
static const uint32_t k_ono_blocked_s[] = NTRIP_ONO_BLOCKED_S;
static const uint32_t k_ono_inuse_s[] = NTRIP_ONO_INUSE_S;
static const uint32_t k_ono_kick_s[] = NTRIP_ONO_KICK_S;
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))

/* The n-th entry (1-based) of a guard row, its last entry repeating. */
static uint32_t guard_entry_s(const uint32_t *row, size_t len, uint32_t n)
{
    return row[(n == 0 ? 1u : (n < len ? n : len)) - 1u];
}

/* A guard delay: entry x U(1.0, 1.2), never below the entry. */
static uint32_t guard_delay_ms(uint32_t entry_s, uint32_t rnd)
{
    return (uint32_t)((uint64_t)entry_s * 1000u * (2000u + rnd % 401u) / 2000u);
}

/* After a run of failures without any reply, every delay is at least the floor. */
static uint32_t silent_floor(const ntrip_sm_t *sm, uint32_t d, uint32_t rnd)
{
    if (sm->cfg.onocoy_guard && sm->silent_run >= NTRIP_ONO_SILENT_RUN) {
        uint32_t lo = guard_delay_ms(NTRIP_ONO_SILENT_FLOOR_S, rnd);
        d = d < lo ? lo : d;
    }
    return d;
}

static uint32_t finish_failure(ntrip_sm_t *sm, ntrip_cls_t cls, ntrip_row_t row, uint32_t d, uint32_t now)
{
    if (sm->attempt < UINT32_MAX) {
        sm->attempt++;
    }
    sm->retry_pending = true;
    sm->retry_at_ms = now + d;
    sm->row = row;
    sm->reason = cls;
    set_state(sm, failure_state(cls, row), now);
    return d;
}

static uint32_t guard_wait(ntrip_sm_t *sm, ntrip_cls_t cls, ntrip_row_t row, uint32_t entry_s, uint32_t now,
                           uint32_t rnd)
{
    sm->hint = NTRIP_HINT_NONE;
    sm->busy_active = false;
    return finish_failure(sm, cls, row, silent_floor(sm, guard_delay_ms(entry_s, rnd), rnd), now);
}

static uint32_t guard_stop(ntrip_sm_t *sm, ntrip_stop_t why, ntrip_cls_t cls, ntrip_row_t row, uint32_t now)
{
    sm->stop = why;
    sm->hint = NTRIP_HINT_NONE;
    sm->busy_active = false;
    sm->retry_pending = false;
    sm->stagger_pending = false;
    sm->row = row;
    sm->reason = cls;
    if (sm->attempt < UINT32_MAX) {
        sm->attempt++;
    }
    set_state(sm, NTRIP_SM_STOPPED, now);
    return 0;
}

/* A refused login: 300 s (30 s once inside the window on credentials that worked), 1800 s, then stop. */
static uint32_t guard_reject(ntrip_sm_t *sm, bool window_worked, uint32_t now, uint32_t rnd)
{
    if (sm->rejects < UINT8_MAX) {
        sm->rejects++;
    }
    if (sm->rejects >= NTRIP_ONO_REJECT_STOP) {
        return guard_stop(sm, NTRIP_STOP_REJECTED, NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, now);
    }
    uint32_t entry = guard_entry_s(k_ono_reject_s, COUNT_OF(k_ono_reject_s), sm->rejects);
    if (sm->rejects == 1u && window_worked) {
        entry = NTRIP_ONO_REJECT_WINDOW_S;
    }
    return guard_wait(sm, NTRIP_CLS_AUTH, NTRIP_ROW_AUTH, entry, now, rnd);
}

/* "Too many unsuccessful logins": an hour, two hours, then stop. */
static uint32_t guard_blocked(ntrip_sm_t *sm, uint32_t now, uint32_t rnd)
{
    if (sm->blocks < UINT8_MAX) {
        sm->blocks++;
    }
    if (sm->blocks >= NTRIP_ONO_BLOCKED_STOP) {
        return guard_stop(sm, NTRIP_STOP_LOGIN_BLOCKED, NTRIP_CLS_LOGIN_BLOCKED, NTRIP_ROW_AUTH, now);
    }
    uint32_t entry = guard_entry_s(k_ono_blocked_s, COUNT_OF(k_ono_blocked_s), sm->blocks);
    return guard_wait(sm, NTRIP_CLS_LOGIN_BLOCKED, NTRIP_ROW_AUTH, entry, now, rnd);
}

/* The credential is in use: only this station's own dead session may explain that, briefly. */
static uint32_t guard_in_use(ntrip_sm_t *sm, bool window_worked, uint32_t now, uint32_t rnd)
{
    if (!window_worked) {
        return guard_stop(sm, NTRIP_STOP_IN_USE, NTRIP_CLS_IN_USE, NTRIP_ROW_MOUNT_BUSY, now);
    }
    if (sm->inuse < UINT8_MAX) {
        sm->inuse++;
    }
    if (sm->inuse >= NTRIP_ONO_INUSE_STOP) {
        return guard_stop(sm, NTRIP_STOP_IN_USE, NTRIP_CLS_IN_USE, NTRIP_ROW_MOUNT_BUSY, now);
    }
    uint32_t entry = guard_entry_s(k_ono_inuse_s, COUNT_OF(k_ono_inuse_s), sm->inuse);
    return guard_wait(sm, NTRIP_CLS_IN_USE, NTRIP_ROW_MOUNT_BUSY, entry, now, rnd);
}

/*
 * The guard's decision for a failure that reached the caster on a guard
 * slot. @return true with *d set when it decided; false: the profile's rows
 * apply as for any slot (TRANSIENT and the other failures without a login
 * reply), after it counted what it needs.
 */
static bool guard_failed(ntrip_sm_t *sm, const ntrip_failure_t *f, bool was_session, uint32_t session_ms,
                         uint32_t now, uint32_t rnd, uint32_t *d)
{
    ntrip_onocoy_reason_t why = (ntrip_onocoy_reason_t)f->onocoy;
    ntrip_cls_t cls = ntrip_guard_class(f);
    bool window_worked = (uint32_t)(now - sm->window_start_ms) < sm->cfg.auth_window_ms && sm->good_valid &&
                         sm->good_hash == sm->cred_hash;

    /* f->cls may be the raw class or the guard's already (the server counts
     * and logs the guard's): every test below is on the guard's class. */

    /* A duplicate stream or a ban: going on risks the account-wide penalty. */
    if (why == NTRIP_ONO_DUPLICATE) {
        *d = guard_stop(sm, NTRIP_STOP_DUPLICATE, cls, NTRIP_ROW_BANNED, now);
        return true;
    }
    if (why == NTRIP_ONO_BANNED || cls == NTRIP_CLS_BANNED) {
        *d = guard_stop(sm, NTRIP_STOP_BANNED, NTRIP_CLS_BANNED, NTRIP_ROW_BANNED, now);
        return true;
    }
    if (why == NTRIP_ONO_LOGIN_BLOCKED) {
        *d = guard_blocked(sm, now, rnd);
        return true;
    }
    if (why == NTRIP_ONO_AUTH) {
        *d = guard_reject(sm, window_worked, now, rnd);
        return true;
    }
    if (was_session) {
        /* Take-over: another device on the credential ends this station's sessions. */
        uint32_t score = 0;
        if (why == NTRIP_ONO_REPLACED || why == NTRIP_ONO_IN_USE) {
            score = 2;   /* this station never opens a second session: another device logged in */
        } else if (cls == NTRIP_CLS_REMOTE_CLOSE && session_ms < NTRIP_ONO_KICK_SHORT_S * 1000u) {
            score = 1;   /* EOF or a reset from the caster's side; our own failures (LOCAL, TLS) never score */
        }
        if (score == 0) {
            return false;   /* our own drop or a long session: the fast TRANSIENT row */
        }
        uint32_t k = sm->kick_score + score;
        sm->kick_score = (uint8_t)(k > UINT8_MAX ? UINT8_MAX : k);
        if (sm->kick_score >= NTRIP_ONO_KICK_STOP_SCORE) {
            *d = guard_stop(sm, NTRIP_STOP_KICKED, cls, NTRIP_ROW_TRANSIENT, now);
        } else {
            uint32_t entry = guard_entry_s(k_ono_kick_s, COUNT_OF(k_ono_kick_s), sm->kick_score);
            *d = guard_wait(sm, cls, NTRIP_ROW_TRANSIENT, entry, now, rnd);
        }
        return true;
    }
    if (!f->replied) {
        /* No login reply: a transport failure on the TRANSIENT row. A silent
         * close after the request may itself be a refused login. */
        if (f->reached_caster && !f->refused && cls != NTRIP_CLS_TLS && cls != NTRIP_CLS_LOCAL &&
            f->row == NTRIP_ROW_TRANSIENT && sm->silent_run < UINT8_MAX) {
            sm->silent_run++;
        }
        return false;
    }
    /* In use: Onocoy's text, or 409 / "Mount Point Taken" / "Already Connected" (IN_USE as the guard's class). */
    if (cls == NTRIP_CLS_IN_USE || cls == NTRIP_CLS_REPLACED) {
        *d = guard_in_use(sm, window_worked, now, rnd);
        return true;
    }
    if (f->row == NTRIP_ROW_TRANSIENT) {
        if (f->rule != NTRIP_RULE_ERROR_LINE) {
            return false;   /* 500, 503, "Too many sources": the caster's own trouble, the TRANSIENT row */
        }
        if (NTRIP_ONO_LINE_TRANSIENT_FREE > 0u && !sm->line_transient_used) {
            sm->line_transient_used = true;   /* one unrecognised error line per run: the TRANSIENT row */
            return false;
        }
    }
    *d = guard_reject(sm, window_worked, now, rnd);
    return true;
}

uint32_t ntrip_sm_failed(ntrip_sm_t *sm, const ntrip_failure_t *f, uint32_t now_ms, uint32_t rnd)
{
    bool was_session = sm->session;
    uint32_t session_ms = now_ms - sm->session_start_ms;
    end_session(sm, now_ms);

    if (sm->cfg.onocoy_guard && !ntrip_failure_unreached(f)) {
        if (!sm->streak_active) {
            sm->streak_active = true;
            sm->streak_start_ms = now_ms;
        }
        sm->unreached_active = false;
        uint32_t d = 0;
        if (guard_failed(sm, f, was_session, session_ms, now_ms, rnd, &d)) {
            return d;
        }
    }

    ntrip_row_t row = f->row == NTRIP_ROW_NONE ? NTRIP_ROW_TRANSIENT : f->row;
    ntrip_cls_t cls = sm->cfg.onocoy_guard ? ntrip_guard_class(f) : f->cls;
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

    bool unreached = ntrip_failure_unreached(f);
    if (!unreached) {
        sm->unreached_active = false;   /* the caster (or its host) answered: the run ends */
    } else if (!sm->unreached_active) {
        sm->unreached_active = true;
        sm->unreached_start_ms = now_ms;
    }
    if (unreached && sm->cfg.protected_slot) {
        /* The caster never saw it: a flat short retry, the count stays. */
        uint32_t flat = (uint32_t)((uint64_t)NTRIP_UNREACHED_RETRY_MS * (800u + (rnd % 401u)) / 1000u);
        sm->retry_pending = true;
        sm->retry_at_ms = now_ms + flat;
        sm->row = row;
        sm->reason = cls;
        set_state(sm, failure_state(cls, row), now_ms);
        return flat;
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
    return finish_failure(sm, cls, row, silent_floor(sm, d, rnd), now_ms);
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

/* The user acted (a save or a retry): delays, counts and a stop end; an attempt may start at once. */
static void user_reset(ntrip_sm_t *sm)
{
    reset_streaks(sm);
    reset_guard_incident(sm);
    sm->kick_score = 0;
    sm->stop = NTRIP_STOP_NONE;
    sm->unreached_active = false;
    sm->retry_pending = false;
    sm->stagger_pending = false;
    sm->row = NTRIP_ROW_NONE;
    sm->reason = NTRIP_CLS_NONE;
    sm->hint = NTRIP_HINT_NONE;
}

void ntrip_sm_user_edit(ntrip_sm_t *sm, const ntrip_sm_cfg_t *cfg, uint32_t cred_hash, uint32_t now_ms)
{
    (void)now_ms;
    sm->cfg = *cfg;
    sm->cred_hash = cred_hash;
    user_reset(sm);
}

void ntrip_sm_retry(ntrip_sm_t *sm, uint32_t now_ms)
{
    (void)now_ms;
    user_reset(sm);
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

ntrip_cls_t ntrip_sm_retry_class(const ntrip_sm_t *sm)
{
    return sm->row != NTRIP_ROW_NONE ? sm->reason : NTRIP_CLS_NONE;
}

uint32_t ntrip_sm_unreached_ms(const ntrip_sm_t *sm, uint32_t now_ms)
{
    if (!sm->unreached_active) {
        return 0;
    }
    uint32_t d = now_ms - sm->unreached_start_ms;
    return d == 0 ? 1u : d;   /* a run in progress is never reported as none */
}

/* The take-over score as stored: it stops changing at the stop score. */
static uint8_t kick_key(const ntrip_sm_t *sm)
{
    return sm->kick_score < NTRIP_ONO_KICK_STOP_SCORE ? sm->kick_score : (uint8_t)NTRIP_ONO_KICK_STOP_SCORE;
}

/* The in-use count as stored: it stops changing at the stop count. */
static uint8_t inuse_key(const ntrip_sm_t *sm)
{
    return sm->inuse < NTRIP_ONO_INUSE_STOP ? sm->inuse : (uint8_t)NTRIP_ONO_INUSE_STOP;
}

/* The run without a reply as stored: it stops changing once the floor applies. */
static uint8_t silent_key(const ntrip_sm_t *sm)
{
    return sm->silent_run < NTRIP_ONO_SILENT_RUN ? sm->silent_run : (uint8_t)NTRIP_ONO_SILENT_RUN;
}

bool ntrip_sm_persist_due(const ntrip_sm_t *sm)
{
    ntrip_row_t row = long_row(sm->row);
    return row != sm->saved_row || long_step(sm->cfg.profile, row, sm->attempt) != sm->saved_step ||
           sm->hourly != sm->saved_hourly || reached_key(sm) != sm->saved_reached || sm->saved_dirty ||
           sm->rejects != sm->saved_rejects || sm->blocks != sm->saved_blocks || kick_key(sm) != sm->saved_kick ||
           sm->stop != sm->saved_stop || inuse_key(sm) != sm->saved_inuse || silent_key(sm) != sm->saved_silent ||
           sm->line_transient_used != sm->saved_line;
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
    out->rejects = sm->rejects;
    out->blocks = sm->blocks;
    out->kick_score = kick_key(sm);
    out->stop = sm->stop;
    out->inuse = inuse_key(sm);
    out->silent_run = silent_key(sm);
    out->line_transient_used = sm->line_transient_used;
}

void ntrip_sm_persist_done(ntrip_sm_t *sm)
{
    sm->saved_row = long_row(sm->row);
    sm->saved_step = long_step(sm->cfg.profile, sm->saved_row, sm->attempt);
    sm->saved_reached = reached_key(sm);
    sm->saved_hourly = sm->hourly;
    sm->saved_dirty = false;
    sm->saved_rejects = sm->rejects;
    sm->saved_blocks = sm->blocks;
    sm->saved_kick = kick_key(sm);
    sm->saved_stop = sm->stop;
    sm->saved_inuse = inuse_key(sm);
    sm->saved_silent = silent_key(sm);
    sm->saved_line = sm->line_transient_used;
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
    if (sm->cfg.onocoy_guard) {
        /* The guard's counts and stop (a slot that is no longer a guard slot drops them). */
        sm->rejects = rec->rejects < NTRIP_ONO_REJECT_STOP ? rec->rejects : (uint8_t)NTRIP_ONO_REJECT_STOP;
        sm->blocks = rec->blocks < NTRIP_ONO_BLOCKED_STOP ? rec->blocks : (uint8_t)NTRIP_ONO_BLOCKED_STOP;
        sm->kick_score = rec->kick_score < NTRIP_ONO_KICK_STOP_SCORE ? rec->kick_score
                                                                      : (uint8_t)NTRIP_ONO_KICK_STOP_SCORE;
        sm->stop = rec->stop < NTRIP_STOP_COUNT ? rec->stop : NTRIP_STOP_NONE;
        sm->inuse = rec->inuse < NTRIP_ONO_INUSE_STOP ? rec->inuse : (uint8_t)NTRIP_ONO_INUSE_STOP;
        sm->silent_run = rec->silent_run < NTRIP_ONO_SILENT_RUN ? rec->silent_run : (uint8_t)NTRIP_ONO_SILENT_RUN;
        sm->line_transient_used = rec->line_transient_used;
        sm->saved_rejects = sm->rejects;
        sm->saved_blocks = sm->blocks;
        sm->saved_kick = kick_key(sm);
        sm->saved_stop = sm->stop;
        sm->saved_inuse = inuse_key(sm);
        sm->saved_silent = silent_key(sm);
        sm->saved_line = sm->line_transient_used;
    } else {
        /* A stop or a count stored for a guard slot: the next write drops it. */
        sm->saved_rejects = rec->rejects;
        sm->saved_blocks = rec->blocks;
        sm->saved_kick = rec->kick_score;
        sm->saved_stop = rec->stop;
        sm->saved_inuse = rec->inuse;
        sm->saved_silent = rec->silent_run;
        sm->saved_line = rec->line_transient_used;
    }
    if (sm->stop != NTRIP_STOP_NONE) {
        /* Stopped before the restart: still stopped, nothing pending. */
        sm->row = row;
        sm->reason = rec->cls;
        sm->attempt = rec->attempt;
        set_state(sm, NTRIP_SM_STOPPED, now_ms);
        return;
    }
    if (sm->cfg.onocoy_guard && sm->silent_run >= NTRIP_ONO_SILENT_RUN && row == NTRIP_ROW_NONE && !rec->hourly) {
        /* The floor after a run without a reply holds until a healthy
         * session or the user: a restart waits it before its first attempt. */
        sm->retry_pending = true;
        sm->retry_at_ms = now_ms + NTRIP_ONO_SILENT_FLOOR_S * 1000u;
        sm->row = NTRIP_ROW_TRANSIENT;
        sm->reason = rec->cls;
        return;
    }
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
        if (sm->cfg.onocoy_guard && row == NTRIP_ROW_AUTH && rec->cls == NTRIP_CLS_LOGIN_BLOCKED && sm->blocks > 0) {
            nominal = (uint64_t)guard_entry_s(k_ono_blocked_s, COUNT_OF(k_ono_blocked_s), sm->blocks) * 1000u;
        } else if (sm->cfg.onocoy_guard && row == NTRIP_ROW_AUTH && sm->rejects > 0) {
            /* the full entry: a rejection's 300 s, never the 30 s variant */
            nominal = (uint64_t)guard_entry_s(k_ono_reject_s, COUNT_OF(k_ono_reject_s), sm->rejects) * 1000u;
        }
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
    blob_put_u8(&w, rec->rejects);
    blob_put_u8(&w, rec->blocks);
    blob_put_u8(&w, rec->kick_score);
    blob_put_u8(&w, (uint8_t)rec->stop);
    blob_put_u8(&w, rec->inuse);
    blob_put_u8(&w, rec->silent_run);
    blob_put_bool(&w, rec->line_transient_used);
    return blob_writer_ok(&w) ? w.len : 0;
}

bool ntrip_backoff_rec_decode(const uint8_t *buf, size_t len, ntrip_backoff_rec_t *out)
{
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    memset(out, 0, sizeof(*out));
    uint8_t version = blob_get_u8(&r);
    if (version < 1u || version > NTRIP_BACKOFF_REC_VERSION) {
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
    uint8_t stop = NTRIP_STOP_NONE;
    if (version >= 3u) {
        rec.rejects = blob_get_u8(&r);
        rec.blocks = blob_get_u8(&r);
        rec.kick_score = blob_get_u8(&r);
        stop = blob_get_u8(&r);
    }
    if (version >= 4u) {
        rec.inuse = blob_get_u8(&r);
        rec.silent_run = blob_get_u8(&r);
        rec.line_transient_used = blob_get_bool(&r);
    }
    if (!blob_reader_ok(&r) || cls >= NTRIP_CLS_COUNT || long_row((ntrip_row_t)row) != (ntrip_row_t)row ||
        stop >= NTRIP_STOP_COUNT) {
        return false;
    }
    rec.stop = (ntrip_stop_t)stop;
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

/* ---- connection history ---------------------------------------------------------------- */

int32_t ntrip_hist_pack(uint32_t slot, uint32_t cls, uint32_t count)
{
    if (count > NTRIP_HIST_COUNT_MAX) {
        count = NTRIP_HIST_COUNT_MAX;
    }
    return (int32_t)((slot & 0xFFu) | ((cls & 0xFFu) << 8) | (count << 16));
}

void ntrip_hist_init(ntrip_hist_t *h)
{
    memset(h, 0, sizeof(*h));
}

void ntrip_hist_failed(ntrip_hist_t *h, uint32_t now_s)
{
    if (h->fails < UINT32_MAX) {
        h->fails++;
    }
    if (!h->first_fail_valid) {
        h->first_fail_valid = true;
        h->first_fail_s = now_s;
    }
}

bool ntrip_hist_up(ntrip_hist_t *h, uint32_t slot, uint32_t now_s, int32_t *a, int32_t *b)
{
    bool write = false;
    bool after_gap = h->streamed && (uint32_t)(now_s - h->prev_end_s) >= NTRIP_HIST_UP_GAP_S;
    if (h->fails > 0 || after_gap) {
        uint32_t outage = 0;
        if (h->streamed) {
            outage = now_s - h->prev_end_s;
        } else if (h->first_fail_valid) {
            outage = now_s - h->first_fail_s;
        }
        bool limited = h->streamed && h->prev_short && h->up_written &&
                       (uint32_t)(now_s - h->up_at_s) < NTRIP_HIST_LIMIT_S;
        if (!limited) {
            write = true;
            *a = ntrip_hist_pack(slot, 0, h->fails);
            *b = (int32_t)(outage > INT32_MAX ? INT32_MAX : outage);
            h->up_written = true;
            h->up_at_s = now_s;
        }
    }
    h->in_session = true;
    h->session_start_s = now_s;
    h->fails = 0;
    return write;
}

bool ntrip_hist_down(ntrip_hist_t *h, uint32_t slot, uint8_t cls, uint32_t now_s, int32_t *a, int32_t *b)
{
    if (!h->in_session) {
        return false;
    }
    uint32_t len = now_s - h->session_start_s;
    bool is_short = len < NTRIP_HIST_SHORT_S;
    bool write = !is_short || !h->down_written || (uint32_t)(now_s - h->down_at_s) >= NTRIP_HIST_LIMIT_S;
    if (write) {
        *a = ntrip_hist_pack(slot, cls, h->skipped);
        *b = (int32_t)(len > INT32_MAX ? INT32_MAX : len);
        h->skipped = 0;
        h->down_written = true;
        h->down_at_s = now_s;
    } else if (h->skipped < UINT32_MAX) {
        h->skipped++;
    }
    h->in_session = false;
    h->streamed = true;
    h->prev_end_s = now_s;
    h->prev_short = is_short;
    return write;
}

/* ---- Onocoy daily budget ----------------------------------------------------------- */

uint16_t ntrip_graced_so_far_permille(uint32_t unplanned_offline_s, uint32_t elapsed_s)
{
    if (elapsed_s == 0 || unplanned_offline_s <= NTRIP_GRACE_S) {
        return 1000u;
    }
    uint64_t lost = (uint64_t)(unplanned_offline_s - NTRIP_GRACE_S) * 1000u;
    uint64_t per = (lost + elapsed_s / 2u) / elapsed_s;   /* rounded */
    return per >= 1000u ? 0u : (uint16_t)(1000u - per);
}

void ntrip_day_metrics(const ntrip_day_in_t *in, ntrip_day_out_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!in->counted) {
        return;
    }
    out->valid = true;
    out->rate_valid = in->epochs_sent_today > 0;
    uint32_t rate = 1000u;
    if (out->rate_valid && in->rate_online_s > 0) {
        uint64_t r = (uint64_t)in->epochs_sent_today * 1000u / in->rate_online_s;
        rate = r > 1000u ? 1000u : (uint32_t)r;
    }
    out->data_rate_permille = (uint16_t)rate;
    /* The budget and the streak from the best rate the day can still reach,
     * not from this boot's ratio: one epoch lost 30 s after a start is 97 %
     * of that boot, but a day that loses nothing more ends near 100 %. */
    out->best_rate_ppm = ntrip_best_rate_ppm(in->lost_epochs, in->offline_s);
    uint32_t budget = ntrip_offline_budget_ppm_s(out->best_rate_ppm);
    out->budget_s = budget;
    uint64_t graced = ((uint64_t)in->online_s + NTRIP_GRACE_S) * 1000u / NTRIP_DAY_S;
    out->graced_uptime_permille = (uint16_t)(graced > 1000u ? 1000u : graced);
    out->budget_left_s = budget > in->offline_s ? budget - in->offline_s : 0;
    out->streak_ok = out->best_rate_ppm >= NTRIP_TARGET_PERMILLE * 1000u && in->offline_s <= budget;
    if (in->day_known) {
        out->so_far_valid = true;
        out->graced_so_far_permille = ntrip_graced_so_far_permille(in->unplanned_offline_s, in->elapsed_s);
    }
}

uint32_t ntrip_lost_epochs(uint32_t online_s, uint32_t epochs_sent)
{
    return online_s > epochs_sent ? online_s - epochs_sent : 0;
}

void ntrip_rate_clock_init(ntrip_rate_clock_t *c, uint32_t now_ms)
{
    c->on_ms = 0;
    c->last_ms = now_ms;
}

void ntrip_rate_clock_tick(ntrip_rate_clock_t *c, uint32_t now_ms, const ntrip_rate_in_t *in)
{
    uint32_t dt = now_ms - c->last_ms;
    c->last_ms = now_ms;
    if (!in->enabled || !in->session_open || !in->streaming || !in->rtcm_ok) {
        return;
    }
    /* A session accepted since the previous tick: the connect before it is not online. */
    uint32_t since_accept = now_ms - in->session_start_ms;
    c->on_ms += since_accept < dt ? since_accept : dt;
}

uint32_t ntrip_best_rate_ppm(uint32_t lost_epochs, uint32_t offline_s)
{
    if (offline_s >= NTRIP_DAY_S) {
        return lost_epochs == 0 ? 1000000u : 0;
    }
    uint32_t expected = NTRIP_DAY_S - offline_s;   /* epochs the day can still expect at 1 Hz */
    if (lost_epochs >= expected) {
        return 0;
    }
    uint64_t lost_ppm = ((uint64_t)lost_epochs * 1000000u + expected - 1u) / expected;   /* rounded up */
    return (uint32_t)(1000000u - lost_ppm);
}

uint32_t ntrip_offline_budget_s(uint32_t data_rate_permille)
{
    if (data_rate_permille > 1000u) {
        data_rate_permille = 1000u;
    }
    return ntrip_offline_budget_ppm_s(data_rate_permille * 1000u);
}

uint32_t ntrip_offline_budget_ppm_s(uint32_t data_rate_ppm)
{
    if (data_rate_ppm > 1000000u) {
        data_rate_ppm = 1000000u;
    }
    if (data_rate_ppm < NTRIP_TARGET_PERMILLE * 1000u) {
        return 0;
    }
    double rate = (double)data_rate_ppm / 1000000.0;
    double target = (double)NTRIP_TARGET_PERMILLE / 1000.0;
    double u = 0.8 + 0.2 * sqrt(target / rate);           /* lowest graced uptime that still qualifies */
    double offline = (double)NTRIP_DAY_S + (double)NTRIP_GRACE_S - (double)NTRIP_DAY_S * u;
    if (offline <= 0.0) {
        return 0;
    }
    return (uint32_t)floor(offline + 1e-9);
}
