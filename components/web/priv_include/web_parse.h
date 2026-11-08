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
#include "web_shell_parse.h"
#include "web_shell_auth_parse.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/**
 * POST /api/gnss/query: {"command": c}, c one of the receiver queries
 * (um98x_query.h: VERSIONA, CONFIG, MODE, ANT1GROUPA, BASEPOSA, SPPNAVA;
 * case-insensitive): *query its um98x_query_t.
 */
bool web_parse_query(const cJSON *root, unsigned *query, web_bad_t *bad);

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

/** PUT /api/casters/{n}/ca: {"pem": "..."} with PEM certificates only. *pem points into root. */
bool web_parse_ca_pem(const cJSON *root, const char **pem, web_bad_t *bad);

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
    bool no_trial;   /**< "no_trial": the new network is the station's at once, kept even without an IP */
} web_wifi_put_t;

/** PUT /api/wifi (strings point into root). */
bool web_parse_wifi_put(const cJSON *root, web_wifi_put_t *out, web_bad_t *bad);

typedef struct {
    bool have_hostname;
    const char *hostname;
    bool have_public_status;
    bool public_status;
    bool have_wiring;
    bool wiring_default;           /**< "wiring": null, the build's pins and port */
    int8_t rx_gpio;                /**< 0-48 (the board checks which it may use) */
    int8_t tx_gpio;
    uint8_t port;                  /**< 1-3 */
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

#ifdef __cplusplus
}
#endif
