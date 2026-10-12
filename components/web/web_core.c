/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_core.h"

#include <stdio.h>
#include <string.h>

#include "web_json.h"

/* ---- errors -------------------------------------------------------------------------- */

typedef struct {
    const char *code;
    int status;
    const char *message;
} err_row_t;

static const err_row_t s_errors[WEB_ERR_COUNT] = {
    [WEB_ERR_BAD_JSON] = { "bad_json", 400, "The request body is not valid JSON" },
    [WEB_ERR_INVALID_FIELD] = { "invalid_field", 400, "A field has an invalid value" },
    [WEB_ERR_CONFIRM_REQUIRED] = { "confirm_required", 400, "This action needs {\"confirm\": true}" },
    [WEB_ERR_BAD_REQUEST] = { "bad_request", 400, "Bad request" },
    [WEB_ERR_INVALID_IMAGE] = { "invalid_image", 400, "Not a valid app image for this chip" },
    [WEB_ERR_SIZE_MISMATCH] = { "size_mismatch", 400, "Fewer bytes arrived than announced" },
    [WEB_ERR_UNAUTHORIZED] = { "unauthorized", 401, "Log in first" },
    [WEB_ERR_FORBIDDEN] = { "forbidden", 403, "Request refused" },
    [WEB_ERR_WRONG_PASSWORD] = { "wrong_password", 403, "Wrong password" },
    [WEB_ERR_NOT_ON_SETUP_AP] = { "not_on_setup_ap", 403, "The first password can only be set over the station's own Wi-Fi" },
    [WEB_ERR_NOT_FOUND] = { "not_found", 404, "Not found" },
    [WEB_ERR_NOTHING_STAGED] = { "nothing_staged", 404, "No uploaded receiver package is waiting" },
    [WEB_ERR_NO_JOURNAL] = { "no_journal", 404, "No receiver update session is open" },
    [WEB_ERR_NO_COREDUMP] = { "no_coredump", 404, "No core dump stored" },
    [WEB_ERR_NOTHING_PENDING] = { "nothing_pending", 404, "No Wi-Fi credentials are pending" },
    [WEB_ERR_METHOD_NOT_ALLOWED] = { "method_not_allowed", 405, "Method not allowed" },
    [WEB_ERR_BUSY] = { "busy", 409, "Another operation is running" },
    [WEB_ERR_JOURNAL_OPEN] = { "journal_open", 409, "A receiver update session is open" },
    [WEB_ERR_NOT_WAITING] = { "not_waiting", 409, "The receiver update session is still running" },
    [WEB_ERR_APP_TRIAL] = { "app_trial", 409, "The new app image is still on trial" },
    [WEB_ERR_APP_UPDATE] = { "app_update", 409, "An app update is running" },
    [WEB_ERR_SAFE_MODE] = { "safe_mode", 409, "Not available in safe mode" },
    [WEB_ERR_NOT_IN_SAFE_MODE] = { "not_in_safe_mode", 409, "The station is not in safe mode" },
    [WEB_ERR_RECEIVER_NOT_STREAMING] = { "receiver_not_streaming", 409, "The receiver is not streaming" },
    [WEB_ERR_UNSUPPORTED_MODEL] = { "unsupported_model", 409, "This receiver model is not supported" },
    [WEB_ERR_PACKAGE_PINNED] = { "package_pinned", 409, "A receiver package is held by an open update session" },
    [WEB_ERR_MANIFEST_CHANGED] = { "manifest_changed", 409, "The offered update changed" },
    [WEB_ERR_NOT_OFFERED] = { "not_offered", 409, "This version is not offered" },
    [WEB_ERR_ROLLED_BACK_ACK_REQUIRED] = { "rolled_back_ack_required", 409, "This version was rolled back here before" },
    [WEB_ERR_REBOOT_INHIBITED] = { "reboot_inhibited", 409, "A reboot is held off right now" },
    [WEB_ERR_SETTINGS_UNAVAILABLE] = { "settings_unavailable", 409, "Settings cannot be stored this boot" },
    [WEB_ERR_NO_POSITION_YET] = { "no_position_yet", 409, "The receiver has not reported a position yet" },
    [WEB_ERR_CASTER_POLICY] = { "caster_policy", 409, "This caster combination is not allowed" },
    [WEB_ERR_SHA256_MISMATCH] = { "sha256_mismatch", 409, "The SHA-256 does not match the uploaded package" },
    [WEB_ERR_DOWNGRADE] = { "downgrade", 409, "The package is older than the receiver's firmware" },
    [WEB_ERR_NO_ROLLBACK_IMAGE] = { "no_rollback_image", 409, "No previous app image to go back to" },
    [WEB_ERR_ADMIN_ALREADY_SET] = { "admin_already_set", 409, "An admin password is already set" },
    [WEB_ERR_ADMIN_NOT_SET] = { "admin_not_set", 409, "No admin password set yet" },
    [WEB_ERR_RECEIVER_MAINTENANCE] = { "receiver_maintenance", 409,
                                       "Receiver maintenance: nothing is sent to the receiver" },
    [WEB_ERR_NOT_IN_MAINTENANCE] = { "not_in_maintenance", 409, "Receiver maintenance is not on" },
    [WEB_ERR_LIVE_VIEW_CLOSED] = { "live_view_closed", 409, "The live view is not open" },
    [WEB_ERR_LENGTH_REQUIRED] = { "length_required", 411, "Content-Length required" },
    [WEB_ERR_TOO_LARGE] = { "too_large", 413, "Request body too large" },
    [WEB_ERR_UNSUPPORTED_MEDIA_TYPE] = { "unsupported_media_type", 415, "Unsupported content type" },
    [WEB_ERR_BAD_HOST] = { "bad_host", 421, "Unknown host name" },
    [WEB_ERR_NOT_THIS_PROJECT] = { "not_this_project", 422, "The image belongs to another project" },
    [WEB_ERR_RATE_LIMITED] = { "rate_limited", 429, "Too many attempts, try again later" },
    [WEB_ERR_INTERNAL] = { "internal", 500, "Internal error" },
    [WEB_ERR_NOT_AVAILABLE_YET] = { "not_available_yet", 501, "Not available in this firmware version yet" },
    [WEB_ERR_CHECKS_DISABLED] = { "checks_disabled", 501, "This build cannot check for updates" },
    [WEB_ERR_TOO_MANY_CLIENTS] = { "too_many_clients", 503, "Too many live connections" },
    [WEB_ERR_NOT_NO_RECEIVER] = { "not_no_receiver", 409, "The station is not without a receiver: nothing to search for" },
    [WEB_ERR_SAME_IMAGE] = { "same_image", 409, "This image is the one already running (send X-Reinstall: 1 to install it again)" },
};

const char *web_err_code(web_err_t e)
{
    return (unsigned)e < WEB_ERR_COUNT ? s_errors[e].code : "internal";
}

const char *web_err_message(web_err_t e)
{
    return (unsigned)e < WEB_ERR_COUNT ? s_errors[e].message : "Internal error";
}

int web_err_status(web_err_t e)
{
    return (unsigned)e < WEB_ERR_COUNT ? s_errors[e].status : 500;
}

const char *web_status_line(int status)
{
    switch (status) {
    case 200: return "200 OK";
    case 202: return "202 Accepted";
    case 302: return "302 Found";
    case 304: return "304 Not Modified";
    case 400: return "400 Bad Request";
    case 401: return "401 Unauthorized";
    case 403: return "403 Forbidden";
    case 404: return "404 Not Found";
    case 405: return "405 Method Not Allowed";
    case 409: return "409 Conflict";
    case 411: return "411 Length Required";
    case 413: return "413 Content Too Large";
    case 415: return "415 Unsupported Media Type";
    case 421: return "421 Misdirected Request";
    case 422: return "422 Unprocessable Content";
    case 429: return "429 Too Many Requests";
    case 501: return "501 Not Implemented";
    case 503: return "503 Service Unavailable";
    default: return "500 Internal Server Error";
    }
}

/* ---- small string helpers ------------------------------------------------------------ */

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

static int hex_val(char c)
{
    if (is_digit(c)) {
        return c - '0';
    }
    c = lower(c);
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

/* Case-insensitive compare of n bytes. */
static bool eq_nocase_n(const char *a, const char *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (lower(a[i]) != lower(b[i])) {
            return false;
        }
    }
    return true;
}

static bool eq_nocase(const char *a, size_t alen, const char *b)
{
    size_t blen = strlen(b);
    return alen == blen && eq_nocase_n(a, b, alen);
}

/* ---- request checks ------------------------------------------------------------------- */

bool web_host_allowed(const char *host_hdr, const web_host_names_t *names)
{
    if (host_hdr == NULL || names == NULL) {
        return false;
    }
    size_t len = strlen(host_hdr);
    /* Trim surrounding blanks the parser may have kept. */
    while (len > 0 && (host_hdr[len - 1] == ' ' || host_hdr[len - 1] == '\t')) {
        len--;
    }
    while (len > 0 && (*host_hdr == ' ' || *host_hdr == '\t')) {
        host_hdr++;
        len--;
    }
    if (len == 0 || host_hdr[0] == '[') {
        return false;
    }
    /* Port: digits after the last ':' (a second ':' means it is not ours). */
    const char *colon = memchr(host_hdr, ':', len);
    size_t hlen = len;
    if (colon != NULL) {
        hlen = (size_t)(colon - host_hdr);
        size_t plen = len - hlen - 1;
        if (plen == 0 || plen > 5) {
            return false;
        }
        for (size_t i = 0; i < plen; i++) {
            if (!is_digit(colon[1 + i])) {
                return false;
            }
        }
    }
    if (hlen == 0) {
        return false;
    }
    for (size_t i = 0; i < hlen; i++) {
        if (!is_alnum(host_hdr[i]) && host_hdr[i] != '.' && host_hdr[i] != '-') {
            return false;
        }
    }
    if ((names->sta_ip != NULL && names->sta_ip[0] != '\0' && eq_nocase(host_hdr, hlen, names->sta_ip)) ||
        (names->ap_ip != NULL && names->ap_ip[0] != '\0' && eq_nocase(host_hdr, hlen, names->ap_ip))) {
        return true;
    }
    /* A name may come fully qualified, with one trailing dot (the target the
     * _http._tcp service names over DNS-SD, or a typed FQDN): the same name. */
    if (host_hdr[hlen - 1] == '.') {
        hlen--;
        if (hlen == 0) {
            return false;
        }
    }
    /* <mdns_host>.local: what the responder answers (the hostname, or the
     * name it moved to after a conflict). Only while it runs. */
    const char *mh = names->mdns_host;
    if (mh != NULL && mh[0] != '\0') {
        size_t mlen = strlen(mh);
        if (hlen == mlen + 6u && eq_nocase_n(host_hdr, mh, mlen) && eq_nocase(host_hdr + mlen, 6u, ".local")) {
            return true;
        }
    }
    /* The DHCP name: alone, or with a search domain that never resolves in public DNS. */
    const char *hn = names->hostname;
    if (hn == NULL || hn[0] == '\0') {
        return false;
    }
    size_t nlen = strlen(hn);
    if (hlen < nlen || !eq_nocase_n(host_hdr, hn, nlen)) {
        return false;
    }
    if (hlen == nlen) {
        return true;
    }
    if (host_hdr[nlen] != '.') {
        return false;
    }
    const char *suffix = host_hdr + nlen + 1;
    size_t slen = hlen - nlen - 1;
    static const char *const suffixes[] = { WEB_HOST_SUFFIXES };
    for (size_t i = 0; i < sizeof(suffixes) / sizeof(suffixes[0]); i++) {
        if (eq_nocase(suffix, slen, suffixes[i])) {
            return true;
        }
    }
    return false;
}

bool web_origin_ok(const char *origin, const char *host_hdr)
{
    static const char scheme[] = "http://";
    if (origin == NULL || host_hdr == NULL || host_hdr[0] == '\0') {
        return false;
    }
    size_t slen = sizeof(scheme) - 1;
    if (strlen(origin) < slen || !eq_nocase_n(origin, scheme, slen)) {
        return false;
    }
    const char *o = origin + slen;
    size_t olen = strlen(o);
    if (olen > 0 && o[olen - 1] == '/') {
        olen--;   /* tolerate a trailing slash */
    }
    return eq_nocase(o, olen, host_hdr);
}

bool web_content_type_is(const char *ct, const char *type)
{
    if (ct == NULL || type == NULL) {
        return false;
    }
    while (*ct == ' ' || *ct == '\t') {
        ct++;
    }
    size_t tlen = strlen(type);
    if (strlen(ct) < tlen || !eq_nocase_n(ct, type, tlen)) {
        return false;
    }
    const char *rest = ct + tlen;
    while (*rest == ' ' || *rest == '\t') {
        rest++;
    }
    return *rest == '\0' || *rest == ';';
}

bool web_cookie_sid(const char *cookie, char out[33])
{
    if (cookie == NULL) {
        return false;
    }
    const char *p = cookie;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ';') {
            p++;
        }
        const char *name = p;
        while (*p != '\0' && *p != '=' && *p != ';') {
            p++;
        }
        size_t nlen = (size_t)(p - name);
        while (nlen > 0 && (name[nlen - 1] == ' ' || name[nlen - 1] == '\t')) {
            nlen--;
        }
        if (*p != '=') {
            continue;
        }
        p++;
        const char *val = p;
        while (*p != '\0' && *p != ';') {
            p++;
        }
        size_t vlen = (size_t)(p - val);
        while (vlen > 0 && (val[vlen - 1] == ' ' || val[vlen - 1] == '\t')) {
            vlen--;
        }
        if (nlen == 3 && memcmp(name, "sid", 3) == 0 && vlen == 32) {
            bool ok = true;
            for (size_t i = 0; i < 32; i++) {
                if (hex_val(val[i]) < 0) {
                    ok = false;
                    break;
                }
                out[i] = lower(val[i]);
            }
            if (ok) {
                out[32] = '\0';
                return true;
            }
        }
    }
    return false;
}

void web_hex16(const uint8_t in[16], char out[33])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 16; i++) {
        out[2 * i] = hex[in[i] >> 4];
        out[2 * i + 1] = hex[in[i] & 0x0F];
    }
    out[32] = '\0';
}

static bool parse_hex(const char *s, uint8_t *out, size_t n)
{
    if (s == NULL || strlen(s) != 2 * n) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        int hi = hex_val(s[2 * i]);
        int lo = hex_val(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool web_parse_sha256_hex(const char *s, uint8_t out[32])
{
    return parse_hex(s, out, 32);
}

bool web_query_value(const char *query, const char *key, char *out, size_t out_size)
{
    if (query == NULL || key == NULL || out == NULL || out_size == 0) {
        return false;
    }
    size_t klen = strlen(key);
    const char *p = query;
    while (*p != '\0') {
        const char *pair = p;
        while (*p != '\0' && *p != '&') {
            p++;
        }
        size_t plen = (size_t)(p - pair);
        if (*p == '&') {
            p++;
        }
        if (plen < klen || memcmp(pair, key, klen) != 0 || (plen > klen && pair[klen] != '=')) {
            continue;
        }
        const char *v = plen > klen ? pair + klen + 1 : pair + klen;
        size_t vlen = plen > klen ? plen - klen - 1 : 0;
        size_t o = 0;
        for (size_t i = 0; i < vlen; i++) {
            char c = v[i];
            if (c == '+') {
                c = ' ';
            } else if (c == '%') {
                if (i + 2 >= vlen) {
                    return false;
                }
                int hi = hex_val(v[i + 1]);
                int lo = hex_val(v[i + 2]);
                if (hi < 0 || lo < 0) {
                    return false;
                }
                c = (char)((hi << 4) | lo);
                i += 2;
            }
            if (o + 1 >= out_size) {
                return false;
            }
            out[o++] = c;
        }
        out[o] = '\0';
        return true;
    }
    return false;
}

/* ---- sessions -------------------------------------------------------------------------- */

void web_sess_init(web_sessions_t *t)
{
    memset(t, 0, sizeof(*t));
    t->next_gen = 1;
}

static bool expired(const web_session_t *s, uint32_t now_s)
{
    return (uint32_t)(now_s - s->last_s) >= WEB_SESSION_IDLE_S || (uint32_t)(now_s - s->created_s) >= WEB_SESSION_LIFE_S;
}

int web_sess_create(web_sessions_t *t, const uint8_t token[16], uint32_t now_s)
{
    int idx = -1;
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        if (!t->s[i].used || expired(&t->s[i], now_s)) {
            idx = (int)i;
            break;
        }
    }
    if (idx < 0) {
        /* Least recently used: the longest time since its last request. */
        uint32_t oldest = 0;
        for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
            uint32_t age = now_s - t->s[i].last_s;
            if (idx < 0 || age > oldest) {
                oldest = age;
                idx = (int)i;
            }
        }
    }
    web_session_t *s = &t->s[idx];
    memset(s, 0, sizeof(*s));
    s->used = true;
    memcpy(s->token, token, sizeof(s->token));
    s->created_s = now_s;
    s->last_s = now_s;
    s->gen = t->next_gen++;
    if (t->next_gen == 0) {
        t->next_gen = 1;
    }
    return idx;
}

int web_sess_find(web_sessions_t *t, const uint8_t token[16], uint32_t now_s)
{
    int found = -1;
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        web_session_t *s = &t->s[i];
        if (!s->used) {
            continue;
        }
        uint8_t diff = 0;
        for (size_t k = 0; k < sizeof(s->token); k++) {
            diff |= (uint8_t)(s->token[k] ^ token[k]);
        }
        if (diff == 0) {
            found = (int)i;
        }
    }
    if (found >= 0 && expired(&t->s[found], now_s)) {
        web_sess_end(t, found);
        return -1;
    }
    return found;
}

void web_sess_touch(web_sessions_t *t, int idx, uint32_t now_s)
{
    if (idx >= 0 && (size_t)idx < WEB_SESSIONS_MAX && t->s[idx].used) {
        t->s[idx].last_s = now_s;
    }
}

uint32_t web_sess_left_s(const web_sessions_t *t, int idx, uint32_t now_s)
{
    if (idx < 0 || (size_t)idx >= WEB_SESSIONS_MAX || !t->s[idx].used) {
        return 0;
    }
    const web_session_t *s = &t->s[idx];
    uint32_t idle = now_s - s->last_s;
    uint32_t life = now_s - s->created_s;
    uint32_t idle_left = idle >= WEB_SESSION_IDLE_S ? 0 : WEB_SESSION_IDLE_S - idle;
    uint32_t life_left = life >= WEB_SESSION_LIFE_S ? 0 : WEB_SESSION_LIFE_S - life;
    return idle_left < life_left ? idle_left : life_left;
}

void web_sess_end(web_sessions_t *t, int idx)
{
    if (idx >= 0 && (size_t)idx < WEB_SESSIONS_MAX) {
        uint32_t gen = t->s[idx].gen;
        memset(&t->s[idx], 0, sizeof(t->s[idx]));
        t->s[idx].gen = gen;   /* kept, so web_sess_alive() of old clients stays false */
    }
}

uint32_t web_sess_end_all_except(web_sessions_t *t, int keep)
{
    uint32_t mask = 0;
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        if ((int)i != keep && t->s[i].used) {
            web_sess_end(t, (int)i);
            mask |= 1u << i;
        }
    }
    return mask;
}

uint32_t web_sess_expire(web_sessions_t *t, uint32_t now_s)
{
    uint32_t mask = 0;
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        if (t->s[i].used && expired(&t->s[i], now_s)) {
            web_sess_end(t, (int)i);
            mask |= 1u << i;
        }
    }
    return mask;
}

bool web_sess_alive(const web_sessions_t *t, int idx, uint32_t gen)
{
    return idx >= 0 && (size_t)idx < WEB_SESSIONS_MAX && t->s[idx].used && t->s[idx].gen == gen;
}

/* ---- login throttling -------------------------------------------------------------------- */

uint32_t web_throttle_delay_s(uint32_t fails)
{
    if (fails < WEB_THROTTLE_FREE) {
        return 0;
    }
    uint32_t exp = fails - (WEB_THROTTLE_FREE - 1u);   /* n - 4 */
    if (exp >= 9) {
        return WEB_THROTTLE_MAX_S;                      /* 2^9 = 512 > 300 */
    }
    uint32_t d = 1u << exp;
    return d > WEB_THROTTLE_MAX_S ? WEB_THROTTLE_MAX_S : d;
}

uint32_t web_throttle_wait_s(const web_throttle_t *t, uint32_t now_s)
{
    uint32_t delay = web_throttle_delay_s(t->fails);
    if (delay == 0) {
        return 0;
    }
    uint32_t since = now_s - t->last_s;
    return since >= delay ? 0 : delay - since;
}

void web_throttle_fail(web_throttle_t *t, uint32_t now_s)
{
    if (t->fails < UINT32_MAX) {
        t->fails++;
    }
    t->last_s = now_s;
}

void web_throttle_ok(web_throttle_t *t)
{
    t->fails = 0;
    t->last_s = 0;
}

void web_throttles_init(web_throttles_t *t, uint32_t now_s)
{
    memset(t, 0, sizeof(*t));
    for (size_t i = 0; i < 2; i++) {
        t->iface[i].left = WEB_THROTTLE_IFACE_BURST;
        t->iface[i].at_s = now_s;
    }
}

static void bucket_refill(web_throttle_bucket_t *b, uint32_t now_s)
{
    if (b->left >= WEB_THROTTLE_IFACE_BURST) {
        b->left = WEB_THROTTLE_IFACE_BURST;
        b->at_s = now_s;   /* full: the refill clock starts at the next failure */
        return;
    }
    uint32_t add = (now_s - b->at_s) / WEB_THROTTLE_IFACE_REFILL_S;
    if (add == 0) {
        return;
    }
    if (add >= WEB_THROTTLE_IFACE_BURST - b->left) {
        b->left = WEB_THROTTLE_IFACE_BURST;
        b->at_s = now_s;
    } else {
        b->left += add;
        b->at_s += add * WEB_THROTTLE_IFACE_REFILL_S;
    }
}

static web_throttle_client_t *client_find(web_throttles_t *t, uint64_t key, bool ap)
{
    for (size_t i = 0; i < WEB_THROTTLE_CLIENTS; i++) {
        web_throttle_client_t *c = &t->c[i];
        if (c->used && c->key == key && c->ap == ap) {
            return c;
        }
    }
    return NULL;
}

uint32_t web_throttles_wait_s(web_throttles_t *t, uint64_t key, bool ap, uint32_t now_s)
{
    web_throttle_bucket_t *b = &t->iface[ap ? 1 : 0];
    bucket_refill(b, now_s);
    uint32_t wait = 0;
    if (b->left == 0) {
        uint32_t since = now_s - b->at_s;
        wait = since >= WEB_THROTTLE_IFACE_REFILL_S ? 1u : WEB_THROTTLE_IFACE_REFILL_S - since;
    }
    web_throttle_client_t *c = client_find(t, key, ap);
    if (c != NULL) {
        c->seen_s = now_s;
        uint32_t own = web_throttle_wait_s(&c->t, now_s);
        if (own > wait) {
            wait = own;
        }
    }
    return wait;
}

uint32_t web_throttles_fail(web_throttles_t *t, uint64_t key, bool ap, uint32_t now_s)
{
    web_throttle_bucket_t *b = &t->iface[ap ? 1 : 0];
    bucket_refill(b, now_s);
    if (b->left > 0) {
        b->left--;
    }
    web_throttle_client_t *c = client_find(t, key, ap);
    if (c == NULL) {
        /* A free entry, else the one seen longest ago. */
        c = &t->c[0];
        for (size_t i = 0; i < WEB_THROTTLE_CLIENTS; i++) {
            web_throttle_client_t *e = &t->c[i];
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
        c->ap = ap;
    }
    c->seen_s = now_s;
    web_throttle_fail(&c->t, now_s);
    return c->t.fails;
}

void web_throttles_ok(web_throttles_t *t, uint64_t key, bool ap)
{
    web_throttle_client_t *c = client_find(t, key, ap);
    if (c != NULL) {
        memset(c, 0, sizeof(*c));
    }
}

/* ---- field rules --------------------------------------------------------------------------- */

bool web_no_ctrl(const char *s)
{
    if (s == NULL) {
        return false;
    }
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    return true;
}

bool web_password_valid(const char *pw)
{
    if (pw == NULL) {
        return false;
    }
    size_t len = strlen(pw);
    if (len < 8 || len > 64 || !web_no_ctrl(pw)) {
        return false;
    }
    const uint8_t *p = (const uint8_t *)pw;
    size_t i = 0;
    while (i < len) {
        size_t n = wj_utf8_seq_len(p + i, len - i);
        if (n == 0) {
            return false;
        }
        /* C1 control characters (U+0080..U+009F) are control characters too. */
        if (n == 2 && p[i] == 0xC2 && p[i + 1] < 0xA0) {
            return false;
        }
        i += n;
    }
    return true;
}

bool web_hostname_valid(const char *name)
{
    if (name == NULL) {
        return false;
    }
    size_t len = strlen(name);
    if (len < 1 || len > 32 || name[0] == '-' || name[len - 1] == '-') {
        return false;
    }
    return web_chars_ok(name, "-");
}

bool web_ssid_valid(const char *ssid)
{
    if (ssid == NULL) {
        return false;
    }
    size_t len = strlen(ssid);
    return len >= 1 && len <= 32;
}

static bool printable_ascii(const char *s)
{
    for (; *s != '\0'; s++) {
        if ((unsigned char)*s < 0x20 || (unsigned char)*s > 0x7E) {
            return false;
        }
    }
    return true;
}

bool web_wifi_password_valid(const char *pw)
{
    if (pw == NULL) {
        return false;
    }
    size_t len = strlen(pw);
    if (len == 0) {
        return true;
    }
    if (len == 64) {
        for (size_t i = 0; i < len; i++) {
            if (hex_val(pw[i]) < 0) {
                return false;
            }
        }
        return true;
    }
    return len >= 8 && len <= 63 && printable_ascii(pw);
}

bool web_ap_password_valid(const char *pw)
{
    if (pw == NULL) {
        return false;
    }
    size_t len = strlen(pw);
    return len >= 8 && len <= 63 && printable_ascii(pw);
}

bool web_chars_ok(const char *s, const char *extra)
{
    if (s == NULL) {
        return false;
    }
    for (; *s != '\0'; s++) {
        if (!is_alnum(*s) && (extra == NULL || *s == '\0' || strchr(extra, *s) == NULL)) {
            return false;
        }
    }
    return true;
}

static bool ipv4_literal(const char *s)
{
    int parts = 0;
    const char *p = s;
    while (parts < 4) {
        if (!is_digit(*p)) {
            return false;
        }
        unsigned v = 0;
        int digits = 0;
        while (is_digit(*p)) {
            v = v * 10u + (unsigned)(*p - '0');
            digits++;
            p++;
            if (digits > 3 || v > 255) {
                return false;
            }
        }
        parts++;
        if (parts < 4) {
            if (*p != '.') {
                return false;
            }
            p++;
        }
    }
    return *p == '\0';
}

bool web_host_name_ok(const char *host)
{
    if (host == NULL) {
        return false;
    }
    size_t len = strlen(host);
    if (len == 0 || len > 63) {
        return false;
    }
    if (ipv4_literal(host)) {
        return true;
    }
    size_t label = 0;
    bool all_digits_label = true;
    for (size_t i = 0; i <= len; i++) {
        char c = host[i];
        if (c == '.' || c == '\0') {
            if (label == 0 || label > 63 || host[i - 1] == '-') {
                return false;
            }
            label = 0;
            if (c == '\0') {
                break;
            }
            all_digits_label = true;
            continue;
        }
        if (!is_alnum(c) && c != '-') {
            return false;
        }
        if (label == 0 && c == '-') {
            return false;
        }
        if (!is_digit(c)) {
            all_digits_label = false;
        }
        label++;
    }
    /* A name whose last label is all digits is a malformed IP address. */
    return !all_digits_label;
}

uint32_t web_expected_build(const char *fw)
{
    if (fw == NULL || strlen(fw) > 47 || !web_no_ctrl(fw)) {
        return 0;
    }
    const char *b = strstr(fw, "Build");
    if (b == NULL) {
        return 0;
    }
    b += 5;
    uint64_t v = 0;
    int digits = 0;
    while (is_digit(*b) && digits < 10) {
        v = v * 10u + (uint64_t)(*b - '0');
        b++;
        digits++;
    }
    if (digits == 0 || v == 0 || v > UINT32_MAX) {
        return 0;
    }
    return (uint32_t)v;
}

bool web_maint_duration_ok(uint32_t s)
{
    static const uint32_t choices[] = WEB_MAINT_CHOICES_S;
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); i++) {
        if (choices[i] == s) {
            return true;
        }
    }
    return false;
}

/* ---- caster policy ------------------------------------------------------------------------- */

const char *web_policy_reason(ntrip_policy_t p)
{
    switch (p) {
    case NTRIP_POLICY_SECOND_ONOCOY:
        return "second_onocoy";
    case NTRIP_POLICY_SECOND_RTK2GO:
        return "second_rtk2go";
    case NTRIP_POLICY_NEED_DUP_ACK:
        return "dup_stream_ack_required";
    default:
        return NULL;
    }
}

/* ---- app image check ------------------------------------------------------------------------ */

static void copy_field(const uint8_t *src, char out[33])
{
    size_t n = 0;
    while (n < WEB_DESC_FIELD_LEN && src[n] != '\0') {
        n++;
    }
    memcpy(out, src, n);
    out[n] = '\0';
}

web_image_check_t web_image_check(const uint8_t *head, size_t len, uint16_t chip_id, const char *project,
                                  web_image_info_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (head == NULL || len < WEB_IMAGE_HEAD_MIN || head[0] != WEB_IMAGE_MAGIC) {
        return WEB_IMAGE_NOT_APP;
    }
    uint16_t chip = (uint16_t)(head[WEB_IMAGE_CHIP_ID_OFS] | ((uint16_t)head[WEB_IMAGE_CHIP_ID_OFS + 1] << 8));
    if (chip != chip_id) {
        return WEB_IMAGE_NOT_APP;
    }
    const uint8_t *d = head + WEB_IMAGE_DESC_OFS;
    uint32_t magic = (uint32_t)d[0] | ((uint32_t)d[1] << 8) | ((uint32_t)d[2] << 16) | ((uint32_t)d[3] << 24);
    if (magic != WEB_DESC_MAGIC) {
        return WEB_IMAGE_NOT_APP;
    }
    web_image_info_t info;
    memset(&info, 0, sizeof(info));
    copy_field(d + WEB_DESC_VERSION_OFS, info.version);
    copy_field(d + WEB_DESC_PROJECT_OFS, info.project);
    if (len >= WEB_IMAGE_HEAD_SHA) {
        info.has_elf_sha = true;
        memcpy(info.elf_sha256, d + WEB_DESC_ELF_SHA_OFS, sizeof(info.elf_sha256));
    }
    if (out != NULL) {
        *out = info;
    }
    if (project != NULL && strcmp(info.project, project) != 0) {
        return WEB_IMAGE_OTHER_PROJECT;
    }
    return WEB_IMAGE_OK;
}

web_err_t web_app_upload_verdict(web_image_check_t chk, bool rolled_back, bool rolled_back_ack, bool same_image,
                                 bool reinstall)
{
    switch (chk) {
    case WEB_IMAGE_OK:
        if (same_image && !reinstall) {
            return WEB_ERR_SAME_IMAGE;
        }
        return (rolled_back && !rolled_back_ack) ? WEB_ERR_ROLLED_BACK_ACK_REQUIRED : WEB_ERR_COUNT;
    case WEB_IMAGE_OTHER_PROJECT:
        return WEB_ERR_NOT_THIS_PROJECT;
    default:
        return WEB_ERR_INVALID_IMAGE;
    }
}

const char *web_app_refusal_result(web_err_t verdict)
{
    switch (verdict) {
    case WEB_ERR_SAME_IMAGE:
        return "same_image";
    case WEB_ERR_ROLLED_BACK_ACK_REQUIRED:
        return "rolled_back_ack_required";
    default:
        return "invalid_image";
    }
}

/* ---- status deltas, dates, names --------------------------------------------------------------- */

uint32_t web_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void web_utc_date(int32_t utc_day, char out[11])
{
    /* Days to civil date (H. Hinnant's algorithm). */
    int64_t z = (int64_t)utc_day + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
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
    if (y < 0 || y > 9999) {
        strcpy(out, "0000-00-00");
        return;
    }
    char tmp[40];
    snprintf(tmp, sizeof(tmp), "%04u-%02u-%02u", (unsigned)y, (unsigned)m, (unsigned)d);
    memcpy(out, tmp, 10);
    out[10] = '\0';
}

void web_safe_filename_part(const char *in, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    size_t o = 0;
    for (; in != NULL && *in != '\0' && o + 1 < out_size; in++) {
        char c = *in;
        out[o++] = (is_alnum(c) || c == '.' || c == '_' || c == '+' || c == '-') ? c : '_';
    }
    out[o] = '\0';
}

/* ---- WebSocket client queue --------------------------------------------------------------------- */

web_wsq_decision_t web_wsq_admit(const web_msg_kind_t *kinds, const size_t *sizes, size_t count,
                                 web_msg_kind_t kind, size_t size)
{
    web_wsq_decision_t d = { .accept = false, .evict = -1 };
    size_t bytes = 0;
    for (size_t i = 0; i < count; i++) {
        bytes += sizes[i];
    }
    if (size > WEB_WSQ_MAX_BYTES) {
        return d;
    }
    if (kind == WEB_MSG_PROGRESS) {
        /* Only the newest progress message counts: replace a queued one. */
        for (size_t i = 0; i < count; i++) {
            if (kinds[i] == WEB_MSG_PROGRESS) {
                if (bytes - sizes[i] + size <= WEB_WSQ_MAX_BYTES) {
                    d.accept = true;
                    d.evict = (int)i;
                }
                return d;
            }
        }
    }
    bool fits = count < WEB_WSQ_MAX_MSGS && bytes + size <= WEB_WSQ_MAX_BYTES;
    if (fits) {
        d.accept = true;
        return d;
    }
    if (kind == WEB_MSG_EVENT) {
        for (size_t i = 0; i < count; i++) {
            if (kinds[i] == WEB_MSG_EVENT && bytes - sizes[i] + size <= WEB_WSQ_MAX_BYTES) {
                d.accept = true;
                d.evict = (int)i;
                return d;
            }
        }
    }
    return d;
}
