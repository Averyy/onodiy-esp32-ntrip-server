/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_parse.h"

#include <string.h>

#include "station.h"
#include "um98x_profile.h"
#include "um98x_query.h"
#include "web_enums.h"

static bool fail(web_bad_t *bad, web_err_t err, const char *field)
{
    if (bad != NULL) {
        bad->err = err;
        bad->field = field;
    }
    return false;
}

static bool invalid(web_bad_t *bad, const char *field)
{
    return fail(bad, WEB_ERR_INVALID_FIELD, field);
}

/* invalid_field with the field's rule in words (the error's message). */
static bool invalid_rule(web_bad_t *bad, const char *field, const char *rule)
{
    if (bad != NULL) {
        bad->rule = rule;
    }
    return fail(bad, WEB_ERR_INVALID_FIELD, field);
}

static const char *const s_setup_steps[] = {
    [WEB_SETUP_STEP_WIFI] = "wifi",
    [WEB_SETUP_STEP_RECEPTION] = "reception",
    [WEB_SETUP_STEP_ONOCOY] = "onocoy",
    [WEB_SETUP_STEP_CASTERS] = "casters",
};

const char *web_setup_step_name(uint8_t step)
{
    if (step == WEB_SETUP_STEP_NONE || step >= sizeof(s_setup_steps) / sizeof(s_setup_steps[0])) {
        return NULL;
    }
    return s_setup_steps[step];
}

bool web_parse_setup_step(const cJSON *root, uint8_t *step, web_bad_t *bad)
{
    const char *name = NULL;
    if (web_member_str(root, "step", 16, &name) != WEB_MEMBER_OK) {
        return invalid(bad, "step");
    }
    for (uint8_t i = WEB_SETUP_STEP_WIFI; i < sizeof(s_setup_steps) / sizeof(s_setup_steps[0]); i++) {
        if (strcmp(name, s_setup_steps[i]) == 0) {
            *step = i;
            return true;
        }
    }
    return invalid(bad, "step");
}

/* ---- receiver ------------------------------------------------------------------------------- */

bool web_rtcm_is_ephemeris(uint16_t msg)
{
    return msg == 1019 || msg == 1020 || msg == 1042 || msg == 1045 || msg == 1046;
}

static bool parse_rtcm(const cJSON *arr, um98x_base_profile_t *p, web_bad_t *bad)
{
    if (!cJSON_IsArray(arr)) {
        return invalid(bad, "rtcm");
    }
    int n = cJSON_GetArraySize(arr);
    if (n < 0 || n > UM98X_MAX_RTCM_MSGS) {
        return invalid(bad, "rtcm");
    }
    um98x_rtcm_out_t list[UM98X_MAX_RTCM_MSGS];
    int i = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, arr) {
        int64_t msg = 0;
        int64_t period = 0;
        if (!cJSON_IsObject(e) || web_member_int(e, "msg", 1001, 4095, &msg) != WEB_MEMBER_OK ||
            web_member_int(e, "period_s", 1, UINT16_MAX, &period) != WEB_MEMBER_OK) {
            return invalid(bad, "rtcm");
        }
        /* 1101-1107 never reach a caster: the station keeps them out of the hub. */
        if (msg >= 1101 && msg <= 1107) {
            return invalid(bad, "rtcm");
        }
        for (int k = 0; k < i; k++) {
            if (list[k].msg == (uint16_t)msg) {
                return invalid(bad, "rtcm");
            }
        }
        list[i].msg = (uint16_t)msg;
        list[i].period_s = (uint16_t)period;
        i++;
    }
    p->rtcm_count = (uint8_t)i;
    memcpy(p->rtcm, list, sizeof(list[0]) * (size_t)i);
    return true;
}

/*
 * An antenna descriptor or serial number: trailing spaces dropped (the
 * stored form, so what comes back is what is kept), then the profile's own
 * rule (um98x_antenna_str_valid: printable ASCII without '"', at most
 * UM98X_ANTENNA_STR_MAX characters). Inner spaces stay: an IGS name pads
 * the antenna type before its radome code.
 */
static bool antenna_str(const cJSON *obj, const char *key, char *dst, size_t dst_size, web_bad_t *bad)
{
    const cJSON *it = web_member(obj, key);
    if (it == NULL) {
        return true;
    }
    if (!cJSON_IsString(it) || it->valuestring == NULL) {
        return invalid(bad, key);
    }
    const char *s = it->valuestring;
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }
    char tmp[UM98X_ANTENNA_STR_MAX + 1];
    if (n > UM98X_ANTENNA_STR_MAX || n >= dst_size) {
        return invalid(bad, key);
    }
    memcpy(tmp, s, n);
    tmp[n] = '\0';
    if (!um98x_antenna_str_valid(tmp, sizeof(tmp))) {
        return invalid(bad, key);
    }
    memset(dst, 0, dst_size);
    memcpy(dst, tmp, n + 1);
    return true;
}

static bool parse_profile(const cJSON *obj, const um98x_base_profile_t *stored, um98x_base_profile_t *p,
                          web_bad_t *bad)
{
    if (!cJSON_IsObject(obj)) {
        return invalid(bad, "profile");
    }
    *p = *stored;
    int64_t v = 0;
    web_member_t m;

    if ((m = web_member_int(obj, "signal_group_master", 0, 255, &v)) == WEB_MEMBER_BAD) {
        return invalid(bad, "signal_group_master");
    } else if (m == WEB_MEMBER_OK) {
        p->signal_group_master = (uint8_t)v;
    }
    if ((m = web_member_int(obj, "signal_group_slave", 0, 255, &v)) == WEB_MEMBER_BAD) {
        return invalid(bad, "signal_group_slave");
    } else if (m == WEB_MEMBER_OK) {
        p->signal_group_slave = (uint8_t)v;
    }
    const char *mode = NULL;
    if ((m = web_member_str(obj, "base_mode", 16, &mode)) == WEB_MEMBER_BAD || (m == WEB_MEMBER_OK && !web_base_mode_parse(mode, &p->base_mode))) {
        return invalid(bad, "base_mode");
    }
    if ((m = web_member_int(obj, "station_id", 0, UM98X_STATION_ID_MAX, &v)) == WEB_MEMBER_BAD) {
        return invalid(bad, "station_id");
    } else if (m == WEB_MEMBER_OK) {
        p->station_id = (uint16_t)v;
    }
    if ((m = web_member_int(obj, "com1_baud", 0, UINT32_MAX, &v)) == WEB_MEMBER_BAD || (m == WEB_MEMBER_OK && !um98x_baud_supported((uint32_t)v))) {
        return invalid(bad, "com1_baud");
    } else if (m == WEB_MEMBER_OK) {
        p->com1_baud = (uint32_t)v;
    }
    bool sat_view = false;
    if ((m = web_member_bool(obj, "sat_view_output", &sat_view)) == WEB_MEMBER_BAD) {
        return invalid(bad, "sat_view_output");
    } else if (m == WEB_MEMBER_OK) {
        p->sat_view_output = sat_view;
    }
    if (!antenna_str(obj, "antenna_model", p->antenna_model, sizeof(p->antenna_model), bad) ||
        !antenna_str(obj, "antenna_serial", p->antenna_serial, sizeof(p->antenna_serial), bad)) {
        return false;
    }
    if ((m = web_member_int(obj, "antenna_setup_id", 0, 255, &v)) == WEB_MEMBER_BAD) {
        return invalid(bad, "antenna_setup_id");
    } else if (m == WEB_MEMBER_OK) {
        p->antenna_setup_id = (uint8_t)v;
    }
    if ((m = web_member_int(obj, "signal_group_um980", 1, 255, &v)) == WEB_MEMBER_BAD) {
        return invalid(bad, "signal_group_um980");
    } else if (m == WEB_MEMBER_OK) {
        p->signal_group_um980 = (uint8_t)v;
    }
    if ((m = web_member_int(obj, "elevation_mask_deg", 0, UM98X_MASK_MAX_DEG, &v)) == WEB_MEMBER_BAD) {
        return invalid(bad, "elevation_mask_deg");
    } else if (m == WEB_MEMBER_OK) {
        p->elevation_mask_deg = (uint8_t)v;
    }

    if (p->base_mode == UM98X_BASE_SURVEY_IN) {
        /* survey_in_s matters only here; a stored 0 (a fixed profile) falls back to the default. */
        if ((m = web_member_int(obj, "survey_in_s", 0, UINT16_MAX, &v)) == WEB_MEMBER_BAD) {
            return invalid(bad, "survey_in_s");
        }
        if (m == WEB_MEMBER_OK) {
            if (v < 1 || v > UM98X_SURVEY_MAX_S) {
                return invalid(bad, "survey_in_s");
            }
            p->survey_in_s = (uint16_t)v;
        } else if (p->survey_in_s == 0 || p->survey_in_s > UM98X_SURVEY_MAX_S) {
            p->survey_in_s = UM98X_DEFAULT_SURVEY_S;
        }
    } else {
        const cJSON *fixed = web_member(obj, "fixed");
        if (fixed != NULL && !cJSON_IsNull(fixed)) {
            double lat = 0;
            double lon = 0;
            double h = 0;
            if (!cJSON_IsObject(fixed) || web_member_double(fixed, "lat_deg", -90.0, 90.0, &lat) != WEB_MEMBER_OK ||
                web_member_double(fixed, "lon_deg", -180.0, 180.0, &lon) != WEB_MEMBER_OK ||
                web_member_double(fixed, "height_m", UM98X_HEIGHT_MIN_M, UM98X_HEIGHT_MAX_M, &h) != WEB_MEMBER_OK) {
                return invalid(bad, "fixed");
            }
            p->fixed_lat_deg = lat;
            p->fixed_lon_deg = lon;
            p->fixed_height_m = h;
        } else if (stored->base_mode != UM98X_BASE_FIXED) {
            return invalid(bad, "fixed");   /* fixed mode needs a position */
        }
        p->survey_in_s = 0;
    }

    const cJSON *rtcm = web_member(obj, "rtcm");
    if (rtcm != NULL && !parse_rtcm(rtcm, p, bad)) {
        return false;
    }
    if (p->com1_baud < 460800) {
        for (size_t i = 0; i < p->rtcm_count; i++) {
            if (web_rtcm_is_ephemeris(p->rtcm[i].msg)) {
                return invalid(bad, "rtcm");
            }
        }
    }
    /* The profile module has the last word. */
    if (!um98x_profile_valid(p)) {
        return invalid(bad, "profile");
    }
    return true;
}

bool web_parse_profile_put(const cJSON *root, const um98x_base_profile_t *stored, um98x_base_profile_t *profile,
                           bool *have_profile, bool *autocfg, bool *have_autocfg, web_bad_t *bad)
{
    *have_profile = false;
    *have_autocfg = false;
    web_member_t m = web_member_bool(root, "autocfg", autocfg);
    if (m == WEB_MEMBER_BAD) {
        return invalid(bad, "autocfg");
    }
    *have_autocfg = m == WEB_MEMBER_OK;
    const cJSON *obj = web_member(root, "profile");
    if (obj != NULL) {
        if (!parse_profile(obj, stored, profile, bad)) {
            return false;
        }
        *have_profile = true;
    }
    return true;
}

bool web_parse_recovery(const cJSON *root, bool *have, health_recovery_mode_t *mode, web_bad_t *bad)
{
    *have = false;
    const char *s = NULL;
    web_member_t m = web_member_str(root, "recovery", 16, &s);
    if (m == WEB_MEMBER_ABSENT) {
        return true;
    }
    if (m == WEB_MEMBER_BAD || !web_recovery_mode_parse(s, mode)) {
        return invalid(bad, "recovery");
    }
    *have = true;
    return true;
}

bool web_parse_maintenance(const cJSON *root, uint32_t *duration_s, web_bad_t *bad)
{
    int64_t v = 0;
    web_member_t m = web_member_int(root, "duration_s", 0, UINT32_MAX, &v);
    if (m == WEB_MEMBER_BAD || (m == WEB_MEMBER_OK && v != 0 && !web_maint_duration_ok((uint32_t)v))) {
        return invalid(bad, "duration_s");
    }
    *duration_s = m == WEB_MEMBER_OK ? (uint32_t)v : 0;
    return true;
}

bool web_parse_live(const cJSON *root, uint32_t max_min, uint32_t *minutes, web_bad_t *bad)
{
    int64_t v = 0;
    web_member_t m = web_member_int(root, "minutes", 0, max_min, &v);
    if (m == WEB_MEMBER_BAD) {
        return invalid(bad, "minutes");
    }
    *minutes = m == WEB_MEMBER_OK ? (uint32_t)v : 0;
    return true;
}

/* ---- receiver queries ------------------------------------------------------------------------- */

bool web_parse_query(const cJSON *root, unsigned *query, web_bad_t *bad)
{
    const char *s = NULL;
    int q = web_member_str(root, "command", 16, &s) == WEB_MEMBER_OK ? um98x_query_find(s) : -1;
    if (q < 0) {
        return invalid_rule(bad, "command", "one of VERSIONA, CONFIG, MODE, ANT1GROUPA, BASEPOSA, SPPNAVA");
    }
    *query = (unsigned)q;
    return true;
}

/* ---- signal-group comparison ------------------------------------------------------------------- */

bool web_parse_signal_test(const cJSON *root, uint8_t out[2][2], uint32_t *phase_s, web_bad_t *bad)
{
    const cJSON *groups = web_member(root, "groups");
    if (!cJSON_IsArray(groups) || cJSON_GetArraySize(groups) != 2) {
        return invalid_rule(bad, "groups", "two signal groups, each {master, slave} of 0-255");
    }
    for (int i = 0; i < 2; i++) {
        const cJSON *g = cJSON_GetArrayItem(groups, i);
        int64_t m = 0;
        int64_t sl = 0;
        if (!cJSON_IsObject(g) || web_member_int(g, "master", 0, 255, &m) != WEB_MEMBER_OK ||
            web_member_int(g, "slave", 0, 255, &sl) != WEB_MEMBER_OK) {
            return invalid_rule(bad, "groups", "two signal groups, each {master, slave} of 0-255");
        }
        out[i][0] = (uint8_t)m;
        out[i][1] = (uint8_t)sl;
    }
    if (out[0][0] == out[1][0] && out[0][1] == out[1][1]) {
        return invalid_rule(bad, "groups", "two different signal groups");
    }
    int64_t v = 0;
    web_member_t pm = web_member_int(root, "phase_s", WEB_SG_TEST_PHASE_MIN_S, WEB_SG_TEST_PHASE_MAX_S, &v);
    if (pm == WEB_MEMBER_BAD) {
        return invalid_rule(bad, "phase_s", "60 to 7200 seconds");
    }
    *phase_s = pm == WEB_MEMBER_OK ? (uint32_t)v : WEB_SG_TEST_PHASE_DEFAULT_S;
    return true;
}

bool web_parse_signal_test_keep(const cJSON *root, unsigned *group, web_bad_t *bad)
{
    int64_t v = 0;
    if (web_member_int(root, "group", 0, 1, &v) != WEB_MEMBER_OK) {
        return invalid_rule(bad, "group", "0 (the first group) or 1 (the second)");
    }
    *group = (unsigned)v;
    return true;
}

web_err_t web_sgt_error(int err, bool safe_mode, const char **field)
{
    *field = NULL;
    switch (err) {
    case STATION_SGT_E_GROUPS:
        *field = "groups";
        return WEB_ERR_INVALID_FIELD;
    case STATION_SGT_E_PHASE:
        *field = "phase_s";
        return WEB_ERR_INVALID_FIELD;
    case STATION_SGT_E_FIRST_APPLY_HELD:
        return WEB_ERR_FIRST_APPLY_HELD;
    case STATION_SGT_E_AUTOCFG_OFF:
        return WEB_ERR_AUTOCFG_OFF;
    case STATION_SGT_E_MAINTENANCE:
        return WEB_ERR_RECEIVER_MAINTENANCE;
    case STATION_SGT_E_JOURNAL_OPEN:
        return WEB_ERR_JOURNAL_OPEN;
    case STATION_SGT_E_SAFE_MODE:
        return WEB_ERR_SAFE_MODE;
    case STATION_SGT_E_NO_RECEIVER:
        return WEB_ERR_NO_RECEIVER;
    case STATION_SGT_E_UNSUPPORTED:
        return WEB_ERR_UNSUPPORTED_MODEL;
    case STATION_SGT_E_IDENTITY:
        return WEB_ERR_IDENTITY_UNCONFIRMED;
    case STATION_SGT_E_NOT_STREAMING:
        return WEB_ERR_RECEIVER_NOT_STREAMING;
    case STATION_SGT_E_CONFIG_FAILED:
        return WEB_ERR_CONFIG_FAILED;
    case STATION_SGT_E_NOT_LOCKED:
        return WEB_ERR_POSITION_NOT_LOCKED;
    case STATION_SGT_E_RUNNING:
        return WEB_ERR_SIGNAL_TEST_RUNNING;
    case STATION_SGT_E_SETTINGS:
        return WEB_ERR_SETTINGS_UNAVAILABLE;
    case STATION_SGT_E_NOT_DECIDING:
        return WEB_ERR_SIGNAL_TEST_NOT_DECIDING;
    case STATION_SGT_E_IDLE:
        return WEB_ERR_SIGNAL_TEST_IDLE;
    case STATION_SGT_E_NOT_RUNNING:
        return safe_mode ? WEB_ERR_SAFE_MODE : WEB_ERR_BUSY;
    default:
        return WEB_ERR_INTERNAL;
    }
}

/* ---- casters ----------------------------------------------------------------------------------- */

static bool str_field_rule(const cJSON *root, const char *key, char *dst, size_t dst_size, size_t max_len,
                           const char *allowed_extra, const char *rule, web_bad_t *bad)
{
    const char *s = NULL;
    web_member_t m = web_member_str(root, key, max_len, &s);
    if (m == WEB_MEMBER_BAD || (m == WEB_MEMBER_OK && allowed_extra != NULL && !web_chars_ok(s, allowed_extra))) {
        return invalid_rule(bad, key, rule);
    }
    if (m == WEB_MEMBER_OK) {
        strlcpy(dst, s, dst_size);
    }
    return true;
}

/* The caster fields' rules in words (the 400's message; the UI shows "<label>: <rule>"). */
#define RULE_NAME     "at most 23 bytes (fewer characters with accents or symbols), no control characters"
#define RULE_HOST     "a host name or an IPv4 address, at most 63 characters"
#define RULE_PORT     "a port from 1 to 65535"
#define RULE_MOUNT    "letters, digits, \".\", \"_\" and \"-\", at most 47"
#define RULE_USER     "at most 47 bytes, without \":\""
#define RULE_PASSWORD "at most 63 bytes, no control characters"
#define RULE_PW_V1    "no spaces (NTRIP 1.0 sends the password in the request line)"
#define RULE_PW_ONO   "1 to 63 characters, no spaces"
#define RULE_LABEL    "letters and digits only, at most 47"
#define RULE_TEST_ONO "only in bench and QEMU builds"

/* RTK2go's rule a new mountpoint breaks, in words. */
static const char *rtk2go_rule(ntrip_rtk2go_mount_t r)
{
    switch (r) {
    case NTRIP_RTK2GO_BAD_CHARS:
        return "RTK2go mountpoints use letters, digits, \"_\" and \"-\" only (no \".\")";
    case NTRIP_RTK2GO_FIRST_NOT_LETTER:
        return "RTK2go mountpoints start with a letter";
    case NTRIP_RTK2GO_FIRST_A:
        return "RTK2go no longer accepts new mountpoints that start with \"A\"";
    default:
        return "RTK2go mountpoints have 4 to 20 characters";
    }
}

bool web_parse_caster_put(const cJSON *root, const ntrip_caster_cfg_t *stored, ntrip_caster_cfg_t *out,
                          web_bad_t *bad)
{
    ntrip_caster_cfg_t c = *stored;
    memset(c.password, 0, sizeof(c.password));
    c.clear_password = false;
    web_member_t m;
    bool b = false;
    int64_t v = 0;

    if ((m = web_member_bool(root, "enabled", &b)) == WEB_MEMBER_BAD) {
        return invalid(bad, "enabled");
    } else if (m == WEB_MEMBER_OK) {
        c.enabled = b;
    }
    if (!str_field_rule(root, "name", c.name, sizeof(c.name), sizeof(c.name) - 1, NULL, RULE_NAME, bad)) {
        return false;
    }
    const char *host = NULL;
    if ((m = web_member_str(root, "host", sizeof(c.host) - 1, &host)) == WEB_MEMBER_BAD ||
        (m == WEB_MEMBER_OK && host[0] != '\0' && !web_host_name_ok(host))) {
        return invalid_rule(bad, "host", RULE_HOST);
    } else if (m == WEB_MEMBER_OK) {
        strlcpy(c.host, host, sizeof(c.host));
    }
    if ((m = web_member_int(root, "port", 1, 65535, &v)) == WEB_MEMBER_BAD) {
        return invalid_rule(bad, "port", RULE_PORT);
    } else if (m == WEB_MEMBER_OK) {
        c.port = (uint16_t)v;
    }
    if ((m = web_member_int(root, "ntrip_version", 1, 2, &v)) == WEB_MEMBER_BAD) {
        return invalid(bad, "ntrip_version");
    } else if (m == WEB_MEMBER_OK) {
        c.proto = v == 2 ? NTRIP_PROTO_V2 : NTRIP_PROTO_V1;
    }
    if ((m = web_member_bool(root, "tls", &b)) == WEB_MEMBER_BAD) {
        return invalid(bad, "tls");
    } else if (m == WEB_MEMBER_OK) {
        c.tls = b;
    }
    const char *trust = NULL;
    if ((m = web_member_str(root, "trust", 16, &trust)) == WEB_MEMBER_BAD || (m == WEB_MEMBER_OK && !web_ntrip_trust_parse(trust, &c.trust_mode))) {
        return invalid(bad, "trust");
    }
    /* The mountpoint. A new RTK2go name that breaks the generic rule is told
     * RTK2go's own rule: the generic one allows "." and 47 characters, which
     * RTK2go refuses, so its words would send the user to a second refusal. */
    const char *mount = NULL;
    if ((m = web_member_str(root, "mountpoint", sizeof(c.mountpoint) - 1, &mount)) == WEB_MEMBER_BAD ||
        (m == WEB_MEMBER_OK && !web_chars_ok(mount, "._-"))) {
        const cJSON *jm = web_member(root, "mountpoint");
        ntrip_rtk2go_mount_t rk = NTRIP_RTK2GO_OK;
        if (cJSON_IsString(jm) && jm->valuestring != NULL) {
            rk = ntrip_server_rtk2go_check(stored->host, stored->mountpoint, c.host, jm->valuestring);
        }
        return invalid_rule(bad, "mountpoint", rk != NTRIP_RTK2GO_OK ? rtk2go_rule(rk) : RULE_MOUNT);
    } else if (m == WEB_MEMBER_OK) {
        strlcpy(c.mountpoint, mount, sizeof(c.mountpoint));
    }
    const char *user = NULL;
    if ((m = web_member_str(root, "username", sizeof(c.username) - 1, &user)) == WEB_MEMBER_BAD ||
        (m == WEB_MEMBER_OK && strchr(user, ':') != NULL)) {
        return invalid_rule(bad, "username", RULE_USER);
    } else if (m == WEB_MEMBER_OK) {
        strlcpy(c.username, user, sizeof(c.username));
    }
    const char *pw = NULL;
    if ((m = web_member_str(root, "password", sizeof(c.password) - 1, &pw)) == WEB_MEMBER_BAD) {
        return invalid_rule(bad, "password", RULE_PASSWORD);
    } else if (m == WEB_MEMBER_OK) {
        strlcpy(c.password, pw, sizeof(c.password));
    }
    if ((m = web_member_bool(root, "clear_password", &b)) == WEB_MEMBER_BAD) {
        return invalid(bad, "clear_password");
    } else if (m == WEB_MEMBER_OK && b) {
        c.clear_password = true;
        memset(c.password, 0, sizeof(c.password));
    }
    if (!str_field_rule(root, "label", c.label, sizeof(c.label), sizeof(c.label) - 1, "", RULE_LABEL, bad)) {
        return false;
    }
    if ((m = web_member_bool(root, "v2_raw_body", &b)) == WEB_MEMBER_BAD) {
        return invalid(bad, "v2_raw_body");
    } else if (m == WEB_MEMBER_OK) {
        c.v2_raw_body = b;
    }
    if ((m = web_member_bool(root, "dup_stream_ack", &b)) == WEB_MEMBER_BAD) {
        return invalid(bad, "dup_stream_ack");
    } else if (m == WEB_MEMBER_OK) {
        c.dup_stream_ack = b;
    }
    if ((m = web_member_bool(root, "onocoy_stats", &b)) == WEB_MEMBER_BAD) {
        return invalid(bad, "onocoy_stats");
    } else if (m == WEB_MEMBER_OK) {
        c.onocoy_stats = b;
    }
    if ((m = web_member_bool(root, "test_onocoy", &b)) == WEB_MEMBER_BAD) {
        return invalid(bad, "test_onocoy");
    } else if (m == WEB_MEMBER_OK) {
        if (b && !ntrip_test_onocoy_allowed()) {
            return invalid_rule(bad, "test_onocoy", RULE_TEST_ONO);
        }
        c.test_onocoy = b;
    }
    const char *backoff_s = NULL;
    ntrip_backoff_profile_t backoff = c.backoff_profile;
    if ((m = web_member_str(root, "backoff_profile", 16, &backoff_s)) == WEB_MEMBER_BAD ||
        (m == WEB_MEMBER_OK && !web_ntrip_backoff_parse(backoff_s, &backoff))) {
        return invalid(bad, "backoff_profile");
    }

    /* Server-set fields: protected_slot and the backoff profile follow the
     * caster component's host rules (a requested profile they refuse is
     * named here). */
    if (!ntrip_server_apply_host_rules(&c, m == WEB_MEMBER_OK ? &backoff : NULL)) {
        return invalid(bad, "backoff_profile");
    }
    c.password_set = false;
    c.ca_pem_set = false;
    bool onocoy = c.protected_slot;   /* the host rules: exactly the slots that count as Onocoy */

    /* RTK2go's rules for a new mountpoint (a new RTK2go host or a changed
     * mountpoint only: a slot stored before keeps its name). */
    ntrip_rtk2go_mount_t rk = ntrip_server_rtk2go_check(stored->host, stored->mountpoint, c.host, c.mountpoint);
    if (rk != NTRIP_RTK2GO_OK) {
        return invalid_rule(bad, "mountpoint", rtk2go_rule(rk));
    }

    if (onocoy) {
        /* What the Onocoy console gives. NTRIP 1.0 sends the username as the
         * mountpoint and the password in the request line, so both keep
         * those rules whichever version is chosen. */
        if (c.username[0] != '\0' && !web_chars_ok(c.username, "._-")) {
            return invalid_rule(bad, "username", RULE_MOUNT);
        }
        if (strchr(c.password, ' ') != NULL) {
            return invalid_rule(bad, "password", RULE_PW_ONO);
        }
    } else if (c.proto == NTRIP_PROTO_V1 && strchr(c.password, ' ') != NULL) {
        /* NTRIP 1.0 sends the password in the request line. */
        return invalid_rule(bad, "password", RULE_PW_V1);
    }
    if (c.enabled) {
        bool keep_old = !c.clear_password && stored->password_set && strcmp(c.host, stored->host) == 0;
        bool have_pw = c.password[0] != '\0' || keep_old;
        if (c.host[0] == '\0') {
            return invalid_rule(bad, "host", "enter the caster's host name");
        }
        if (c.port == 0) {
            return invalid_rule(bad, "port", RULE_PORT);
        }
        if (c.mountpoint[0] == '\0') {
            return invalid_rule(bad, "mountpoint", "enter the mountpoint");
        }
        if (onocoy && c.username[0] == '\0') {
            return invalid_rule(bad, "username", "enter the username from the Onocoy console");
        }
        if ((c.proto == NTRIP_PROTO_V1 || onocoy) && !have_pw) {
            return invalid_rule(bad, "password", "enter the password");
        }
        if (c.proto == NTRIP_PROTO_V2 && !onocoy && c.username[0] == '\0') {
            return invalid_rule(bad, "username", "NTRIP 2.0 needs a username");
        }
    }
    *out = c;
    return true;
}

bool web_parse_ca_pem(const cJSON *root, const char **pem, web_bad_t *bad)
{
    const cJSON *it = web_member(root, "pem");
    if (it == NULL || !cJSON_IsString(it) || !web_pem_certs_only(it->valuestring)) {
        return invalid(bad, "pem");
    }
    *pem = it->valuestring;
    return true;
}

/* ---- local NTRIP (Your rover) --------------------------------------------------------------------- */

bool web_parse_local_caster_put(const cJSON *root, const local_caster_cfg_t *stored, local_caster_cfg_t *out,
                                web_bad_t *bad)
{
    *out = *stored;
    memset(out->password, 0, sizeof(out->password));
    bool b = false;
    web_member_t m = web_member_bool(root, "enabled", &b);
    if (m == WEB_MEMBER_BAD) {
        return invalid(bad, "enabled");
    }
    if (m == WEB_MEMBER_OK) {
        out->enabled = b;
    }
    /* No rule here: the handler asks local_caster_check_config() for the field's words. */
    if (!str_field_rule(root, "mountpoint", out->mountpoint, sizeof(out->mountpoint), sizeof(out->mountpoint) - 1, NULL,
                        NULL, bad) ||
        !str_field_rule(root, "username", out->username, sizeof(out->username), sizeof(out->username) - 1, NULL, NULL,
                        bad) ||
        !str_field_rule(root, "password", out->password, sizeof(out->password), sizeof(out->password) - 1, NULL, NULL,
                        bad)) {
        memset(out->password, 0, sizeof(out->password));
        return false;
    }
    return true;
}

/* ---- Wi-Fi and system ----------------------------------------------------------------------------- */

bool web_parse_wifi_put(const cJSON *root, web_wifi_put_t *out, web_bad_t *bad)
{
    memset(out, 0, sizeof(*out));
    const cJSON *it = web_member(root, "ssid");
    if (it != NULL) {
        if (!cJSON_IsString(it) || !web_ssid_valid(it->valuestring)) {
            return invalid(bad, "ssid");
        }
        out->have_ssid = true;
        out->ssid = it->valuestring;
    }
    it = web_member(root, "password");
    if (it != NULL) {
        if (!cJSON_IsString(it) || !web_wifi_password_valid(it->valuestring)) {
            return invalid(bad, "password");
        }
        out->have_password = true;
        out->password = it->valuestring;
    }
    it = web_member(root, "ap_password");
    if (it != NULL) {
        if (!cJSON_IsString(it) || (it->valuestring[0] != '\0' && !web_ap_password_valid(it->valuestring))) {
            return invalid(bad, "ap_password");
        }
        out->have_ap_password = true;
        out->ap_password = it->valuestring;
    }
    if (out->have_password && !out->have_ssid) {
        return invalid(bad, "ssid");   /* a password belongs to an SSID */
    }
    if (web_member_bool(root, "no_trial", &out->no_trial) == WEB_MEMBER_BAD) {
        return invalid(bad, "no_trial");
    }
    return true;
}

bool web_parse_system_put(const cJSON *root, web_system_put_t *out, web_bad_t *bad)
{
    memset(out, 0, sizeof(*out));
    const cJSON *it = web_member(root, "hostname");
    if (it != NULL) {
        if (!cJSON_IsString(it) || !web_hostname_valid(it->valuestring)) {
            return invalid(bad, "hostname");
        }
        out->have_hostname = true;
        out->hostname = it->valuestring;
    }
    web_member_t m = web_member_bool(root, "public_status", &out->public_status);
    if (m == WEB_MEMBER_BAD) {
        return invalid(bad, "public_status");
    }
    out->have_public_status = m == WEB_MEMBER_OK;
    const cJSON *w = web_member(root, "wiring");
    if (w != NULL) {
        out->have_wiring = true;
        if (cJSON_IsNull(w)) {
            out->wiring_default = true;
            return true;
        }
        if (!cJSON_IsObject(w)) {
            return invalid(bad, "wiring");
        }
        int64_t rx;
        int64_t tx;
        int64_t port;
        if (web_member_int(w, "rx_gpio", 0, 48, &rx) != WEB_MEMBER_OK) {
            return invalid_rule(bad, "wiring.rx_gpio", "a GPIO number, 0-48");
        }
        if (web_member_int(w, "tx_gpio", 0, 48, &tx) != WEB_MEMBER_OK) {
            return invalid_rule(bad, "wiring.tx_gpio", "a GPIO number, 0-48");
        }
        if (web_member_int(w, "port", 1, 3, &port) != WEB_MEMBER_OK) {
            return invalid_rule(bad, "wiring.port", "1, 2 or 3 (COM1 to COM3)");
        }
        out->rx_gpio = (int8_t)rx;
        out->tx_gpio = (int8_t)tx;
        out->port = (uint8_t)port;
    }
    return true;
}

bool web_parse_log_level(const cJSON *root, const char **tag, esp_log_level_t *level, web_bad_t *bad)
{
    const char *t = NULL;
    if (web_member_str(root, "tag", 15, &t) != WEB_MEMBER_OK || t[0] == '\0') {
        return invalid(bad, "tag");
    }
    const char *l = NULL;
    if (web_member_str(root, "level", 16, &l) != WEB_MEMBER_OK || !web_log_level_parse(l, level)) {
        return invalid(bad, "level");
    }
    *tag = t;
    return true;
}

bool web_parse_admin_password(const cJSON *root, const char **current, const char **next, web_bad_t *bad)
{
    return web_parse_password(root, "current", current, bad) && web_parse_password(root, "new", next, bad);
}

/* ---- updates and uploads -------------------------------------------------------------------------- */

bool web_parse_update_app(const cJSON *root, const char **version, bool *rolled_back_ack, web_bad_t *bad)
{
    if (web_member_str(root, "version", 31, version) != WEB_MEMBER_OK || (*version)[0] == '\0') {
        return invalid(bad, "version");
    }
    *rolled_back_ack = false;
    if (web_member_bool(root, "rolled_back_ack", rolled_back_ack) == WEB_MEMBER_BAD) {
        return invalid(bad, "rolled_back_ack");
    }
    return true;
}

bool web_parse_update_gnss(const cJSON *root, uint32_t *build, bool *cross_layout_ack, web_bad_t *bad)
{
    int64_t v = 0;
    if (web_member_int(root, "build", 1, UINT32_MAX, &v) != WEB_MEMBER_OK) {
        return invalid(bad, "build");
    }
    *build = (uint32_t)v;
    *cross_layout_ack = false;
    if (web_member_bool(root, "cross_layout_ack", cross_layout_ack) == WEB_MEMBER_BAD) {
        return invalid(bad, "cross_layout_ack");
    }
    return true;
}

bool web_parse_upload_confirm(const cJSON *root, uint8_t sha256[32], const char **expected_fw,
                              uint32_t *expected_build, bool *cross_layout_ack, web_bad_t *bad)
{
    const char *hex = NULL;
    if (web_member_str(root, "sha256", 64, &hex) != WEB_MEMBER_OK || !web_parse_sha256_hex(hex, sha256)) {
        return invalid(bad, "sha256");
    }
    const char *fw = NULL;
    uint32_t build = 0;
    if (web_member_str(root, "expected_fw", 47, &fw) != WEB_MEMBER_OK || (build = web_expected_build(fw)) == 0) {
        return invalid(bad, "expected_fw");
    }
    *expected_fw = fw;
    *expected_build = build;
    *cross_layout_ack = false;
    if (web_member_bool(root, "cross_layout_ack", cross_layout_ack) == WEB_MEMBER_BAD) {
        return invalid(bad, "cross_layout_ack");
    }
    return true;
}
