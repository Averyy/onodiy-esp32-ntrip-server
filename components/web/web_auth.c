/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Admin password, sessions and login throttling. Server task only (see
 * web_internal.h), so nothing here takes a lock.
 *
 * Remembered sessions (web_core.h) are kept in settings web/sessions as the
 * SHA-256 of their token with their end in UTC, bound to the admin
 * password's salt. The record is rewritten only when that set changes: a
 * login, a logout, a purge of ended sessions, a password change (or any
 * other end of every session), and once when the clock is first set while a
 * remembered session made without it is open. Never per request.
 */
#include <stdio.h>
#include <string.h>

#include "diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "settings.h"
#include "web_internal.h"
#include "web_pw.h"

static const char *TAG = "web";

static web_sessions_t s_sess;
static web_throttles_t s_throttles;
static web_admin_state_t s_state;
static web_pw_record_t s_rec;
static bool s_ram_only;                        /* claimed while the settings were unavailable */
static uint8_t s_blob[WEB_PW_BLOB_LEN + 8];    /* the stored record as last read or written */
static size_t s_blob_len;                      /* 0 = nothing stored */
static bool s_blob_unreadable;
EXT_RAM_BSS_ATTR static web_lograte_t s_fail_log;   /* wrong-password lines, per client */

/* Read sys/admin_hash into the cache. @return true if what is stored differs from the cache before. */
static bool load(void)
{
    uint8_t buf[sizeof(s_blob)];
    size_t len = sizeof(buf);
    bool unreadable = false;
    esp_err_t err = settings_is_available() ? settings_get_blob(SETTINGS_NS_SYS, SETTINGS_KEY_ADMIN_HASH, buf, &len)
                                            : ESP_ERR_INVALID_STATE;
    if (err == ESP_ERR_NVS_NOT_FOUND || err == ESP_ERR_INVALID_STATE) {
        len = 0;
    } else if (err != ESP_OK) {
        len = 0;
        unreadable = true;
    }
    bool changed = len != s_blob_len || unreadable != s_blob_unreadable || memcmp(buf, s_blob, len) != 0;
    memcpy(s_blob, buf, len);
    s_blob_len = len;
    s_blob_unreadable = unreadable;

    if (!settings_is_available()) {
        s_state = s_ram_only ? WEB_ADMIN_SET : WEB_ADMIN_NONE;
        return false;
    }
    s_ram_only = false;
    if (unreadable) {
        s_state = WEB_ADMIN_UNREADABLE;
    } else if (len == 0) {
        s_state = WEB_ADMIN_NONE;
    } else if (web_pw_decode(buf, len, &s_rec)) {
        s_state = WEB_ADMIN_SET;
    } else {
        s_state = WEB_ADMIN_UNREADABLE;
    }
    return changed;
}

static void restore_sessions(void);

void web_auth_init(void)
{
    web_sess_init(&s_sess);
    web_throttles_init(&s_throttles, web_now_s());
    web_lograte_init(&s_fail_log);
    s_ram_only = false;
    s_blob_len = 0;
    s_blob_unreadable = false;
    (void)load();
    if (s_state == WEB_ADMIN_UNREADABLE) {
        ESP_LOGW(TAG, "stored admin password unreadable: hold the BOOT button to reset it");
    } else if (s_state == WEB_ADMIN_NONE) {
        ESP_LOGI(TAG, "no admin password yet: set it over the setup Wi-Fi");
    }
    restore_sessions();
}

web_admin_state_t web_auth_admin_state(void)
{
    return s_state;
}

void web_auth_reload(void)
{
    if (load()) {
        ESP_LOGW(TAG, "admin password changed outside the web UI: every session ends");
        web_auth_end_sessions(-1);
    }
}

void web_auth_admin_reset(void)
{
    if (s_ram_only) {
        s_ram_only = false;
        memset(&s_rec, 0, sizeof(s_rec));
    }
    (void)load();
    ESP_LOGW(TAG, "admin password cleared with the BOOT button: every session ends");
    web_auth_end_sessions(-1);
}

/* ---- sessions -------------------------------------------------------------------------------- */

static char s_cookie[1024];   /* server task only */
/* settings web/sessions as last read or written (s_kept_len 0 = nothing stored), and the record a store compares. */
static uint8_t s_kept[WEB_SESS_STORE_MAX_LEN];
static size_t s_kept_len;
static bool s_kept_unknown;   /* something unreadable is stored: the next store replaces it */
static uint8_t s_next[WEB_SESS_STORE_MAX_LEN];
static web_sess_rec_t s_recs[WEB_SESSIONS_MAX];

/* Keep settings web/sessions in step with the remembered sessions; written only when they changed. Without a stored
 * password (none, unreadable, or held only in RAM) nothing may stay stored. */
static void store_sessions(void)
{
    if (!settings_is_available()) {
        return;
    }
    size_t n = 0;
    size_t len = 0;
    if (s_state == WEB_ADMIN_SET && !s_ram_only) {
        len = web_sess_store_encode(&s_sess, s_rec.salt, s_next, sizeof(s_next), &n);
    }
    if (n == 0) {
        len = 0;
    }
    if (!s_kept_unknown && len == s_kept_len && memcmp(s_next, s_kept, len) == 0) {
        return;
    }
    esp_err_t err = len > 0 ? settings_set_state_blob(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_SESSIONS, s_next, len)
                            : settings_erase_state_key(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_SESSIONS);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "remembered logins not stored: %s", esp_err_to_name(err));
        return;   /* the next change tries again */
    }
    memcpy(s_kept, s_next, len);
    s_kept_len = len;
    s_kept_unknown = false;
}

/* At boot: the remembered sessions of the stored password whose end has not passed (web_sess_restore); the rest of
 * the record goes. */
static void restore_sessions(void)
{
    s_kept_len = 0;
    s_kept_unknown = false;
    if (!settings_is_available()) {
        return;
    }
    size_t len = sizeof(s_kept);
    esp_err_t err = settings_get_blob(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_SESSIONS, s_kept, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return;
    }
    unsigned back = 0;
    uint8_t bind[WEB_SESS_BIND_LEN];
    size_t n = 0;
    if (err != ESP_OK) {
        s_kept_unknown = true;
    } else {
        s_kept_len = len;
        if (s_state == WEB_ADMIN_SET && web_sess_store_decode(s_kept, len, bind, s_recs, &n) &&
            web_ct_equal(bind, s_rec.salt, sizeof(bind))) {
            int64_t utc = diag_utc_now();
            for (size_t i = 0; i < n; i++) {
                back += web_sess_restore(&s_sess, &s_recs[i], web_now_s(), utc) >= 0;
            }
        }
    }
    ESP_LOGI(TAG, "remembered logins: %u of %u back", back, (unsigned)n);
    store_sessions();
}

/* A session's key: the SHA-256 of its cookie's token. */
static bool token_key(const uint8_t token[16], uint8_t key[WEB_SESSION_KEY_LEN])
{
    return web_sha256(token, 16, key);
}

int web_auth_session(httpd_req_t *req, bool touch)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (len == 0 || len >= sizeof(s_cookie)) {
        return -1;
    }
    if (httpd_req_get_hdr_value_str(req, "Cookie", s_cookie, sizeof(s_cookie)) != ESP_OK) {
        return -1;
    }
    char hex[33];
    if (!web_cookie_sid(s_cookie, hex)) {
        return -1;
    }
    uint8_t token[16];
    for (size_t i = 0; i < 16; i++) {
        unsigned v = 0;
        sscanf(&hex[2 * i], "%2x", &v);
        token[i] = (uint8_t)v;
    }
    uint8_t key[WEB_SESSION_KEY_LEN];
    bool ok = token_key(token, key);
    memset(token, 0, sizeof(token));
    if (!ok) {
        return -1;
    }
    int idx = web_sess_find(&s_sess, key, web_now_s());
    if (idx >= 0 && touch) {
        web_sess_touch(&s_sess, idx, web_now_s());
    }
    return idx;
}

uint32_t web_auth_session_gen(int idx)
{
    return (idx >= 0 && (size_t)idx < WEB_SESSIONS_MAX) ? s_sess.s[idx].gen : 0;
}

bool web_auth_session_alive(int idx, uint32_t gen)
{
    if (!web_sess_alive(&s_sess, idx, gen)) {
        return false;
    }
    return web_sess_left_s(&s_sess, idx, web_now_s()) > 0;
}

uint32_t web_auth_session_left_s(int idx)
{
    return web_sess_left_s(&s_sess, idx, web_now_s());
}

void web_auth_session_touch(int idx)
{
    web_sess_touch(&s_sess, idx, web_now_s());
}

int web_auth_start_session(httpd_req_t *req, bool remember, char cookie_buf[96])
{
    uint8_t token[16];
    uint8_t key[WEB_SESSION_KEY_LEN];
    esp_fill_random(token, sizeof(token));
    if (!token_key(token, key)) {
        memset(token, 0, sizeof(token));
        return -1;
    }
    int idx = web_sess_create(&s_sess, key, remember, web_now_s(), diag_utc_now());
    /* A slot taken from the least recently used session: its clients go. */
    web_ws_close_sessions(1u << idx);
    char hex[33];
    web_hex16(token, hex);
    memset(token, 0, sizeof(token));
    if (remember) {
        snprintf(cookie_buf, 96, "sid=%s; HttpOnly; SameSite=Strict; Path=/; Max-Age=%u", hex,
                 (unsigned)WEB_SESSION_REMEMBER_S);
    } else {
        snprintf(cookie_buf, 96, "sid=%s; HttpOnly; SameSite=Strict; Path=/", hex);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_buf);
    store_sessions();
    return idx;
}

void web_auth_logout(httpd_req_t *req, int idx)
{
    if (idx >= 0) {
        web_sess_end(&s_sess, idx);
        web_ws_close_sessions(1u << idx);
        store_sessions();
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Max-Age=0; Path=/");
}

void web_auth_end_session(int idx)
{
    if (idx >= 0 && (size_t)idx < WEB_SESSIONS_MAX) {
        web_sess_end(&s_sess, idx);
        web_ws_close_sessions(1u << idx);
        store_sessions();
    }
}

void web_auth_end_sessions(int keep)
{
    uint32_t mask = web_sess_end_all_except(&s_sess, keep);
    if (mask != 0) {
        web_ws_close_sessions(mask);
    }
    /* Also with nothing ended: after a password change the kept session is stored under the new salt. */
    store_sessions();
}

void web_auth_expire(void)
{
    uint32_t now = web_now_s();
    uint32_t mask = web_sess_expire(&s_sess, now);
    bool stamped = false;
    int64_t utc = diag_utc_now();
    if (utc > 0) {
        mask |= web_sess_clock(&s_sess, now, utc, &stamped);
    }
    if (mask != 0) {
        web_ws_close_sessions(mask);
    }
    if (mask != 0 || stamped) {
        store_sessions();
    }
}

/* ---- password -------------------------------------------------------------------------------- */

/* The client a throttle entry belongs to (web_client_key): its IPv4
 * address (also when mapped into IPv6), an IPv6 link-local address whole,
 * or the /64 prefix of any other IPv6 address. 0 if the socket cannot tell. */
static uint64_t client_key(httpd_req_t *req)
{
    web_addr_t a;
    return web_req_peer(req, &a) ? web_client_key(&a) : 0;
}

uint32_t web_auth_throttle_wait_s(httpd_req_t *req, bool on_ap)
{
    return web_throttles_wait_s(&s_throttles, client_key(req), on_ap, web_now_s());
}

bool web_auth_check(httpd_req_t *req, bool on_ap, const char *pw)
{
    uint64_t key = client_key(req);
    bool ok = s_state == WEB_ADMIN_SET && web_pw_check(&s_rec, pw);
    if (ok) {
        web_throttles_ok(&s_throttles, key, on_ap);
    } else {
        uint32_t n = web_throttles_fail(&s_throttles, key, on_ap, web_now_s());
        /* Who it was, at most once a minute per client (the count says how many in a row). */
        uint32_t held = 0;
        if (web_lograte_allow(&s_fail_log, key, 0, web_now_s(), &held)) {   /* one reason: tag 0 */
            char who[48];
            web_peer_text(req, who, sizeof(who));
            if (held > 0) {
                ESP_LOGW(TAG, "wrong admin password from %s (%u in a row from this client, +%u not logged)", who,
                         (unsigned)n, (unsigned)held);
            } else {
                ESP_LOGW(TAG, "wrong admin password from %s (%u in a row from this client)", who, (unsigned)n);
            }
        }
    }
    return ok;
}

esp_err_t web_auth_set_password(const char *pw, bool *persisted)
{
    uint8_t salt[WEB_PW_SALT_LEN];
    esp_fill_random(salt, sizeof(salt));
    web_pw_record_t rec;
    if (!web_pw_make(pw, salt, CONFIG_WEB_PBKDF2_ITERATIONS, &rec)) {
        return ESP_FAIL;
    }
    *persisted = false;
    if (settings_is_available()) {
        uint8_t blob[WEB_PW_BLOB_LEN];
        size_t len = web_pw_encode(&rec, blob, sizeof(blob));
        if (len == 0) {
            return ESP_ERR_INVALID_SIZE;
        }
        /* Cache first, so the change event of this write is recognised as ours. */
        uint8_t old[sizeof(s_blob)];
        size_t old_len = s_blob_len;
        bool old_unreadable = s_blob_unreadable;
        memcpy(old, s_blob, sizeof(old));
        memcpy(s_blob, blob, len);
        s_blob_len = len;
        s_blob_unreadable = false;
        esp_err_t err = settings_set_blob(SETTINGS_NS_SYS, SETTINGS_KEY_ADMIN_HASH, blob, len);
        if (err != ESP_OK) {
            memcpy(s_blob, old, sizeof(s_blob));
            s_blob_len = old_len;
            s_blob_unreadable = old_unreadable;
            return err;
        }
        *persisted = true;
        s_ram_only = false;
    } else {
        s_ram_only = true;
    }
    s_rec = rec;
    s_state = WEB_ADMIN_SET;
    web_throttles_init(&s_throttles, web_now_s());   /* failures against the old password are moot */
    return ESP_OK;
}
