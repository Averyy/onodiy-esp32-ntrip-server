/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "lcast_core.h"

#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "settings_codec.h"

/* ---- small helpers --------------------------------------------------------------------- */

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Equal ignoring ASCII case, over n bytes of a and the whole of b. */
static bool eq_nocase(const char *a, size_t n, const char *b)
{
    size_t m = strlen(b);
    if (n != m) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

static bool prefix_nocase(const char *a, size_t n, const char *p)
{
    size_t m = strlen(p);
    return n >= m && eq_nocase(a, m, p);
}

/* Append text to out, keeping it terminated; *len counts what was asked for,
 * so an overflow shows as *len >= cap. */
static void put(char *out, size_t cap, size_t *len, const char *s)
{
    for (; *s != '\0'; s++) {
        if (*len + 1 < cap) {
            out[*len] = *s;
        }
        (*len)++;
    }
    if (cap > 0) {
        out[*len < cap ? *len : cap - 1] = '\0';
    }
}

static void put_u32(char *out, size_t cap, size_t *len, uint32_t v)
{
    char num[12];
    snprintf(num, sizeof(num), "%lu", (unsigned long)v);
    put(out, cap, len, num);
}

static bool addr_v4(const lcast_addr_t *a, uint32_t *v4)
{
    static const uint8_t mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };
    if (!a->v6) {
        memcpy(v4, a->b, 4);
        return true;
    }
    if (memcmp(a->b, mapped, sizeof(mapped)) == 0) {
        memcpy(v4, a->b + 12, 4);
        return true;
    }
    return false;
}

/* ---- addresses ---------------------------------------------------------------------------- */

void lcast_addr_unmap(lcast_addr_t *a)
{
    uint32_t v4;
    if (a->v6 && addr_v4(a, &v4)) {
        memset(a, 0, sizeof(*a));
        memcpy(a->b, &v4, 4);
    }
}

static bool v4_private(const uint8_t q[4])
{
    return q[0] == 10 || (q[0] == 172 && (q[1] & 0xF0u) == 16) || (q[0] == 192 && q[1] == 168) ||
           (q[0] == 169 && q[1] == 254) || q[0] == 127;
}

static bool v6_private(const uint8_t b[16])
{
    static const uint8_t loopback[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
    return (b[0] & 0xFEu) == 0xFC ||                    /* fc00::/7, unique local */
           (b[0] == 0xFE && (b[1] & 0xC0u) == 0x80) ||  /* fe80::/10, link-local */
           memcmp(b, loopback, 16) == 0;
}

lcast_addr_verdict_t lcast_addr_verdict(const lcast_addr_t *peer, const lcast_addr_t *local, uint32_t gw_ip4,
                                        uint32_t ap_ip4, const uint8_t *ap_ll6)
{
    if (peer == NULL) {
        return LCAST_ADDR_NOT_PRIVATE;
    }
    lcast_addr_t p = *peer;
    lcast_addr_unmap(&p);
    if (local != NULL) {
        lcast_addr_t l = *local;
        lcast_addr_unmap(&l);
        uint32_t v4;
        if (!l.v6) {
            memcpy(&v4, l.b, 4);
            if (ap_ip4 != 0 && v4 == ap_ip4) {
                return LCAST_ADDR_HOTSPOT;
            }
        } else if (ap_ll6 != NULL && memcmp(l.b, ap_ll6, 16) == 0) {
            return LCAST_ADDR_HOTSPOT;
        }
    }
    if (p.v6) {
        return v6_private(p.b) ? LCAST_ADDR_OK : LCAST_ADDR_NOT_PRIVATE;
    }
    if (!v4_private(p.b)) {
        return LCAST_ADDR_NOT_PRIVATE;
    }
    uint32_t v4;
    memcpy(&v4, p.b, 4);
    if (gw_ip4 != 0 && v4 == gw_ip4) {
        return LCAST_ADDR_GATEWAY;
    }
    return LCAST_ADDR_OK;
}

size_t lcast_addr_text(const lcast_addr_t *a, char *out, size_t cap)
{
    size_t len = 0;
    if (cap > 0) {
        out[0] = '\0';
    }
    if (a == NULL) {
        put(out, cap, &len, "?");
        return len < cap ? len : (cap > 0 ? cap - 1 : 0);
    }
    char part[8];
    uint32_t v4;
    if (addr_v4(a, &v4)) {
        const uint8_t *q = (const uint8_t *)&v4;
        for (int i = 0; i < 4; i++) {
            snprintf(part, sizeof(part), i == 0 ? "%u" : ".%u", (unsigned)q[i]);
            put(out, cap, &len, part);
        }
        return len < cap ? len : (cap > 0 ? cap - 1 : 0);
    }
    uint16_t g[8];
    for (int i = 0; i < 8; i++) {
        g[i] = (uint16_t)((a->b[2 * i] << 8) | a->b[2 * i + 1]);
    }
    /* The longest run of two or more zero groups (the first of equal runs) becomes "::". */
    int best = -1;
    int best_n = 1;
    for (int i = 0; i < 8;) {
        if (g[i] != 0) {
            i++;
            continue;
        }
        int j = i;
        while (j < 8 && g[j] == 0) {
            j++;
        }
        if (j - i > best_n) {
            best = i;
            best_n = j - i;
        }
        i = j;
    }
    for (int i = 0; i < 8; i++) {
        if (i == best) {
            put(out, cap, &len, "::");
            i += best_n - 1;
            continue;
        }
        bool after_gap = best >= 0 && i == best + best_n;
        snprintf(part, sizeof(part), (i == 0 || after_gap) ? "%x" : ":%x", (unsigned)g[i]);
        put(out, cap, &len, part);
    }
    return len < cap ? len : (cap > 0 ? cap - 1 : 0);
}

uint64_t lcast_client_key(const lcast_addr_t *a)
{
    if (a == NULL) {
        return 0;
    }
    uint32_t v4;
    if (addr_v4(a, &v4)) {
        return v4;
    }
    uint64_t hi;
    memcpy(&hi, a->b, 8);
    if (a->b[0] == 0xFE && (a->b[1] & 0xC0u) == 0x80) {
        uint64_t lo;
        memcpy(&lo, a->b + 8, 8);
        return hi ^ lo;   /* link-local: every host of the link has the same prefix */
    }
    return hi;
}

void lcast_addr_word(const lcast_addr_t *a, uint8_t out[4])
{
    uint32_t v4;
    if (addr_v4(a, &v4)) {
        memcpy(out, &v4, 4);
    } else {
        memcpy(out, a->b + 12, 4);
    }
}

/* ---- request ------------------------------------------------------------------------------ */

void lcast_secure_zero(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len-- > 0) {
        *v++ = 0;
    }
}

void lcast_req_wipe(lcast_req_t *r)
{
    lcast_secure_zero(r, sizeof(*r));
}

void lcast_req_reset(lcast_req_t *r)
{
    lcast_req_wipe(r);
    r->result = LCAST_REQ_NEED_MORE;
}

uint8_t *lcast_req_room(lcast_req_t *r, size_t *room)
{
    *room = (r->result == LCAST_REQ_NEED_MORE && r->len < sizeof(r->buf)) ? sizeof(r->buf) - r->len : 0;
    return r->buf + r->len;
}

/* One line of the header: [p, p + n), without its LF and a CR before it. */
typedef struct {
    const char *p;
    size_t n;
} line_t;

static bool is_space(char c)
{
    return c == ' ' || c == '\t';
}

static void trim(const char **p, size_t *n)
{
    while (*n > 0 && is_space(**p)) {
        (*p)++;
        (*n)--;
    }
    while (*n > 0 && is_space((*p)[*n - 1])) {
        (*n)--;
    }
}

/* The request line: GET, a target, an optional version; printable ASCII only. */
static bool parse_request_line(lcast_req_t *r, line_t l)
{
    const char *tok[4];
    size_t tlen[4];
    size_t nt = 0;
    for (size_t i = 0; i < l.n; i++) {
        unsigned char c = (unsigned char)l.p[i];
        if (c < 0x20u || c > 0x7Eu) {
            return false;
        }
    }
    size_t i = 0;
    while (i < l.n) {
        while (i < l.n && l.p[i] == ' ') {
            i++;
        }
        if (i == l.n) {
            break;
        }
        if (nt == 4) {
            return false;   /* a fourth token */
        }
        tok[nt] = l.p + i;
        size_t s = i;
        while (i < l.n && l.p[i] != ' ') {
            i++;
        }
        tlen[nt] = i - s;
        nt++;
    }
    if (nt < 2 || nt > 3) {
        return false;
    }
    if (tlen[0] != 3 || memcmp(tok[0], "GET", 3) != 0) {
        return false;
    }
    if (nt == 3) {
        if (tlen[2] == 8 && memcmp(tok[2], "HTTP/1.1", 8) == 0) {
            r->http_minor = 1;
        } else if (tlen[2] == 8 && memcmp(tok[2], "HTTP/1.0", 8) == 0) {
            r->http_minor = 0;
        } else {
            return false;
        }
    }
    const char *t = tok[1];
    size_t tn = tlen[1];
    if (prefix_nocase(t, tn, "http://")) {
        /* Absolute form: the scheme and the authority go. */
        t += 7;
        tn -= 7;
        size_t k = 0;
        while (k < tn && t[k] != '/' && t[k] != '?') {
            k++;
        }
        t += k;
        tn -= k;
        if (tn == 0 || t[0] == '?') {
            return true;   /* no path: the root */
        }
    }
    if (tn == 0 || t[0] != '/') {
        return false;
    }
    t++;
    tn--;
    size_t m = 0;
    while (m < tn && t[m] != '?') {
        m++;
    }
    if (m >= sizeof(r->mount)) {
        r->mount_long = true;
        m = sizeof(r->mount) - 1u;
    }
    memcpy(r->mount, t, m);
    r->mount[m] = '\0';
    return true;
}

/* "Basic <base64>" decoded into r->auth. */
static void parse_auth(lcast_req_t *r, const char *v, size_t n)
{
    r->auth_bad = true;
    if (!prefix_nocase(v, n, "Basic") || n < 6 || !is_space(v[5])) {
        return;
    }
    const char *b = v + 6;
    size_t bn = n - 6;
    trim(&b, &bn);
    size_t out = 0;
    if (bn == 0 || !lcast_b64_decode(b, bn, (uint8_t *)r->auth, sizeof(r->auth) - 1u, &out)) {
        lcast_secure_zero(r->auth, sizeof(r->auth));
        return;
    }
    r->auth[out] = '\0';
    if (memchr(r->auth, ':', out) == NULL || memchr(r->auth, '\0', out) != NULL) {
        lcast_secure_zero(r->auth, sizeof(r->auth));
        return;
    }
    r->auth_len = out;
    r->auth_bad = false;
}

static bool parse_header(lcast_req_t *r, line_t l, bool *auth_seen)
{
    const char *colon = memchr(l.p, ':', l.n);
    if (colon == NULL) {
        return true;   /* not a header line: ignored */
    }
    const char *name = l.p;
    size_t nn = (size_t)(colon - l.p);
    trim(&name, &nn);
    const char *v = colon + 1;
    size_t vn = l.n - (size_t)(v - l.p);
    trim(&v, &vn);
    if (eq_nocase(name, nn, "Ntrip-Version")) {
        r->ntrip2 = eq_nocase(v, vn, "Ntrip/2.0");
    } else if (eq_nocase(name, nn, "Authorization")) {
        if (*auth_seen) {
            return false;   /* a second one */
        }
        *auth_seen = true;
        r->auth_present = true;
        parse_auth(r, v, vn);
    } else if (eq_nocase(name, nn, "User-Agent")) {
        if (r->agent[0] == '\0') {
            size_t k = 0;
            for (size_t i = 0; i < vn && k + 1 < sizeof(r->agent); i++) {
                unsigned char c = (unsigned char)v[i];
                if (c >= 0x20u && c <= 0x7Eu) {
                    r->agent[k++] = (char)c;
                }
            }
            r->agent[k] = '\0';
        }
    } else if (eq_nocase(name, nn, "Origin") || prefix_nocase(name, nn, "Sec-Fetch-")) {
        r->browser = true;
    }
    return true;
}

/* The header is buf[0, end): parse it. */
static lcast_req_result_t parse(lcast_req_t *r, size_t end)
{
    const char *p = (const char *)r->buf;
    if (memchr(p, '\0', end) != NULL) {
        return LCAST_REQ_BAD;
    }
    bool auth_seen = false;
    size_t headers = 0;
    size_t i = 0;
    bool first = true;
    while (i < end) {
        const char *nl = memchr(p + i, '\n', end - i);
        size_t stop = nl != NULL ? (size_t)(nl - p) : end;
        line_t l = { p + i, stop - i };
        if (l.n > 0 && l.p[l.n - 1] == '\r') {
            l.n--;
        }
        i = stop + 1u;
        if (first) {
            if (!parse_request_line(r, l)) {
                return LCAST_REQ_BAD;
            }
            first = false;
            continue;
        }
        if (l.n == 0) {
            break;   /* the blank line */
        }
        if (headers >= LCAST_REQ_MAX_HEADERS) {
            continue;
        }
        headers++;
        if (!parse_header(r, l, &auth_seen)) {
            return LCAST_REQ_BAD;
        }
    }
    return LCAST_REQ_DONE;
}

lcast_req_result_t lcast_req_added(lcast_req_t *r, size_t n)
{
    if (r->result != LCAST_REQ_NEED_MORE) {
        return r->result;
    }
    if (n > sizeof(r->buf) - r->len) {
        n = sizeof(r->buf) - r->len;
    }
    r->len += n;
    /* The end of the header: an empty line, LF or CRLF after a LF. Search from
     * a little before the new bytes, so a split terminator is found. */
    size_t from = r->scanned >= 2u ? r->scanned - 2u : 0;
    size_t end = 0;
    for (size_t i = from; i < r->len; i++) {
        if (r->buf[i] != '\n') {
            continue;
        }
        if (i + 1u < r->len && r->buf[i + 1u] == '\n') {
            end = i + 2u;
            break;
        }
        if (i + 2u < r->len && r->buf[i + 1u] == '\r' && r->buf[i + 2u] == '\n') {
            end = i + 3u;
            break;
        }
    }
    r->scanned = r->len;
    if (end == 0) {
        if (r->len >= sizeof(r->buf)) {
            r->result = LCAST_REQ_TOO_LARGE;
        }
        return r->result;
    }
    r->result = parse(r, end);
    if (r->result != LCAST_REQ_DONE) {
        lcast_secure_zero(r->auth, sizeof(r->auth));
        r->auth_len = 0;
    }
    return r->result;
}

lcast_req_result_t lcast_req_feed(lcast_req_t *r, const uint8_t *bytes, size_t n)
{
    size_t room;
    uint8_t *dst = lcast_req_room(r, &room);
    if (n > room) {
        n = room;
    }
    if (n > 0) {
        memcpy(dst, bytes, n);
    }
    return lcast_req_added(r, n);
}

/* ---- decision ------------------------------------------------------------------------------ */

lcast_decision_t lcast_decide(const lcast_req_t *req, lcast_req_result_t res, const char *mountpoint,
                              const lcast_cred_t *cred, uint32_t throttle_wait_s, uint32_t streaming,
                              uint32_t max_streaming)
{
    lcast_decision_t d = { .action = LCAST_ACT_E400, .flavour = LCAST_FLAV_V1 };
    if (res != LCAST_REQ_DONE) {
        return d;
    }
    if (req->ntrip2) {
        d.flavour = req->http_minor == 1 ? LCAST_FLAV_V2_CHUNKED : LCAST_FLAV_V2_RAW;
    }
    if (req->browser) {
        d.action = LCAST_ACT_E403;
        return d;
    }
    const char *mp = mountpoint != NULL ? mountpoint : "";
    if (req->mount[0] == '\0' || req->mount_long || mp[0] == '\0' ||
        !eq_nocase(req->mount, strlen(req->mount), mp)) {
        d.action = LCAST_ACT_SOURCETABLE;
        return d;
    }
    if (throttle_wait_s > 0) {
        d.action = LCAST_ACT_E429;
        return d;
    }
    if (!req->auth_present) {
        d.action = LCAST_ACT_E401;
        return d;
    }
    if (req->auth_bad || cred == NULL || !lcast_cred_match(cred, req->auth, req->auth_len)) {
        d.action = LCAST_ACT_E401;
        d.count_fail = true;
        return d;
    }
    d.login_ok = true;
    d.action = streaming >= max_streaming ? LCAST_ACT_E503 : LCAST_ACT_STREAM;
    return d;
}

/* ---- replies ------------------------------------------------------------------------------- */

size_t lcast_imf_date(int64_t unix_s, char *out, size_t cap)
{
    static const char *const wd[] = { "Thu", "Fri", "Sat", "Sun", "Mon", "Tue", "Wed" };
    static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                       "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    if (cap < 30u || unix_s < 0) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return 0;
    }
    int64_t days = unix_s / 86400;
    int64_t sod = unix_s % 86400;
    /* Civil date from days since 1970-01-01 (proleptic Gregorian, eras of 400 years). */
    int64_t z = days + 719468;
    int64_t era = z / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1;
    int64_t m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) {
        y++;
    }
    if (y > 9999) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, cap, "%s, %02d %s %04d %02d:%02d:%02d GMT", wd[days % 7], (int)d, mon[m - 1], (int)y,
                     (int)(sod / 3600), (int)(sod / 60 % 60), (int)(sod % 60));
    return (n > 0 && (size_t)n < cap) ? (size_t)n : 0;
}

/* "Server:" and, when the clock is trusted, "Date:". */
static void put_server(char *out, size_t cap, size_t *len, const char *ver, int64_t now_unix, bool date)
{
    put(out, cap, len, "Server: NTRIP onodiy/");
    put(out, cap, len, ver != NULL ? ver : "");
    put(out, cap, len, "\r\n");
    char d[32];
    if (date && now_unix >= LCAST_CLOCK_VALID_UNIX && lcast_imf_date(now_unix, d, sizeof(d)) > 0) {
        put(out, cap, len, "Date: ");
        put(out, cap, len, d);
        put(out, cap, len, "\r\n");
    }
}

static size_t done(size_t len, size_t cap)
{
    return len < cap ? len : 0;
}

size_t lcast_reply_stream(char *buf, size_t cap, lcast_flavour_t f, const char *server_ver, int64_t now_unix)
{
    size_t len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
    if (f == LCAST_FLAV_V1) {
        put(buf, cap, &len, "ICY 200 OK\r\n\r\n");
        return done(len, cap);
    }
    put(buf, cap, &len, f == LCAST_FLAV_V2_CHUNKED ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.0 200 OK\r\n");
    put(buf, cap, &len, "Ntrip-Version: Ntrip/2.0\r\n");
    put_server(buf, cap, &len, server_ver, now_unix, true);
    put(buf, cap, &len, "Cache-Control: no-store, no-cache, max-age=0\r\n"
                        "Pragma: no-cache\r\n"
                        "Connection: close\r\n"
                        "Content-Type: gnss/data\r\n");
    if (f == LCAST_FLAV_V2_CHUNKED) {
        put(buf, cap, &len, "Transfer-Encoding: chunked\r\n");
    }
    put(buf, cap, &len, "\r\n");
    return done(len, cap);
}

size_t lcast_reply_error(char *buf, size_t cap, lcast_action_t action, lcast_flavour_t f, const char *realm_mount,
                         uint32_t retry_s, const char *server_ver, int64_t now_unix)
{
    const char *status;
    char body[64];
    switch (action) {
    case LCAST_ACT_E400:
        status = "400 Bad Request";
        snprintf(body, sizeof(body), "Bad request\r\n");
        break;
    case LCAST_ACT_E401:
        status = "401 Unauthorized";
        snprintf(body, sizeof(body), "Wrong or missing username or password\r\n");
        break;
    case LCAST_ACT_E403:
        status = "403 Forbidden";
        snprintf(body, sizeof(body), "Not for web browsers\r\n");
        break;
    case LCAST_ACT_E429:
        status = "429 Too Many Requests";
        snprintf(body, sizeof(body), "Too many wrong logins, try again in %lu s\r\n", (unsigned long)retry_s);
        break;
    case LCAST_ACT_E503:
        status = "503 Service Unavailable";
        snprintf(body, sizeof(body), "At most %u rovers at a time\r\n", (unsigned)LOCAL_CASTER_MAX_CLIENTS);
        break;
    default:
        if (cap > 0) {
            buf[0] = '\0';
        }
        return 0;
    }
    size_t len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
    bool v2 = f != LCAST_FLAV_V1;
    put(buf, cap, &len, f == LCAST_FLAV_V2_CHUNKED ? "HTTP/1.1 " : "HTTP/1.0 ");
    put(buf, cap, &len, status);
    put(buf, cap, &len, "\r\n");
    if (v2) {
        put(buf, cap, &len, "Ntrip-Version: Ntrip/2.0\r\n");
    }
    put_server(buf, cap, &len, server_ver, now_unix, v2);
    if (action == LCAST_ACT_E401) {
        put(buf, cap, &len, "WWW-Authenticate: Basic realm=\"/");
        put(buf, cap, &len, realm_mount != NULL ? realm_mount : "");
        put(buf, cap, &len, "\"\r\n");
    } else if (action == LCAST_ACT_E429) {
        put(buf, cap, &len, "Retry-After: ");
        put_u32(buf, cap, &len, retry_s);
        put(buf, cap, &len, "\r\n");
    }
    put(buf, cap, &len, "Connection: close\r\nContent-Type: text/plain\r\nContent-Length: ");
    put_u32(buf, cap, &len, (uint32_t)strlen(body));
    put(buf, cap, &len, "\r\n\r\n");
    put(buf, cap, &len, body);
    return done(len, cap);
}

size_t lcast_reply_sourcetable(char *buf, size_t cap, lcast_flavour_t f, const char *str_record,
                               const char *server_ver, int64_t now_unix)
{
    static const char end[] = "ENDSOURCETABLE\r\n";
    const char *rec = str_record != NULL ? str_record : "";
    size_t body = strlen(rec) + 2u + sizeof(end) - 1u;
    size_t len = 0;
    if (cap > 0) {
        buf[0] = '\0';
    }
    if (f == LCAST_FLAV_V1) {
        put(buf, cap, &len, "SOURCETABLE 200 OK\r\n");
        put_server(buf, cap, &len, server_ver, now_unix, false);
        put(buf, cap, &len, "Content-Type: text/plain\r\n");
    } else {
        put(buf, cap, &len, f == LCAST_FLAV_V2_CHUNKED ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.0 200 OK\r\n");
        put(buf, cap, &len, "Ntrip-Version: Ntrip/2.0\r\n");
        put_server(buf, cap, &len, server_ver, now_unix, true);
        put(buf, cap, &len, "Connection: close\r\nContent-Type: gnss/sourcetable\r\n");
    }
    put(buf, cap, &len, "Content-Length: ");
    put_u32(buf, cap, &len, (uint32_t)body);
    put(buf, cap, &len, "\r\n\r\n");
    put(buf, cap, &len, rec);
    put(buf, cap, &len, "\r\n");
    put(buf, cap, &len, end);
    return done(len, cap);
}

/* ---- STR record ---------------------------------------------------------------------------- */

size_t lcast_sanitize(const char *in, char *out, size_t cap, bool str_field)
{
    if (cap == 0) {
        return 0;
    }
    size_t k = 0;
    for (const char *s = in != NULL ? in : ""; *s != '\0' && k + 1 < cap; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20u || c > 0x7Eu) {
            continue;
        }
        out[k++] = (str_field && c == ';') ? '_' : (char)c;
    }
    out[k] = '\0';
    return k;
}

static bool is_sbas_msm(uint16_t msg)
{
    return msg >= 1101u && msg <= 1107u;
}

static size_t digits(uint32_t v)
{
    size_t n = 1;
    while (v >= 10u) {
        v /= 10u;
        n++;
    }
    return n;
}

size_t lcast_str_msgs_from_obs(const lcast_obs_msg_t *obs, size_t n, lcast_str_msg_t *out, size_t cap)
{
    if (cap > LCAST_STR_MSGS_MAX) {
        cap = LCAST_STR_MSGS_MAX;
    }
    bool seen_any = false;
    for (size_t i = 0; i < n; i++) {
        if (!is_sbas_msm(obs[i].msg) && obs[i].count >= 1u && obs[i].age_ms <= 60000u) {
            seen_any = true;
            break;
        }
    }
    size_t k = 0;
    size_t chars = 0;
    /* Ascending by message number: pick the next larger one each round (n is small). */
    uint32_t last = 0;
    for (;;) {
        size_t best = n;
        for (size_t i = 0; i < n; i++) {
            const lcast_obs_msg_t *m = &obs[i];
            bool use = seen_any ? (m->count >= 1u && m->age_ms <= 60000u) : (m->expected_period_s > 0u);
            if (!use || is_sbas_msm(m->msg) || m->msg <= last) {
                continue;
            }
            if (best == n || m->msg < obs[best].msg) {
                best = i;
            }
        }
        if (best == n || k >= cap) {
            break;
        }
        const lcast_obs_msg_t *m = &obs[best];
        last = m->msg;
        uint32_t p;
        if (!seen_any) {
            p = m->expected_period_s;
        } else if (m->period_ms == UINT32_MAX) {
            p = m->expected_period_s > 0u ? m->expected_period_s : 1u;
        } else {
            p = (uint32_t)(((uint64_t)m->period_ms + 500u) / 1000u);
        }
        if (p == 0) {
            p = 1;
        }
        if (p > 65535u) {
            p = 65535u;
        }
        /* "msg(p)" plus a comma before every entry but the first */
        size_t need = digits(m->msg) + digits(p) + 2u + (k > 0 ? 1u : 0u);
        if (chars + need > LCAST_STR_DETAILS_MAX) {
            break;
        }
        chars += need;
        out[k].msg = m->msg;
        out[k].period_s = (uint16_t)p;
        k++;
    }
    return k;
}

static bool is_msm(uint16_t msg, uint16_t base)
{
    return msg >= base + 1u && msg <= base + 7u;
}

uint8_t lcast_str_carrier(uint8_t max_bands, const lcast_str_msg_t *msgs, size_t n)
{
    if (max_bands >= 2u) {
        return 2;
    }
    if (max_bands == 1u) {
        return 1;
    }
    static const uint16_t bases[] = { 1070, 1080, 1090, 1110, 1120, 1130 };
    for (size_t i = 0; i < n; i++) {
        for (size_t b = 0; b < sizeof(bases) / sizeof(bases[0]); b++) {
            if (is_msm(msgs[i].msg, bases[b])) {
                return 2;
            }
        }
    }
    return 0;
}

/* One sanitised field, then ';'. */
static void put_field(char *out, size_t cap, size_t *len, const char *v)
{
    char tmp[LCAST_STR_DETAILS_MAX + 1u];
    lcast_sanitize(v, tmp, sizeof(tmp), true);
    put(out, cap, len, tmp);
    put(out, cap, len, ";");
}

size_t lcast_str_build(const lcast_str_in_t *in, char *out, size_t cap)
{
    char details[LCAST_STR_DETAILS_MAX + 1u];
    size_t dl = 0;
    details[0] = '\0';
    size_t n = in->n < LCAST_STR_MSGS_MAX ? in->n : LCAST_STR_MSGS_MAX;
    for (size_t i = 0; i < n; i++) {
        char e[16];
        snprintf(e, sizeof(e), "%s%u(%u)", i > 0 ? "," : "", (unsigned)in->msgs[i].msg,
                 (unsigned)in->msgs[i].period_s);
        if (dl + strlen(e) > LCAST_STR_DETAILS_MAX) {
            break;
        }
        put(details, sizeof(details), &dl, e);
    }

    static const struct {
        uint16_t base;
        const char *name;
    } nav[] = { { 1070, "GPS" }, { 1080, "GLO" }, { 1090, "GAL" }, { 1120, "BDS" }, { 1110, "QZS" }, { 1130, "IRN" } };
    char navs[32];
    size_t nl = 0;
    navs[0] = '\0';
    for (size_t s = 0; s < sizeof(nav) / sizeof(nav[0]); s++) {
        for (size_t i = 0; i < n; i++) {
            if (is_msm(in->msgs[i].msg, nav[s].base)) {
                if (nl > 0) {
                    put(navs, sizeof(navs), &nl, "+");
                }
                put(navs, sizeof(navs), &nl, nav[s].name);
                break;
            }
        }
    }

    char gen[32];
    lcast_sanitize(in->generator, gen, sizeof(gen), true);
    if (gen[0] == '\0') {
        strcpy(gen, "onodiy");
    }
    char num[12];
    size_t len = 0;
    if (cap > 0) {
        out[0] = '\0';
    }
    put(out, cap, &len, "STR;");
    put_field(out, cap, &len, in->mount);
    put_field(out, cap, &len, in->identifier);
    put(out, cap, &len, "RTCM 3.3;");
    put_field(out, cap, &len, details);
    snprintf(num, sizeof(num), "%u", (unsigned)(in->carrier <= 2u ? in->carrier : 2u));
    put_field(out, cap, &len, num);
    put_field(out, cap, &len, navs);
    put(out, cap, &len, "onodiy;;0.00;0.00;0;0;");
    put_field(out, cap, &len, gen);
    put(out, cap, &len, "none;B;N;");
    put_u32(out, cap, &len, in->bitrate);
    put(out, cap, &len, ";");
    return done(len, cap);
}

/* ---- chunks -------------------------------------------------------------------------------- */

size_t lcast_chunk_wrap(uint8_t *buf, size_t len, size_t *start)
{
    if (len == 0 || len > LCAST_CHUNK_DATA) {
        return 0;
    }
    char head[LCAST_CHUNK_HEAD + 1u];
    int h = snprintf(head, sizeof(head), "%x\r\n", (unsigned)len);
    size_t s = LCAST_CHUNK_HEAD - (size_t)h;
    memcpy(buf + s, head, (size_t)h);
    buf[LCAST_CHUNK_HEAD + len] = '\r';
    buf[LCAST_CHUNK_HEAD + len + 1u] = '\n';
    *start = s;
    return (size_t)h + len + LCAST_CHUNK_TAIL;
}

/* ---- credentials --------------------------------------------------------------------------- */

static int b64_value(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+') {
        return 62;
    }
    if (c == '/') {
        return 63;
    }
    return -1;
}

bool lcast_b64_decode(const char *in, size_t n, uint8_t *out, size_t cap, size_t *out_len)
{
    *out_len = 0;
    /* Padding: at most two '=', only at the end, only to a multiple of 4. */
    size_t pad = 0;
    while (n > 0 && in[n - 1] == '=' && pad < 3) {
        n--;
        pad++;
    }
    if (pad > 2 || (pad > 0 && (n + pad) % 4u != 0)) {
        return false;
    }
    if (n % 4u == 1u) {
        return false;
    }
    size_t need = n / 4u * 3u + (n % 4u == 0 ? 0 : n % 4u - 1u);
    if (need > cap) {
        return false;
    }
    uint32_t acc = 0;
    int bits = 0;
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        int v = b64_value(in[i]);
        if (v < 0) {
            lcast_secure_zero(out, k);
            return false;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[k++] = (uint8_t)(acc >> bits);
        }
    }
    *out_len = k;
    return true;
}

bool lcast_cred_set(lcast_cred_t *c, const char *user, const char *pass)
{
    lcast_secure_zero(c, sizeof(*c));
    size_t ul = strlen(user);
    size_t pl = strlen(pass);
    if (ul + 1u + pl >= sizeof(c->v)) {
        return false;
    }
    memcpy(c->v, user, ul);
    c->v[ul] = ':';
    memcpy(c->v + ul + 1u, pass, pl);
    c->len = ul + 1u + pl;
    return true;
}

bool lcast_cred_match(const lcast_cred_t *expected, const char *got, size_t got_len)
{
    if (got_len >= LCAST_CRED_MAX) {
        return false;   /* the parser never gives more; the length is no secret */
    }
    uint8_t w[LCAST_CRED_MAX];
    memset(w, 0, sizeof(w));
    if (got_len > 0) {
        memcpy(w, got, got_len);
    }
    volatile uint32_t diff = (uint32_t)(expected->len ^ got_len);
    for (size_t i = 0; i < LCAST_CRED_MAX; i++) {
        diff |= (uint32_t)((uint8_t)expected->v[i] ^ w[i]);
    }
    lcast_secure_zero(w, sizeof(w));
    return diff == 0 && expected->len > 0;
}

/* ---- login throttle ------------------------------------------------------------------------ */

uint32_t lcast_throttle_delay_s(uint32_t fails)
{
    if (fails < LCAST_THROTTLE_FREE) {
        return 0;
    }
    uint32_t exp = fails - (LCAST_THROTTLE_FREE - 1u);   /* n - 4 */
    if (exp >= 9u) {
        return LCAST_THROTTLE_MAX_S;                       /* 2^9 = 512 > 300 */
    }
    uint32_t d = 1u << exp;
    return d > LCAST_THROTTLE_MAX_S ? LCAST_THROTTLE_MAX_S : d;
}

void lcast_throttles_init(lcast_throttles_t *t, uint32_t now_s)
{
    memset(t, 0, sizeof(*t));
    t->left = LCAST_THROTTLE_BURST;
    t->at_s = now_s;
}

static void bucket_refill(lcast_throttles_t *t, uint32_t now_s)
{
    if (t->left >= LCAST_THROTTLE_BURST) {
        t->left = LCAST_THROTTLE_BURST;
        t->at_s = now_s;   /* full: the refill clock starts at the next failure */
        return;
    }
    uint32_t add = (now_s - t->at_s) / LCAST_THROTTLE_REFILL_S;
    if (add == 0) {
        return;
    }
    if (add >= LCAST_THROTTLE_BURST - t->left) {
        t->left = LCAST_THROTTLE_BURST;
        t->at_s = now_s;
    } else {
        t->left += add;
        t->at_s += add * LCAST_THROTTLE_REFILL_S;
    }
}

static lcast_throttle_client_t *client_find(lcast_throttles_t *t, uint64_t key)
{
    for (size_t i = 0; i < LCAST_THROTTLE_CLIENTS; i++) {
        if (t->c[i].used && t->c[i].key == key) {
            return &t->c[i];
        }
    }
    return NULL;
}

uint32_t lcast_throttles_wait_s(lcast_throttles_t *t, uint64_t key, uint32_t now_s)
{
    bucket_refill(t, now_s);
    uint32_t wait = 0;
    if (t->left == 0) {
        uint32_t since = now_s - t->at_s;
        wait = since >= LCAST_THROTTLE_REFILL_S ? 1u : LCAST_THROTTLE_REFILL_S - since;
    }
    lcast_throttle_client_t *c = client_find(t, key);
    if (c != NULL) {
        c->seen_s = now_s;
        uint32_t delay = lcast_throttle_delay_s(c->fails);
        uint32_t since = now_s - c->last_s;
        uint32_t own = (delay == 0 || since >= delay) ? 0 : delay - since;
        if (own > wait) {
            wait = own;
        }
    }
    return wait;
}

uint32_t lcast_throttles_fail(lcast_throttles_t *t, uint64_t key, uint32_t now_s)
{
    bucket_refill(t, now_s);
    if (t->left > 0) {
        t->left--;
    }
    lcast_throttle_client_t *c = client_find(t, key);
    if (c == NULL) {
        /* A free entry, else the one seen longest ago. */
        c = &t->c[0];
        for (size_t i = 0; i < LCAST_THROTTLE_CLIENTS; i++) {
            lcast_throttle_client_t *e = &t->c[i];
            if (!e->used) {
                c = e;
                break;
            }
            if ((uint32_t)(now_s - e->seen_s) > (uint32_t)(now_s - c->seen_s)) {
                c = e;
            }
        }
        memset(c, 0, sizeof(*c));
        c->used = true;
        c->key = key;
    }
    c->seen_s = now_s;
    if (c->fails < UINT32_MAX) {
        c->fails++;
    }
    c->last_s = now_s;
    return c->fails;
}

void lcast_throttles_ok(lcast_throttles_t *t, uint64_t key)
{
    lcast_throttle_client_t *c = client_find(t, key);
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
    }
}

/* ---- journal gate and queue ------------------------------------------------------------------ */

void lcast_jgate_init(lcast_jgate_t *g, uint32_t now_s)
{
    memset(g, 0, sizeof(*g));
    g->tokens = LCAST_JGATE_TOKENS;
    g->at_s = now_s;
}

static void jgate_refill(lcast_jgate_t *g, uint32_t now_s)
{
    if (g->tokens >= LCAST_JGATE_TOKENS) {
        g->tokens = LCAST_JGATE_TOKENS;
        g->at_s = now_s;
        return;
    }
    uint32_t add = (now_s - g->at_s) / LCAST_JGATE_REFILL_S;
    if (add == 0) {
        return;
    }
    if (add >= LCAST_JGATE_TOKENS - g->tokens) {
        g->tokens = LCAST_JGATE_TOKENS;
        g->at_s = now_s;
    } else {
        g->tokens += add;
        g->at_s += add * LCAST_JGATE_REFILL_S;
    }
}

bool lcast_jgate_try_list(lcast_jgate_t *g, uint32_t age_s, uint32_t now_s, uint32_t *unlisted)
{
    if (age_s < LCAST_JGATE_AFTER_S) {
        return false;
    }
    jgate_refill(g, now_s);
    if (g->tokens == 0) {
        return false;
    }
    g->tokens--;
    *unlisted = g->unlisted;
    g->unlisted = 0;
    return true;
}

void lcast_jgate_unlisted_end(lcast_jgate_t *g)
{
    if (g->unlisted < UINT32_MAX) {
        g->unlisted++;
    }
}

void lcast_jrec_up(uint32_t conn, uint32_t version, const lcast_addr_t *peer, uint32_t age_s, uint32_t unlisted,
                   int32_t *a, int32_t *b)
{
    lcast_addr_t p = *peer;
    lcast_addr_unmap(&p);
    uint8_t q[4];
    lcast_addr_word(&p, q);
    *a = diag_rover_up_a(conn, version, p.v6, age_s, unlisted);
    *b = diag_rover_addr_b(q);
}

void lcast_jrec_down(uint32_t conn, local_caster_end_t reason, uint32_t age_s, int32_t *a, int32_t *b)
{
    *a = diag_rover_down_a(conn, (uint32_t)reason);
    *b = age_s > (uint32_t)INT32_MAX ? INT32_MAX : (int32_t)age_s;
}

bool lcast_jq_push(lcast_jq_t *q, uint16_t kind, int32_t a, int32_t b, uint32_t now_s)
{
    bool dropped = false;
    if (q->n >= LCAST_JQ_LEN) {
        lcast_jq_pop(q);
        dropped = true;
    }
    q->e[q->n] = (lcast_jq_entry_t) { .kind = kind, .a = a, .b = b, .at_s = now_s };
    q->n++;
    return dropped;
}

bool lcast_jq_head(lcast_jq_t *q, uint32_t now_s, lcast_jq_entry_t *out, uint32_t *expired)
{
    while (q->n > 0 && (uint32_t)(now_s - q->e[0].at_s) > LCAST_JQ_MAX_S) {
        lcast_jq_pop(q);
        if (expired != NULL) {
            (*expired)++;
        }
    }
    if (q->n == 0) {
        return false;
    }
    *out = q->e[0];
    return true;
}

void lcast_jq_pop(lcast_jq_t *q)
{
    if (q->n == 0) {
        return;
    }
    memmove(&q->e[0], &q->e[1], (size_t)(q->n - 1u) * sizeof(q->e[0]));
    q->n--;
}

/* ---- bit rate -------------------------------------------------------------------------------- */

void lcast_rate_init(lcast_rate_t *r)
{
    memset(r, 0, sizeof(*r));
}

void lcast_rate_sample(lcast_rate_t *r, uint32_t now_s, uint64_t bytes_in)
{
    if (r->n > 0) {
        uint32_t last = (r->head + LCAST_RATE_SAMPLES - 1u) % LCAST_RATE_SAMPLES;
        uint32_t since = now_s - r->at_s[last];
        if (since < LCAST_RATE_STEP_S) {
            return;
        }
        if (since > 2u * LCAST_RATE_STEP_S || bytes_in < r->bytes[last]) {
            lcast_rate_init(r);   /* a gap (the caster was off) or a counter reset: start over */
        }
    }
    r->bytes[r->head] = bytes_in;
    r->at_s[r->head] = now_s;
    r->head = (r->head + 1u) % LCAST_RATE_SAMPLES;
    if (r->n < LCAST_RATE_SAMPLES) {
        r->n++;
    }
}

uint32_t lcast_rate_bps(const lcast_rate_t *r)
{
    if (r->n < 2) {
        return 0;
    }
    uint32_t newest = (r->head + LCAST_RATE_SAMPLES - 1u) % LCAST_RATE_SAMPLES;
    uint32_t oldest = (r->head + LCAST_RATE_SAMPLES - r->n) % LCAST_RATE_SAMPLES;
    uint32_t span = r->at_s[newest] - r->at_s[oldest];
    if (span == 0) {
        return 0;
    }
    uint64_t bits = (r->bytes[newest] - r->bytes[oldest]) * 8u;
    uint64_t bps = bits / span;
    return bps > UINT32_MAX ? UINT32_MAX : (uint32_t)bps;
}

/* ---- stored config ----------------------------------------------------------------------------- */

bool lcast_name_ok(const char *s, size_t size)
{
    size_t n = strnlen(s, size);
    if (n == 0 || n == size) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' ||
                  c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool lcast_password_ok(const char *s)
{
    size_t n = strnlen(s, LOCAL_CASTER_PASS_LEN);
    if (n < LOCAL_CASTER_PASS_MIN || n == LOCAL_CASTER_PASS_LEN) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x21u || c > 0x7Eu) {
            return false;
        }
    }
    return true;
}

static bool refuse(const char *f, const char *r, const char **field, const char **rule)
{
    if (field != NULL) {
        *field = f;
    }
    if (rule != NULL) {
        *rule = r;
    }
    return false;
}

bool lcast_cfg_check(const local_caster_cfg_t *cfg, bool password_stored, const char **field, const char **rule)
{
    if (cfg == NULL) {
        return refuse(NULL, NULL, field, rule);
    }
    if (!lcast_name_ok(cfg->mountpoint, sizeof(cfg->mountpoint))) {
        return refuse("mountpoint", "1 to 47 letters, digits, dots, hyphens or underscores", field, rule);
    }
    if (!lcast_name_ok(cfg->username, sizeof(cfg->username))) {
        return refuse("username", "1 to 31 letters, digits, dots, hyphens or underscores", field, rule);
    }
    /* An empty password keeps the stored one. */
    bool given = cfg->password[0] != '\0';
    if (given && !lcast_password_ok(cfg->password)) {
        return refuse("password", "8 to 63 characters, letters, digits and symbols, no spaces", field, rule);
    }
    if (cfg->enabled && !given && !password_stored) {
        return refuse("password", "needed to turn this on", field, rule);
    }
    return true;
}

/* Pure, so it lives here: the web's parser and the host tests use it without the task. */
esp_err_t local_caster_check_config(const local_caster_cfg_t *cfg, bool password_stored, const char **field,
                                    const char **rule)
{
    return lcast_cfg_check(cfg, password_stored, field, rule) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

size_t lcast_cfg_encode(const local_caster_cfg_t *c, uint8_t *buf, size_t cap)
{
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, LCAST_CFG_VERSION);
    blob_put_bool(&w, c->enabled);
    blob_put_str(&w, c->mountpoint);
    blob_put_str(&w, c->username);
    blob_put_str(&w, c->password);
    if (!blob_writer_ok(&w)) {
        lcast_secure_zero(buf, cap);
        return 0;
    }
    return w.len;
}

bool lcast_cfg_decode(const uint8_t *buf, size_t len, local_caster_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    uint8_t version = blob_get_u8(&r);
    local_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = blob_get_bool(&r);
    blob_get_str(&r, c.mountpoint, sizeof(c.mountpoint));
    blob_get_str(&r, c.username, sizeof(c.username));
    blob_get_str(&r, c.password, sizeof(c.password));
    /* A later version only appends fields: what follows is not ours to read. */
    bool ok = blob_reader_ok(&r) && version >= LCAST_CFG_VERSION && lcast_name_ok(c.mountpoint, sizeof(c.mountpoint)) &&
              lcast_name_ok(c.username, sizeof(c.username)) &&
              (c.password[0] == '\0' || lcast_password_ok(c.password)) && (!c.enabled || c.password[0] != '\0');
    if (ok) {
        c.password_set = c.password[0] != '\0';
        *out = c;
    }
    lcast_secure_zero(&c, sizeof(c));
    return ok;
}

const char *lcast_end_name(local_caster_end_t reason)
{
    switch (reason) {
    case LOCAL_CASTER_END_CLOSED:
        return "closed";
    case LOCAL_CASTER_END_STALLED:
        return "stalled";
    case LOCAL_CASTER_END_LOST:
        return "lost";
    case LOCAL_CASTER_END_SETTINGS:
        return "settings";
    case LOCAL_CASTER_END_NETWORK:
        return "network";
    case LOCAL_CASTER_END_RESTART:
        return "restart";
    case LOCAL_CASTER_END_NONE:
    default:
        return "none";
    }
}
