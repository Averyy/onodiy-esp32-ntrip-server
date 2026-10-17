/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Web bodies of the receiver read-back, the published 1033, receiver
 * maintenance, the live view, recovery modes, changed Wi-Fi credentials and
 * the connection history.
 */
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "um98x_profile.h"
#include "unity.h"
#include "web_build.h"
#include "web_enums.h"

static char s_out[32768];

static const cJSON *at(const cJSON *o, const char *k)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, k);
    return it;
}

static cJSON *parse_out(void)
{
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    return root;
}

/* ---- GET /api/gnss ---------------------------------------------------------------------- */

static station_status_t s_st;

static cJSON *build_gnss(const web_gnss_snap_t *g)
{
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_gnss(&w, g);
    TEST_ASSERT_TRUE(wj_ok(&w));
    return parse_out();
}

static void gnss_fixture(web_gnss_snap_t *g)
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = STATION_STATE_STREAMING;
    memset(g, 0, sizeof(*g));
    g->st = &s_st;
    g->link.mode = GNSS_LINK_MODE_STREAM;
    g->link.link_open = true;
    g->link.last_rx_age_ms = 10;
    g->link.last_rtcm_age_ms = 20;
    um98x_profile_default(&g->profile, 1234);
    g->autocfg = true;
}

TEST_CASE("gnss: read-back unknown before an apply, the 1033 null before the first one", "[web_live]")
{
    web_gnss_snap_t g;
    gnss_fixture(&g);
    g.rtcm1033.bad_frames = 2;
    cJSON *root = build_gnss(&g);
    const cJSON *rx = at(root, "rx_settings");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(rx, "valid")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(rx, "age_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(rx, "read_unix")));
    TEST_ASSERT_EQUAL_STRING("unknown", at(rx, "b1cb2a")->valuestring);
    TEST_ASSERT_EQUAL_STRING("unknown", at(rx, "mmp")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(rx, "mask_deg")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(rx, "antenna")));
    const cJSON *steps = at(root, "apply_steps");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(at(steps, "antenna"), "sent")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(steps, "b1cb2a"), "reply")));
    const cJSON *d = at(root, "rtcm1033");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(d, "valid")));
    TEST_ASSERT_EQUAL(2, at(d, "bad_frames")->valueint);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(d, "antenna"));
    TEST_ASSERT_EQUAL_STRING("full", at(root, "recovery")->valuestring);
    const cJSON *link = at(root, "link");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(link, "open")));
    TEST_ASSERT_EQUAL(0, at(link, "open_failures")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(link, "hands_off")));
    const cJSON *p = at(root, "profile");
    TEST_ASSERT_EQUAL_STRING("", at(p, "antenna_model")->valuestring);
    TEST_ASSERT_EQUAL(0, at(p, "antenna_setup_id")->valueint);
    cJSON_Delete(root);
}

TEST_CASE("gnss: read-back values, step outcomes and the published 1033", "[web_live]")
{
    web_gnss_snap_t g;
    gnss_fixture(&g);
    s_st.rx_settings_valid = true;
    s_st.rx_settings_age_s = 120;
    s_st.rx_settings_unix = 1760000000;
    s_st.rx_settings.b1cb2a = UM98X_SETTING_OFF;
    s_st.rx_settings.mmp = UM98X_SETTING_ON;
    s_st.rx_settings.mask_known = true;
    s_st.rx_settings.mask_ddeg = 105;
    s_st.rx_settings.antenna_known = true;
    strcpy(s_st.rx_settings.antenna_model, "TRM59800.00     NONE");
    strcpy(s_st.rx_settings.antenna_serial, "12345");
    s_st.rx_settings.antenna_setup_id = 3;
    s_st.antenna_sent = true;
    s_st.antenna_ok = false;
    strcpy(s_st.antenna_reply, "PARSING FAILED");
    s_st.b1cb2a_sent = true;
    s_st.b1cb2a_ok = true;
    strcpy(s_st.b1cb2a_reply, "ignored when accepted");
    g.rtcm1033.valid = true;
    g.rtcm1033.station_id = 1234;
    strcpy(g.rtcm1033.antenna, "TRM59800.00     NONE");
    g.rtcm1033.setup_id = 3;
    strcpy(g.rtcm1033.receiver_type, "UM982");
    strcpy(g.rtcm1033.receiver_fw, "R4.10\"Build\x01");
    g.rtcm1033.age_ms = UINT32_MAX;
    g.recovery = HEALTH_RECOVERY_ALERTS;
    strcpy(g.profile.antenna_model, "TRM59800.00     NONE");
    strcpy(g.profile.antenna_serial, "12345");
    g.profile.antenna_setup_id = 3;

    cJSON *root = build_gnss(&g);
    const cJSON *rx = at(root, "rx_settings");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(rx, "valid")));
    TEST_ASSERT_EQUAL(120, at(rx, "age_s")->valueint);
    TEST_ASSERT_EQUAL_DOUBLE(1760000000.0, at(rx, "read_unix")->valuedouble);
    TEST_ASSERT_EQUAL_STRING("off", at(rx, "b1cb2a")->valuestring);
    TEST_ASSERT_EQUAL_STRING("on", at(rx, "mmp")->valuestring);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 10.5, at(rx, "mask_deg")->valuedouble);
    const cJSON *ant = at(rx, "antenna");
    TEST_ASSERT_EQUAL_STRING("TRM59800.00     NONE", at(ant, "model")->valuestring);   /* inner spaces kept */
    TEST_ASSERT_EQUAL_STRING("12345", at(ant, "serial")->valuestring);
    TEST_ASSERT_EQUAL(3, at(ant, "setup_id")->valueint);

    const cJSON *a = at(at(root, "apply_steps"), "antenna");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(a, "sent")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(a, "ok")));
    TEST_ASSERT_EQUAL_STRING("PARSING FAILED", at(a, "reply")->valuestring);
    const cJSON *bb = at(at(root, "apply_steps"), "b1cb2a");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(bb, "ok")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(bb, "reply")));   /* a reply only explains a refusal */

    const cJSON *d = at(root, "rtcm1033");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(d, "valid")));
    TEST_ASSERT_EQUAL(1234, at(d, "station_id")->valueint);
    TEST_ASSERT_EQUAL_STRING("TRM59800.00     NONE", at(d, "antenna")->valuestring);
    TEST_ASSERT_EQUAL_STRING("", at(d, "antenna_serial")->valuestring);
    TEST_ASSERT_EQUAL_STRING("R4.10\"Build\x01", at(d, "receiver_fw")->valuestring);   /* escaped, as sent */
    TEST_ASSERT_TRUE(cJSON_IsNull(at(d, "age_ms")));
    TEST_ASSERT_EQUAL_STRING("alerts_only", at(root, "recovery")->valuestring);
    const cJSON *p = at(root, "profile");
    TEST_ASSERT_EQUAL_STRING("TRM59800.00     NONE", at(p, "antenna_model")->valuestring);
    TEST_ASSERT_EQUAL(3, at(p, "antenna_setup_id")->valueint);
    cJSON_Delete(root);
}

TEST_CASE("gnss: maintenance state with its choices; left and chosen only while on", "[web_live]")
{
    web_gnss_snap_t g;
    gnss_fixture(&g);
    s_st.maintenance_left_s = 99;   /* stale numbers of a state that is off */
    cJSON *root = build_gnss(&g);
    const cJSON *m = at(root, "maintenance");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(m, "on")));
    TEST_ASSERT_EQUAL(0, at(m, "left_s")->valueint);
    const cJSON *ch = at(m, "choices_s");
    TEST_ASSERT_EQUAL(5, cJSON_GetArraySize(ch));
    TEST_ASSERT_EQUAL(900, cJSON_GetArrayItem(ch, 0)->valueint);
    TEST_ASSERT_EQUAL(14400, cJSON_GetArrayItem(ch, 4)->valueint);
    TEST_ASSERT_EQUAL(1800, at(m, "default_s")->valueint);
    TEST_ASSERT_EQUAL(14400, at(m, "max_left_s")->valueint);
    cJSON_Delete(root);

    s_st.rx_maintenance = true;
    s_st.state = STATION_STATE_IDLE;
    s_st.maintenance_left_s = 1500;
    s_st.maintenance_chosen_s = 1800;
    g.link.hands_off = true;
    root = build_gnss(&g);
    m = at(root, "maintenance");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(m, "on")));
    TEST_ASSERT_EQUAL(1500, at(m, "left_s")->valueint);
    TEST_ASSERT_EQUAL(1800, at(m, "chosen_s")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(at(root, "link"), "hands_off")));
    TEST_ASSERT_EQUAL_STRING("idle", at(root, "state")->valuestring);
    cJSON_Delete(root);

    for (uint32_t s = 0; s < 20000; s++) {
        bool want = s == 900 || s == 1800 || s == 3600 || s == 7200 || s == 14400;
        TEST_ASSERT_EQUAL(want, web_maint_duration_ok(s));
    }
}

TEST_CASE("maintenance and live view refusals map to the contract's errors", "[web_live]")
{
    const char *field = "x";
    const char *reason = "x";
    TEST_ASSERT_EQUAL(WEB_ERR_JOURNAL_OPEN, web_maint_refusal(STATION_MAINT_REFUSED_FLASH, false, &field, &reason));
    TEST_ASSERT_NULL(field);
    TEST_ASSERT_NULL(reason);
    TEST_ASSERT_EQUAL(WEB_ERR_BUSY, web_maint_refusal(STATION_MAINT_REFUSED_SESSION, false, &field, &reason));
    TEST_ASSERT_EQUAL_STRING("receiver_session", reason);
    TEST_ASSERT_EQUAL(WEB_ERR_BUSY, web_maint_refusal(STATION_MAINT_REFUSED_SKY_OUTPUT, false, &field, &reason));
    TEST_ASSERT_EQUAL_STRING("live_view_output", reason);
    TEST_ASSERT_NULL(field);
    TEST_ASSERT_EQUAL(WEB_ERR_INVALID_FIELD, web_maint_refusal(STATION_MAINT_REFUSED_DURATION, false, &field, &reason));
    TEST_ASSERT_EQUAL_STRING("duration_s", field);
    TEST_ASSERT_NULL(reason);
    TEST_ASSERT_EQUAL(WEB_ERR_NOT_IN_MAINTENANCE, web_maint_refusal(STATION_MAINT_NOT_ON, false, NULL, NULL));
    TEST_ASSERT_EQUAL(WEB_ERR_SAFE_MODE, web_maint_refusal(STATION_MAINT_REFUSED_NOT_RUNNING, true, NULL, NULL));
    TEST_ASSERT_EQUAL(WEB_ERR_INTERNAL, web_maint_refusal(STATION_MAINT_REFUSED_NOT_RUNNING, false, NULL, NULL));

    TEST_ASSERT_EQUAL(WEB_ERR_JOURNAL_OPEN, web_live_refusal(STATION_LIVE_WHY_FLASH, false));
    TEST_ASSERT_EQUAL(WEB_ERR_SAFE_MODE, web_live_refusal(STATION_LIVE_WHY_NOT_RUNNING, true));
    TEST_ASSERT_EQUAL(WEB_ERR_INTERNAL, web_live_refusal(STATION_LIVE_WHY_NOT_RUNNING, false));

    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_RECEIVER_MAINTENANCE));
    TEST_ASSERT_EQUAL_STRING("receiver_maintenance", web_err_code(WEB_ERR_RECEIVER_MAINTENANCE));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_NOT_IN_MAINTENANCE));
    TEST_ASSERT_EQUAL(409, web_err_status(WEB_ERR_LIVE_VIEW_CLOSED));
    TEST_ASSERT_EQUAL(404, web_err_status(WEB_ERR_NOTHING_PENDING));
    TEST_ASSERT_EQUAL_STRING("nothing_pending", web_err_code(WEB_ERR_NOTHING_PENDING));
}

TEST_CASE("receiver maintenance refuses the receiver's update actions, not clearing a session", "[web_live]")
{
    web_err_t e = WEB_ERR_COUNT;
    web_flash_ctx_t c = { .journal = WEB_JOURNAL_NONE, .station_running = true, .receiver_in_loader = true,
                          .maintenance = true, .flash_available = true };
    TEST_ASSERT_EQUAL_STRING("receiver_maintenance", web_flash_action_why(&c, WEB_FLASH_RESTART_RECEIVER, &e));
    TEST_ASSERT_EQUAL(WEB_ERR_RECEIVER_MAINTENANCE, e);
    c.journal = WEB_JOURNAL_WAITING;
    TEST_ASSERT_EQUAL_STRING("receiver_maintenance", web_flash_action_why(&c, WEB_FLASH_RETRY, &e));
    TEST_ASSERT_EQUAL_STRING("receiver_maintenance", web_flash_action_why(&c, WEB_FLASH_RESTART_RECEIVER, &e));
    TEST_ASSERT_NULL(web_flash_action_why(&c, WEB_FLASH_CLEAR, &e));
    c.maintenance = false;
    TEST_ASSERT_NULL(web_flash_action_why(&c, WEB_FLASH_RETRY, &e));
}

/* ---- live view ------------------------------------------------------------------------------- */

static cJSON *build_live(const web_live_snap_t *l)
{
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_live(&w, l);
    TEST_ASSERT_TRUE(wj_ok(&w));
    return parse_out();
}

TEST_CASE("live view: window, totals per band and constellation, satellites with and without a position", "[web_live]")
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = STATION_STATE_STREAMING;
    s_st.live.open = true;
    s_st.live.left_s = 540;
    s_st.live.idle_left_s = 118;
    s_st.live.open_s = 60;
    s_st.live.max_s = 3600;
    s_st.live.sky = STATION_LIVE_SKY_ON;
    s_st.live.sky_period_s = 5;
    s_st.position.source = UM98X_POS_SURVEY;
    s_st.position.lat_deg = 45.123456789;
    s_st.position.lon_deg = -75.5;
    s_st.position.height_m = 85.25;

    static gnss_obs_live_sat_t sats[3];
    memset(sats, 0, sizeof(sats));
    sats[0].sys = GNSS_SYS_GPS;
    sats[0].prn = 12;
    sats[0].in_msm = true;
    sats[0].msm_age_ms = 400;
    sats[0].signal_mask = (1u << 1) | (1u << 14);   /* IDs 2 and 15 */
    sats[0].band_mask = (1u << GNSS_BAND_L1) | (1u << GNSS_BAND_L2);
    sats[0].cnr_dbhz[GNSS_BAND_L1] = 44;
    sats[0].cnr_dbhz[GNSS_BAND_L2] = 0;   /* present, no CNR (MSM without it) */
    sats[0].has_pos = true;
    sats[0].elevation_deg = 45;
    sats[0].azimuth_deg = 210;
    sats[0].sky_cnr_dbhz = 43;
    sats[0].pos_age_ms = 1200;
    sats[1].sys = GNSS_SYS_QZSS;   /* MSM number 2: PRN 194 */
    sats[1].prn = 2;
    sats[1].in_msm = true;
    sats[1].msm_age_ms = 400;
    sats[1].band_mask = 1u << GNSS_BAND_L1;
    sats[1].cnr_dbhz[GNSS_BAND_L1] = 38;
    sats[1].pos_age_ms = UINT32_MAX;
    sats[2].sys = GNSS_SYS_GALILEO;   /* only the sky view knows it */
    sats[2].prn = 7;
    sats[2].msm_age_ms = UINT32_MAX;
    sats[2].has_pos = true;
    sats[2].elevation_deg = -2;
    sats[2].azimuth_deg = 359;
    sats[2].pos_age_ms = 3000;

    static gnss_obs_live_t t;
    memset(&t, 0, sizeof(t));
    t.satellites = 3;
    t.tracked = 2;
    t.signals = 3;
    t.with_pos = 2;
    t.sys_sats[GNSS_SYS_GPS] = 1;
    t.sys_sats[GNSS_SYS_QZSS] = 1;
    t.sys_band_sats[GNSS_SYS_GPS][GNSS_BAND_L1] = 1;
    t.sys_band_sats[GNSS_SYS_GPS][GNSS_BAND_L2] = 1;
    t.sys_band_sats[GNSS_SYS_QZSS][GNSS_BAND_L1] = 1;
    t.band_sats[GNSS_BAND_L1] = 2;
    t.band_sats[GNSS_BAND_L2] = 1;
    t.band_cnr_dbhz[GNSS_BAND_L1] = 41;
    t.cnr_dbhz = 41;
    t.msm_age_ms = 400;
    t.sky_age_ms = UINT32_MAX;

    web_live_snap_t l = { .st = &s_st, .limits = { 10, 60, 120 }, .sats = sats, .count = 3, .totals = &t };
    cJSON *root = build_live(&l);
    const cJSON *win = at(root, "window");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(win, "open")));
    TEST_ASSERT_EQUAL(540, at(win, "left_s")->valueint);
    TEST_ASSERT_EQUAL(118, at(win, "idle_left_s")->valueint);
    TEST_ASSERT_EQUAL(10, at(win, "default_min")->valueint);
    TEST_ASSERT_EQUAL(60, at(win, "max_min")->valueint);
    TEST_ASSERT_EQUAL(120, at(win, "idle_s")->valueint);
    TEST_ASSERT_EQUAL_STRING("on", at(win, "sky")->valuestring);
    TEST_ASSERT_EQUAL_STRING("none", at(win, "sky_why")->valuestring);
    TEST_ASSERT_EQUAL(5, at(win, "sky_period_s")->valueint);
    TEST_ASSERT_EQUAL_STRING("none", at(win, "last_end")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(win, "sky_reply")));
    TEST_ASSERT_EQUAL_STRING("streaming", at(root, "receiver_state")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "rx_maintenance")));
    const cJSON *pos = at(root, "position");
    TEST_ASSERT_EQUAL_STRING("survey", at(pos, "source")->valuestring);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9, 45.123456789, at(pos, "lat_deg")->valuedouble);

    const cJSON *tot = at(root, "totals");
    TEST_ASSERT_EQUAL(2, at(tot, "tracked")->valueint);
    TEST_ASSERT_EQUAL(41, at(tot, "cnr_dbhz")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(tot, "sky_age_ms")));
    const cJSON *bands = at(tot, "bands");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(bands));   /* only bands with satellites */
    TEST_ASSERT_EQUAL(2, at(at(bands, "l1"), "satellites")->valueint);
    TEST_ASSERT_EQUAL(41, at(at(bands, "l1"), "cnr_dbhz")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(bands, "l2"), "cnr_dbhz")));
    const cJSON *systems = at(tot, "systems");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(systems));
    const cJSON *gps = cJSON_GetArrayItem(systems, 0);
    TEST_ASSERT_EQUAL_STRING("gps", at(gps, "sys")->valuestring);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(at(gps, "bands")));
    TEST_ASSERT_EQUAL(1, at(at(gps, "bands"), "l2")->valueint);
    TEST_ASSERT_EQUAL_STRING("qzss", at(cJSON_GetArrayItem(systems, 1), "sys")->valuestring);

    const cJSON *list = at(root, "satellites");
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(list));
    const cJSON *s0 = cJSON_GetArrayItem(list, 0);
    TEST_ASSERT_EQUAL(12, at(s0, "prn")->valueint);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(at(s0, "signal_ids")));
    TEST_ASSERT_EQUAL(15, cJSON_GetArrayItem(at(s0, "signal_ids"), 1)->valueint);
    TEST_ASSERT_EQUAL(44, at(at(s0, "cnr_dbhz"), "l1")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(s0, "cnr_dbhz"), "l2")));
    TEST_ASSERT_EQUAL(45, at(s0, "el_deg")->valueint);
    TEST_ASSERT_EQUAL(210, at(s0, "az_deg")->valueint);
    TEST_ASSERT_EQUAL(43, at(s0, "sky_cnr_dbhz")->valueint);
    TEST_ASSERT_EQUAL(1200, at(s0, "pos_age_ms")->valueint);
    const cJSON *s1 = cJSON_GetArrayItem(list, 1);
    TEST_ASSERT_EQUAL(194, at(s1, "prn")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(s1, "el_deg")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(s1, "az_deg")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(s1, "sky_cnr_dbhz")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(s1, "pos_age_ms")));
    const cJSON *s2 = cJSON_GetArrayItem(list, 2);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(s2, "in_msm")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(s2, "msm_age_ms")));
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(at(s2, "cnr_dbhz")));
    TEST_ASSERT_EQUAL(-2, at(s2, "el_deg")->valueint);
    cJSON_Delete(root);
}

TEST_CASE("live view: a closed window with its end reason, no data without totals", "[web_live]")
{
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = STATION_STATE_IDLE;
    s_st.rx_maintenance = true;
    s_st.live.left_s = 77;   /* not meaningful while closed */
    s_st.live.max_s = 3600;
    s_st.live.sky = STATION_LIVE_SKY_BLOCKED;
    s_st.live.sky_why = STATION_LIVE_WHY_REJECTED;
    s_st.live.last_end = STATION_LIVE_END_IDLE;
    strcpy(s_st.live.sky_reply, "COMMAND REJECTED");
    gnss_obs_live_sat_t one = { .sys = GNSS_SYS_GPS, .prn = 1 };
    web_live_snap_t l = { .st = &s_st, .limits = { 10, 60, 120 }, .sats = &one, .count = 1, .totals = NULL };
    cJSON *root = build_live(&l);
    const cJSON *win = at(root, "window");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(win, "open")));
    TEST_ASSERT_EQUAL(0, at(win, "left_s")->valueint);
    TEST_ASSERT_EQUAL_STRING("blocked", at(win, "sky")->valuestring);
    TEST_ASSERT_EQUAL_STRING("rejected", at(win, "sky_why")->valuestring);
    TEST_ASSERT_EQUAL_STRING("idle", at(win, "last_end")->valuestring);
    TEST_ASSERT_EQUAL_STRING("COMMAND REJECTED", at(win, "sky_reply")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "rx_maintenance")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "totals")));
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(at(root, "satellites")));
    const cJSON *pos = at(root, "position");
    TEST_ASSERT_EQUAL_STRING("none", at(pos, "source")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(pos, "lat_deg")));
    cJSON_Delete(root);
}

TEST_CASE("live view, recovery and read-back enum names", "[web_live]")
{
    TEST_ASSERT_EQUAL_STRING("off", web_live_sky_name(STATION_LIVE_SKY_OFF));
    TEST_ASSERT_EQUAL_STRING("on_profile", web_live_sky_name(STATION_LIVE_SKY_ON_PROFILE));
    TEST_ASSERT_EQUAL_STRING("stopping", web_live_sky_name(STATION_LIVE_SKY_STOPPING));
    TEST_ASSERT_EQUAL_STRING("unknown", web_live_sky_name(STATION_LIVE_SKY_STOPPING + 1));
    TEST_ASSERT_EQUAL_STRING("autocfg_off", web_live_why_name(STATION_LIVE_WHY_AUTOCFG_OFF));
    TEST_ASSERT_EQUAL_STRING("reconfigured", web_live_why_name(STATION_LIVE_WHY_RECONFIGURED));
    TEST_ASSERT_EQUAL_STRING("not_running", web_live_why_name(STATION_LIVE_WHY_NOT_RUNNING));
    TEST_ASSERT_EQUAL_STRING("expired", web_live_end_name(STATION_LIVE_END_EXPIRED));
    TEST_ASSERT_EQUAL_STRING("flash", web_live_end_name(STATION_LIVE_END_FLASH));
    TEST_ASSERT_EQUAL_STRING("full", web_recovery_mode_name(HEALTH_RECOVERY_FULL));
    TEST_ASSERT_EQUAL_STRING("no_reboot", web_recovery_mode_name(HEALTH_RECOVERY_NO_REBOOT));
    TEST_ASSERT_EQUAL_STRING("alerts_only", web_recovery_mode_name(HEALTH_RECOVERY_ALERTS));
    TEST_ASSERT_EQUAL_STRING("none", web_health_step_phase_name(HEALTH_STEP_PHASE_NONE));
    TEST_ASSERT_EQUAL_STRING("running", web_health_step_phase_name(HEALTH_STEP_PHASE_RUNNING));
    TEST_ASSERT_EQUAL_STRING("waiting", web_health_step_phase_name(HEALTH_STEP_PHASE_WAITING));
    TEST_ASSERT_EQUAL_STRING("unknown", web_health_step_phase_name(HEALTH_STEP_PHASE_WAITING + 1));
    TEST_ASSERT_EQUAL_STRING("unknown", web_setting_name(UM98X_SETTING_UNKNOWN));
    TEST_ASSERT_EQUAL_STRING("on", web_setting_name(UM98X_SETTING_ON));
    TEST_ASSERT_EQUAL_STRING("off", web_setting_name(UM98X_SETTING_OFF));
    TEST_ASSERT_EQUAL_STRING("unknown", web_setting_name(7));
    TEST_ASSERT_EQUAL_STRING("protected", web_ladder_trigger_name(NETWORK_LADDER_TRIG_PROTECTED));
    TEST_ASSERT_EQUAL_STRING("assoc_no_ip", web_ladder_trigger_name(NETWORK_LADDER_TRIG_ASSOC_NO_IP));
    TEST_ASSERT_EQUAL_STRING("unreached", web_ladder_trigger_name(NETWORK_LADDER_TRIG_UNREACHED));
    TEST_ASSERT_NULL(web_ladder_trigger_name(1u << 3));
    health_recovery_mode_t m = HEALTH_RECOVERY_FULL;
    TEST_ASSERT_TRUE(web_recovery_mode_parse("alerts_only", &m));
    TEST_ASSERT_EQUAL(HEALTH_RECOVERY_ALERTS, m);
    TEST_ASSERT_FALSE(web_recovery_mode_parse("alerts", &m));
    TEST_ASSERT_FALSE(web_recovery_mode_parse("2", &m));
}

/* ---- status, Wi-Fi, health ----------------------------------------------------------------- */

TEST_CASE("status: credentials on trial or pending, maintenance, UART and recovery mode (admin form only)", "[web_live]")
{
    static web_status_snap_t s;
    memset(&s, 0, sizeof(s));
    strcpy(s.hostname, "onocoydiy");
    s.rtcm_age_ms = UINT32_MAX;
    s.net.sta_configured = true;
    strcpy(s.net.ssid, "Home");
    s.net.assoc_no_ip_s = 42;
    s.net.ladder_active = true;
    s.net.ladder_triggers = NETWORK_LADDER_TRIG_ASSOC_NO_IP | NETWORK_LADDER_TRIG_UNREACHED;
    s.net.cred_trial = true;
    s.net.cred_trial_left_s = 61;
    s.net.cred_pending = true;
    strcpy(s.net.pending_ssid, "NewNet");
    s.st.state = STATION_STATE_IDLE;
    s.st.rx_maintenance = true;
    s.st.maintenance_left_s = 1500;
    s.st.maintenance_chosen_s = 1800;
    s.uart_closed = true;
    s.health.level = HEALTH_RECOVERING;
    s.health.recovery_mode = HEALTH_RECOVERY_NO_REBOOT;
    s.health.gnss_reboot_recheck = true;
    s.health.factory_reset_refused_age_s = UINT32_MAX;
    s.health.last_step = HEALTH_STEP_RECONFIGURE;
    s.health.gnss_step = HEALTH_STEP_RECONFIGURE;
    s.health.gnss_step_phase = HEALTH_STEP_PHASE_WAITING;
    s.slots = 1;
    s.cfg_valid = true;
    s.cst[0].unreached_s = 185;

    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_status(&w, &s, false);
    cJSON *root = parse_out();
    const cJSON *n = at(root, "network");
    TEST_ASSERT_EQUAL(42, at(n, "assoc_no_ip_s")->valueint);
    const cJSON *trig = at(n, "ladder_triggers");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(trig));
    TEST_ASSERT_EQUAL_STRING("assoc_no_ip", cJSON_GetArrayItem(trig, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("unreached", cJSON_GetArrayItem(trig, 1)->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(n, "cred_trial")));
    TEST_ASSERT_EQUAL(61, at(n, "cred_trial_left_s")->valueint);
    TEST_ASSERT_EQUAL_STRING("NewNet", at(n, "pending_ssid")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(n, "cred_reverted")));
    const cJSON *rx = at(root, "receiver");
    TEST_ASSERT_EQUAL_STRING("idle", at(rx, "state")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(rx, "rx_maintenance")));
    TEST_ASSERT_EQUAL(1500, at(rx, "maintenance_left_s")->valueint);
    TEST_ASSERT_EQUAL(1800, at(rx, "maintenance_chosen_s")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(rx, "uart_closed")));
    const cJSON *hl = at(root, "health");
    TEST_ASSERT_EQUAL_STRING("no_reboot", at(hl, "recovery_mode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(hl, "reboot_recheck")));
    /* What the recovery does now: the step and where it is. */
    TEST_ASSERT_EQUAL_STRING("reconfigure", at(hl, "step")->valuestring);
    TEST_ASSERT_EQUAL_STRING("waiting", at(hl, "step_phase")->valuestring);
    TEST_ASSERT_EQUAL(185, at(cJSON_GetArrayItem(at(root, "casters"), 0), "unreached_s")->valueint);
    cJSON_Delete(root);

    /* Pending only, reverted: no time left shown. */
    s.net.cred_trial = false;
    s.net.cred_reverted = true;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_status(&w, &s, false);
    root = parse_out();
    n = at(root, "network");
    TEST_ASSERT_EQUAL(0, at(n, "cred_trial_left_s")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(n, "cred_reverted")));
    cJSON_Delete(root);

    /* None of it in the public form. */
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_status(&w, &s, true);
    TEST_ASSERT_TRUE(wj_ok(&w));
    static const char *const never[] = { "NewNet", "rx_maintenance", "uart_closed", "recovery_mode", "unreached_s",
                                         "pending_ssid", "reboot_recheck", "step" };
    for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
        TEST_ASSERT_NULL_MESSAGE(strstr(s_out, never[i]), never[i]);
    }

    /* Nothing pending: the SSID is null. */
    memset(&s.net, 0, sizeof(s.net));
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_status(&w, &s, false);
    root = parse_out();
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "network"), "pending_ssid")));
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(at(at(root, "network"), "ladder_triggers")));
    cJSON_Delete(root);
}

TEST_CASE("Wi-Fi and health carry the pending credentials, triggers, recovery mode and UART state", "[web_live]")
{
    static network_status_t net;
    memset(&net, 0, sizeof(net));
    net.sta_configured = true;
    strcpy(net.ssid, "Home");
    net.cred_pending = true;
    net.cred_reverted = true;
    strcpy(net.pending_ssid, "Typo");
    net.ladder_triggers = NETWORK_LADDER_TRIG_PROTECTED;
    net.probe_age_s = UINT32_MAX;
    web_wifi_snap_t ws = { .net = &net, .scan_age_s = UINT32_MAX };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_wifi(&w, &ws);
    cJSON *root = parse_out();
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "cred_pending")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "cred_reverted")));
    TEST_ASSERT_EQUAL_STRING("Typo", at(root, "pending_ssid")->valuestring);
    TEST_ASSERT_EQUAL_STRING("protected", cJSON_GetArrayItem(at(at(root, "ladder"), "triggers"), 0)->valuestring);
    cJSON_Delete(root);

    static health_status_t hs;
    static lifecycle_boot_info_t boot;
    static gnss_link_stats_t link;
    static rtcm_hub_stats_t hub;
    static station_status_t st;
    static gnss_obs_summary_t obs;
    static diag_coredump_info_t cd;
    memset(&hs, 0, sizeof(hs));
    hs.recovery_mode = HEALTH_RECOVERY_ALERTS;
    hs.gnss_reboot_recheck = true;
    hs.gnss_step = HEALTH_STEP_RESCAN;
    hs.gnss_step_phase = HEALTH_STEP_PHASE_RUNNING;
    memset(&link, 0, sizeof(link));
    link.mode = GNSS_LINK_MODE_STREAM;
    link.link_open = false;
    link.open_failures = 3;
    link.hands_off = true;
    memset(&st, 0, sizeof(st));
    st.rx_maintenance = true;
    web_health_snap_t h = { .health = &hs, .boot = &boot, .link = &link, .hub = &hub, .st = &st, .obs = &obs,
                            .net = &net, .coredump = &cd };
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_health(&w, &h);
    root = parse_out();
    TEST_ASSERT_EQUAL_STRING("alerts_only", at(root, "recovery_mode")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "reboot_recheck")));
    TEST_ASSERT_EQUAL_STRING("rescan", at(root, "step")->valuestring);
    TEST_ASSERT_EQUAL_STRING("running", at(root, "step_phase")->valuestring);
    const cJSON *l = at(root, "link");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(l, "open")));
    TEST_ASSERT_EQUAL(3, at(l, "open_failures")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(l, "hands_off")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(at(root, "station"), "rx_maintenance")));
    TEST_ASSERT_EQUAL_STRING("protected",
                             cJSON_GetArrayItem(at(at(at(root, "network"), "ladder"), "triggers"), 0)->valuestring);
    cJSON_Delete(root);
}

/* ---- connection history --------------------------------------------------------------------- */

TEST_CASE("connection history: the four kinds, newest first, nothing past the page's top", "[web_live]")
{
    diag_journal_entry_t page[6] = {
        { .seq = 10, .kind = DIAG_EV_BOOT },
        { .seq = 11, .kind = DIAG_EV_WIFI_DISCONNECT, .a = 8, .b = -70 },
        { .seq = 12, .kind = DIAG_EV_WIFI_UP, .a = 3, .b = 45 },
        { .seq = 13, .kind = DIAG_EV_LADDER_STEP },
        { .seq = 14, .kind = DIAG_EV_CASTER_UP, .a = diag_caster_a(1, 0, 4), .b = 60 },
        { .seq = 15, .kind = DIAG_EV_CASTER_DOWN, .a = diag_caster_a(0, NTRIP_FAIL_REMOTE_CLOSE, 2), .b = 3600 },
    };
    diag_journal_entry_t out[8];
    size_t n = 0;
    /* A record newer than the page's top (written meanwhile) belongs to the page before. */
    TEST_ASSERT_TRUE(web_history_collect(page, 6, 14, out, &n, 8));
    TEST_ASSERT_EQUAL(3, n);
    TEST_ASSERT_EQUAL_UINT32(14, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(12, out[1].seq);
    TEST_ASSERT_EQUAL_UINT32(11, out[2].seq);
    /* Full: stops at max and says so. */
    n = 0;
    TEST_ASSERT_FALSE(web_history_collect(page, 6, 15, out, &n, 2));
    TEST_ASSERT_EQUAL(2, n);
    TEST_ASSERT_EQUAL_UINT32(15, out[0].seq);
    TEST_ASSERT_EQUAL_UINT32(14, out[1].seq);
    TEST_ASSERT_FALSE(web_history_kind(DIAG_EV_LIVE_VIEW));
    TEST_ASSERT_TRUE(web_history_kind(DIAG_EV_WIFI_DISCONNECT));

    n = 0;
    (void)web_history_collect(page, 6, 15, out, &n, 8);
    out[0].unix_s = 1760000000;
    ntrip_caster_cfg_t cfg[2];
    memset(cfg, 0, sizeof(cfg));
    strcpy(cfg[0].name, "Onocoy");
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_connections(&w, out, n, cfg, 2, true);
    cJSON *root = parse_out();
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "complete")));
    const cJSON *list = at(root, "entries");
    TEST_ASSERT_EQUAL(4, cJSON_GetArraySize(list));
    const cJSON *down = cJSON_GetArrayItem(list, 0);
    TEST_ASSERT_EQUAL_STRING("caster_down", at(down, "kind")->valuestring);
    TEST_ASSERT_EQUAL_DOUBLE(1760000000.0, at(down, "time_unix")->valuedouble);
    TEST_ASSERT_EQUAL(0, at(down, "slot")->valueint);
    TEST_ASSERT_EQUAL_STRING("Onocoy", at(down, "slot_name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("remote_close", at(down, "class")->valuestring);
    TEST_ASSERT_EQUAL(2, at(down, "count")->valueint);
    TEST_ASSERT_EQUAL(3600, at(down, "duration_s")->valueint);
    const cJSON *up = cJSON_GetArrayItem(list, 1);
    TEST_ASSERT_EQUAL_STRING("caster_up", at(up, "kind")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(up, "time_unix")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(up, "slot_name")));   /* a slot without a name */
    TEST_ASSERT_TRUE(cJSON_IsNull(at(up, "class")));
    TEST_ASSERT_EQUAL(4, at(up, "count")->valueint);
    TEST_ASSERT_EQUAL(60, at(up, "duration_s")->valueint);
    const cJSON *wup = cJSON_GetArrayItem(list, 2);
    TEST_ASSERT_EQUAL_STRING("wifi_up", at(wup, "kind")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(wup, "slot")));
    TEST_ASSERT_EQUAL(3, at(wup, "count")->valueint);
    TEST_ASSERT_EQUAL(45, at(wup, "duration_s")->valueint);
    const cJSON *wd = cJSON_GetArrayItem(list, 3);
    TEST_ASSERT_EQUAL_STRING("wifi_disconnect", at(wd, "kind")->valuestring);
    TEST_ASSERT_EQUAL(8, at(wd, "reason")->valueint);
    TEST_ASSERT_EQUAL(-70, at(wd, "rssi")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(wd, "duration_s")));
    cJSON_Delete(root);

    /* Safe mode: no slot names; a slot beyond the table has none either. */
    out[0].a = diag_caster_a(5, NTRIP_FAIL_DNS, 0);
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_connections(&w, out, 1, NULL, 2, false);
    root = parse_out();
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "complete")));
    down = cJSON_GetArrayItem(at(root, "entries"), 0);
    TEST_ASSERT_EQUAL(5, at(down, "slot")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(down, "slot_name")));
    TEST_ASSERT_EQUAL_STRING("dns", at(down, "class")->valuestring);
    cJSON_Delete(root);
}

/* web_ws.c builds every status section into 1024 bytes and every caster summary into 768. */
TEST_CASE("status sections with the new fields still fit the WebSocket's section buffers", "[web_live]")
{
    static web_status_snap_t s;
    memset(&s, 0, sizeof(s));
    memset(s.hostname, 'h', sizeof(s.hostname) - 1);
    memset(s.version, 'v', sizeof(s.version) - 1);
    s.rtcm_age_ms = UINT32_MAX - 1;
    s.st.state = STATION_STATE_FLASH_RECOVERY;
    s.st.receiver.valid = true;
    memset(s.st.receiver.model, 0x01, sizeof(s.st.receiver.model) - 1);         /* control bytes: \u0001 each */
    memset(s.st.receiver.firmware, 0x01, sizeof(s.st.receiver.firmware) - 1);
    s.st.survey_window_left_s = UINT32_MAX - 1;
    s.st.rx_maintenance = true;
    s.st.maintenance_left_s = UINT32_MAX - 1;
    s.st.maintenance_chosen_s = UINT32_MAX - 1;
    s.st.flash_journal_open = true;
    s.uart_closed = true;
    s.obs_valid = true;
    s.best_bands = 255;
    s.net.sta_configured = true;
    s.net.online = true;
    s.net.associated = true;
    memset(s.net.ssid, 0x01, sizeof(s.net.ssid) - 1);
    memset(s.net.ip, '9', sizeof(s.net.ip) - 1);
    s.net.rssi = -128;
    s.net.offline_s = UINT32_MAX - 1;
    s.net.setup_ap_active = true;
    s.net.ap_reasons = 0x0F;
    s.net.assoc_no_ip_s = UINT32_MAX - 1;
    s.net.ladder_triggers = 0x07;
    s.net.cred_trial = true;
    s.net.cred_trial_left_s = UINT32_MAX - 1;
    s.net.cred_pending = true;
    memset(s.net.pending_ssid, 0x01, sizeof(s.net.pending_ssid) - 1);
    s.net.cred_reverted = true;
    s.health.level = HEALTH_RECOVERING;
    s.health.last_step = HEALTH_STEP_RECEIVER_RESET;
    s.health.gnss_step = HEALTH_STEP_RECEIVER_RESET;
    s.health.gnss_step_phase = HEALTH_STEP_PHASE_RUNNING;
    s.health.recovery_mode = HEALTH_RECOVERY_NO_REBOOT;
    s.health.gnss_reboot_recheck = true;
    s.health.factory_reset_refused_age_s = 1;
    s.health.gnss_reboot_hold = 0x0F;
    s.health.gnss_reboot_hold_left_s = UINT32_MAX - 1;
    s.health.gnss_reboot_cap_wait_s = UINT32_MAX - 1;
    s.slots = 1;
    s.cfg_valid = true;
    /* Caster strings are validated on input (no control characters; host a DNS name): '"' escapes worst. */
    memset(s.cfg[0].name, '"', sizeof(s.cfg[0].name) - 1);
    memset(s.cfg[0].host, 'a', sizeof(s.cfg[0].host) - 1);
    s.cfg[0].protected_slot = true;
    s.cfg[0].enabled = true;
    s.cst[0].state = NTRIP_STATE_CONFIG_ERROR;
    s.cst[0].reason = NTRIP_FAIL_REMOTE_CLOSE;
    s.cst[0].state_age_s = UINT32_MAX - 1;
    s.cst[0].connected_s = UINT32_MAX - 1;
    s.cst[0].today_online_s = UINT32_MAX - 1;
    s.cst[0].today_offline_s = UINT32_MAX - 1;
    s.cst[0].budget_left_s = UINT32_MAX - 1;
    s.cst[0].budget_s = UINT32_MAX - 1;
    s.cst[0].day_metrics_valid = true;   /* every Onocoy day figure a number, not null */
    s.cst[0].so_far_valid = true;
    s.cst[0].data_rate_valid = true;
    s.cst[0].graced_uptime_permille = 1000;
    s.cst[0].graced_uptime_so_far_permille = 1000;
    s.cst[0].data_rate_permille = 1000;
    s.cst[0].bytes_sent = UINT64_C(0x1FFFFFFFFFFFFF);
    s.cst[0].lag_ms_now = UINT32_MAX - 1;
    s.cst[0].next_retry_s = UINT32_MAX - 1;
    s.cst[0].unreached_s = UINT32_MAX - 1;
    memset(s.cst[0].hint, '"', sizeof(s.cst[0].hint) - 1);   /* the caster component's own text */

    static char frag[1024];
    for (int sec = 0; sec < WEB_SEC_COUNT; sec++) {
        if (sec == WEB_SEC_UPDATE) {
            continue;   /* not changed here */
        }
        wj_t w;
        wj_init_members(&w, frag, sizeof(frag));
        web_build_section(&w, &s, (web_section_t)sec, false);
        TEST_ASSERT_TRUE_MESSAGE(wj_ok(&w), web_section_name((web_section_t)sec));
    }
    static char cst[896];
    wj_t w;
    wj_init_elements(&w, cst, sizeof(cst));
    web_build_caster_summary(&w, &s, 0, false);
    TEST_ASSERT_TRUE(wj_ok(&w));
}
