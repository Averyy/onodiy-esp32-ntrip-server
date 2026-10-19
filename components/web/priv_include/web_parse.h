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
#include "local_caster.h"
#include "ntrip_server.h"
#include "web_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Why a body was refused: the error, the field it names (NULL = none) and,
 * where a parser words it, the field's rule ("at most 23 bytes"; NULL =
 * none: the code's default message).
 */
typedef struct {
    web_err_t err;
    const char *field;
    const char *rule;
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

/** POST /api/session: {"password", "remember"} (remember optional, default false). */
bool web_parse_login(const cJSON *root, const char **pw, bool *remember, web_bad_t *bad);

/** {"confirm": true}. */
bool web_parse_confirm(const cJSON *root, web_bad_t *bad);

/** The setup wizard's steps after the password, as stored in sys/setup_step (0 = none). */
typedef enum {
    WEB_SETUP_STEP_NONE = 0,
    WEB_SETUP_STEP_WIFI = 1,
    WEB_SETUP_STEP_RECEPTION = 2,
    WEB_SETUP_STEP_ONOCOY = 3,
    WEB_SETUP_STEP_CASTERS = 4,
} web_setup_step_t;

/** API name of a stored step ("wifi", ...); NULL for none or a value out of range. */
const char *web_setup_step_name(uint8_t step);

/**
 * POST /api/setup/step: {"step": "wifi" | "reception" | "onocoy" |
 * "casters"}. Anything else (also "done": that is POST /api/setup/done) is
 * invalid_field "step".
 */
bool web_parse_setup_step(const cJSON *root, uint8_t *step, web_bad_t *bad);

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

#define WEB_SG_TEST_PHASE_MIN_S     60u
#define WEB_SG_TEST_PHASE_MAX_S     7200u
#define WEB_SG_TEST_PHASE_DEFAULT_S 1800u

/**
 * POST /api/gnss/signal-test: {"groups": [{"master": m, "slave": s}, {...}],
 * "phase_s": n}. groups: exactly two different pairs of 0-255 (else
 * invalid_field "groups"); phase_s 60-7200, left out = 1800 (else
 * invalid_field "phase_s"). out[0] is A, out[1] is B.
 */
bool web_parse_signal_test(const cJSON *root, uint8_t out[2][2], uint32_t *phase_s, web_bad_t *bad);

/** POST /api/gnss/signal-test/keep: {"group": 0|1} (else invalid_field "group"). */
bool web_parse_signal_test_keep(const cJSON *root, unsigned *group, web_bad_t *bad);

/**
 * The API error of a refused comparison request (station_sgt_err_t);
 * *field names the field of an invalid_field (else NULL). `safe_mode`: the
 * station does not run because of safe mode (else it is busy starting).
 */
web_err_t web_sgt_error(int err, bool safe_mode, const char **field);

/* ---- casters ------------------------------------------------------------------------------ */

/**
 * PUT /api/casters/{n}. `stored` is the slot as stored (its password blank,
 * password_set telling whether one is stored). *out gets the merged config
 * to hand to ntrip_server_set_config(): `password` holds the new password
 * or "" (keep), clear_password set when asked; protected_slot and the
 * backoff profile follow ntrip_server_apply_host_rules() (a fast profile
 * requested for a host that is not Onocoy: field "backoff_profile").
 * test_onocoy true in a build without CONFIG_NTRIP_TEST_ONOCOY_SLOT: field
 * "test_onocoy". A new RTK2go host or a changed RTK2go mountpoint that
 * breaks RTK2go's rules (ntrip_server_rtk2go_check): field "mountpoint"
 * with the rule in words.
 */
bool web_parse_caster_put(const cJSON *root, const ntrip_caster_cfg_t *stored, ntrip_caster_cfg_t *out,
                          web_bad_t *bad);

/** Largest custom CA accepted (bytes of PEM text). */
#define WEB_CA_PEM_MAX 4096u

/** PUT /api/casters/{n}/ca: {"pem": "..."} with PEM certificates only. *pem points into root. */
bool web_parse_ca_pem(const cJSON *root, const char **pem, web_bad_t *bad);

/** PEM text made only of CERTIFICATE blocks (and blank space), at most WEB_CA_PEM_MAX bytes. */
bool web_pem_certs_only(const char *pem);

/* ---- local NTRIP (Your rover) ------------------------------------------------------------- */

/**
 * PUT /api/local-caster: any of {"enabled": bool, "mountpoint", "username",
 * "password": string}, merged into `stored` (local_caster_get_config(), its
 * password ""). A password that is empty or left out keeps the stored one
 * (out->password ""); password_set is ignored. Types and sizes only: a wrong
 * type, a string longer than its field holds or a control character is
 * invalid_field for that field without a rule (the rules and their words are
 * local_caster_set_config()'s). On a refusal out->password is wiped; on
 * success it is the caller's to wipe.
 */
bool web_parse_local_caster_put(const cJSON *root, const local_caster_cfg_t *stored, local_caster_cfg_t *out,
                                web_bad_t *bad);

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
