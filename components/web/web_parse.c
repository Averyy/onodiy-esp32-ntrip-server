/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_parse.h"

#include <math.h>
#include <string.h>

#include "um98x_profile.h"
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

bool web_json_depth_ok(const char *text, size_t len, unsigned max_depth)
{
    unsigned depth = 0;
    bool in_str = false;
    bool esc = false;
    for (size_t i = 0; text != NULL && i < len; i++) {
        char c = text[i];
        if (in_str) {
            if (esc) {
                esc = false;
            } else if (c == '\\') {
                esc = true;
            } else if (c == '"') {
                in_str = false;
            }
            continue;
        }
        if (c == '"') {
            in_str = true;
        } else if (c == '{' || c == '[') {
            if (++depth > max_depth) {
                return false;
            }
        } else if ((c == '}' || c == ']') && depth > 0) {
            depth--;
        }
    }
    return true;
}

cJSON *web_parse_body(const char *body, size_t len, web_bad_t *bad)
{
    bool blank = true;
    for (size_t i = 0; body != NULL && i < len; i++) {
        if (body[i] != ' ' && body[i] != '\t' && body[i] != '\r' && body[i] != '\n') {
            blank = false;
            break;
        }
    }
    /* cJSON recurses per level: a deep body must not reach the server's stack. */
    if (!blank && !web_json_depth_ok(body, len, WEB_JSON_MAX_DEPTH)) {
        fail(bad, WEB_ERR_BAD_JSON, NULL);
        return NULL;
    }
    cJSON *root = blank ? cJSON_CreateObject() : cJSON_ParseWithLength(body, len);
    if (root == NULL || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        fail(bad, WEB_ERR_BAD_JSON, NULL);
        return NULL;
    }
    return root;
}

/* ---- typed member access -------------------------------------------------------------- */

typedef enum { M_ABSENT, M_OK, M_BAD } member_t;

static const cJSON *get(const cJSON *obj, const char *key)
{
    return cJSON_GetObjectItemCaseSensitive(obj, key);
}

static member_t get_bool(const cJSON *obj, const char *key, bool *out)
{
    const cJSON *it = get(obj, key);
    if (it == NULL) {
        return M_ABSENT;
    }
    if (!cJSON_IsBool(it)) {
        return M_BAD;
    }
    *out = cJSON_IsTrue(it);
    return M_OK;
}

/* An integer within [min, max] (JSON numbers are doubles; fractions refused). */
static member_t get_int(const cJSON *obj, const char *key, int64_t min, int64_t max, int64_t *out)
{
    const cJSON *it = get(obj, key);
    if (it == NULL) {
        return M_ABSENT;
    }
    if (!cJSON_IsNumber(it)) {
        return M_BAD;
    }
    double v = it->valuedouble;
    if (!isfinite(v) || v != floor(v) || v < (double)min || v > (double)max) {
        return M_BAD;
    }
    *out = (int64_t)v;
    return M_OK;
}

static member_t get_double(const cJSON *obj, const char *key, double min, double max, double *out)
{
    const cJSON *it = get(obj, key);
    if (it == NULL) {
        return M_ABSENT;
    }
    if (!cJSON_IsNumber(it) || !isfinite(it->valuedouble) || it->valuedouble < min || it->valuedouble > max) {
        return M_BAD;
    }
    *out = it->valuedouble;
    return M_OK;
}

/* A string of at most max_len bytes without control characters. */
static member_t get_str(const cJSON *obj, const char *key, size_t max_len, const char **out)
{
    const cJSON *it = get(obj, key);
    if (it == NULL) {
        return M_ABSENT;
    }
    if (!cJSON_IsString(it) || it->valuestring == NULL || strlen(it->valuestring) > max_len ||
        !web_no_ctrl(it->valuestring)) {
        return M_BAD;
    }
    *out = it->valuestring;
    return M_OK;
}

/* ---- session and setup -------------------------------------------------------------------- */

bool web_parse_password(const cJSON *root, const char *key, const char **out, web_bad_t *bad)
{
    const cJSON *it = get(root, key);
    if (it == NULL || !cJSON_IsString(it) || it->valuestring == NULL) {
        return invalid(bad, key);
    }
    *out = it->valuestring;
    return true;
}

bool web_parse_confirm(const cJSON *root, web_bad_t *bad)
{
    bool confirm = false;
    if (get_bool(root, "confirm", &confirm) != M_OK || !confirm) {
        return fail(bad, WEB_ERR_CONFIRM_REQUIRED, "confirm");
    }
    return true;
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
    if (get_str(root, "step", 16, &name) != M_OK) {
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
        if (!cJSON_IsObject(e) || get_int(e, "msg", 1001, 4095, &msg) != M_OK ||
            get_int(e, "period_s", 1, UINT16_MAX, &period) != M_OK) {
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
    const cJSON *it = get(obj, key);
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
    member_t m;

    if ((m = get_int(obj, "signal_group_master", 0, 255, &v)) == M_BAD) {
        return invalid(bad, "signal_group_master");
    } else if (m == M_OK) {
        p->signal_group_master = (uint8_t)v;
    }
    if ((m = get_int(obj, "signal_group_slave", 0, 255, &v)) == M_BAD) {
        return invalid(bad, "signal_group_slave");
    } else if (m == M_OK) {
        p->signal_group_slave = (uint8_t)v;
    }
    const char *mode = NULL;
    if ((m = get_str(obj, "base_mode", 16, &mode)) == M_BAD || (m == M_OK && !web_base_mode_parse(mode, &p->base_mode))) {
        return invalid(bad, "base_mode");
    }
    if ((m = get_int(obj, "station_id", 0, UM98X_STATION_ID_MAX, &v)) == M_BAD) {
        return invalid(bad, "station_id");
    } else if (m == M_OK) {
        p->station_id = (uint16_t)v;
    }
    if ((m = get_int(obj, "com1_baud", 0, UINT32_MAX, &v)) == M_BAD || (m == M_OK && !um98x_baud_supported((uint32_t)v))) {
        return invalid(bad, "com1_baud");
    } else if (m == M_OK) {
        p->com1_baud = (uint32_t)v;
    }
    bool sat_view = false;
    if ((m = get_bool(obj, "sat_view_output", &sat_view)) == M_BAD) {
        return invalid(bad, "sat_view_output");
    } else if (m == M_OK) {
        p->sat_view_output = sat_view;
    }
    if (!antenna_str(obj, "antenna_model", p->antenna_model, sizeof(p->antenna_model), bad) ||
        !antenna_str(obj, "antenna_serial", p->antenna_serial, sizeof(p->antenna_serial), bad)) {
        return false;
    }
    if ((m = get_int(obj, "antenna_setup_id", 0, 255, &v)) == M_BAD) {
        return invalid(bad, "antenna_setup_id");
    } else if (m == M_OK) {
        p->antenna_setup_id = (uint8_t)v;
    }

    if (p->base_mode == UM98X_BASE_SURVEY_IN) {
        /* survey_in_s matters only here; a stored 0 (a fixed profile) falls back to the default. */
        if ((m = get_int(obj, "survey_in_s", 0, UINT16_MAX, &v)) == M_BAD) {
            return invalid(bad, "survey_in_s");
        }
        if (m == M_OK) {
            if (v < 1 || v > UM98X_SURVEY_MAX_S) {
                return invalid(bad, "survey_in_s");
            }
            p->survey_in_s = (uint16_t)v;
        } else if (p->survey_in_s == 0 || p->survey_in_s > UM98X_SURVEY_MAX_S) {
            p->survey_in_s = UM98X_DEFAULT_SURVEY_S;
        }
    } else {
        const cJSON *fixed = get(obj, "fixed");
        if (fixed != NULL && !cJSON_IsNull(fixed)) {
            double lat = 0;
            double lon = 0;
            double h = 0;
            if (!cJSON_IsObject(fixed) || get_double(fixed, "lat_deg", -90.0, 90.0, &lat) != M_OK ||
                get_double(fixed, "lon_deg", -180.0, 180.0, &lon) != M_OK ||
                get_double(fixed, "height_m", UM98X_HEIGHT_MIN_M, UM98X_HEIGHT_MAX_M, &h) != M_OK) {
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

    const cJSON *rtcm = get(obj, "rtcm");
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
    member_t m = get_bool(root, "autocfg", autocfg);
    if (m == M_BAD) {
        return invalid(bad, "autocfg");
    }
    *have_autocfg = m == M_OK;
    const cJSON *obj = get(root, "profile");
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
    member_t m = get_str(root, "recovery", 16, &s);
    if (m == M_ABSENT) {
        return true;
    }
    if (m == M_BAD || !web_recovery_mode_parse(s, mode)) {
        return invalid(bad, "recovery");
    }
    *have = true;
    return true;
}

bool web_parse_maintenance(const cJSON *root, uint32_t *duration_s, web_bad_t *bad)
{
    int64_t v = 0;
    member_t m = get_int(root, "duration_s", 0, UINT32_MAX, &v);
    if (m == M_BAD || (m == M_OK && v != 0 && !web_maint_duration_ok((uint32_t)v))) {
        return invalid(bad, "duration_s");
    }
    *duration_s = m == M_OK ? (uint32_t)v : 0;
    return true;
}

bool web_parse_live(const cJSON *root, uint32_t max_min, uint32_t *minutes, web_bad_t *bad)
{
    int64_t v = 0;
    member_t m = get_int(root, "minutes", 0, max_min, &v);
    if (m == M_BAD) {
        return invalid(bad, "minutes");
    }
    *minutes = m == M_OK ? (uint32_t)v : 0;
    return true;
}

/* ---- casters ----------------------------------------------------------------------------------- */

static bool str_field_rule(const cJSON *root, const char *key, char *dst, size_t dst_size, size_t max_len,
                           const char *allowed_extra, const char *rule, web_bad_t *bad)
{
    const char *s = NULL;
    member_t m = get_str(root, key, max_len, &s);
    if (m == M_BAD || (m == M_OK && allowed_extra != NULL && !web_chars_ok(s, allowed_extra))) {
        return invalid_rule(bad, key, rule);
    }
    if (m == M_OK) {
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

bool web_parse_caster_put(const cJSON *root, const ntrip_caster_cfg_t *stored, ntrip_caster_cfg_t *out,
                          web_bad_t *bad)
{
    ntrip_caster_cfg_t c = *stored;
    memset(c.password, 0, sizeof(c.password));
    c.clear_password = false;
    member_t m;
    bool b = false;
    int64_t v = 0;

    if ((m = get_bool(root, "enabled", &b)) == M_BAD) {
        return invalid(bad, "enabled");
    } else if (m == M_OK) {
        c.enabled = b;
    }
    if (!str_field_rule(root, "name", c.name, sizeof(c.name), sizeof(c.name) - 1, NULL, RULE_NAME, bad)) {
        return false;
    }
    const char *host = NULL;
    if ((m = get_str(root, "host", sizeof(c.host) - 1, &host)) == M_BAD ||
        (m == M_OK && host[0] != '\0' && !web_host_name_ok(host))) {
        return invalid_rule(bad, "host", RULE_HOST);
    } else if (m == M_OK) {
        strlcpy(c.host, host, sizeof(c.host));
    }
    if ((m = get_int(root, "port", 1, 65535, &v)) == M_BAD) {
        return invalid_rule(bad, "port", RULE_PORT);
    } else if (m == M_OK) {
        c.port = (uint16_t)v;
    }
    if ((m = get_int(root, "ntrip_version", 1, 2, &v)) == M_BAD) {
        return invalid(bad, "ntrip_version");
    } else if (m == M_OK) {
        c.proto = v == 2 ? NTRIP_PROTO_V2 : NTRIP_PROTO_V1;
    }
    if ((m = get_bool(root, "tls", &b)) == M_BAD) {
        return invalid(bad, "tls");
    } else if (m == M_OK) {
        c.tls = b;
    }
    const char *trust = NULL;
    if ((m = get_str(root, "trust", 16, &trust)) == M_BAD || (m == M_OK && !web_ntrip_trust_parse(trust, &c.trust_mode))) {
        return invalid(bad, "trust");
    }
    if (!str_field_rule(root, "mountpoint", c.mountpoint, sizeof(c.mountpoint), sizeof(c.mountpoint) - 1, "._-",
                        RULE_MOUNT, bad)) {
        return false;
    }
    const char *user = NULL;
    if ((m = get_str(root, "username", sizeof(c.username) - 1, &user)) == M_BAD ||
        (m == M_OK && strchr(user, ':') != NULL)) {
        return invalid_rule(bad, "username", RULE_USER);
    } else if (m == M_OK) {
        strlcpy(c.username, user, sizeof(c.username));
    }
    const char *pw = NULL;
    if ((m = get_str(root, "password", sizeof(c.password) - 1, &pw)) == M_BAD) {
        return invalid_rule(bad, "password", RULE_PASSWORD);
    } else if (m == M_OK) {
        strlcpy(c.password, pw, sizeof(c.password));
    }
    if ((m = get_bool(root, "clear_password", &b)) == M_BAD) {
        return invalid(bad, "clear_password");
    } else if (m == M_OK && b) {
        c.clear_password = true;
        memset(c.password, 0, sizeof(c.password));
    }
    if (!str_field_rule(root, "label", c.label, sizeof(c.label), sizeof(c.label) - 1, "", RULE_LABEL, bad)) {
        return false;
    }
    if ((m = get_bool(root, "v2_raw_body", &b)) == M_BAD) {
        return invalid(bad, "v2_raw_body");
    } else if (m == M_OK) {
        c.v2_raw_body = b;
    }
    if ((m = get_bool(root, "dup_stream_ack", &b)) == M_BAD) {
        return invalid(bad, "dup_stream_ack");
    } else if (m == M_OK) {
        c.dup_stream_ack = b;
    }
    const char *backoff_s = NULL;
    ntrip_backoff_profile_t backoff = c.backoff_profile;
    if ((m = get_str(root, "backoff_profile", 16, &backoff_s)) == M_BAD ||
        (m == M_OK && !web_ntrip_backoff_parse(backoff_s, &backoff))) {
        return invalid(bad, "backoff_profile");
    }

    /* Server-set fields: protected_slot and the backoff profile follow the
     * caster component's host rules (a requested profile they refuse is
     * named here). */
    if (!ntrip_server_apply_host_rules(&c, m == M_OK ? &backoff : NULL)) {
        return invalid(bad, "backoff_profile");
    }
    c.password_set = false;
    c.ca_pem_set = false;
    bool onocoy = c.protected_slot;   /* the host rules: exactly the Onocoy hosts */

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

bool web_pem_certs_only(const char *pem)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----";
    static const char end[] = "-----END CERTIFICATE-----";
    if (pem == NULL) {
        return false;
    }
    size_t len = strlen(pem);
    if (len == 0 || len > WEB_CA_PEM_MAX) {
        return false;
    }
    size_t certs = 0;
    const char *p = pem;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        if (strncmp(p, begin, sizeof(begin) - 1) != 0) {
            return false;
        }
        p += sizeof(begin) - 1;
        const char *e = strstr(p, end);
        if (e == NULL) {
            return false;
        }
        /* The body is base64 and line breaks only. */
        for (const char *q = p; q < e; q++) {
            char ch = *q;
            bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '+' ||
                      ch == '/' || ch == '=' || ch == '\r' || ch == '\n';
            if (!ok) {
                return false;
            }
        }
        p = e + sizeof(end) - 1;
        certs++;
    }
    return certs > 0;
}

bool web_parse_ca_pem(const cJSON *root, const char **pem, web_bad_t *bad)
{
    const cJSON *it = get(root, "pem");
    if (it == NULL || !cJSON_IsString(it) || !web_pem_certs_only(it->valuestring)) {
        return invalid(bad, "pem");
    }
    *pem = it->valuestring;
    return true;
}

/* ---- Wi-Fi and system ----------------------------------------------------------------------------- */

bool web_parse_wifi_put(const cJSON *root, web_wifi_put_t *out, web_bad_t *bad)
{
    memset(out, 0, sizeof(*out));
    const cJSON *it = get(root, "ssid");
    if (it != NULL) {
        if (!cJSON_IsString(it) || !web_ssid_valid(it->valuestring)) {
            return invalid(bad, "ssid");
        }
        out->have_ssid = true;
        out->ssid = it->valuestring;
    }
    it = get(root, "password");
    if (it != NULL) {
        if (!cJSON_IsString(it) || !web_wifi_password_valid(it->valuestring)) {
            return invalid(bad, "password");
        }
        out->have_password = true;
        out->password = it->valuestring;
    }
    it = get(root, "ap_password");
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
    return true;
}

bool web_parse_system_put(const cJSON *root, web_system_put_t *out, web_bad_t *bad)
{
    memset(out, 0, sizeof(*out));
    const cJSON *it = get(root, "hostname");
    if (it != NULL) {
        if (!cJSON_IsString(it) || !web_hostname_valid(it->valuestring)) {
            return invalid(bad, "hostname");
        }
        out->have_hostname = true;
        out->hostname = it->valuestring;
    }
    member_t m = get_bool(root, "public_status", &out->public_status);
    if (m == M_BAD) {
        return invalid(bad, "public_status");
    }
    out->have_public_status = m == M_OK;
    return true;
}

bool web_parse_log_level(const cJSON *root, const char **tag, esp_log_level_t *level, web_bad_t *bad)
{
    const char *t = NULL;
    if (get_str(root, "tag", 15, &t) != M_OK || t[0] == '\0') {
        return invalid(bad, "tag");
    }
    const char *l = NULL;
    if (get_str(root, "level", 16, &l) != M_OK || !web_log_level_parse(l, level)) {
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
    if (get_str(root, "version", 31, version) != M_OK || (*version)[0] == '\0') {
        return invalid(bad, "version");
    }
    *rolled_back_ack = false;
    if (get_bool(root, "rolled_back_ack", rolled_back_ack) == M_BAD) {
        return invalid(bad, "rolled_back_ack");
    }
    return true;
}

bool web_parse_update_gnss(const cJSON *root, uint32_t *build, bool *cross_layout_ack, web_bad_t *bad)
{
    int64_t v = 0;
    if (get_int(root, "build", 1, UINT32_MAX, &v) != M_OK) {
        return invalid(bad, "build");
    }
    *build = (uint32_t)v;
    *cross_layout_ack = false;
    if (get_bool(root, "cross_layout_ack", cross_layout_ack) == M_BAD) {
        return invalid(bad, "cross_layout_ack");
    }
    return true;
}

bool web_parse_upload_confirm(const cJSON *root, uint8_t sha256[32], const char **expected_fw,
                              uint32_t *expected_build, bool *cross_layout_ack, web_bad_t *bad)
{
    const char *hex = NULL;
    if (get_str(root, "sha256", 64, &hex) != M_OK || !web_parse_sha256_hex(hex, sha256)) {
        return invalid(bad, "sha256");
    }
    const char *fw = NULL;
    uint32_t build = 0;
    if (get_str(root, "expected_fw", 47, &fw) != M_OK || (build = web_expected_build(fw)) == 0) {
        return invalid(bad, "expected_fw");
    }
    *expected_fw = fw;
    *expected_build = build;
    *cross_layout_ack = false;
    if (get_bool(root, "cross_layout_ack", cross_layout_ack) == M_BAD) {
        return invalid(bad, "cross_layout_ack");
    }
    return true;
}

/* ---- WebSocket ---------------------------------------------------------------------------------------- */

web_ws_msg_t web_parse_ws_message(const char *text, size_t len, web_ws_sub_t *sub)
{
    if (!web_json_depth_ok(text, len, WEB_JSON_MAX_DEPTH)) {
        return WEB_WS_MSG_INVALID;
    }
    cJSON *root = cJSON_ParseWithLength(text, len);
    web_ws_msg_t kind = WEB_WS_MSG_INVALID;
    const char *type = NULL;
    if (root != NULL && cJSON_IsObject(root) && get_str(root, "type", 16, &type) == M_OK) {
        if (strcmp(type, "ping") == 0) {
            kind = WEB_WS_MSG_PING;
        } else if (strcmp(type, "subscribe") == 0) {
            const cJSON *list = get(root, "topics");
            if (cJSON_IsArray(list)) {
                uint32_t bits = 0;
                const cJSON *t;
                cJSON_ArrayForEach(t, list) {
                    if (!cJSON_IsString(t)) {
                        continue;
                    }
                    if (strcmp(t->valuestring, "status") == 0) {
                        bits |= WEB_TOPIC_STATUS;
                    } else if (strcmp(t->valuestring, "progress") == 0) {
                        bits |= WEB_TOPIC_PROGRESS;
                    } else if (strcmp(t->valuestring, "log") == 0) {
                        bits |= WEB_TOPIC_LOG;
                    }
                }
                int64_t cur = 0;
                member_t mc = get_int(root, "log_cursor", 0, UINT32_MAX, &cur);
                if (mc != M_BAD) {
                    if (sub != NULL) {
                        sub->topics = bits;
                        sub->has_log_cursor = mc == M_OK;
                        sub->log_cursor = (uint32_t)cur;
                    }
                    kind = WEB_WS_MSG_SUBSCRIBE;
                }
            }
        }
    }
    cJSON_Delete(root);
    return kind;
}
