/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_core.h"

#include <stdio.h>
#include <string.h>

#include "settings_codec.h"
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
    [WEB_ERR_UNAUTHORIZED] = { "unauthorized", 401, "Login first" },
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
    [WEB_ERR_NEEDS_FIRMWARE] = { "needs_firmware", 409, "This receiver firmware does not have that command" },
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
    [WEB_ERR_SLOT_DISABLED] = { "slot_disabled", 409, "The caster slot is not enabled" },
    [WEB_ERR_SIGNAL_TEST_RUNNING] = { "signal_test_running", 409,
                                      "A signal-group comparison runs: stop it first" },
    [WEB_ERR_SIGNAL_TEST_IDLE] = { "signal_test_idle", 409, "No signal-group comparison runs" },
    [WEB_ERR_SIGNAL_TEST_NOT_DECIDING] = { "signal_test_not_deciding", 409,
                                           "The comparison is not waiting for your choice yet" },
    [WEB_ERR_FIRST_APPLY_HELD] = { "first_apply_held", 409,
                                   "The receiver's first configuration waits for you: apply it first" },
    [WEB_ERR_AUTOCFG_OFF] = { "autocfg_off", 409, "Automatic receiver configuration is off" },
    [WEB_ERR_IDENTITY_UNCONFIRMED] = { "identity_unconfirmed", 409, "The receiver has not identified itself yet" },
    [WEB_ERR_CONFIG_FAILED] = { "config_failed", 409, "The last receiver configuration failed" },
    [WEB_ERR_POSITION_NOT_LOCKED] = { "position_not_locked", 409,
                                      "Lock the position first: every receiver restart would survey again" },
    [WEB_ERR_NO_RECEIVER] = { "no_receiver", 409, "No receiver is connected" },
    [WEB_ERR_PACKAGE_NEEDED] = { "package_needed", 409,
                                 "Upload the same receiver package again: the station kept it only until the restart" },
    [WEB_ERR_QUERY_RUNNING] = { "query_running", 409, "A receiver query runs: wait for its answer" },
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

/* ---- response headers ------------------------------------------------------------------ */

static const web_hdr_t s_common_hdrs[] = {
    { "X-Content-Type-Options", "nosniff" },
    { "X-Frame-Options", "DENY" },
    { "Referrer-Policy", "no-referrer" },
    { "Cache-Control", "no-store" },   /* API answers only: the last row */
};
#define COMMON_HDRS_ALL (sizeof(s_common_hdrs) / sizeof(s_common_hdrs[0]))

const web_hdr_t *web_common_header(size_t i, bool api)
{
    size_t n = api ? COMMON_HDRS_ALL : COMMON_HDRS_ALL - 1u;
    return i < n ? &s_common_hdrs[i] : NULL;
}

/* Append `text` at *len; false (and nothing more is written) when it does not fit with its terminator. */
static bool head_put(char *buf, size_t cap, size_t *len, const char *text)
{
    size_t n = strlen(text);
    if (*len + n + 1u > cap) {
        return false;
    }
    memcpy(buf + *len, text, n + 1u);
    *len += n;
    return true;
}

static bool head_hdr(char *buf, size_t cap, size_t *len, const char *name, const char *value)
{
    return head_put(buf, cap, len, name) && head_put(buf, cap, len, ": ") && head_put(buf, cap, len, value) &&
           head_put(buf, cap, len, "\r\n");
}

size_t web_stream_head(char *buf, size_t cap, int status, const char *type, const char *disposition, bool api,
                       int64_t content_length)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    size_t len = 0;
    bool ok = head_put(buf, cap, &len, "HTTP/1.1 ") && head_put(buf, cap, &len, web_status_line(status)) &&
              head_put(buf, cap, &len, "\r\n") &&
              head_hdr(buf, cap, &len, "Content-Type", type != NULL ? type : "application/octet-stream");
    const web_hdr_t *h;
    for (size_t i = 0; ok && (h = web_common_header(i, api)) != NULL; i++) {
        ok = head_hdr(buf, cap, &len, h->name, h->value);
    }
    if (ok && disposition != NULL) {
        ok = head_hdr(buf, cap, &len, "Content-Disposition", disposition);
    }
    if (ok) {
        ok = head_hdr(buf, cap, &len, "Connection", "close");
    }
    if (ok && content_length >= 0) {
        char n[24];
        snprintf(n, sizeof(n), "%lld", (long long)content_length);
        ok = head_hdr(buf, cap, &len, "Content-Length", n);
    }
    if (ok) {
        ok = head_put(buf, cap, &len, "\r\n");
    }
    if (!ok) {
        buf[0] = '\0';
        return 0;
    }
    return len;
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
    uint32_t life = now_s - s->created_s;
    if (s->remember) {
        return life >= WEB_SESSION_REMEMBER_S;
    }
    return (uint32_t)(now_s - s->last_s) >= WEB_SESSION_IDLE_S || life >= WEB_SESSION_LIFE_S;
}

/* The least recently used session (the longest time since its last request) among the remembered ones or the others. */
static int least_recent(const web_sessions_t *t, bool remember, uint32_t now_s)
{
    int idx = -1;
    uint32_t oldest = 0;
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        uint32_t age = now_s - t->s[i].last_s;
        if (t->s[i].remember == remember && (idx < 0 || age > oldest)) {
            oldest = age;
            idx = (int)i;
        }
    }
    return idx;
}

static int free_slot(const web_sessions_t *t, uint32_t now_s)
{
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        if (!t->s[i].used || expired(&t->s[i], now_s)) {
            return (int)i;
        }
    }
    return -1;
}

static web_session_t *take(web_sessions_t *t, int idx, const uint8_t key[WEB_SESSION_KEY_LEN], bool remember,
                           uint32_t created_s, uint32_t now_s)
{
    web_session_t *s = &t->s[idx];
    memset(s, 0, sizeof(*s));
    s->used = true;
    s->remember = remember;
    memcpy(s->key, key, sizeof(s->key));
    s->created_s = created_s;
    s->last_s = now_s;
    s->gen = t->next_gen++;
    if (t->next_gen == 0) {
        t->next_gen = 1;
    }
    return s;
}

int web_sess_create(web_sessions_t *t, const uint8_t key[WEB_SESSION_KEY_LEN], bool remember, uint32_t now_s,
                    int64_t now_unix)
{
    int idx = free_slot(t, now_s);
    if (idx < 0) {
        idx = least_recent(t, false, now_s);
    }
    if (idx < 0) {
        idx = least_recent(t, true, now_s);
    }
    web_session_t *s = take(t, idx, key, remember, now_s, now_s);
    if (remember && now_unix > 0) {
        s->end_unix = now_unix + WEB_SESSION_REMEMBER_S;
    }
    web_sess_mark(t, now_unix, now_s);
    return idx;
}

int web_sess_find(web_sessions_t *t, const uint8_t key[WEB_SESSION_KEY_LEN], uint32_t now_s)
{
    int found = -1;
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        web_session_t *s = &t->s[i];
        if (!s->used) {
            continue;
        }
        uint8_t diff = 0;
        for (size_t k = 0; k < sizeof(s->key); k++) {
            diff |= (uint8_t)(s->key[k] ^ key[k]);
        }
        if (diff == 0) {
            found = (int)i;
        }
    }
    return found >= 0 && expired(&t->s[found], now_s) ? -1 : found;
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
    uint32_t life = now_s - s->created_s;
    if (s->remember) {
        return life >= WEB_SESSION_REMEMBER_S ? 0 : WEB_SESSION_REMEMBER_S - life;
    }
    uint32_t idle = now_s - s->last_s;
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

void web_sess_mark(web_sessions_t *t, int64_t unix_s, uint32_t now_s)
{
    if (unix_s > 0 && unix_s > web_sess_mark_now(t, now_s, 0)) {
        t->mark_unix = unix_s;
        t->mark_s = now_s;
    }
}

int64_t web_sess_mark_now(const web_sessions_t *t, uint32_t now_s, int64_t now_unix)
{
    int64_t carried = t->mark_unix > 0 ? t->mark_unix + (int64_t)(uint32_t)(now_s - t->mark_s) : 0;
    return now_unix > carried ? now_unix : carried;
}

bool web_sess_mark_due(int64_t stored_mark, int64_t mark_now)
{
    return mark_now > 0 && mark_now - stored_mark >= (int64_t)WEB_SESS_MARK_EVERY_S;
}

uint32_t web_sess_clock(web_sessions_t *t, uint32_t now_s, int64_t now_unix, bool *changed)
{
    uint32_t mask = 0;
    *changed = false;
    if (now_unix <= 0) {
        return 0;
    }
    web_sess_mark(t, now_unix, now_s);
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        web_session_t *s = &t->s[i];
        if (!s->used || !s->remember) {
            continue;
        }
        uint32_t left = web_sess_left_s(t, (int)i, now_s);
        if (s->end_unix == 0) {
            s->end_unix = now_unix + left;
            *changed = true;
        } else if (s->end_unix <= now_unix) {
            web_sess_end(t, (int)i);
            mask |= 1u << i;
            continue;
        } else if (s->end_unix - now_unix > (int64_t)WEB_SESSION_REMEMBER_S) {
            s->end_unix = now_unix + WEB_SESSION_REMEMBER_S;   /* the clock went back, or was ahead at the login */
            *changed = true;
        }
        if (s->end_unix - now_unix < (int64_t)left) {
            s->created_s = now_s - (WEB_SESSION_REMEMBER_S - (uint32_t)(s->end_unix - now_unix));
        }
    }
    return mask;
}

size_t web_sess_store_encode(const web_sessions_t *t, const uint8_t bind[WEB_SESS_BIND_LEN], int64_t mark_unix,
                             uint8_t *buf, size_t cap, size_t *count)
{
    size_t n = 0;
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        n += t->s[i].used && t->s[i].remember && t->s[i].end_unix > 0;
    }
    *count = n;
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, WEB_SESS_STORE_VERSION);
    for (size_t i = 0; i < WEB_SESS_BIND_LEN; i++) {
        blob_put_u8(&w, bind[i]);
    }
    blob_put_i64(&w, mark_unix);
    blob_put_u8(&w, (uint8_t)n);
    for (size_t i = 0; i < WEB_SESSIONS_MAX; i++) {
        const web_session_t *s = &t->s[i];
        if (!s->used || !s->remember || s->end_unix <= 0) {
            continue;
        }
        for (size_t k = 0; k < WEB_SESSION_KEY_LEN; k++) {
            blob_put_u8(&w, s->key[k]);
        }
        blob_put_i64(&w, s->end_unix);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}

bool web_sess_store_decode(const uint8_t *buf, size_t len, uint8_t bind[WEB_SESS_BIND_LEN], int64_t *mark_unix,
                           web_sess_rec_t out[WEB_SESSIONS_MAX], size_t *count)
{
    if (buf == NULL) {
        return false;
    }
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    uint8_t version = blob_get_u8(&r);
    for (size_t i = 0; i < WEB_SESS_BIND_LEN; i++) {
        bind[i] = blob_get_u8(&r);
    }
    int64_t mark = blob_get_i64(&r);
    uint8_t n = blob_get_u8(&r);
    if (!blob_reader_ok(&r) || version != WEB_SESS_STORE_VERSION || n > WEB_SESSIONS_MAX) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        for (size_t k = 0; k < WEB_SESSION_KEY_LEN; k++) {
            out[i].key[k] = blob_get_u8(&r);
        }
        out[i].end_unix = blob_get_i64(&r);
    }
    if (!blob_reader_ok(&r) || blob_reader_remaining(&r) != 0) {
        return false;
    }
    *mark_unix = mark;
    *count = n;
    return true;
}

int web_sess_restore(web_sessions_t *t, const web_sess_rec_t *rec, uint32_t now_s, int64_t now_unix)
{
    int64_t from = web_sess_mark_now(t, now_s, now_unix);
    int64_t end = rec->end_unix;
    if (now_unix > 0 && end - now_unix > (int64_t)WEB_SESSION_REMEMBER_S) {
        end = now_unix + WEB_SESSION_REMEMBER_S;
    }
    if (end <= 0 || end <= from) {
        return -1;
    }
    uint32_t left = WEB_SESSION_REMEMBER_S;
    if (from > 0 && end - from < (int64_t)left) {
        left = (uint32_t)(end - from);
    }
    int idx = free_slot(t, now_s);
    if (idx < 0) {
        return -1;
    }
    web_session_t *s = take(t, idx, rec->key, true, now_s - (WEB_SESSION_REMEMBER_S - left), now_s);
    s->end_unix = end;
    return idx;
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

/* ---- client and local addresses ------------------------------------------------------- */

static const uint8_t s_v4_mapped[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF };

/* The IPv4 address of an IPv4 or v4-mapped address (network order as stored). */
static bool addr_v4(const web_addr_t *a, uint32_t *v4)
{
    if (!a->v6) {
        memcpy(v4, a->b, 4);
        return true;
    }
    if (memcmp(a->b, s_v4_mapped, sizeof(s_v4_mapped)) == 0) {
        memcpy(v4, a->b + 12, 4);
        return true;
    }
    return false;
}

uint64_t web_client_key(const web_addr_t *a)
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
    if (a->b[0] == 0xFE && (a->b[1] & 0xC0) == 0x80) {
        uint64_t lo;
        memcpy(&lo, a->b + 8, 8);
        return hi ^ lo;   /* link-local: every host of the link has the same prefix */
    }
    return hi;
}

bool web_addr_on_ap(const web_addr_t *local, const web_addr_t *peer, uint32_t ap_ip4, uint32_t ap_mask4,
                    const uint8_t *ap_ll6)
{
    if (local == NULL || peer == NULL) {
        return false;
    }
    uint32_t v4;
    if (addr_v4(local, &v4)) {
        /* The AP's address alone is not enough: lwIP takes a packet for it on the station
         * interface too. A peer inside the AP's subnet gets its answers over the AP, so a
         * home-network host cannot complete a connection with a forged one. */
        uint32_t p4;
        return ap_ip4 != 0 && ap_mask4 != 0 && v4 == ap_ip4 && addr_v4(peer, &p4) &&
               (p4 & ap_mask4) == (ap_ip4 & ap_mask4);
    }
    /* IPv6: link-local addresses only, which lwIP takes only on their own interface. */
    return ap_ll6 != NULL && memcmp(local->b, ap_ll6, 16) == 0;
}

void web_wipe(void *p, size_t len)
{
    volatile uint8_t *v = (volatile uint8_t *)p;
    while (len-- > 0) {
        *v++ = 0;
    }
}

/* Append text, keeping out terminated; counts what it would have written. */
static void put_text(char *out, size_t cap, size_t *len, const char *s)
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

size_t web_addr_text(const web_addr_t *a, char *out, size_t cap)
{
    size_t len = 0;
    if (cap > 0) {
        out[0] = '\0';
    }
    if (a == NULL) {
        put_text(out, cap, &len, "?");
        return len < cap ? len : (cap > 0 ? cap - 1 : 0);
    }
    char part[8];
    uint32_t v4;
    if (addr_v4(a, &v4)) {
        const uint8_t *q = (const uint8_t *)&v4;
        for (int i = 0; i < 4; i++) {
            snprintf(part, sizeof(part), i == 0 ? "%u" : ".%u", (unsigned)q[i]);
            put_text(out, cap, &len, part);
        }
        return len < cap ? len : (cap > 0 ? cap - 1 : 0);
    }
    uint16_t g[8];
    for (int i = 0; i < 8; i++) {
        g[i] = (uint16_t)((a->b[2 * i] << 8) | a->b[2 * i + 1]);
    }
    /* The longest run of zero groups (two or more; the first of equal runs) becomes "::". */
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
            put_text(out, cap, &len, "::");
            i += best_n - 1;
            continue;
        }
        bool after_gap = best >= 0 && i == best + best_n;
        snprintf(part, sizeof(part), (i == 0 || after_gap) ? "%x" : ":%x", (unsigned)g[i]);
        put_text(out, cap, &len, part);
    }
    return len < cap ? len : (cap > 0 ? cap - 1 : 0);
}

/* ---- requests no route took ------------------------------------------------------------ */

web_fallback_t web_fallback_classify(const char *uri, bool options, bool get_or_head, bool asset, bool on_ap,
                                     bool (*api_known)(const char *path))
{
    if (options) {
        return WEB_FB_METHOD;   /* no CORS, ever */
    }
    if (uri == NULL) {
        uri = "";
    }
    size_t len = strcspn(uri, "?");
    bool api = (len >= 5 && strncmp(uri, "/api/", 5) == 0) || (len == 4 && strncmp(uri, "/api", 4) == 0) ||
               (len == 3 && strncmp(uri, "/ws", 3) == 0);
    if (api) {
        char path[64];
        bool known = len < sizeof(path) && api_known != NULL;
        if (known) {
            memcpy(path, uri, len);
            path[len] = '\0';
            known = api_known(path);
        }
        return known ? WEB_FB_METHOD : WEB_FB_NOT_FOUND;
    }
    if (asset) {
        return get_or_head ? WEB_FB_ASSET : WEB_FB_METHOD;
    }
    return on_ap ? WEB_FB_REDIRECT : WEB_FB_NOT_FOUND;
}

/* ---- connection handling ------------------------------------------------------------------- */

unsigned web_connection_tokens(const char *value)
{
    unsigned tok = 0;
    if (value == NULL) {
        return 0;
    }
    const char *p = value;
    while (*p != '\0') {
        while (*p == ' ' || *p == '\t' || *p == ',') {
            p++;
        }
        const char *start = p;
        while (*p != '\0' && *p != ',') {
            p++;
        }
        const char *end = p;
        while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
            end--;
        }
        size_t n = (size_t)(end - start);
        if (eq_nocase(start, n, "close")) {
            tok |= WEB_CONN_TOK_CLOSE;
        } else if (eq_nocase(start, n, "keep-alive")) {
            tok |= WEB_CONN_TOK_KEEP_ALIVE;
        }
    }
    return tok;
}

bool web_conn_should_close(unsigned tokens, bool http10)
{
    return (tokens & WEB_CONN_TOK_CLOSE) != 0 || http10;
}

web_req_body_t web_req_body(size_t content_len, bool transfer_encoding)
{
    if (transfer_encoding) {
        return WEB_REQ_BODY_UNFRAMED;
    }
    return content_len > 0 ? WEB_REQ_BODY_LENGTH : WEB_REQ_BODY_NONE;
}

/* ---- log lines about clients, rate-limited --------------------------------------------------- */

void web_lograte_init(web_lograte_t *r)
{
    memset(r, 0, sizeof(*r));
}

bool web_lograte_allow(web_lograte_t *r, uint64_t key, uint8_t tag, uint32_t now_s, uint32_t *held)
{
    web_lograte_client_t *c = NULL;
    for (size_t i = 0; i < WEB_LOGRATE_CLIENTS && c == NULL; i++) {
        if (r->c[i].used && r->c[i].key == key && r->c[i].tag == tag) {
            c = &r->c[i];
        }
    }
    if (c != NULL && (uint32_t)(now_s - c->last_s) < WEB_LOGRATE_PERIOD_S) {
        if (c->held < UINT32_MAX) {
            c->held++;
        }
        return false;
    }
    if (c == NULL) {
        /* A free entry, else the one whose last line is oldest. */
        c = &r->c[0];
        for (size_t i = 0; i < WEB_LOGRATE_CLIENTS; i++) {
            web_lograte_client_t *e = &r->c[i];
            if (!e->used) {
                c = e;
                break;
            }
            if ((uint32_t)(now_s - e->last_s) > (uint32_t)(now_s - c->last_s)) {
                c = e;
            }
        }
        c->used = true;
        c->key = key;
        c->tag = tag;
        c->held = 0;
    }
    if (held != NULL) {
        *held = c->held;
    }
    c->held = 0;
    c->last_s = now_s;
    return true;
}

const char *web_log_level_text(unsigned level)
{
    static const char *const names[] = { "none", "error", "warn", "info", "debug", "verbose" };
    return level < sizeof(names) / sizeof(names[0]) ? names[level] : "?";
}

/* The query has `key` (with or without a value), whatever the value looks like. */
static bool query_has(const char *query, const char *key)
{
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
        if (plen >= klen && memcmp(pair, key, klen) == 0 && (plen == klen || pair[klen] == '=')) {
            return true;
        }
    }
    return false;
}

bool web_query_u32(const char *query, const char *key, uint32_t *out)
{
    char v[128];   /* longer than any query the server keeps (96), so a value is never cut */
    *out = 0;
    if (query == NULL || key == NULL) {
        return true;
    }
    if (!web_query_value(query, key, v, sizeof(v))) {
        return !query_has(query, key);   /* absent, or a broken %-escape */
    }
    size_t n = strlen(v);
    if (n == 0 || n > 10) {
        return false;
    }
    uint64_t x = 0;
    for (size_t i = 0; i < n; i++) {
        if (!is_digit(v[i])) {
            return false;
        }
        x = x * 10u + (uint64_t)(v[i] - '0');
    }
    if (x > UINT32_MAX) {
        return false;
    }
    *out = (uint32_t)x;
    return true;
}
