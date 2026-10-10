/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Admin password, sessions and login throttling. Server task only (see
 * web_internal.h), so nothing here takes a lock.
 */
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_random.h"
#include "sdkconfig.h"
#include "settings.h"
#include "web_internal.h"
#include "web_pw.h"

static const char *TAG = "web";

static web_sessions_t s_sess;
static web_throttle_t s_throttle;
static web_admin_state_t s_state;
static web_pw_record_t s_rec;
static bool s_ram_only;                        /* claimed while the settings were unavailable */
static uint8_t s_blob[WEB_PW_BLOB_LEN + 8];    /* the stored record as last read or written */
static size_t s_blob_len;                      /* 0 = nothing stored */
static bool s_blob_unreadable;

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

void web_auth_init(void)
{
    web_sess_init(&s_sess);
    memset(&s_throttle, 0, sizeof(s_throttle));
    s_ram_only = false;
    s_blob_len = 0;
    s_blob_unreadable = false;
    (void)load();
    if (s_state == WEB_ADMIN_UNREADABLE) {
        ESP_LOGW(TAG, "stored admin password unreadable: hold the BOOT button to reset it");
    } else if (s_state == WEB_ADMIN_NONE) {
        ESP_LOGI(TAG, "no admin password yet: set it over the setup Wi-Fi");
    }
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
    int idx = web_sess_find(&s_sess, token, web_now_s());
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

int web_auth_start_session(httpd_req_t *req, char cookie_buf[96])
{
    uint8_t token[16];
    esp_fill_random(token, sizeof(token));
    int idx = web_sess_create(&s_sess, token, web_now_s());
    /* A slot taken from the least recently used session: its clients go. */
    web_ws_close_sessions(1u << idx);
    char hex[33];
    web_hex16(token, hex);
    memset(token, 0, sizeof(token));
    snprintf(cookie_buf, 96, "sid=%s; HttpOnly; SameSite=Strict; Path=/", hex);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie_buf);
    return idx;
}

void web_auth_logout(httpd_req_t *req, int idx)
{
    if (idx >= 0) {
        web_sess_end(&s_sess, idx);
        web_ws_close_sessions(1u << idx);
    }
    httpd_resp_set_hdr(req, "Set-Cookie", "sid=; Max-Age=0; Path=/");
}

void web_auth_end_session(int idx)
{
    if (idx >= 0 && (size_t)idx < WEB_SESSIONS_MAX) {
        web_sess_end(&s_sess, idx);
        web_ws_close_sessions(1u << idx);
    }
}

void web_auth_end_sessions(int keep)
{
    uint32_t mask = web_sess_end_all_except(&s_sess, keep);
    if (mask != 0) {
        web_ws_close_sessions(mask);
    }
}

void web_auth_expire(void)
{
    uint32_t mask = web_sess_expire(&s_sess, web_now_s());
    if (mask != 0) {
        web_ws_close_sessions(mask);
    }
}

/* ---- password -------------------------------------------------------------------------------- */

uint32_t web_auth_throttle_wait_s(void)
{
    return web_throttle_wait_s(&s_throttle, web_now_s());
}

bool web_auth_check(const char *pw)
{
    bool ok = s_state == WEB_ADMIN_SET && web_pw_check(&s_rec, pw);
    if (ok) {
        web_throttle_ok(&s_throttle);
    } else {
        web_throttle_fail(&s_throttle, web_now_s());
        ESP_LOGW(TAG, "wrong admin password (%u in a row)", (unsigned)s_throttle.fails);
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
    web_throttle_ok(&s_throttle);
    return ESP_OK;
}
