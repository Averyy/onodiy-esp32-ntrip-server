/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Request bodies of the API: parsing and the field rules of the contract
 * (pure logic over cJSON and the component types, no RTOS or driver calls,
 * host-testable). Every parser checks types and ranges and names the first
 * offending field, so a 400 invalid_field can say which one.
 *
 * PUT bodies merge into the stored value: fields left out keep it, so a
 * form that does not show a field cannot reset it.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_log.h"
#include "gnss_types.h"
#include "health.h"
#include "ntrip_server.h"
#include "web_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Why a body was refused: the error and the field it names (NULL = none). */
typedef struct {
    web_err_t err;
    const char *field;
} web_bad_t;

/** Deepest nesting of objects and arrays a request may have (cJSON parses recursively). */
#define WEB_JSON_MAX_DEPTH 8u

/** True if the text nests objects and arrays at most max_depth deep (strings skipped). */
bool web_json_depth_ok(const char *text, size_t len, unsigned max_depth);

/**
 * Parse a JSON body. NULL (with bad = bad_json) for invalid JSON, nesting
 * deeper than WEB_JSON_MAX_DEPTH, or a non-object root; an empty body is {}.
 */
cJSON *web_parse_body(const char *body, size_t len, web_bad_t *bad);

/* ---- session and setup --------------------------------------------------------------- */

/** {"password": "..."}: a string (its rules are checked by the caller, which words the error). */
bool web_parse_password(const cJSON *root, const char *key, const char **out, web_bad_t *bad);

/** {"confirm": true}. */
bool web_parse_confirm(const cJSON *root, web_bad_t *bad);

/* ---- receiver --------------------------------------------------------------------------- */

/** Ephemeris RTCM types that need a COM1 rate of at least 460800. */
bool web_rtcm_is_ephemeris(uint16_t msg);

/**
 * PUT /api/gnss/profile. `stored` is the profile in force; *profile gets
 * the merged result when the body has "profile" (*have_profile), *autocfg
 * when it has "autocfg" (*have_autocfg). The antenna strings
 * (antenna_model, antenna_serial) lose their trailing spaces (the stored
 * form) and must then pass um98x_antenna_str_valid(): at most 31 printable
 * ASCII characters without '"'; inner spaces stay.
 */
bool web_parse_profile_put(const cJSON *root, const um98x_base_profile_t *stored, um98x_base_profile_t *profile,
                           bool *have_profile, bool *autocfg, bool *have_autocfg, web_bad_t *bad);

/**
 * The "recovery" member of PUT /api/gnss/profile (gnss/recovery): one of
 * "full", "no_reboot", "alerts_only". *have false when it is left out.
 */
bool web_parse_recovery(const cJSON *root, bool *have, health_recovery_mode_t *mode, web_bad_t *bad);

/**
 * POST /api/gnss/maintenance: {"duration_s": n}, one of WEB_MAINT_CHOICES_S;
 * left out (or 0) = the default (*duration_s 0: the station's default).
 */
bool web_parse_maintenance(const cJSON *root, uint32_t *duration_s, web_bad_t *bad);

/**
 * POST /api/gnss/live: {"minutes": n}, 0..max_min; left out (or 0) = the
 * station's default window.
 */
bool web_parse_live(const cJSON *root, uint32_t max_min, uint32_t *minutes, web_bad_t *bad);

/* ---- casters ------------------------------------------------------------------------------ */

/**
 * PUT /api/casters/{n}. `stored` is the slot as stored (its password blank,
 * password_set telling whether one is stored). *out gets the merged config
 * to hand to ntrip_server_set_config(): `password` holds the new password
 * or "" (keep), clear_password set when asked; protected_slot and the
 * backoff profile follow ntrip_server_apply_host_rules() (a fast profile
 * requested for a host that is not Onocoy: field "backoff_profile").
 */
bool web_parse_caster_put(const cJSON *root, const ntrip_caster_cfg_t *stored, ntrip_caster_cfg_t *out,
                          web_bad_t *bad);

/** Largest custom CA accepted (bytes of PEM text). */
#define WEB_CA_PEM_MAX 4096u

/** PUT /api/casters/{n}/ca: {"pem": "..."} with PEM certificates only. *pem points into root. */
bool web_parse_ca_pem(const cJSON *root, const char **pem, web_bad_t *bad);

/** PEM text made only of CERTIFICATE blocks (and blank space), at most WEB_CA_PEM_MAX bytes. */
bool web_pem_certs_only(const char *pem);

/* ---- Wi-Fi and system ------------------------------------------------------------------------ */

typedef struct {
    bool have_ssid;
    const char *ssid;
    bool have_password;
    const char *password;
    bool have_ap_password;
    const char *ap_password;
} web_wifi_put_t;

/** PUT /api/wifi (strings point into root). */
bool web_parse_wifi_put(const cJSON *root, web_wifi_put_t *out, web_bad_t *bad);

typedef struct {
    bool have_hostname;
    const char *hostname;
    bool have_public_status;
    bool public_status;
} web_system_put_t;

/** PUT /api/system. */
bool web_parse_system_put(const cJSON *root, web_system_put_t *out, web_bad_t *bad);

/** PUT /api/diag/log-level: tag <= 15 characters or "*". */
bool web_parse_log_level(const cJSON *root, const char **tag, esp_log_level_t *level, web_bad_t *bad);

/** PUT /api/admin-password: {"current", "new"} (the rules of "new" are checked by the caller). */
bool web_parse_admin_password(const cJSON *root, const char **current, const char **next, web_bad_t *bad);

/* ---- updates and uploads ---------------------------------------------------------------------- */

/** POST /api/update/app: {"version", "rolled_back_ack"}. */
bool web_parse_update_app(const cJSON *root, const char **version, bool *rolled_back_ack, web_bad_t *bad);

/** POST /api/update/gnss: {"build", "cross_layout_ack"}. */
bool web_parse_update_gnss(const cJSON *root, uint32_t *build, bool *cross_layout_ack, web_bad_t *bad);

/** POST /api/upload/gnss/confirm: {"sha256", "expected_fw", "cross_layout_ack"}. */
bool web_parse_upload_confirm(const cJSON *root, uint8_t sha256[32], const char **expected_fw,
                              uint32_t *expected_build, bool *cross_layout_ack, web_bad_t *bad);

/* ---- WebSocket ---------------------------------------------------------------------------------- */

#define WEB_TOPIC_STATUS   (1u << 0)
#define WEB_TOPIC_PROGRESS (1u << 1)
#define WEB_TOPIC_LOG      (1u << 2)

typedef enum {
    WEB_WS_MSG_INVALID = 0,
    WEB_WS_MSG_SUBSCRIBE,
    WEB_WS_MSG_PING,
} web_ws_msg_t;

/** What a subscribe message asks for. */
typedef struct {
    uint32_t topics;          /**< topic bits (unknown names ignored) */
    bool has_log_cursor;      /**< "log_cursor" given: send log lines after it */
    uint32_t log_cursor;
} web_ws_sub_t;

/**
 * A client text message; for subscribe, *sub gets the topics and the
 * optional log cursor (a "log_cursor" that is not an integer of 0..2^32-1
 * makes the message invalid).
 */
web_ws_msg_t web_parse_ws_message(const char *text, size_t len, web_ws_sub_t *sub);

#ifdef __cplusplus
}
#endif
