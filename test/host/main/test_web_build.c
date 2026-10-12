/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <string.h>

#include "cJSON.h"
#include "unity.h"
#include "web_build.h"
#include "web_enums.h"

static char s_out[16384];
static web_status_snap_t s_snap;

static void fixture(void)
{
    web_status_snap_t *s = &s_snap;
    memset(s, 0, sizeof(*s));
    s->uptime_s = 86400;
    strcpy(s->hostname, "onocoydiy");
    strcpy(s->version, "v0.3.0");
    s->time_unix = 1760090000;
    s->clock = LEDGER_CLOCK_GNSS;
    s->settings_available = true;
    s->net.sta_configured = true;
    s->net.online = true;
    s->net.associated = true;
    strcpy(s->net.ssid, "Home");
    strcpy(s->net.ip, "192.168.1.50");
    s->net.rssi = -61;
    s->net.probe = NETWORK_PROBE_OK;
    s->st.state = STATION_STATE_STREAMING;
    s->st.receiver.valid = true;
    strcpy(s->st.receiver.model, "UM982");
    strcpy(s->st.receiver.firmware, "R4.10Build18975");
    s->st.receiver.build = 18975;
    s->st.alerts = STATION_ALERT_CONFIG_FAILED;
    s->st.config_retry_in_s = 600;
    s->st.position.source = UM98X_POS_SURVEY;
    s->st.position.lat_deg = 45.123456789;
    s->rtcm_age_ms = UINT32_MAX;
    s->obs_valid = true;
    s->best_sys = GNSS_SYS_GALILEO;
    s->best_bands = 4;
    s->slots = 2;
    s->cfg_valid = true;
    s->cfg[0].enabled = true;
    s->cfg[0].protected_slot = true;
    strcpy(s->cfg[0].name, "Onocoy");
    strcpy(s->cfg[0].host, "servers.onocoy.com");
    strcpy(s->cfg[0].mountpoint, "secretuser");
    strcpy(s->cfg[0].username, "secretname");
    strcpy(s->cfg[0].password, "secretpw");
    s->cfg[0].password_set = true;
    strcpy(s->cfg[1].name, "Spare");
    strcpy(s->cfg[1].host, "rtk2go.com");
    s->cst[0].state = NTRIP_STATE_STREAMING;
    s->cst[0].budget_left_s = 469;
    s->cst[0].budget_s = 473;
    s->cst[0].graced_uptime_permille = 1000;
    s->cst[0].streak_ok = true;
    s->cst[0].day_metrics_valid = true;
    s->cst[0].data_rate_valid = true;
    s->cst[0].data_rate_permille = 1000;
    s->cst[0].so_far_valid = true;
    s->cst[0].graced_uptime_so_far_permille = 1000;
    s->cst[1].state = NTRIP_STATE_DISABLED;
    s->cst[1].budget_left_s = 77;
    s->health.level = HEALTH_DEGRADED;
    s->health.alerts = HEALTH_ALERT_LINK_LOSS;
    strcpy(s->fw.latest_version, "v0.4.0");
    s->fw.app_update_available = true;
    s->ledger_budget_left_s = 473;
}

static cJSON *build_status(bool pub)
{
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_status(&w, &s_snap, pub);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    return root;
}

static const cJSON *at(const cJSON *o, const char *k)
{
    const cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    TEST_ASSERT_NOT_NULL_MESSAGE(it, k);
    return it;
}

TEST_CASE("admin status maps sentinels, alerts and the protected slot", "[web_build]")
{
    fixture();
    cJSON *root = build_status(false);
    TEST_ASSERT_EQUAL(1, at(root, "api")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "public")));
    const cJSON *dev = at(root, "device");
    TEST_ASSERT_EQUAL_STRING("v0.3.0", at(dev, "version")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(dev, "dev_build")));
    TEST_ASSERT_EQUAL_STRING("gnss", at(dev, "time_source")->valuestring);
    const cJSON *rx = at(root, "receiver");
    TEST_ASSERT_TRUE(cJSON_IsNull(at(rx, "rtcm_age_ms")));
    TEST_ASSERT_EQUAL_STRING("galileo", at(rx, "best_sys")->valuestring);
    TEST_ASSERT_EQUAL_STRING("none", at(rx, "journal")->valuestring);
    TEST_ASSERT_EQUAL_STRING("streaming", at(rx, "state")->valuestring);
    const cJSON *casters = at(root, "casters");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(casters));
    const cJSON *c0 = cJSON_GetArrayItem(casters, 0);
    const cJSON *c1 = cJSON_GetArrayItem(casters, 1);
    TEST_ASSERT_EQUAL(469, at(c0, "budget_left_s")->valueint);
    TEST_ASSERT_EQUAL(473, at(c0, "budget_s")->valueint);   /* the day's allowance, not left + offline */
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(c0, "protected")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c1, "budget_left_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c1, "budget_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c1, "streak_ok")));
    TEST_ASSERT_EQUAL_STRING("disabled", at(c1, "state")->valuestring);

    const cJSON *alerts = at(root, "alerts");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(alerts));
    TEST_ASSERT_EQUAL_STRING("link_loss", at(cJSON_GetArrayItem(alerts, 0), "id")->valuestring);
    TEST_ASSERT_EQUAL_STRING("health", at(cJSON_GetArrayItem(alerts, 0), "source")->valuestring);
    TEST_ASSERT_EQUAL_STRING("config_failed", at(cJSON_GetArrayItem(alerts, 1), "id")->valuestring);
    TEST_ASSERT_EQUAL(600, at(cJSON_GetArrayItem(alerts, 1), "retry_in_s")->valueint);

    TEST_ASSERT_EQUAL(469, at(at(root, "availability"), "budget_left_s")->valueint);
    const cJSON *up = at(root, "update");
    TEST_ASSERT_EQUAL_STRING("v0.4.0", at(up, "app_version")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(up, "gnss_flash_available")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(up, "gnss_build")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(up, "last_check_unix")));
    /* Secrets are never part of a status. */
    TEST_ASSERT_NULL(strstr(s_out, "secretpw"));
    TEST_ASSERT_NULL(strstr(s_out, "secretuser"));
    TEST_ASSERT_NULL(strstr(s_out, "secretname"));
    cJSON_Delete(root);
}

TEST_CASE("public status leaves out hosts, addresses, SSID and versions", "[web_build]")
{
    fixture();
    cJSON *root = build_status(true);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "public")));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "network"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "update"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "alerts"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "safe_mode"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "availability"));
    const cJSON *dev = at(root, "device");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(dev));
    TEST_ASSERT_EQUAL_STRING("onocoydiy", at(dev, "hostname")->valuestring);
    const cJSON *rx = at(root, "receiver");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(rx));
    TEST_ASSERT_EQUAL(4, at(rx, "best_bands")->valueint);
    const cJSON *c0 = cJSON_GetArrayItem(at(root, "casters"), 0);
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(c0));
    TEST_ASSERT_EQUAL_STRING("Onocoy", at(c0, "name")->valuestring);
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(at(root, "health")));
    static const char *const never[] = { "servers.onocoy.com", "rtk2go", "192.168", "Home", "v0.3.0", "secret",
                                         "UM982", "45.12" };
    for (size_t i = 0; i < sizeof(never) / sizeof(never[0]); i++) {
        TEST_ASSERT_NULL_MESSAGE(strstr(s_out, never[i]), never[i]);
    }
    cJSON_Delete(root);
}

TEST_CASE("status sections build as fragments; the public form skips admin ones", "[web_build]")
{
    fixture();
    char frag[1024];
    wj_t w;
    wj_init_members(&w, frag, sizeof(frag));
    web_build_section(&w, &s_snap, WEB_SEC_UPDATE, true);
    TEST_ASSERT_EQUAL_STRING("", wj_text(&w));
    wj_init_members(&w, frag, sizeof(frag));
    web_build_section(&w, &s_snap, WEB_SEC_HEALTH, true);
    TEST_ASSERT_EQUAL_STRING("\"health\":{\"level\":\"degraded\"}", wj_text(&w));
    wj_init_elements(&w, frag, sizeof(frag));
    web_build_caster_summary(&w, &s_snap, 1, true);
    TEST_ASSERT_EQUAL_STRING("{\"slot\":1,\"name\":\"Spare\",\"state\":\"disabled\"}", wj_text(&w));
    for (int i = 0; i < WEB_SEC_COUNT; i++) {
        TEST_ASSERT_NOT_NULL(web_section_name((web_section_t)i));
    }
}

TEST_CASE("budget comes from the protected slot, the ledger, or nowhere", "[web_build]")
{
    fixture();
    TEST_ASSERT_EQUAL_UINT32(469, web_budget_left_s(&s_snap));
    s_snap.cst[0].state = NTRIP_STATE_DISABLED;
    TEST_ASSERT_EQUAL_UINT32(473, web_budget_left_s(&s_snap));
    s_snap.clock = LEDGER_CLOCK_NONE;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, web_budget_left_s(&s_snap));
    s_snap.clock = LEDGER_CLOCK_SNTP;
    s_snap.cfg[0].enabled = false;
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, web_budget_left_s(&s_snap));
    s_snap.cfg_valid = false;   /* safe mode: configs never loaded */
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, web_budget_left_s(&s_snap));
}

TEST_CASE("caster config never carries the password", "[web_build]")
{
    fixture();
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_caster_entry(&w, 0, &s_snap.cfg[0], &s_snap.cst[0]);
    TEST_ASSERT_TRUE(wj_ok(&w));
    TEST_ASSERT_NULL(strstr(s_out, "secretpw"));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL(root);
    const cJSON *cfg = at(root, "config");
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(cfg, "password_set")));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(cfg, "password"));
    TEST_ASSERT_EQUAL(1, at(cfg, "ntrip_version")->valueint);
    TEST_ASSERT_EQUAL_STRING("bundle", at(cfg, "trust")->valuestring);
    const cJSON *st = at(root, "status");
    TEST_ASSERT_EQUAL(NTRIP_FAIL_CLASS_COUNT - 1, cJSON_GetArraySize(at(st, "failures_by_class")));   /* all but none */
    TEST_ASSERT_TRUE(cJSON_IsNull(at(st, "tls_version")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(st, "connected_since_unix")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(st, "last_err")));
    cJSON_Delete(root);

    /* Safe mode: no config at all. */
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_caster_entry(&w, 1, NULL, &s_snap.cst[1]);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "config")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "status"), "budget_left_s")));
    cJSON_Delete(root);
}

TEST_CASE("journal view follows the station's record", "[web_build]")
{
    station_status_t st;
    memset(&st, 0, sizeof(st));
    TEST_ASSERT_EQUAL(WEB_JOURNAL_NONE, web_journal_view(&st, false));
    st.flash_journal_open = true;
    TEST_ASSERT_EQUAL(WEB_JOURNAL_UNREADABLE, web_journal_view(&st, false));
    st.flash_journal_readable = true;
    st.flash_journal.state = UM98X_JOURNAL_TRANSFERRING;
    TEST_ASSERT_EQUAL(WEB_JOURNAL_RUNNING, web_journal_view(&st, false));
    TEST_ASSERT_EQUAL(WEB_JOURNAL_WAITING, web_journal_view(&st, true));   /* nothing runs in safe mode */
    st.flash.phase = UM98X_FLASH_NEEDS_USER;                                /* report-only recovery */
    TEST_ASSERT_EQUAL(WEB_JOURNAL_WAITING, web_journal_view(&st, false));
    st.flash.phase = UM98X_FLASH_IDLE;
    st.flash_journal.state = UM98X_JOURNAL_NEEDS_USER;
    TEST_ASSERT_EQUAL(WEB_JOURNAL_WAITING, web_journal_view(&st, false));
    /* The user's retry was accepted: the record says "needs user" until the
     * session's first write, but the journal is worked on (no clear, no
     * second action). */
    st.flash_session_running = true;
    TEST_ASSERT_EQUAL(WEB_JOURNAL_RUNNING, web_journal_view(&st, false));
    web_flash_ctx_t c = { .journal = web_journal_view(&st, false), .station_running = true, .flash_available = true };
    TEST_ASSERT_EQUAL_STRING("not_waiting", web_flash_action_why(&c, WEB_FLASH_CLEAR, NULL));
    TEST_ASSERT_EQUAL_STRING("not_waiting", web_flash_action_why(&c, WEB_FLASH_RETRY, NULL));
    st.flash_session_running = false;
    st.flash_journal_settings_unavailable = true;
    TEST_ASSERT_EQUAL(WEB_JOURNAL_UNKNOWN, web_journal_view(&st, false));
    TEST_ASSERT_EQUAL_STRING("unknown", web_journal_name(WEB_JOURNAL_UNKNOWN));
}

TEST_CASE("receiver update progress: only the station's final message is done", "[web_build]")
{
    um98x_flash_progress_t p;
    memset(&p, 0, sizeof(p));
    p.bytes_total = 3000000;
    p.bytes_sent = 1500000;
    p.transfers = 1;
    web_progress_t m;

    /* A transfer failed after the point of no return and the session goes
     * on (back to the loader, the same package again): not the end. */
    p.phase = UM98X_FLASH_FAILED;
    web_flash_progress_msg(&p, false, false, NULL, &m);
    TEST_ASSERT_FALSE(m.done);
    TEST_ASSERT_NULL(m.result);
    TEST_ASSERT_EQUAL_STRING("failed", m.phase);
    TEST_ASSERT_EQUAL_STRING("gnss_flash", m.job);
    TEST_ASSERT_EQUAL_INT(50, m.pct);
    TEST_ASSERT_EQUAL(ESP_OK, m.err);
    p.phase = UM98X_FLASH_NEEDS_USER;   /* the session's own stop, before the station's end */
    web_flash_progress_msg(&p, false, false, NULL, &m);
    TEST_ASSERT_FALSE(m.done);

    /* The end: how it ended comes from the station's last outcome. */
    station_flash_last_t last;
    memset(&last, 0, sizeof(last));
    last.outcome = STATION_FLASH_OUT_NEEDS_USER;
    p.last_err = ESP_ERR_TIMEOUT;
    web_flash_progress_msg(&p, true, false, &last, &m);
    TEST_ASSERT_TRUE(m.done);
    TEST_ASSERT_EQUAL_STRING("needs_user", m.result);
    TEST_ASSERT_EQUAL(ESP_ERR_TIMEOUT, m.err);
    TEST_ASSERT_EQUAL_PTR(&last, m.last);
    last.outcome = STATION_FLASH_OUT_SUCCESS;
    p.phase = UM98X_FLASH_DONE;
    web_flash_progress_msg(&p, true, false, &last, &m);
    TEST_ASSERT_EQUAL_STRING("ok", m.result);
    TEST_ASSERT_EQUAL(ESP_OK, m.err);
    web_flash_progress_msg(&p, true, false, NULL, &m);
    TEST_ASSERT_EQUAL_STRING("ok", m.result);
    p.phase = UM98X_FLASH_FAILED;
    web_flash_progress_msg(&p, true, false, NULL, &m);
    TEST_ASSERT_EQUAL_STRING("failed", m.result);

    /* Builds without flashing: a package check. */
    p.phase = UM98X_FLASH_CHECKING;
    web_flash_progress_msg(&p, false, true, NULL, &m);
    TEST_ASSERT_EQUAL_STRING("gnss_check", m.job);
    TEST_ASSERT_EQUAL_STRING("checking", m.phase);
    TEST_ASSERT_NULL(m.flash);
    TEST_ASSERT_EQUAL_INT(-1, m.pct);
    last.outcome = STATION_FLASH_OUT_REJECTED;
    p.phase = UM98X_FLASH_FAILED;
    web_flash_progress_msg(&p, true, true, &last, &m);
    TEST_ASSERT_TRUE(m.done);
    TEST_ASSERT_EQUAL_STRING("package_rejected", m.result);
}

static const char *why(web_flash_ctx_t c, web_flash_action_t a, web_err_t *err)
{
    return web_flash_action_why(&c, a, err);
}

TEST_CASE("receiver update actions are refused as the contract's table says", "[web_build]")
{
    web_err_t e;
    web_flash_ctx_t c = { .journal = WEB_JOURNAL_NONE, .station_running = true };
    TEST_ASSERT_EQUAL_STRING("no_journal", why(c, WEB_FLASH_RETRY, &e));
    TEST_ASSERT_EQUAL(WEB_ERR_NO_JOURNAL, e);
    TEST_ASSERT_EQUAL_STRING("no_journal", why(c, WEB_FLASH_CLEAR, &e));
    TEST_ASSERT_EQUAL_STRING("no_journal", why(c, WEB_FLASH_RESTART_RECEIVER, &e));
    c.receiver_in_loader = true;   /* found at its loader without a session */
    TEST_ASSERT_NULL(why(c, WEB_FLASH_RESTART_RECEIVER, &e));

    c = (web_flash_ctx_t){ .journal = WEB_JOURNAL_RUNNING, .station_running = true, .flash_available = true };
    for (int a = 0; a < 3; a++) {
        TEST_ASSERT_EQUAL_STRING("not_waiting", why(c, (web_flash_action_t)a, &e));
        TEST_ASSERT_EQUAL(WEB_ERR_NOT_WAITING, e);
    }

    c = (web_flash_ctx_t){ .journal = WEB_JOURNAL_WAITING, .station_running = true, .flash_available = true };
    TEST_ASSERT_NULL(why(c, WEB_FLASH_RETRY, &e));
    TEST_ASSERT_NULL(why(c, WEB_FLASH_RESTART_RECEIVER, &e));
    TEST_ASSERT_NULL(why(c, WEB_FLASH_CLEAR, &e));
    c.app_trial = true;
    TEST_ASSERT_EQUAL_STRING("app_trial", why(c, WEB_FLASH_RETRY, &e));
    TEST_ASSERT_NULL(why(c, WEB_FLASH_RESTART_RECEIVER, &e));
    c.app_trial = false;
    c.app_update = true;
    TEST_ASSERT_EQUAL_STRING("app_update", why(c, WEB_FLASH_RETRY, &e));
    TEST_ASSERT_EQUAL(WEB_ERR_APP_UPDATE, e);
    TEST_ASSERT_NULL(why(c, WEB_FLASH_CLEAR, &e));
    c.app_update = false;
    c.safe_mode = true;
    TEST_ASSERT_EQUAL_STRING("safe_mode", why(c, WEB_FLASH_RETRY, &e));
    TEST_ASSERT_EQUAL_STRING("safe_mode", why(c, WEB_FLASH_RESTART_RECEIVER, &e));
    TEST_ASSERT_NULL(why(c, WEB_FLASH_CLEAR, &e));   /* only clear in safe mode */
    c.safe_mode = false;
    c.flash_available = false;   /* receiver flashing not built yet */
    TEST_ASSERT_EQUAL_STRING("not_available_yet", why(c, WEB_FLASH_RETRY, &e));
    TEST_ASSERT_EQUAL(WEB_ERR_NOT_AVAILABLE_YET, e);
    TEST_ASSERT_EQUAL(501, web_err_status(e));
    TEST_ASSERT_NULL(why(c, WEB_FLASH_CLEAR, &e));

    c = (web_flash_ctx_t){ .journal = WEB_JOURNAL_UNREADABLE, .station_running = true, .flash_available = true };
    TEST_ASSERT_NULL(why(c, WEB_FLASH_CLEAR, &e));
    TEST_ASSERT_NOT_NULL(why(c, WEB_FLASH_RETRY, &e));
    c.journal = WEB_JOURNAL_UNKNOWN;
    TEST_ASSERT_EQUAL_STRING("settings_unavailable", why(c, WEB_FLASH_CLEAR, &e));
    TEST_ASSERT_EQUAL(WEB_ERR_SETTINGS_UNAVAILABLE, e);
}

TEST_CASE("flash view: record only for an open readable journal, actions listed", "[web_build]")
{
    station_status_t st;
    memset(&st, 0, sizeof(st));
    st.flash_journal_open = true;
    st.flash_journal_readable = true;
    st.flash_journal.state = UM98X_JOURNAL_NEEDS_USER;
    st.flash_journal.phase = UM98X_JOURNAL_TRANSFERRING;
    st.flash_journal.failure_class = UM98X_FLASH_FAIL_TRANSPORT;
    st.flash.phase = UM98X_FLASH_NEEDS_USER;
    st.flash.last_err = ESP_ERR_TIMEOUT;
    web_flash_ctx_t c = { .journal = web_journal_view(&st, false), .station_running = true };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_flash(&w, &st, &c, "text");
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL_STRING("waiting", at(root, "journal")->valuestring);
    const cJSON *rec = at(root, "record");
    TEST_ASSERT_EQUAL_STRING("needs_user", at(rec, "state")->valuestring);
    TEST_ASSERT_EQUAL_STRING("transferring", at(rec, "phase")->valuestring);
    TEST_ASSERT_EQUAL_STRING("transport", at(rec, "failure")->valuestring);
    TEST_ASSERT_EQUAL(64, (int)strlen(at(rec, "sha256")->valuestring));
    TEST_ASSERT_EQUAL_STRING("ESP_ERR_TIMEOUT", at(at(root, "progress"), "last_err")->valuestring);
    const cJSON *acts = at(root, "actions");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(at(acts, "retry"), "allowed")));
    TEST_ASSERT_EQUAL_STRING("not_available_yet", at(at(acts, "retry"), "why")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(at(acts, "clear"), "allowed")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(acts, "clear"), "why")));
    cJSON_Delete(root);

    st.flash_journal_readable = false;
    c.journal = web_journal_view(&st, false);
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_flash(&w, &st, &c, "");
    root = cJSON_Parse(s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "record")));
    cJSON_Delete(root);
}

TEST_CASE("flash view and progress: counters, the last session, and why flashing is off", "[web_build]")
{
    station_status_t st;
    memset(&st, 0, sizeof(st));
    st.flash.phase = UM98X_FLASH_TRANSFERRING;
    st.flash.bytes_sent = 4096;
    st.flash.bytes_total = 3027628;
    st.flash.frames_acked = 4;
    st.flash.frames_total = 2957;
    st.flash.frames_sent = 5;
    st.flash.entry_attempts = 1;
    st.flash.journal_state = UM98X_JOURNAL_TRANSFERRING;
    st.flash_last.outcome = STATION_FLASH_OUT_REJECTED;
    st.flash_last.failed_rule = 8;
    st.flash_last.ended_age_s = UINT32_MAX;
    strcpy(st.flash_last.reason, "package refused by rule 8");
    web_flash_ctx_t c = { .journal = WEB_JOURNAL_NONE, .station_running = true, .flash_available = false };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_flash(&w, &st, &c, "");
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    const cJSON *pr = at(root, "progress");
    TEST_ASSERT_EQUAL(4, at(pr, "frames_acked")->valueint);
    TEST_ASSERT_EQUAL(2957, at(pr, "frames_total")->valueint);
    TEST_ASSERT_EQUAL(5, at(pr, "frames_sent")->valueint);
    TEST_ASSERT_EQUAL_STRING("transferring", at(pr, "journal_state")->valuestring);
    const cJSON *last = at(root, "last");
    TEST_ASSERT_EQUAL_STRING("rejected", at(last, "outcome")->valuestring);
    TEST_ASSERT_EQUAL(8, at(last, "failed_rule")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(last, "ended_age_s")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "flash_available")));
    TEST_ASSERT_EQUAL_STRING("not_enabled_in_build", at(root, "flash_reason")->valuestring);
    cJSON_Delete(root);

    st.flash_last.outcome = STATION_FLASH_OUT_NONE;
    st.flash.journal_state = 0;
    c.flash_available = true;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_flash(&w, &st, &c, "");
    root = cJSON_Parse(s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "last")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "flash_reason")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "progress"), "journal_state")));
    cJSON_Delete(root);

    /* The final progress message carries the outcome. */
    station_flash_last_t l;
    memset(&l, 0, sizeof(l));
    l.outcome = STATION_FLASH_OUT_SUCCESS;
    l.build = 19152;
    strcpy(l.firmware, "R4.10Build19152");
    web_progress_t p = { .job = "gnss_flash", .phase = "done", .pct = 100, .bytes = -1, .done = true,
                         .result = web_flash_outcome_result(l.outcome), .flash = &st.flash, .last = &l };
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_progress(&w, &p);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL_STRING("ok", at(root, "result")->valuestring);
    TEST_ASSERT_EQUAL_STRING("success", at(root, "outcome")->valuestring);
    TEST_ASSERT_EQUAL(19152, at(root, "build")->valueint);
    TEST_ASSERT_EQUAL(2957, at(root, "frames_total")->valueint);
    cJSON_Delete(root);
    TEST_ASSERT_EQUAL_STRING("package_rejected", web_flash_outcome_result(STATION_FLASH_OUT_REJECTED));
    TEST_ASSERT_EQUAL_STRING("old_firmware", web_flash_outcome_result(STATION_FLASH_OUT_OLD_FIRMWARE));
    TEST_ASSERT_EQUAL_STRING("ok", web_flash_outcome_result(STATION_FLASH_OUT_CHECKED));
    TEST_ASSERT_EQUAL_STRING("receiver", web_flash_failure_name(UM98X_FLASH_FAIL_RECEIVER));
}

TEST_CASE("GET /api/update lists every version rolled back here, newest first", "[web_build]")
{
    fixture();
    s_snap.fw.latest_rolled_back = true;
    strcpy(s_snap.fw.reverted_version, "v0.4.0");
    static const char list[2][32] = { "v0.4.0", "v0.3.1" };
    web_update_snap_t u = { .fw = &s_snap.fw, .st = &s_snap.st, .rolled_back = list, .rolled_back_count = 2 };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_update(&w, &u);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    const cJSON *app = at(root, "app");
    const cJSON *rb = at(app, "rolled_back");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(rb));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", cJSON_GetArrayItem(rb, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("v0.3.1", cJSON_GetArrayItem(rb, 1)->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(app, "latest_rolled_back")));
    TEST_ASSERT_EQUAL_STRING("v0.4.0", at(app, "reverted")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "release_repo")));   /* none given */
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "signed_manifests")));
    cJSON_Delete(root);

    u.release_repo = "Averyy/onocoydiy-esp-um982";
    u.signed_manifests = true;
    u.checks_enabled = true;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_update(&w, &u);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL_STRING("Averyy/onocoydiy-esp-um982", at(root, "release_repo")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "signed_manifests")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "checks_enabled")));
    cJSON_Delete(root);

    u.rolled_back = NULL;   /* none: an empty list, never null */
    u.rolled_back_count = 0;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_update(&w, &u);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(at(at(root, "app"), "rolled_back")));
    cJSON_Delete(root);
}

TEST_CASE("update job results map the esp_err_t", "[web_build]")
{
    TEST_ASSERT_EQUAL_STRING("ok", web_result_code("check", ESP_OK));
    TEST_ASSERT_EQUAL_STRING("not_available_yet", web_result_code("gnss_update", ESP_ERR_NOT_SUPPORTED));
    TEST_ASSERT_EQUAL_STRING("failed", web_result_code("check", ESP_ERR_NOT_SUPPORTED));
    TEST_ASSERT_EQUAL_STRING("signature_failed", web_result_code("check", ESP_ERR_INVALID_CRC));
    TEST_ASSERT_EQUAL_STRING("checksum_mismatch", web_result_code("app_update", ESP_ERR_INVALID_CRC));
    TEST_ASSERT_EQUAL_STRING("size_mismatch", web_result_code("gnss_update", ESP_ERR_INVALID_SIZE));
    TEST_ASSERT_EQUAL_STRING("refused", web_result_code("gnss_update", ESP_ERR_INVALID_STATE));
    TEST_ASSERT_EQUAL_STRING("download_failed", web_result_code("app_update", ESP_ERR_TIMEOUT));
    TEST_ASSERT_EQUAL_STRING("invalid_image", web_result_code("app_update", ESP_ERR_INVALID_VERSION));
    TEST_ASSERT_EQUAL_STRING("invalid_image", web_result_code("app_upload", 0x1503));
    TEST_ASSERT_EQUAL_STRING("package_rejected", web_result_code("gnss_flash", ESP_ERR_INVALID_VERSION));
    TEST_ASSERT_EQUAL_STRING("failed", web_result_code("gnss_upload", ESP_ERR_NO_MEM));
    /* A redirect off the repository's releases on GitHub's release hosts, on any download job. */
    TEST_ASSERT_EQUAL_STRING("untrusted_host", web_result_code("check", ESP_ERR_NOT_ALLOWED));
    TEST_ASSERT_EQUAL_STRING("untrusted_host", web_result_code("app_update", ESP_ERR_NOT_ALLOWED));
    TEST_ASSERT_EQUAL_STRING("untrusted_host", web_result_code("gnss_update", ESP_ERR_NOT_ALLOWED));
    TEST_ASSERT_EQUAL_STRING("failed", web_result_code("app_upload", ESP_ERR_NOT_ALLOWED));
}

TEST_CASE("signals: per-band CNR null when unknown, messages merged with the profile", "[web_build]")
{
    gnss_obs_summary_t sum;
    memset(&sum, 0, sizeof(sum));
    sum.sys[GNSS_SYS_GALILEO].present = true;
    sum.sys[GNSS_SYS_GALILEO].satellites = 9;
    sum.sys[GNSS_SYS_GALILEO].band_mask = (1u << GNSS_BAND_L1) | (1u << GNSS_BAND_L6);
    sum.sys[GNSS_SYS_GALILEO].signal_mask = (1u << 1) | (1u << 14);
    sum.sys[GNSS_SYS_GALILEO].cnr_dbhz[GNSS_BAND_L1] = 44;
    sum.best_sys = GNSS_SYS_GALILEO;
    sum.best_bands = 2;
    sum.epoch_age_ms = UINT32_MAX;
    um98x_base_profile_t p;
    memset(&p, 0, sizeof(p));
    p.rtcm_count = 2;
    p.rtcm[0] = (um98x_rtcm_out_t){ 1005, 1 };
    p.rtcm[1] = (um98x_rtcm_out_t){ 1097, 1 };

    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_signals(&w, &sum, NULL, 0, &p, true);
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "messages")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "epoch_age_ms")));
    const cJSON *gal = cJSON_GetArrayItem(at(root, "systems"), 0);
    TEST_ASSERT_EQUAL_STRING("galileo", at(gal, "sys")->valuestring);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(at(gal, "bands")));
    TEST_ASSERT_EQUAL(2, cJSON_GetArrayItem(at(gal, "signal_ids"), 0)->valueint);
    TEST_ASSERT_EQUAL(15, cJSON_GetArrayItem(at(gal, "signal_ids"), 1)->valueint);
    TEST_ASSERT_EQUAL(44, at(at(gal, "cnr_dbhz"), "l1")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(gal, "cnr_dbhz"), "l6")));
    cJSON_Delete(root);

    gnss_obs_msg_stat_t stats[2] = {
        { .msg = 1097, .count = 10, .period_ms = 1000, .age_ms = 400 },
        { .msg = 1230, .count = 3, .period_ms = 10000, .age_ms = UINT32_MAX },
    };
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_signals(&w, &sum, stats, 2, &p, true);
    root = cJSON_Parse(s_out);
    const cJSON *msgs = at(root, "messages");
    TEST_ASSERT_EQUAL(3, cJSON_GetArraySize(msgs));
    TEST_ASSERT_EQUAL(1005, at(cJSON_GetArrayItem(msgs, 0), "msg")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(cJSON_GetArrayItem(msgs, 0), "period_ms")));
    TEST_ASSERT_EQUAL(1, at(cJSON_GetArrayItem(msgs, 0), "expected_period_s")->valueint);
    TEST_ASSERT_EQUAL(10, at(cJSON_GetArrayItem(msgs, 1), "count")->valueint);
    TEST_ASSERT_EQUAL(1230, at(cJSON_GetArrayItem(msgs, 2), "msg")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(cJSON_GetArrayItem(msgs, 2), "expected_period_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(cJSON_GetArrayItem(msgs, 2), "age_ms")));
    cJSON_Delete(root);
}

TEST_CASE("error body and progress messages", "[web_build]")
{
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_error(&w, WEB_ERR_RATE_LIMITED, NULL, NULL, 30, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("{\"error\":{\"code\":\"rate_limited\",\"message\":\"Too many attempts, try again later\","
                             "\"field\":null,\"retry_in_s\":30,\"by\":null}}",
                             s_out);
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_error(&w, WEB_ERR_CASTER_POLICY, NULL, NULL, UINT32_MAX, NULL, "second_onocoy");
    cJSON *root = cJSON_Parse(s_out);
    const cJSON *e = at(root, "error");
    TEST_ASSERT_EQUAL_STRING("caster_policy", at(e, "code")->valuestring);
    TEST_ASSERT_EQUAL_STRING("second_onocoy", at(e, "reason")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(e, "retry_in_s")));
    cJSON_Delete(root);

    web_progress_t p = { .job = "gnss_update", .phase = "downloading", .pct = 42, .bytes = -1 };
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_progress(&w, &p);
    TEST_ASSERT_EQUAL_STRING(
        "{\"type\":\"progress\",\"job\":\"gnss_update\",\"phase\":\"downloading\",\"pct\":42,\"done\":false}", s_out);
    p.done = true;
    p.err = ESP_ERR_NOT_SUPPORTED;
    p.pct = -1;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_progress(&w, &p);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_EQUAL_STRING("not_available_yet", at(root, "result")->valuestring);
    TEST_ASSERT_EQUAL_STRING("ESP_ERR_NOT_SUPPORTED", at(root, "message")->valuestring);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(root, "pct"));
    cJSON_Delete(root);
}

TEST_CASE("availability days carry dates and per-slot arrays", "[web_build]")
{
    ledger_day_t today;
    memset(&today, 0, sizeof(today));
    today.utc_day = 20736;
    today.down_s[0] = 4;
    today.up_s[0] = 36000;
    ntrip_caster_cfg_t cfg[2];
    memset(cfg, 0, sizeof(cfg));
    strcpy(cfg[0].name, "Onocoy");
    cfg[0].protected_slot = true;
    ntrip_caster_status_t pst;
    memset(&pst, 0, sizeof(pst));
    pst.data_rate_permille = 1000;
    web_avail_snap_t a = {
        .clock = LEDGER_CLOCK_NONE,
        .second_of_day = -1,
        .slots = 2,
        .cfg = cfg,
        .protected_slot = 0,
        .protected_status = &pst,
        .down_today_s = 4,
        .budget_left_s = UINT32_MAX,
        .today_valid = true,
        .today = &today,
        .history = &today,
        .history_count = 1,
    };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_availability(&w, &a);
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL_STRING("none", at(root, "clock")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "utc_second_of_day")));
    TEST_ASSERT_EQUAL_STRING("2026-10-10", at(at(root, "today"), "date")->valuestring);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(at(at(root, "today"), "down_s")));
    TEST_ASSERT_EQUAL(36000, cJSON_GetArrayItem(at(at(root, "today"), "up_s"), 0)->valueint);
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(at(at(root, "today"), "up_s")));
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(at(root, "history")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "protected"), "budget_left_s")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(cJSON_GetArrayItem(at(root, "slots"), 1), "protected")));
    cJSON_Delete(root);
}

TEST_CASE("journal entries use the event names", "[web_build]")
{
    diag_journal_entry_t e[2] = {
        { .seq = 77, .unix_s = 1760000000, .uptime_s = 3, .kind = DIAG_EV_BOOT, .a = 1, .b = 4 },
        { .seq = 78, .unix_s = 0, .uptime_s = 4, .kind = DIAG_EV_SESSION, .a = -1, .b = 3 },
    };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_journal(&w, e, 2, 78, false);
    cJSON *root = cJSON_Parse(s_out);
    const cJSON *list = at(root, "entries");
    TEST_ASSERT_EQUAL_STRING("boot", at(cJSON_GetArrayItem(list, 0), "kind")->valuestring);
    TEST_ASSERT_EQUAL_STRING("session", at(cJSON_GetArrayItem(list, 1), "kind")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(cJSON_GetArrayItem(list, 1), "time_unix")));
    TEST_ASSERT_EQUAL(-1, at(cJSON_GetArrayItem(list, 1), "a")->valueint);
    cJSON_Delete(root);
}

TEST_CASE("journal names cover the connection-history and receiver kinds; the gap is unknown", "[web_build]")
{
    diag_journal_entry_t e[6] = {
        { .seq = 1, .kind = DIAG_EV_CASTER_DOWN, .a = diag_caster_a(0, 5, 2), .b = 30 },
        { .seq = 2, .kind = DIAG_EV_CASTER_UP, .a = diag_caster_a(1, 0, 3), .b = 12 },
        { .seq = 3, .kind = DIAG_EV_WIFI_UP, .a = 4, .b = 90 },
        { .seq = 4, .kind = DIAG_EV_RX_MAINT, .a = 1, .b = 1800 },
        { .seq = 5, .kind = DIAG_EV_LIVE_VIEW, .a = DIAG_LIVE_OPENED, .b = 600 },
        { .seq = 6, .kind = 15, .a = 0, .b = 0 },   /* between the two groups: never written */
    };
    static const char *const want[] = { "caster_down", "caster_up", "wifi_up", "rx_maintenance", "live_view", "unknown" };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_journal(&w, e, 6, 6, false);
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    const cJSON *list = at(root, "entries");
    TEST_ASSERT_EQUAL(6, cJSON_GetArraySize(list));
    for (int i = 0; i < 6; i++) {
        TEST_ASSERT_EQUAL_STRING(want[i], at(cJSON_GetArrayItem(list, i), "kind")->valuestring);
    }
    TEST_ASSERT_EQUAL(0 | (5 << 8) | (2 << 16), at(cJSON_GetArrayItem(list, 0), "a")->valueint);
    cJSON_Delete(root);

    /* Stored in the journal: the numbers keep their meaning. */
    TEST_ASSERT_EQUAL(10, DIAG_EV_SESSION);
    TEST_ASSERT_EQUAL(11, DIAG_EV_CASTER_DOWN);
    TEST_ASSERT_EQUAL(12, DIAG_EV_CASTER_UP);
    TEST_ASSERT_EQUAL(13, DIAG_EV_WIFI_UP);
    TEST_ASSERT_EQUAL(20, DIAG_EV_RX_MAINT);
    TEST_ASSERT_EQUAL(21, DIAG_EV_LIVE_VIEW);
}

TEST_CASE("the power-lost alert and the new network ladder reasons have API names", "[web_build]")
{
    TEST_ASSERT_EQUAL_STRING("power_lost", web_health_alert_name(HEALTH_ALERT_POWER_LOST));
    TEST_ASSERT_EQUAL_STRING("link_loss", web_health_alert_name(HEALTH_ALERT_LINK_LOSS));
    TEST_ASSERT_EQUAL_STRING("no_reboot_trigger", web_ladder_why_name(NETWORK_LADDER_WHY_NO_REBOOT_TRIGGER));
    TEST_ASSERT_EQUAL_STRING("ap_overridden", web_ladder_why_name(NETWORK_LADDER_WHY_AP_OVERRIDDEN));
    TEST_ASSERT_EQUAL_STRING("unknown", web_ladder_why_name(NETWORK_LADDER_WHY_AP_OVERRIDDEN + 1));
}

TEST_CASE("signals: epoch gaps only against a configured period; frames per message", "[web_build]")
{
    gnss_obs_summary_t sum;
    memset(&sum, 0, sizeof(sum));
    sum.epoch_age_ms = 420;
    sum.epochs = 100;
    sum.epoch_gaps = 3;
    sum.epoch_jumps = 1;
    um98x_base_profile_t p;
    memset(&p, 0, sizeof(p));
    p.rtcm_count = 1;
    p.rtcm[0] = (um98x_rtcm_out_t){ 1077, 1 };

    /* No MSM period configured: gaps are not counted, so they are null. */
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_signals(&w, &sum, NULL, 0, &p, true);
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "epoch_gaps")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "epoch_period_ms")));
    TEST_ASSERT_EQUAL(1, at(root, "epoch_jumps")->valueint);
    cJSON_Delete(root);

    sum.epoch_period_ms = 1000;
    gnss_obs_msg_stat_t stats[1] = {
        { .msg = 1077, .expected_period_s = 1, .count = 100, .frames = 200, .period_ms = 1000, .age_ms = 300 },
    };
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_signals(&w, &sum, stats, 1, &p, true);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL(3, at(root, "epoch_gaps")->valueint);
    TEST_ASSERT_EQUAL(1000, at(root, "epoch_period_ms")->valueint);
    const cJSON *m = cJSON_GetArrayItem(at(root, "messages"), 0);
    TEST_ASSERT_EQUAL(100, at(m, "count")->valueint);
    TEST_ASSERT_EQUAL(200, at(m, "frames")->valueint);
    cJSON_Delete(root);
}

TEST_CASE("sky: QZSS and SBAS show their PRN, GLONASS its slot", "[web_build]")
{
    TEST_ASSERT_EQUAL_UINT(193, web_sky_prn(GNSS_SYS_QZSS, 1));
    TEST_ASSERT_EQUAL_UINT(120, web_sky_prn(GNSS_SYS_SBAS, 1));
    TEST_ASSERT_EQUAL_UINT(12, web_sky_prn(GNSS_SYS_GPS, 12));
    TEST_ASSERT_EQUAL_UINT(7, web_sky_prn(GNSS_SYS_GLONASS, 7));

    gnss_obs_sat_t sats[2] = {
        { .sys = GNSS_SYS_QZSS, .prn = 2, .elevation_deg = 40, .azimuth_deg = 180, .cnr_dbhz = 0 },
        { .sys = GNSS_SYS_SBAS, .prn = 12, .elevation_deg = 30, .azimuth_deg = 200, .cnr_dbhz = 41 },
    };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_sky(&w, sats, 2, NULL);
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "available")));
    const cJSON *list = at(root, "satellites");
    TEST_ASSERT_EQUAL(194, at(cJSON_GetArrayItem(list, 0), "prn")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(cJSON_GetArrayItem(list, 0), "cnr_dbhz")));
    TEST_ASSERT_EQUAL(131, at(cJSON_GetArrayItem(list, 1), "prn")->valueint);
    cJSON_Delete(root);

    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_sky(&w, sats, 2, "no_data");
    root = cJSON_Parse(s_out);
    TEST_ASSERT_FALSE(cJSON_IsTrue(at(root, "available")));
    TEST_ASSERT_EQUAL_STRING("no_data", at(root, "reason")->valuestring);
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(at(root, "satellites")));
    cJSON_Delete(root);
}

TEST_CASE("health: retry, refused factory reset, journal and log counters", "[web_build]")
{
    static health_status_t hs;
    static lifecycle_boot_info_t boot;
    static gnss_link_stats_t link;
    static rtcm_hub_stats_t hub;
    static station_status_t st;
    static gnss_obs_summary_t obs;
    static network_status_t net;
    static diag_coredump_info_t cd;
    static diag_heap_info_t heap;
    static diag_journal_info_t journal;
    static diag_log_stats_t log;
    memset(&hs, 0, sizeof(hs));
    hs.factory_reset_refused_age_s = UINT32_MAX;
    hs.rtcm_gap_ms = UINT32_MAX;
    obs.epoch_period_ms = 1000;
    obs.epoch_gaps = 2;
    heap.psram_min = 1234;
    journal.available = true;
    journal.capacity = 4064;
    journal.newest_seq = 77;
    log.lines = 812;
    log.prev_boot_available = true;
    web_health_snap_t h = {
        .health = &hs, .boot = &boot, .link = &link, .hub = &hub, .st = &st, .obs = &obs, .net = &net,
        .coredump = &cd, .heap = &heap, .journal = &journal, .log = &log,
    };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_health(&w, &h);
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "gnss_retry_in_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "factory_reset_refused_age_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "rtcm_gap_ms")));
    TEST_ASSERT_EQUAL(1234, at(at(root, "heap"), "psram_min")->valueint);
    TEST_ASSERT_EQUAL(2, at(at(root, "obs"), "epoch_gaps")->valueint);
    TEST_ASSERT_EQUAL(4064, at(at(root, "journal"), "capacity")->valueint);
    TEST_ASSERT_EQUAL(812, at(at(root, "log"), "lines")->valueint);
    cJSON_Delete(root);

    hs.gnss_retry_in_s = 420;
    hs.factory_reset_refused_age_s = 30;
    hs.admin_resets = 1;
    h.journal = NULL;
    h.log = NULL;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_health(&w, &h);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL(420, at(root, "gnss_retry_in_s")->valueint);
    TEST_ASSERT_EQUAL(30, at(root, "factory_reset_refused_age_s")->valueint);
    TEST_ASSERT_EQUAL(1, at(root, "admin_resets")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "journal")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "log")));
    cJSON_Delete(root);
}

TEST_CASE("status: a refused factory reset shows for 10 minutes, admin form only", "[web_build]")
{
    fixture();
    char frag[512];
    wj_t w;
    s_snap.health.factory_reset_refused_age_s = 30;
    wj_init_members(&w, frag, sizeof(frag));
    web_build_section(&w, &s_snap, WEB_SEC_HEALTH, false);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(wj_text(&w), "\"factory_reset_refused\":true"), wj_text(&w));
    s_snap.health.factory_reset_refused_age_s = WEB_FACTORY_REFUSED_SHOW_S;
    wj_init_members(&w, frag, sizeof(frag));
    web_build_section(&w, &s_snap, WEB_SEC_HEALTH, false);
    TEST_ASSERT_NOT_NULL(strstr(wj_text(&w), "\"factory_reset_refused\":false"));
    s_snap.health.factory_reset_refused_age_s = UINT32_MAX;
    wj_init_members(&w, frag, sizeof(frag));
    web_build_section(&w, &s_snap, WEB_SEC_HEALTH, false);
    TEST_ASSERT_NOT_NULL(strstr(wj_text(&w), "\"factory_reset_refused\":false"));
    wj_init_members(&w, frag, sizeof(frag));
    web_build_section(&w, &s_snap, WEB_SEC_HEALTH, true);
    TEST_ASSERT_NULL(strstr(wj_text(&w), "factory_reset_refused"));
}

/* ---- session, Wi-Fi and the restart hold ---------------------------------------------- */

static cJSON *build_session(const web_session_snap_t *s)
{
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_session(&w, s);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    return root;
}

TEST_CASE("session: names and the stored setup step", "[web_build]")
{
    web_session_snap_t s = {
        .authenticated = true, .admin_set = true, .setup_done = false, .claim_allowed = false, .on_setup_ap = false,
        .public_status = false, .settings_available = true, .idle_left_s = 1740, .hostname = "onocoydiy",
        .mdns_host = "onocoydiy-2", .ap_ssid = "onocoydiy-1A2B", .setup_step = 3,
    };
    cJSON *root = build_session(&s);
    TEST_ASSERT_EQUAL(1, at(root, "api")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "authenticated")));
    TEST_ASSERT_EQUAL(1740, at(root, "idle_left_s")->valueint);
    TEST_ASSERT_EQUAL_STRING("onocoydiy", at(root, "hostname")->valuestring);
    TEST_ASSERT_EQUAL_STRING("onocoydiy-2", at(root, "mdns_host")->valuestring);
    TEST_ASSERT_EQUAL_STRING("onocoydiy-1A2B", at(root, "ap_ssid")->valuestring);
    TEST_ASSERT_EQUAL_STRING("onocoy", at(root, "setup_step")->valuestring);
    cJSON_Delete(root);

    /* every stored step by name; nothing stored or out of range: null */
    static const char *const names[] = { NULL, "wifi", "reception", "onocoy", "casters", NULL, NULL };
    for (uint8_t v = 0; v < sizeof(names) / sizeof(names[0]); v++) {
        s.setup_step = v == 6 ? 255 : v;
        root = build_session(&s);
        const cJSON *it = at(root, "setup_step");
        if (names[v] == NULL) {
            TEST_ASSERT_TRUE_MESSAGE(cJSON_IsNull(it), s_out);
        } else {
            TEST_ASSERT_EQUAL_STRING(names[v], it->valuestring);
        }
        cJSON_Delete(root);
    }

    /* setup done: no step, whatever is stored */
    s.setup_step = 2;
    s.setup_done = true;
    root = build_session(&s);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "setup_step")));
    cJSON_Delete(root);
}

TEST_CASE("session: without a session no step, the names stay; mDNS off is null", "[web_build]")
{
    web_session_snap_t s = {
        .authenticated = false, .admin_set = true, .setup_done = false, .settings_available = true,
        .hostname = "onocoydiy", .mdns_host = "", .ap_ssid = "", .setup_step = 3, .idle_left_s = 99,
    };
    cJSON *root = build_session(&s);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "idle_left_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "setup_step")));
    TEST_ASSERT_EQUAL_STRING("onocoydiy", at(root, "hostname")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "mdns_host")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "ap_ssid")));
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "claim_allowed")));
    cJSON_Delete(root);
}

static cJSON *build_wifi(const network_status_t *n)
{
    web_wifi_snap_t ws = { .net = n, .scan_age_s = UINT32_MAX };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_wifi(&w, &ws);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    return root;
}

TEST_CASE("wifi: mDNS name, the AP's countdown and why credentials were reverted", "[web_build]")
{
    static network_status_t n;
    memset(&n, 0, sizeof(n));
    n.sta_configured = true;
    n.online = true;
    strcpy(n.ssid, "Home");
    strcpy(n.ip, "192.168.1.50");
    strcpy(n.ap_ssid, "onocoydiy-1A2B");
    strcpy(n.mdns_host, "onocoydiy");
    n.ap_close_in_s = UINT32_MAX;
    cJSON *root = build_wifi(&n);
    TEST_ASSERT_EQUAL_STRING("onocoydiy", at(root, "mdns_host")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "ap"), "close_in_s")));   /* AP down */
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "cred_reverted_why")));
    cJSON_Delete(root);

    /* AP up, lingering: counts down */
    n.setup_ap_active = true;
    n.ap_reasons = NETWORK_AP_LINGER;
    n.ap_close_in_s = 12;
    root = build_wifi(&n);
    TEST_ASSERT_EQUAL(12, at(at(root, "ap"), "close_in_s")->valueint);
    cJSON_Delete(root);

    /* AP up for setup: nothing scheduled */
    n.ap_reasons = NETWORK_AP_SETUP;
    n.ap_close_in_s = UINT32_MAX;
    n.mdns_host[0] = '\0';
    root = build_wifi(&n);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "ap"), "close_in_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "mdns_host")));
    cJSON_Delete(root);

    /* a refused password sent the station back */
    n.cred_pending = true;
    n.cred_reverted = true;
    n.cred_reverted_why = NETWORK_CRED_REVERT_AUTH;
    strcpy(n.pending_ssid, "Home");
    root = build_wifi(&n);
    TEST_ASSERT_EQUAL_STRING("auth_failed", at(root, "cred_reverted_why")->valuestring);
    cJSON_Delete(root);
    n.cred_reverted_why = NETWORK_CRED_REVERT_NO_IP;
    root = build_wifi(&n);
    TEST_ASSERT_EQUAL_STRING("no_ip", at(root, "cred_reverted_why")->valuestring);
    cJSON_Delete(root);
    n.cred_reverted = false;   /* the reason only counts while reverted */
    root = build_wifi(&n);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "cred_reverted_why")));
    cJSON_Delete(root);
}

TEST_CASE("status: network carries the mDNS name and the AP's countdown, admin form only", "[web_build]")
{
    fixture();
    strcpy(s_snap.net.mdns_host, "onocoydiy");
    s_snap.net.setup_ap_active = true;
    s_snap.net.ap_reasons = NETWORK_AP_LINGER;
    s_snap.net.ap_close_in_s = 9;
    cJSON *root = build_status(false);
    const cJSON *net = at(root, "network");
    TEST_ASSERT_EQUAL_STRING("onocoydiy", at(net, "mdns_host")->valuestring);
    TEST_ASSERT_EQUAL(9, at(net, "ap_close_in_s")->valueint);
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", at(net, "ip")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(net, "cred_reverted_why")));
    TEST_ASSERT_EQUAL(0, at(net, "probe_age_s")->valueint);   /* the fixture's 0 s */
    cJSON_Delete(root);
    s_snap.net.probe_age_s = 659;   /* whole minutes only */
    root = build_status(false);
    TEST_ASSERT_EQUAL(600, at(at(root, "network"), "probe_age_s")->valueint);
    cJSON_Delete(root);
    s_snap.net.probe_age_s = UINT32_MAX;
    root = build_status(false);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "network"), "probe_age_s")));
    cJSON_Delete(root);
    s_snap.net.setup_ap_active = false;
    root = build_status(false);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "network"), "ap_close_in_s")));
    cJSON_Delete(root);
    root = build_status(true);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(root, "network"));
    cJSON_Delete(root);
}

TEST_CASE("health: restart hold reasons, when the hold ends at the latest and the cap's wait", "[web_build]")
{
    fixture();
    s_snap.health.gnss_reboot_hold = HEALTH_HOLD_SETUP | HEALTH_HOLD_POWER_ON;
    s_snap.health.gnss_reboot_hold_left_s = UINT32_MAX;
    s_snap.health.gnss_reboot_cap_wait_s = UINT32_MAX;
    cJSON *root = build_status(false);
    const cJSON *hl = at(root, "health");
    const cJSON *hold = at(hl, "reboot_hold");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(hold));
    TEST_ASSERT_EQUAL_STRING("setup", cJSON_GetArrayItem(hold, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("power_on", cJSON_GetArrayItem(hold, 1)->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(hl, "reboot_hold_left_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(hl, "reboot_cap_wait_s")));
    cJSON_Delete(root);

    s_snap.health.gnss_reboot_hold = HEALTH_HOLD_AP_CLIENT | HEALTH_HOLD_ADMIN;
    s_snap.health.gnss_reboot_hold_left_s = 1200;
    s_snap.health.gnss_reboot_cap_wait_s = 3000;
    root = build_status(false);
    hl = at(root, "health");
    hold = at(hl, "reboot_hold");
    TEST_ASSERT_EQUAL(2, cJSON_GetArraySize(hold));
    TEST_ASSERT_EQUAL_STRING("ap_client", cJSON_GetArrayItem(hold, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("admin", cJSON_GetArrayItem(hold, 1)->valuestring);
    TEST_ASSERT_EQUAL(1200, at(hl, "reboot_hold_left_s")->valueint);
    TEST_ASSERT_EQUAL(3000, at(hl, "reboot_cap_wait_s")->valueint);
    cJSON_Delete(root);

    /* power_on alone: it ends with its grace, which the station names */
    s_snap.health.gnss_reboot_hold = HEALTH_HOLD_POWER_ON;
    s_snap.health.gnss_reboot_hold_left_s = 3120;
    root = build_status(false);
    TEST_ASSERT_EQUAL(3120, at(at(root, "health"), "reboot_hold_left_s")->valueint);
    cJSON_Delete(root);

    s_snap.health.gnss_reboot_hold = 0;
    s_snap.health.gnss_reboot_hold_left_s = UINT32_MAX;
    root = build_status(false);
    TEST_ASSERT_EQUAL(0, cJSON_GetArraySize(at(at(root, "health"), "reboot_hold")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "health"), "reboot_hold_left_s")));
    cJSON_Delete(root);
    root = build_status(true);
    TEST_ASSERT_NULL(cJSON_GetObjectItemCaseSensitive(at(root, "health"), "reboot_hold"));
    cJSON_Delete(root);

    /* GET /api/health: the same fields at the top level */
    static health_status_t hs;
    static lifecycle_boot_info_t boot;
    static gnss_link_stats_t link;
    static rtcm_hub_stats_t hub;
    static station_status_t st;
    static gnss_obs_summary_t obs;
    static network_status_t net;
    static diag_coredump_info_t cd;
    memset(&hs, 0, sizeof(hs));
    hs.gnss_reboot_hold = HEALTH_HOLD_ADMIN;
    hs.gnss_reboot_hold_left_s = 61;
    hs.gnss_reboot_cap_wait_s = UINT32_MAX;
    web_health_snap_t h = { .health = &hs, .boot = &boot, .link = &link, .hub = &hub, .st = &st, .obs = &obs,
                            .net = &net, .coredump = &cd };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_health(&w, &h);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL_STRING("admin", cJSON_GetArrayItem(at(root, "reboot_hold"), 0)->valuestring);
    TEST_ASSERT_EQUAL(61, at(root, "reboot_hold_left_s")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "reboot_cap_wait_s")));
    cJSON_Delete(root);
}

/* ---- no receiver, message 1230, update check, Onocoy set up ------------------------------------- */

TEST_CASE("status: no receiver is an alert with the bytes seen and the next search", "[web_build]")
{
    fixture();
    s_snap.st.state = STATION_STATE_NO_RECEIVER;
    s_snap.st.alerts = STATION_ALERT_NO_RECEIVER;
    s_snap.st.detect_saw_bytes = true;
    s_snap.st.next_detect_in_s = 27;
    s_snap.health.alerts = 0;
    cJSON *root = build_status(false);
    const cJSON *alerts = at(root, "alerts");
    TEST_ASSERT_EQUAL(1, cJSON_GetArraySize(alerts));
    const cJSON *a = cJSON_GetArrayItem(alerts, 0);
    TEST_ASSERT_EQUAL_STRING("station", at(a, "source")->valuestring);
    TEST_ASSERT_EQUAL_STRING("no_receiver", at(a, "id")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(a, "saw_bytes")));
    TEST_ASSERT_EQUAL(27, at(a, "next_detect_in_s")->valueint);
    TEST_ASSERT_EQUAL(27, at(at(root, "receiver"), "next_detect_in_s")->valueint);
    cJSON_Delete(root);

    s_snap.st.next_detect_in_s = UINT32_MAX;   /* nothing scheduled */
    s_snap.st.detect_saw_bytes = false;
    root = build_status(false);
    a = cJSON_GetArrayItem(at(root, "alerts"), 0);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(a, "saw_bytes")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(a, "next_detect_in_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "receiver"), "next_detect_in_s")));
    cJSON_Delete(root);
}

static cJSON *build_gnss_json(const station_status_t *st)
{
    static web_gnss_snap_t g;
    memset(&g, 0, sizeof(g));
    g.st = st;
    g.link.last_rx_age_ms = UINT32_MAX;
    g.link.last_rtcm_age_ms = UINT32_MAX;
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_gnss(&w, &g);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    return root;
}

TEST_CASE("receiver: message 1230 is null until it was ever applied", "[web_build]")
{
    fixture();
    station_status_t *st = &s_snap.st;
    st->rtcm1230_known = false;   /* gnss/rtcm1230_ok missing */
    st->rtcm1230_supported = false;
    cJSON *root = build_gnss_json(st);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "rtcm1230_supported")));
    cJSON_Delete(root);
    st->rtcm1230_known = true;    /* stored 0 */
    root = build_gnss_json(st);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "rtcm1230_supported")));
    cJSON_Delete(root);
    st->rtcm1230_supported = true;   /* stored 1 */
    root = build_gnss_json(st);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "rtcm1230_supported")));
    cJSON_Delete(root);

    /* The signals view says the same. */
    gnss_obs_summary_t sum;
    memset(&sum, 0, sizeof(sum));
    um98x_base_profile_t p;
    memset(&p, 0, sizeof(p));
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_signals(&w, &sum, NULL, 0, &p, -1);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "rtcm1230_supported")));
    cJSON_Delete(root);
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_signals(&w, &sum, NULL, 0, &p, 0);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "rtcm1230_supported")));
    cJSON_Delete(root);
}

TEST_CASE("GET /api/update: a failed check says why, when, and the app slot's size", "[web_build]")
{
    fixture();
    s_snap.fw.last_check_unix = 0;   /* never succeeded */
    s_snap.fw.last_attempt_unix = 1760090000;
    s_snap.fw.last_check_result = FW_CHECK_RESULT_NO_RELEASE;
    s_snap.fw.last_http_status = 404;
    s_snap.fw.next_check_in_s = 86000;
    static const uint8_t sha[32] = { 0xAB, 0xCD };
    web_update_snap_t u = { .fw = &s_snap.fw, .st = &s_snap.st, .app_max_bytes = 4194304, .running_elf_sha256 = sha };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_update(&w, &u);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_EQUAL_STRING("no_release", at(root, "last_check_result")->valuestring);
    TEST_ASSERT_EQUAL(404, at(root, "last_http_status")->valueint);
    TEST_ASSERT_EQUAL_DOUBLE(1760090000.0, at(root, "last_attempt_unix")->valuedouble);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "last_check_unix")));
    TEST_ASSERT_EQUAL(86000, at(root, "next_check_in_s")->valueint);
    TEST_ASSERT_EQUAL(4194304, at(root, "app_max_bytes")->valueint);
    TEST_ASSERT_EQUAL_STRING_LEN("abcd0000", at(root, "running_elf_sha256")->valuestring, 8);
    TEST_ASSERT_EQUAL(64, (int)strlen(at(root, "running_elf_sha256")->valuestring));
    cJSON_Delete(root);

    /* No check this boot: nothing to say. */
    s_snap.fw.last_attempt_unix = 0;
    s_snap.fw.last_check_result = FW_CHECK_RESULT_NONE;
    s_snap.fw.last_http_status = 0;
    s_snap.fw.next_check_in_s = UINT32_MAX;
    u.app_max_bytes = 0;
    u.running_elf_sha256 = NULL;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_update(&w, &u);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "last_check_result")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "last_http_status")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "last_attempt_unix")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "next_check_in_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "app_max_bytes")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(root, "running_elf_sha256")));
    cJSON_Delete(root);
}

TEST_CASE("Onocoy day figures are null unless the slot is enabled and set up", "[web_build]")
{
    fixture();
    /* Set up and enabled: the figures, the day so far, ready. */
    cJSON *root = build_status(false);
    const cJSON *c0 = cJSON_GetArrayItem(at(root, "casters"), 0);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(c0, "onocoy_ready")));
    TEST_ASSERT_EQUAL(1000, at(c0, "graced_uptime_so_far_permille")->valueint);
    TEST_ASSERT_EQUAL(1000, at(c0, "data_rate_permille")->valueint);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(cJSON_GetArrayItem(at(root, "casters"), 1), "onocoy_ready")));
    cJSON_Delete(root);

    /* Nothing sent today yet: no data rate. */
    s_snap.cst[0].data_rate_valid = false;
    root = build_status(false);
    c0 = cJSON_GetArrayItem(at(root, "casters"), 0);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c0, "data_rate_permille")));
    TEST_ASSERT_EQUAL(469, at(c0, "budget_left_s")->valueint);
    cJSON_Delete(root);

    /* Not set up (no username: stored before the console's three values): nothing, and no budget. */
    fixture();
    s_snap.cfg[0].username[0] = '\0';
    s_snap.cst[0].day_metrics_valid = false;
    root = build_status(false);
    c0 = cJSON_GetArrayItem(at(root, "casters"), 0);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(c0, "onocoy_ready")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c0, "graced_uptime_permille")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c0, "streak_ok")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c0, "budget_left_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c0, "budget_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c0, "data_rate_permille")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(c0, "graced_uptime_so_far_permille")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(at(root, "availability"), "budget_left_s")));
    cJSON_Delete(root);

    /* GET /api/casters: the entry says whether the slot is set up. */
    fixture();
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_caster_entry(&w, 0, &s_snap.cfg[0], &s_snap.cst[0]);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(root, "onocoy_ready")));
    TEST_ASSERT_EQUAL(1000, at(at(root, "status"), "graced_uptime_so_far_permille")->valueint);
    TEST_ASSERT_EQUAL(473, at(at(root, "status"), "budget_s")->valueint);
    cJSON_Delete(root);
    s_snap.cfg[0].password_set = false;   /* no password: not set up */
    s_snap.cfg[0].password[0] = '\0';
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_caster_entry(&w, 0, &s_snap.cfg[0], &s_snap.cst[0]);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(root, "onocoy_ready")));
    cJSON_Delete(root);

    /* Availability with the slot off: no percentages. */
    ledger_day_t today;
    memset(&today, 0, sizeof(today));
    today.utc_day = 20736;
    fixture();
    s_snap.cfg[0].enabled = false;
    s_snap.cst[0].day_metrics_valid = false;
    web_avail_snap_t a = {
        .clock = LEDGER_CLOCK_GNSS, .second_of_day = 3600, .slots = 2, .cfg = s_snap.cfg, .protected_slot = 0,
        .protected_status = &s_snap.cst[0], .down_today_s = 0, .budget_left_s = 473, .today_valid = true,
        .today = &today,
    };
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_availability(&w, &a);
    root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    const cJSON *pr = at(root, "protected");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(pr, "enabled")));
    TEST_ASSERT_TRUE(cJSON_IsTrue(at(pr, "onocoy_ready")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(pr, "graced_uptime_permille")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(pr, "data_rate_permille")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(pr, "streak_ok")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(pr, "budget_left_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(pr, "budget_s")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(pr, "graced_uptime_so_far_permille")));
    cJSON_Delete(root);
}

/* ---- the Onocoy login guard, the test Onocoy slot, the day's volume ------------------------------ */

TEST_CASE("caster guard names: the stopped state, the new classes and the stop reasons", "[web_build]")
{
    TEST_ASSERT_EQUAL_STRING("stopped", web_ntrip_state_name(NTRIP_STATE_STOPPED));
    TEST_ASSERT_EQUAL(10, NTRIP_STATE_STOPPED);
    TEST_ASSERT_EQUAL_STRING("login_blocked", web_ntrip_fail_name(NTRIP_FAIL_LOGIN_BLOCKED));
    TEST_ASSERT_EQUAL_STRING("in_use", web_ntrip_fail_name(NTRIP_FAIL_IN_USE));
    TEST_ASSERT_EQUAL_STRING("replaced", web_ntrip_fail_name(NTRIP_FAIL_REPLACED));
    TEST_ASSERT_EQUAL(12, NTRIP_FAIL_LOGIN_BLOCKED);
    TEST_ASSERT_EQUAL(14, NTRIP_FAIL_REPLACED);
    static const char *const stops[] = { "none", "rejected", "login_blocked", "in_use", "kicked", "duplicate",
                                         "banned" };
    TEST_ASSERT_EQUAL(sizeof(stops) / sizeof(stops[0]), NTRIP_STOP_COUNT);
    for (int i = 0; i < NTRIP_STOP_COUNT; i++) {
        TEST_ASSERT_EQUAL_STRING(stops[i], web_ntrip_stop_name((ntrip_stop_t)i));
    }
    TEST_ASSERT_EQUAL_STRING("unknown", web_ntrip_stop_name(NTRIP_STOP_COUNT));
    TEST_ASSERT_EQUAL_STRING("caster_stop", web_diag_event_name(DIAG_EV_CASTER_STOP));
    TEST_ASSERT_EQUAL(14, DIAG_EV_CASTER_STOP);
    TEST_ASSERT_TRUE(web_history_kind(DIAG_EV_CASTER_STOP));
}

TEST_CASE("caster status: the guard's fields, every class counted, the day's bytes", "[web_build]")
{
    fixture();
    s_snap.cst[0].state = NTRIP_STATE_STOPPED;
    s_snap.cst[0].reason = NTRIP_FAIL_REPLACED;
    s_snap.cst[0].stop_reason = NTRIP_STOP_KICKED;
    s_snap.cst[0].kicked_sessions = 3;
    s_snap.cst[0].login_rejects = 1;
    s_snap.cst[0].login_blocks = 2;
    s_snap.cst[0].failures_by_class[NTRIP_FAIL_IN_USE] = 4;
    s_snap.cst[0].bytes_sent = 5000000;
    s_snap.cst[0].bytes_sent_today = 1234567;
    strcpy(s_snap.cst[0].last_session_text, "Device reconnected");
    strcpy(s_snap.cst[0].hint, "stopped: sessions kept being taken over");
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_caster_entry(&w, 0, &s_snap.cfg[0], &s_snap.cst[0]);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    const cJSON *st = at(root, "status");
    TEST_ASSERT_EQUAL_STRING("stopped", at(st, "state")->valuestring);
    TEST_ASSERT_EQUAL_STRING("replaced", at(st, "reason")->valuestring);
    TEST_ASSERT_EQUAL_STRING("kicked", at(st, "stop_reason")->valuestring);
    TEST_ASSERT_EQUAL(3, at(st, "kicked_sessions")->valueint);
    TEST_ASSERT_EQUAL(1, at(st, "login_rejects")->valueint);
    TEST_ASSERT_EQUAL(2, at(st, "login_blocks")->valueint);
    TEST_ASSERT_EQUAL_STRING("Device reconnected", at(st, "last_session_text")->valuestring);
    TEST_ASSERT_EQUAL(1234567, at(st, "bytes_sent_today")->valuedouble);
    const cJSON *fb = at(st, "failures_by_class");
    TEST_ASSERT_EQUAL(14, cJSON_GetArraySize(fb));   /* every class but none */
    TEST_ASSERT_EQUAL(4, at(fb, "in_use")->valueint);
    TEST_ASSERT_EQUAL(0, at(fb, "login_blocked")->valueint);
    TEST_ASSERT_EQUAL(0, at(fb, "replaced")->valueint);
    const cJSON *cfg = at(root, "config");
    TEST_ASSERT_TRUE(cJSON_IsFalse(at(cfg, "test_onocoy")));
    cJSON_Delete(root);

    /* The status summary carries the stop reason (admin form only). */
    root = build_status(false);
    const cJSON *c0 = cJSON_GetArrayItem(at(root, "casters"), 0);
    TEST_ASSERT_EQUAL_STRING("kicked", at(c0, "stop_reason")->valuestring);
    TEST_ASSERT_EQUAL_STRING("stopped", at(c0, "state")->valuestring);
    TEST_ASSERT_EQUAL_STRING("none", at(cJSON_GetArrayItem(at(root, "casters"), 1), "stop_reason")->valuestring);
    cJSON_Delete(root);
    root = build_status(true);
    c0 = cJSON_GetArrayItem(at(root, "casters"), 0);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(c0, "stop_reason"));
    cJSON_Delete(root);
}

TEST_CASE("caster config: test_onocoy only where the build allows it", "[web_build]")
{
    fixture();
    strcpy(s_snap.cfg[1].host, "10.0.2.2");
    s_snap.cfg[1].test_onocoy = true;
    for (int allowed = 0; allowed < 2; allowed++) {
        ntrip_test_onocoy_set_allowed(allowed != 0);
        wj_t w;
        wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
        web_build_caster_entry(&w, 1, &s_snap.cfg[1], &s_snap.cst[1]);
        cJSON *root = cJSON_Parse(s_out);
        TEST_ASSERT_NOT_NULL(root);
        TEST_ASSERT_EQUAL(allowed != 0, cJSON_IsTrue(at(at(root, "config"), "test_onocoy")));
        cJSON_Delete(root);
    }
    ntrip_test_onocoy_set_allowed(false);
}

TEST_CASE("connections: a caster_stop row names the slot and the stop reason", "[web_build]")
{
    fixture();
    diag_journal_entry_t e[2] = {
        { .seq = 7, .unix_s = 1760000000, .uptime_s = 300, .kind = DIAG_EV_CASTER_STOP,
          .a = (int32_t)(0u | ((uint32_t)NTRIP_STOP_KICKED << 8)), .b = 0 },
        { .seq = 6, .unix_s = 1759999990, .uptime_s = 290, .kind = DIAG_EV_CASTER_DOWN,
          .a = diag_caster_a(0, NTRIP_FAIL_REPLACED, 0), .b = 10 },
    };
    wj_t w;
    wj_init(&w, s_out, sizeof(s_out), NULL, NULL);
    web_build_connections(&w, e, 2, s_snap.cfg, s_snap.slots, true);
    TEST_ASSERT_TRUE(wj_ok(&w));
    cJSON *root = cJSON_Parse(s_out);
    TEST_ASSERT_NOT_NULL_MESSAGE(root, s_out);
    const cJSON *stop = cJSON_GetArrayItem(at(root, "entries"), 0);
    TEST_ASSERT_EQUAL_STRING("caster_stop", at(stop, "kind")->valuestring);
    TEST_ASSERT_EQUAL(0, at(stop, "slot")->valueint);
    TEST_ASSERT_EQUAL_STRING("Onocoy", at(stop, "slot_name")->valuestring);
    TEST_ASSERT_EQUAL_STRING("kicked", at(stop, "stop_reason")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(stop, "class")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(stop, "count")));
    TEST_ASSERT_TRUE(cJSON_IsNull(at(stop, "duration_s")));
    const cJSON *down = cJSON_GetArrayItem(at(root, "entries"), 1);
    TEST_ASSERT_EQUAL_STRING("replaced", at(down, "class")->valuestring);
    TEST_ASSERT_TRUE(cJSON_IsNull(at(down, "stop_reason")));
    TEST_ASSERT_EQUAL(10, at(down, "duration_s")->valueint);
    cJSON_Delete(root);
}

TEST_CASE("a caster summary with the longest stop reason still fits the WebSocket's buffer", "[web_build]")
{
    fixture();
    memset(s_snap.cfg[0].name, '"', sizeof(s_snap.cfg[0].name) - 1);
    memset(s_snap.cfg[0].host, 'a', sizeof(s_snap.cfg[0].host) - 1);
    s_snap.cst[0].state = NTRIP_STATE_CONFIG_ERROR;
    s_snap.cst[0].reason = NTRIP_FAIL_LOGIN_BLOCKED;
    s_snap.cst[0].stop_reason = NTRIP_STOP_LOGIN_BLOCKED;
    s_snap.cst[0].state_age_s = UINT32_MAX - 1;
    s_snap.cst[0].connected_s = UINT32_MAX - 1;
    s_snap.cst[0].today_online_s = UINT32_MAX - 1;
    s_snap.cst[0].today_offline_s = UINT32_MAX - 1;
    s_snap.cst[0].budget_left_s = UINT32_MAX - 1;
    s_snap.cst[0].budget_s = UINT32_MAX - 1;
    s_snap.cst[0].bytes_sent = UINT64_C(0x1FFFFFFFFFFFFF);
    s_snap.cst[0].lag_ms_now = UINT32_MAX - 1;
    s_snap.cst[0].next_retry_s = UINT32_MAX - 1;
    s_snap.cst[0].unreached_s = UINT32_MAX - 1;
    memset(s_snap.cst[0].hint, '"', sizeof(s_snap.cst[0].hint) - 1);
    static char cst[896];   /* web_ws.c CST_TEXT_MAX */
    wj_t w;
    wj_init_elements(&w, cst, sizeof(cst));
    web_build_caster_summary(&w, &s_snap, 0, false);
    TEST_ASSERT_TRUE(wj_ok(&w));
}
