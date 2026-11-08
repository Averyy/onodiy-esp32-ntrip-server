/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#pragma once
#include "web_shell_http.h"

#ifdef __cplusplus
extern "C" {
#endif
/* ---- authentication (web_shell_auth.c) --------------------------------------------------------------- */

/** Load the admin password record; reset sessions and throttling; take the remembered sessions back. */
void web_auth_init(void);

typedef enum {
    WEB_ADMIN_NONE = 0,      /**< no password: claim over the AP */
    WEB_ADMIN_SET,           /**< a record is loaded (stored, or RAM-only this boot) */
    WEB_ADMIN_UNREADABLE,    /**< something is stored but it cannot be read */
} web_admin_state_t;

web_admin_state_t web_auth_admin_state(void);

/** Session of the request's cookie (extends its idle timeout when `touch`). -1 if none. */
int web_auth_session(httpd_req_t *req, bool touch);

/** Generation of a live session (links WebSocket clients to it). */
uint32_t web_auth_session_gen(int idx);
bool web_auth_session_alive(int idx, uint32_t gen);
uint32_t web_auth_session_left_s(int idx);
void web_auth_session_touch(int idx);

/**
 * Seconds this request's client must still wait before a password attempt
 * (0 = allowed now). Throttling is per client (peer address, and whether it
 * came over the AP; web_core.h).
 */
uint32_t web_auth_throttle_wait_s(httpd_req_t *req, bool on_ap);

/**
 * Check the password for this request's client (counts its failure, resets
 * its count on success). Call only when the throttle allows it.
 */
bool web_auth_check(httpd_req_t *req, bool on_ap, const char *pw);

/**
 * New session; sets the cookie header on the response (`cookie_buf` must
 * outlive the response). `remember`: 30 days without an idle timeout, kept
 * across restarts (web_core.h), and a cookie with that Max-Age. `replace`:
 * the session this browser had (-1 = none), ended in the same store.
 * @return its index, -1 if no token key could be made (no session, no cookie;
 * `replace` stays).
 */
int web_auth_start_session(httpd_req_t *req, bool remember, int replace, char cookie_buf[96]);

/** End the request's session (and its WebSocket clients); sets the clearing cookie. */
void web_auth_logout(httpd_req_t *req, int idx);

/**
 * Store a new password (first claim or change). `ram_only` when the
 * settings are unavailable (claim only). @return ESP_OK or the settings error.
 */
esp_err_t web_auth_set_password(const char *pw, bool *persisted);

/** End every session but `keep` (-1 = all); their WebSocket clients close with 4001. */
void web_auth_end_sessions(int keep);

/** End expired sessions, and remembered ones whose end the trusted clock shows has passed (once a second). */
void web_auth_expire(void);

/** SETTINGS_EVENT for sys/admin_hash or a factory reset: reload, end sessions if it changed behind our back. */
void web_auth_reload(void);

/**
 * The product reports an admin-password reset: forget a password held only
 * in RAM (settings unavailable), reload
 * the stored state and end every session.
 */
void web_auth_admin_reset(void);

#ifdef __cplusplus
}
#endif
