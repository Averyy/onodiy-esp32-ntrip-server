/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "um98x_profile.h"
#include "unity.h"
#include "web_parse.h"

static cJSON *body(const char *json)
{
    web_bad_t b = { 0 };
    cJSON *root = web_parse_body(json, strlen(json), &b);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, json);
    return root;
}

TEST_CASE("bodies: JSON objects only, empty means {}", "[web_parse]")
{
    web_bad_t b = { 0 };
    cJSON *root = web_parse_body("", 0, &b);
    TEST_ASSERT_NOT_NULL(root);
    cJSON_Delete(root);
    root = web_parse_body("  \r\n", 4, &b);
    TEST_ASSERT_NOT_NULL(root);
    cJSON_Delete(root);
    TEST_ASSERT_NULL(web_parse_body("[1,2]", 5, &b));
    TEST_ASSERT_EQUAL(WEB_ERR_BAD_JSON, b.err);
    TEST_ASSERT_NULL(web_parse_body("{\"a\":", 5, &b));
    TEST_ASSERT_NULL(web_parse_body("true", 4, &b));

    /* Deep nesting is refused before cJSON recurses into it. */
    char deep[2048];
    size_t n = 0;
    deep[n++] = '{';
    deep[n++] = '"';
    deep[n++] = 'a';
    deep[n++] = '"';
    deep[n++] = ':';
    while (n < 1500) {
        deep[n++] = '[';
    }
    deep[n] = '\0';
    TEST_ASSERT_NULL(web_parse_body(deep, n, &b));
    TEST_ASSERT_EQUAL(WEB_ERR_BAD_JSON, b.err);
    TEST_ASSERT_TRUE(web_json_depth_ok("{\"a\":[{\"b\":[1]}]}", 17, 4));
    TEST_ASSERT_FALSE(web_json_depth_ok("{\"a\":[{\"b\":[1]}]}", 17, 3));
    TEST_ASSERT_TRUE(web_json_depth_ok("{\"s\":\"[[[[[[[[[[\\\"[[[[\"}", 24, 1));   /* brackets inside strings */

    root = body("{\"confirm\":true}");
    TEST_ASSERT_TRUE(web_parse_confirm(root, &b));
    cJSON_Delete(root);
    root = body("{\"confirm\":1}");
    TEST_ASSERT_FALSE(web_parse_confirm(root, &b));
    TEST_ASSERT_EQUAL(WEB_ERR_CONFIRM_REQUIRED, b.err);
    cJSON_Delete(root);
}

/* ---- profile ---------------------------------------------------------------------------- */

static um98x_base_profile_t stored_profile(void)
{
    um98x_base_profile_t p;
    um98x_profile_default(&p, 1234);
    return p;
}

static bool profile_put(const char *json, um98x_base_profile_t *out, web_bad_t *b)
{
    um98x_base_profile_t st = stored_profile();
    bool have_p = false;
    bool autocfg = false;
    bool have_a = false;
    cJSON *root = body(json);
    bool ok = web_parse_profile_put(root, &st, out, &have_p, &autocfg, &have_a, b);
    cJSON_Delete(root);
    return ok;
}

TEST_CASE("profile edits merge into the stored profile", "[web_parse]")
{
    um98x_base_profile_t st = stored_profile();
    um98x_base_profile_t p;
    bool have_p = false;
    bool autocfg = true;
    bool have_a = false;
    web_bad_t b = { 0 };
    cJSON *root = body("{\"autocfg\":false}");
    TEST_ASSERT_TRUE(web_parse_profile_put(root, &st, &p, &have_p, &autocfg, &have_a, &b));
    TEST_ASSERT_FALSE(have_p);
    TEST_ASSERT_TRUE(have_a);
    TEST_ASSERT_FALSE(autocfg);
    cJSON_Delete(root);

    TEST_ASSERT_TRUE(profile_put("{\"profile\":{\"station_id\":7,\"survey_in_s\":120}}", &p, &b));
    TEST_ASSERT_EQUAL_UINT16(7, p.station_id);
    TEST_ASSERT_EQUAL_UINT16(120, p.survey_in_s);
    TEST_ASSERT_EQUAL(st.rtcm_count, p.rtcm_count);
    TEST_ASSERT_EQUAL_UINT32(115200, p.com1_baud);

    TEST_ASSERT_TRUE(profile_put("{\"profile\":{\"base_mode\":\"fixed\",\"fixed\":{\"lat_deg\":45.123456789,"
                                 "\"lon_deg\":-75.123456789,\"height_m\":85.1234}}}",
                                 &p, &b));
    TEST_ASSERT_EQUAL(UM98X_BASE_FIXED, p.base_mode);
    TEST_ASSERT_EQUAL_UINT16(0, p.survey_in_s);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 45.123456789, p.fixed_lat_deg);

    TEST_ASSERT_TRUE(profile_put("{\"profile\":{\"com1_baud\":460800,\"rtcm\":[{\"msg\":1005,\"period_s\":1},"
                                 "{\"msg\":1019,\"period_s\":30}]}}",
                                 &p, &b));
    TEST_ASSERT_EQUAL(2, p.rtcm_count);
    TEST_ASSERT_EQUAL_UINT16(1019, p.rtcm[1].msg);
    TEST_ASSERT_EQUAL_UINT16(30, p.rtcm[1].period_s);
}

TEST_CASE("profile field rules name the field", "[web_parse]")
{
    static const struct {
        const char *json;
        const char *field;
    } bad[] = {
        { "{\"profile\":{\"station_id\":4096}}", "station_id" },
        { "{\"profile\":{\"station_id\":1.5}}", "station_id" },
        { "{\"profile\":{\"com1_baud\":12345}}", "com1_baud" },
        { "{\"profile\":{\"survey_in_s\":0}}", "survey_in_s" },
        { "{\"profile\":{\"survey_in_s\":3601}}", "survey_in_s" },
        { "{\"profile\":{\"base_mode\":\"rover\"}}", "base_mode" },
        { "{\"profile\":{\"base_mode\":\"fixed\"}}", "fixed" },
        { "{\"profile\":{\"base_mode\":\"fixed\",\"fixed\":{\"lat_deg\":91,\"lon_deg\":0,\"height_m\":0}}}", "fixed" },
        { "{\"profile\":{\"base_mode\":\"fixed\",\"fixed\":{\"lat_deg\":0,\"lon_deg\":0,\"height_m\":20000}}}", "fixed" },
        { "{\"profile\":{\"rtcm\":[{\"msg\":1000,\"period_s\":1}]}}", "rtcm" },
        { "{\"profile\":{\"rtcm\":[{\"msg\":1005,\"period_s\":0}]}}", "rtcm" },
        { "{\"profile\":{\"rtcm\":[{\"msg\":1104,\"period_s\":1}]}}", "rtcm" },
        { "{\"profile\":{\"rtcm\":[{\"msg\":1019,\"period_s\":30}]}}", "rtcm" },
        { "{\"profile\":{\"rtcm\":[{\"msg\":1005,\"period_s\":1},{\"msg\":1005,\"period_s\":5}]}}", "rtcm" },
        { "{\"profile\":{\"rtcm\":{}}}", "rtcm" },
        { "{\"profile\":{\"sat_view_output\":1}}", "sat_view_output" },
        { "{\"profile\":[]}", "profile" },
        { "{\"autocfg\":\"yes\"}", "autocfg" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        um98x_base_profile_t p;
        web_bad_t b = { 0 };
        TEST_ASSERT_FALSE_MESSAGE(profile_put(bad[i].json, &p, &b), bad[i].json);
        TEST_ASSERT_EQUAL(WEB_ERR_INVALID_FIELD, b.err);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bad[i].field, b.field, bad[i].json);
    }
    /* 17 messages: one more than the receiver takes. */
    char json[1024] = "{\"profile\":{\"rtcm\":[";
    for (int i = 0; i < 17; i++) {
        char item[48];
        snprintf(item, sizeof(item), "%s{\"msg\":%d,\"period_s\":1}", i ? "," : "", 1200 + i);
        strcat(json, item);
    }
    strcat(json, "]}}");
    um98x_base_profile_t p;
    web_bad_t b = { 0 };
    TEST_ASSERT_FALSE(profile_put(json, &p, &b));
    TEST_ASSERT_EQUAL_STRING("rtcm", b.field);
}

/* ---- casters ------------------------------------------------------------------------------ */

static ntrip_caster_cfg_t stored_caster(void)
{
    ntrip_caster_cfg_t c;
    memset(&c, 0, sizeof(c));
    c.enabled = true;
    c.protected_slot = true;
    strcpy(c.name, "Onocoy");
    strcpy(c.host, "servers.onocoy.com");
    c.port = 2101;
    c.proto = NTRIP_PROTO_V1;
    strcpy(c.mountpoint, "user123");
    c.password_set = true;
    c.backoff_profile = NTRIP_BACKOFF_FAST;
    return c;
}

static bool caster_put(const char *json, ntrip_caster_cfg_t *out, web_bad_t *b)
{
    ntrip_caster_cfg_t st = stored_caster();
    cJSON *root = body(json);
    bool ok = web_parse_caster_put(root, &st, out, b);
    cJSON_Delete(root);
    return ok;
}

TEST_CASE("antenna descriptor: trailing spaces dropped, inner kept, the profile's character rule", "[web_parse]")
{
    um98x_base_profile_t p;
    web_bad_t b = { 0 };
    TEST_ASSERT_TRUE(profile_put("{\"profile\":{\"antenna_model\":\"TRM59800.00     SCIS  \","
                                 "\"antenna_serial\":\"5000123 \",\"antenna_setup_id\":2}}",
                                 &p, &b));
    TEST_ASSERT_EQUAL_STRING("TRM59800.00     SCIS", p.antenna_model);
    TEST_ASSERT_EQUAL_STRING("5000123", p.antenna_serial);
    TEST_ASSERT_EQUAL_UINT8(2, p.antenna_setup_id);
    TEST_ASSERT_TRUE(um98x_profile_valid(&p));

    /* 31 characters fit, also with trailing spaces beyond them; 32 do not. */
    TEST_ASSERT_TRUE(profile_put("{\"profile\":{\"antenna_model\":\"1234567890123456789012345678901    \"}}", &p, &b));
    TEST_ASSERT_EQUAL(31, (int)strlen(p.antenna_model));
    TEST_ASSERT_FALSE(profile_put("{\"profile\":{\"antenna_model\":\"12345678901234567890123456789012\"}}", &p, &b));
    TEST_ASSERT_EQUAL_STRING("antenna_model", b.field);

    /* Empty clears it (the receiver keeps its own value). */
    TEST_ASSERT_TRUE(profile_put("{\"profile\":{\"antenna_model\":\"   \"}}", &p, &b));
    TEST_ASSERT_EQUAL_STRING("", p.antenna_model);

    static const struct {
        const char *json;
        const char *field;
    } bad[] = {
        { "{\"profile\":{\"antenna_model\":\"ANT\\\"X\"}}", "antenna_model" },     /* '"' */
        { "{\"profile\":{\"antenna_model\":\"ANT\\tX\"}}", "antenna_model" },       /* control character */
        { "{\"profile\":{\"antenna_model\":\"ANT\\u00e9\"}}", "antenna_model" },    /* not ASCII */
        { "{\"profile\":{\"antenna_model\":7}}", "antenna_model" },
        { "{\"profile\":{\"antenna_serial\":\"S\\\"N\"}}", "antenna_serial" },
        { "{\"profile\":{\"antenna_setup_id\":256}}", "antenna_setup_id" },
        { "{\"profile\":{\"antenna_setup_id\":-1}}", "antenna_setup_id" },
        { "{\"profile\":{\"antenna_setup_id\":1.5}}", "antenna_setup_id" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        b = (web_bad_t){ 0 };
        TEST_ASSERT_FALSE_MESSAGE(profile_put(bad[i].json, &p, &b), bad[i].json);
        TEST_ASSERT_EQUAL(WEB_ERR_INVALID_FIELD, b.err);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bad[i].field, b.field, bad[i].json);
    }
}

TEST_CASE("recovery mode, maintenance duration and live view minutes", "[web_parse]")
{
    web_bad_t b = { 0 };
    bool have = true;
    health_recovery_mode_t mode = HEALTH_RECOVERY_ALERTS;
    cJSON *root = body("{\"autocfg\":true}");
    TEST_ASSERT_TRUE(web_parse_recovery(root, &have, &mode, &b));
    TEST_ASSERT_FALSE(have);
    cJSON_Delete(root);
    static const struct {
        const char *json;
        health_recovery_mode_t mode;
    } good[] = {
        { "{\"recovery\":\"full\"}", HEALTH_RECOVERY_FULL },
        { "{\"recovery\":\"no_reboot\"}", HEALTH_RECOVERY_NO_REBOOT },
        { "{\"recovery\":\"alerts_only\"}", HEALTH_RECOVERY_ALERTS },
    };
    for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
        root = body(good[i].json);
        TEST_ASSERT_TRUE(web_parse_recovery(root, &have, &mode, &b));
        TEST_ASSERT_TRUE(have);
        TEST_ASSERT_EQUAL(good[i].mode, mode);
        cJSON_Delete(root);
    }
    static const char *const bad_modes[] = { "{\"recovery\":2}", "{\"recovery\":\"alerts\"}", "{\"recovery\":\"FULL\"}",
                                             "{\"recovery\":null}", "{\"recovery\":\"\"}" };
    for (size_t i = 0; i < sizeof(bad_modes) / sizeof(bad_modes[0]); i++) {
        root = body(bad_modes[i]);
        b = (web_bad_t){ 0 };
        TEST_ASSERT_FALSE_MESSAGE(web_parse_recovery(root, &have, &mode, &b), bad_modes[i]);
        TEST_ASSERT_EQUAL_STRING("recovery", b.field);
        cJSON_Delete(root);
    }

    uint32_t dur = 1;
    root = body("{}");
    TEST_ASSERT_TRUE(web_parse_maintenance(root, &dur, &b));
    TEST_ASSERT_EQUAL_UINT32(0, dur);   /* the station's default */
    cJSON_Delete(root);
    static const uint32_t ok_dur[] = { 0, 900, 1800, 3600, 7200, 14400 };
    for (size_t i = 0; i < sizeof(ok_dur) / sizeof(ok_dur[0]); i++) {
        char json[48];
        snprintf(json, sizeof(json), "{\"duration_s\":%u}", (unsigned)ok_dur[i]);
        root = body(json);
        TEST_ASSERT_TRUE_MESSAGE(web_parse_maintenance(root, &dur, &b), json);
        TEST_ASSERT_EQUAL_UINT32(ok_dur[i], dur);
        cJSON_Delete(root);
    }
    static const char *const bad_dur[] = { "{\"duration_s\":600}", "{\"duration_s\":28800}", "{\"duration_s\":-900}",
                                           "{\"duration_s\":\"1800\"}", "{\"duration_s\":1800.5}" };
    for (size_t i = 0; i < sizeof(bad_dur) / sizeof(bad_dur[0]); i++) {
        root = body(bad_dur[i]);
        b = (web_bad_t){ 0 };
        TEST_ASSERT_FALSE_MESSAGE(web_parse_maintenance(root, &dur, &b), bad_dur[i]);
        TEST_ASSERT_EQUAL_STRING("duration_s", b.field);
        cJSON_Delete(root);
    }

    uint32_t minutes = 9;
    root = body("{}");
    TEST_ASSERT_TRUE(web_parse_live(root, 60, &minutes, &b));
    TEST_ASSERT_EQUAL_UINT32(0, minutes);
    cJSON_Delete(root);
    root = body("{\"minutes\":60}");
    TEST_ASSERT_TRUE(web_parse_live(root, 60, &minutes, &b));
    TEST_ASSERT_EQUAL_UINT32(60, minutes);
    cJSON_Delete(root);
    static const char *const bad_min[] = { "{\"minutes\":61}", "{\"minutes\":-1}", "{\"minutes\":true}", "{\"minutes\":2.5}" };
    for (size_t i = 0; i < sizeof(bad_min) / sizeof(bad_min[0]); i++) {
        root = body(bad_min[i]);
        b = (web_bad_t){ 0 };
        TEST_ASSERT_FALSE_MESSAGE(web_parse_live(root, 60, &minutes, &b), bad_min[i]);
        TEST_ASSERT_EQUAL_STRING("minutes", b.field);
        cJSON_Delete(root);
    }
}

TEST_CASE("caster edits keep left-out fields and the stored password", "[web_parse]")
{
    ntrip_caster_cfg_t c;
    web_bad_t b = { 0 };
    TEST_ASSERT_TRUE(caster_put("{\"name\":\"Main\",\"password_set\":false,\"protected\":false}", &c, &b));
    TEST_ASSERT_EQUAL_STRING("Main", c.name);
    TEST_ASSERT_EQUAL_STRING("servers.onocoy.com", c.host);
    TEST_ASSERT_EQUAL_STRING("user123", c.mountpoint);
    TEST_ASSERT_EQUAL_STRING("", c.password);           /* "" = keep the stored one (same host) */
    TEST_ASSERT_FALSE(c.clear_password);
    TEST_ASSERT_TRUE(c.protected_slot);                  /* server-set: Onocoy */
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_FAST, c.backoff_profile);

    TEST_ASSERT_TRUE(caster_put("{\"password\":\"newpass\"}", &c, &b));
    TEST_ASSERT_EQUAL_STRING("newpass", c.password);
    TEST_ASSERT_TRUE(caster_put("{\"enabled\":false,\"clear_password\":true}", &c, &b));
    TEST_ASSERT_TRUE(c.clear_password);
    TEST_ASSERT_EQUAL_STRING("", c.password);
}

TEST_CASE("caster backoff and protection follow the host", "[web_parse]")
{
    ntrip_caster_cfg_t c;
    web_bad_t b = { 0 };
    TEST_ASSERT_TRUE(caster_put("{\"host\":\"rtk2go.com\",\"password\":\"pw\",\"backoff_profile\":\"standard\"}", &c, &b));
    TEST_ASSERT_FALSE(c.protected_slot);
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_POLITE, c.backoff_profile);
    TEST_ASSERT_TRUE(caster_put("{\"host\":\"caster.example.org\",\"password\":\"pw\"}", &c, &b));
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_STANDARD, c.backoff_profile);   /* no longer Onocoy: not fast */
    TEST_ASSERT_TRUE(caster_put("{\"host\":\"caster.example.org\",\"password\":\"pw\",\"backoff_profile\":\"polite\"}",
                                &c, &b));
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_POLITE, c.backoff_profile);
    TEST_ASSERT_FALSE(caster_put("{\"host\":\"caster.example.org\",\"password\":\"pw\",\"backoff_profile\":\"fast\"}",
                                 &c, &b));
    TEST_ASSERT_EQUAL_STRING("backoff_profile", b.field);
    TEST_ASSERT_TRUE(caster_put("{\"backoff_profile\":\"polite\"}", &c, &b));
    TEST_ASSERT_EQUAL(NTRIP_BACKOFF_FAST, c.backoff_profile);       /* forced for Onocoy */
}

TEST_CASE("caster fields are validated and named", "[web_parse]")
{
    static const struct {
        const char *json;
        const char *field;
    } bad[] = {
        { "{\"name\":\"123456789012345678901234\"}", "name" },
        { "{\"name\":\"tab\\there\"}", "name" },
        { "{\"host\":\"bad host\"}", "host" },
        { "{\"port\":0}", "port" },
        { "{\"port\":65536}", "port" },
        { "{\"ntrip_version\":3}", "ntrip_version" },
        { "{\"mountpoint\":\"MP/1\"}", "mountpoint" },
        { "{\"username\":\"a:b\"}", "username" },
        { "{\"password\":\"with space\"}", "password" },
        { "{\"label\":\"my-label\"}", "label" },
        { "{\"trust\":\"none\"}", "trust" },
        { "{\"tls\":\"yes\"}", "tls" },
        { "{\"host\":\"\"}", "host" },                                      /* enabled needs a host */
        { "{\"mountpoint\":\"\"}", "mountpoint" },
        { "{\"host\":\"caster.example.org\"}", "password" },                  /* new host: old password not kept */
        { "{\"host\":\"caster.example.org\",\"ntrip_version\":2,\"password\":\"p\"}", "username" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        ntrip_caster_cfg_t c;
        web_bad_t b = { 0 };
        TEST_ASSERT_FALSE_MESSAGE(caster_put(bad[i].json, &c, &b), bad[i].json);
        TEST_ASSERT_EQUAL(WEB_ERR_INVALID_FIELD, b.err);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(bad[i].field, b.field, bad[i].json);
    }
    ntrip_caster_cfg_t c;
    web_bad_t b = { 0 };
    /* NTRIP 2.0 Onocoy needs no username; spaces are fine in a 2.0 password. */
    TEST_ASSERT_TRUE(caster_put("{\"ntrip_version\":2,\"password\":\"with space\",\"label\":\"Roof1\"}", &c, &b));
    TEST_ASSERT_EQUAL(NTRIP_PROTO_V2, c.proto);
    /* Disabled slots may be incomplete. */
    TEST_ASSERT_TRUE(caster_put("{\"enabled\":false,\"host\":\"\",\"mountpoint\":\"\"}", &c, &b));
}

TEST_CASE("CA certificates: PEM certificate blocks only", "[web_parse]")
{
    static const char cert[] = "-----BEGIN CERTIFICATE-----\nMIIBszCCAVmgAwIBAgIU\n-----END CERTIFICATE-----\n";
    TEST_ASSERT_TRUE(web_pem_certs_only(cert));
    char two[256];
    snprintf(two, sizeof(two), "%s\r\n%s", cert, cert);
    TEST_ASSERT_TRUE(web_pem_certs_only(two));
    TEST_ASSERT_FALSE(web_pem_certs_only(""));
    TEST_ASSERT_FALSE(web_pem_certs_only("-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n"));
    TEST_ASSERT_FALSE(web_pem_certs_only("-----BEGIN CERTIFICATE-----\nAA AA\n-----END CERTIFICATE-----\n"));
    TEST_ASSERT_FALSE(web_pem_certs_only("-----BEGIN CERTIFICATE-----\nAAAA\n"));
    snprintf(two, sizeof(two), "%sjunk", cert);
    TEST_ASSERT_FALSE(web_pem_certs_only(two));

    cJSON *root = body("{\"pem\":\"-----BEGIN CERTIFICATE-----\\nAAAA\\n-----END CERTIFICATE-----\\n\"}");
    const char *pem = NULL;
    web_bad_t b = { 0 };
    TEST_ASSERT_TRUE(web_parse_ca_pem(root, &pem, &b));
    cJSON_Delete(root);
    root = body("{\"pem\":5}");
    TEST_ASSERT_FALSE(web_parse_ca_pem(root, &pem, &b));
    TEST_ASSERT_EQUAL_STRING("pem", b.field);
    cJSON_Delete(root);
}

/* ---- Wi-Fi, system, diagnostics ---------------------------------------------------------------- */

TEST_CASE("Wi-Fi and system edits", "[web_parse]")
{
    web_wifi_put_t w;
    web_bad_t b = { 0 };
    cJSON *root = body("{\"ssid\":\"Home\",\"password\":\"\",\"ap_password\":\"\"}");
    TEST_ASSERT_TRUE(web_parse_wifi_put(root, &w, &b));
    TEST_ASSERT_TRUE(w.have_ssid && w.have_password && w.have_ap_password);
    cJSON_Delete(root);
    root = body("{\"ap_password\":\"apsecret\"}");
    TEST_ASSERT_TRUE(web_parse_wifi_put(root, &w, &b));
    TEST_ASSERT_FALSE(w.have_ssid);
    cJSON_Delete(root);
    static const struct {
        const char *json;
        const char *field;
    } bad[] = {
        { "{\"ssid\":\"\"}", "ssid" },
        { "{\"ssid\":\"Home\",\"password\":\"short\"}", "password" },
        { "{\"ap_password\":\"short\"}", "ap_password" },
        { "{\"password\":\"longenough\"}", "ssid" },
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        root = body(bad[i].json);
        TEST_ASSERT_FALSE_MESSAGE(web_parse_wifi_put(root, &w, &b), bad[i].json);
        TEST_ASSERT_EQUAL_STRING(bad[i].field, b.field);
        cJSON_Delete(root);
    }

    web_system_put_t s;
    root = body("{\"hostname\":\"roof-station\",\"public_status\":true}");
    TEST_ASSERT_TRUE(web_parse_system_put(root, &s, &b));
    TEST_ASSERT_TRUE(s.have_hostname && s.have_public_status && s.public_status);
    cJSON_Delete(root);
    root = body("{\"hostname\":\"roof_station\"}");
    TEST_ASSERT_FALSE(web_parse_system_put(root, &s, &b));
    TEST_ASSERT_EQUAL_STRING("hostname", b.field);
    cJSON_Delete(root);

    const char *tag = NULL;
    esp_log_level_t level = ESP_LOG_NONE;
    root = body("{\"tag\":\"station\",\"level\":\"debug\"}");
    TEST_ASSERT_TRUE(web_parse_log_level(root, &tag, &level, &b));
    TEST_ASSERT_EQUAL_STRING("station", tag);
    TEST_ASSERT_EQUAL(ESP_LOG_DEBUG, level);
    cJSON_Delete(root);
    root = body("{\"tag\":\"1234567890123456\",\"level\":\"info\"}");
    TEST_ASSERT_FALSE(web_parse_log_level(root, &tag, &level, &b));
    TEST_ASSERT_EQUAL_STRING("tag", b.field);
    cJSON_Delete(root);
    root = body("{\"tag\":\"*\",\"level\":\"loud\"}");
    TEST_ASSERT_FALSE(web_parse_log_level(root, &tag, &level, &b));
    TEST_ASSERT_EQUAL_STRING("level", b.field);
    cJSON_Delete(root);
}

TEST_CASE("update and upload confirmation bodies", "[web_parse]")
{
    web_bad_t b = { 0 };
    const char *version = NULL;
    bool ack = true;
    cJSON *root = body("{\"version\":\"v0.4.0\"}");
    TEST_ASSERT_TRUE(web_parse_update_app(root, &version, &ack, &b));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", version);
    TEST_ASSERT_FALSE(ack);
    cJSON_Delete(root);

    uint32_t build = 0;
    root = body("{\"build\":19152,\"cross_layout_ack\":false}");
    TEST_ASSERT_TRUE(web_parse_update_gnss(root, &build, &ack, &b));
    TEST_ASSERT_EQUAL_UINT32(19152, build);
    cJSON_Delete(root);
    root = body("{\"build\":\"19152\"}");
    TEST_ASSERT_FALSE(web_parse_update_gnss(root, &build, &ack, &b));
    TEST_ASSERT_EQUAL_STRING("build", b.field);
    cJSON_Delete(root);

    uint8_t sha[32];
    const char *fw = NULL;
    root = body("{\"sha256\":\"9ac25d1d359e5815b13291f61405faef53d71e306a602184546b32584b35ec71\","
                "\"expected_fw\":\"R4.10Build19152\",\"cross_layout_ack\":true}");
    TEST_ASSERT_TRUE(web_parse_upload_confirm(root, sha, &fw, &build, &ack, &b));
    TEST_ASSERT_EQUAL_UINT32(19152, build);
    TEST_ASSERT_TRUE(ack);
    TEST_ASSERT_EQUAL_HEX8(0x9a, sha[0]);
    cJSON_Delete(root);
    root = body("{\"sha256\":\"9ac2\",\"expected_fw\":\"R4.10Build19152\"}");
    TEST_ASSERT_FALSE(web_parse_upload_confirm(root, sha, &fw, &build, &ack, &b));
    TEST_ASSERT_EQUAL_STRING("sha256", b.field);
    cJSON_Delete(root);
    root = body("{\"sha256\":\"9ac25d1d359e5815b13291f61405faef53d71e306a602184546b32584b35ec71\","
                "\"expected_fw\":\"R4.10\"}");
    TEST_ASSERT_FALSE(web_parse_upload_confirm(root, sha, &fw, &build, &ack, &b));
    TEST_ASSERT_EQUAL_STRING("expected_fw", b.field);
    cJSON_Delete(root);
}

TEST_CASE("WebSocket client messages", "[web_parse]")
{
    web_ws_sub_t topics = { 0 };
    const char *sub = "{\"type\":\"subscribe\",\"topics\":[\"status\",\"log\",\"bogus\",7]}";
    TEST_ASSERT_EQUAL(WEB_WS_MSG_SUBSCRIBE, web_parse_ws_message(sub, strlen(sub), &topics));
    TEST_ASSERT_EQUAL_UINT32(WEB_TOPIC_STATUS | WEB_TOPIC_LOG, topics.topics);
    TEST_ASSERT_FALSE(topics.has_log_cursor);
    sub = "{\"type\":\"subscribe\",\"topics\":[\"log\"],\"log_cursor\":4294967295}";
    TEST_ASSERT_EQUAL(WEB_WS_MSG_SUBSCRIBE, web_parse_ws_message(sub, strlen(sub), &topics));
    TEST_ASSERT_TRUE(topics.has_log_cursor);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, topics.log_cursor);
    sub = "{\"type\":\"subscribe\",\"topics\":[\"log\"],\"log_cursor\":-1}";
    TEST_ASSERT_EQUAL(WEB_WS_MSG_INVALID, web_parse_ws_message(sub, strlen(sub), &topics));
    sub = "{\"type\":\"subscribe\",\"topics\":[\"log\"],\"log_cursor\":\"5\"}";
    TEST_ASSERT_EQUAL(WEB_WS_MSG_INVALID, web_parse_ws_message(sub, strlen(sub), &topics));
    TEST_ASSERT_EQUAL(WEB_WS_MSG_PING, web_parse_ws_message("{\"type\":\"ping\"}", 15, &topics));
    TEST_ASSERT_EQUAL(WEB_WS_MSG_INVALID, web_parse_ws_message("{\"type\":\"subscribe\"}", 20, &topics));
    TEST_ASSERT_EQUAL(WEB_WS_MSG_INVALID, web_parse_ws_message("ping", 4, &topics));
    TEST_ASSERT_EQUAL(WEB_WS_MSG_INVALID, web_parse_ws_message("{\"type\":\"other\"}", 16, &topics));
}
