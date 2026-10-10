/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * REST handlers of the API contract (components/web/API.md). Each handler
 * reads snapshots of the owning components, formats them with the builders
 * (web_build.c) and asks the components to act; nothing here waits for long
 * (station, caster, network and update calls are asynchronous). Snapshots
 * live in PSRAM; handlers run on the server task one at a time.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "diag.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "fw_update.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "health.h"
#include "ledger.h"
#include "lifecycle.h"
#include "network.h"
#include "ntrip_server.h"
#include "pkg_store.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "settings.h"
#include "station.h"
#include "um98x_flash.h"
#include "um98x_profile.h"
#include "web_enums.h"
#include "web_internal.h"
#include "web_parse.h"

static const char *TAG = "web";

#define SCAN_MIN_INTERVAL_S   15u
#define CHECK_MIN_INTERVAL_S  60u
#define REBOOT_DELAY_MS       1000u
#define JOURNAL_PAGE          64u
#define LOG_PAGE              8192u
#define TASKS_MAX             48u
#define MSG_STATS_MAX         32u
#define SKY_MAX               64u
#define HISTORY_DAYS_MAX      90u

/* Server-task snapshots (PSRAM). */
EXT_RAM_BSS_ATTR static web_status_snap_t s_status;
EXT_RAM_BSS_ATTR static station_status_t s_st;
EXT_RAM_BSS_ATTR static ntrip_caster_cfg_t s_cfg[WEB_MAX_SLOTS];
EXT_RAM_BSS_ATTR static ntrip_caster_status_t s_cst[WEB_MAX_SLOTS];
EXT_RAM_BSS_ATTR static network_status_t s_net;
EXT_RAM_BSS_ATTR static network_scan_entry_t s_scan[NETWORK_SCAN_MAX];
EXT_RAM_BSS_ATTR static fw_update_status_t s_fw;
EXT_RAM_BSS_ATTR static gnss_link_stats_t s_link;
EXT_RAM_BSS_ATTR static gnss_obs_summary_t s_obs;
EXT_RAM_BSS_ATTR static gnss_obs_msg_stat_t s_msgs[MSG_STATS_MAX];
EXT_RAM_BSS_ATTR static gnss_obs_sat_t s_sky[SKY_MAX];
EXT_RAM_BSS_ATTR static lifecycle_boot_record_t s_boots[LIFECYCLE_BOOT_HISTORY_LEN];
EXT_RAM_BSS_ATTR static ledger_day_t s_days[HISTORY_DAYS_MAX];
EXT_RAM_BSS_ATTR static diag_task_info_t s_tasks[TASKS_MAX];
EXT_RAM_BSS_ATTR static diag_journal_entry_t s_journal[JOURNAL_PAGE];
EXT_RAM_BSS_ATTR static diag_journal_entry_t s_hist[WEB_HISTORY_MAX];
EXT_RAM_BSS_ATTR static gnss_obs_live_sat_t s_live[GNSS_OBS_MAX_LIVE];
EXT_RAM_BSS_ATTR static gnss_obs_live_t s_live_totals;
EXT_RAM_BSS_ATTR static char s_text[LOG_PAGE + 1];

/* ---- shared helpers ------------------------------------------------------------------------ */

static esp_err_t bad(httpd_req_t *req, const web_bad_t *b)
{
    return web_send_error(req, b->err, NULL, b->field, UINT32_MAX, NULL, NULL);
}

static esp_err_t ok(httpd_req_t *req)
{
    return web_send_json_literal(req, 200, "{\"ok\":true}");
}

static esp_err_t accepted(httpd_req_t *req)
{
    return web_send_json_literal(req, 202, "{\"accepted\":true}");
}

static void resp_begin(web_resp_t *r, httpd_req_t *req, int status)
{
    size_t cap = 0;
    char *buf = web_http_buf(&cap);
    web_resp_begin(r, req, status, buf, cap);
}

static bool settings_ok(void)
{
    return settings_is_available();
}

static uint8_t read_u8(const char *ns, const char *key, uint8_t def)
{
    uint8_t v = def;
    if (settings_get_u8(ns, key, &v) != ESP_OK) {
        v = def;
    }
    return v;
}

static void station_snapshot(station_status_t *st)
{
    if (station_get_status(st) != ESP_OK) {
        memset(st, 0, sizeof(*st));
    }
}

static void load_profile(um98x_base_profile_t *p, bool *stored)
{
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t len = sizeof(buf);
    if (settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, buf, &len) == ESP_OK &&
        um98x_profile_decode(buf, len, p)) {
        if (stored != NULL) {
            *stored = true;
        }
        return;
    }
    /* What the station applies when none is stored: the defaults, station ID from the base MAC. */
    uint8_t mac[6] = { 0 };
    (void)esp_read_mac(mac, ESP_MAC_BASE);
    um98x_profile_default(p, um98x_station_id_from_mac(mac));
    if (stored != NULL) {
        *stored = false;
    }
}

static bool journal_open_now(const station_status_t *st)
{
    return st->flash_journal_open || st->flash_journal_settings_unavailable;
}

bool web_rx_hands_off(void)
{
    gnss_link_stats_t l;
    return gnss_link_get_stats(&l) == ESP_OK && l.hands_off;
}

/* gnss/recovery as health reads it: a value out of range is full recovery. */
static health_recovery_mode_t stored_recovery(void)
{
    uint8_t v = read_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, (uint8_t)HEALTH_RECOVERY_FULL);
    return v <= (uint8_t)HEALTH_RECOVERY_ALERTS ? (health_recovery_mode_t)v : HEALTH_RECOVERY_FULL;
}

/* ---- status snapshot ------------------------------------------------------------------------- */

void web_snap_status(web_status_snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->uptime_s = web_now_s();
    strlcpy(s->hostname, web_hostname_active(), sizeof(s->hostname));
    strlcpy(s->version, esp_app_get_description()->version, sizeof(s->version));
    s->clock = ledger_clock_source();
    s->time_unix = s->clock != LEDGER_CLOCK_NONE ? (int64_t)time(NULL) : 0;
    s->settings_available = settings_ok();
    lifecycle_get_boot_info(&s->boot);
    if (network_get_status(&s->net) != ESP_OK) {
        memset(&s->net, 0, sizeof(s->net));
    }
    station_snapshot(&s->st);
    s->rtcm_age_ms = UINT32_MAX;
    if (gnss_link_get_stats(&s_link) == ESP_OK) {
        s->rtcm_age_ms = s_link.last_rtcm_age_ms;
        /* Closed after it was opened once: gnss_link reopens it on its own (OFF = never opened, safe mode). */
        s->uart_closed = s_link.mode != GNSS_LINK_MODE_OFF && !s_link.link_open;
    }
    if (gnss_obs_get_summary(&s_obs) == ESP_OK) {
        s->obs_valid = true;
        s->best_sys = s_obs.best_sys;
        s->best_bands = s_obs.best_bands;
    }
    s->slots = ntrip_server_slot_count();
    if (s->slots > WEB_MAX_SLOTS) {
        s->slots = WEB_MAX_SLOTS;
    }
    s->cfg_valid = true;
    for (size_t i = 0; i < s->slots; i++) {
        if (ntrip_server_get_config(i, &s->cfg[i], false) != ESP_OK) {
            s->cfg_valid = false;
        }
        memset(s->cfg[i].password, 0, sizeof(s->cfg[i].password));
        if (ntrip_server_get_status(i, &s->cst[i]) != ESP_OK) {
            memset(&s->cst[i], 0, sizeof(s->cst[i]));
        }
    }
    if (health_get_status(&s->health) != ESP_OK) {
        memset(&s->health, 0, sizeof(s->health));
        s->health.rtcm_gap_ms = UINT32_MAX;
        s->health.factory_reset_refused_age_s = UINT32_MAX;
    }
    if (fw_update_get_status(&s->fw) != ESP_OK) {
        memset(&s->fw, 0, sizeof(s->fw));
    }
    s->protected_down_today_s = ledger_protected_down_today_s();
    s->ledger_budget_left_s = ledger_protected_budget_left_s();
    s->flash_available = WEB_FLASH_AVAILABLE;
}

uint32_t web_budget_now(void)
{
    if (ledger_clock_source() == LEDGER_CLOCK_NONE) {
        return UINT32_MAX;
    }
    ntrip_caster_cfg_t *cfg = heap_caps_malloc(sizeof(*cfg), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ntrip_caster_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint32_t budget = UINT32_MAX;
    size_t slots = ntrip_server_slot_count();
    for (size_t i = 0; cfg != NULL && st != NULL && i < slots; i++) {
        if (ntrip_server_get_config(i, cfg, false) != ESP_OK) {
            break;
        }
        if (cfg->enabled && cfg->protected_slot) {
            bool live = ntrip_server_get_status(i, st) == ESP_OK && st->state != NTRIP_STATE_DISABLED;
            budget = live ? st->budget_left_s : ledger_protected_budget_left_s();
            break;
        }
    }
    free(cfg);
    free(st);
    return budget;
}

/* ---- update jobs: progress and last result ----------------------------------------------------------- */

static portMUX_TYPE s_res_lock = portMUX_INITIALIZER_UNLOCKED;
static web_last_result_t s_last;

void web_note_result(const char *job, esp_err_t err, const char *result)
{
    portENTER_CRITICAL(&s_res_lock);
    s_last.valid = true;
    strlcpy(s_last.job, job, sizeof(s_last.job));
    strlcpy(s_last.result, result != NULL ? result : web_result_code(job, err), sizeof(s_last.result));
    s_last.err = err;
    s_last.failed_rule = 0;
    s_last.cross_layout = false;
    portEXIT_CRITICAL(&s_res_lock);
}

static const char *s_fw_job;          /* job seen running */
static const char *s_pending_job;     /* job the server queued, not seen running yet */
static uint32_t s_pending_since_s;

void web_note_job_started(const char *job)
{
    s_pending_job = job;
    s_pending_since_s = web_now_s();
}

static const char *job_of(fw_update_state_t state)
{
    switch (state) {
    case FW_UPDATE_CHECKING:
        return "check";
    case FW_UPDATE_DOWNLOADING_APP:
        return "app_update";
    case FW_UPDATE_DOWNLOADING_GNSS:
        return "gnss_update";
    case FW_UPDATE_UPLOADING_APP:
        return "app_upload";
    default:
        return NULL;
    }
}

void web_track_fw_state(bool event)
{
    /* A job starts with a state change (an event); between events only a
     * running or just-queued job needs a look. */
    if (!event && s_fw_job == NULL && s_pending_job == NULL) {
        return;
    }
    fw_update_status_t fw;
    if (fw_update_get_status(&fw) != ESP_OK) {
        return;
    }
    const char *running = job_of(fw.state);
    if (running != NULL) {
        s_fw_job = running;
        s_pending_job = NULL;
        if (strcmp(running, "app_upload") != 0) {   /* the upload reports its own progress */
            bool downloading = fw.state == FW_UPDATE_DOWNLOADING_APP || fw.state == FW_UPDATE_DOWNLOADING_GNSS;
            web_progress_t p = {
                .job = running,
                .phase = downloading ? "downloading" : "checking",
                .pct = downloading ? fw.progress_pct : -1,
                .bytes = -1,
            };
            web_ws_progress(&p);
        }
        return;
    }
    const char *job = s_fw_job;
    if (job == NULL && s_pending_job != NULL && (uint32_t)(web_now_s() - s_pending_since_s) >= 2u) {
        /* Queued but never seen running: it ended (or was refused) right away. */
        job = s_pending_job;
    }
    if (job == NULL) {
        return;
    }
    s_fw_job = NULL;
    s_pending_job = NULL;
    esp_err_t err = fw.state == FW_UPDATE_FAILED ? fw.last_err : ESP_OK;
    if (err == ESP_OK && fw.state == FW_UPDATE_FAILED) {
        err = ESP_FAIL;
    }
    const char *result = web_result_code(job, err);
    if (strcmp(job, "app_upload") == 0) {
        return;   /* the upload notes its own result */
    }
    web_note_result(job, err, result);
    web_progress_t p = {
        .job = job,
        .phase = fw.state == FW_UPDATE_REBOOT_PENDING ? "rebooting" : (strcmp(job, "check") == 0 ? "checking" : "downloading"),
        .pct = -1,
        .bytes = -1,
        .done = true,
        .result = result,
        .err = err,
    };
    web_ws_progress(&p);
}

/* ---- scan bookkeeping -------------------------------------------------------------------------------- */

static bool s_scan_running;
static bool s_scan_done_seen;
static uint32_t s_scan_done_s;
static bool s_scan_started;
static uint32_t s_scan_started_s;
static bool s_check_started;
static uint32_t s_check_started_s;

void web_note_scan_done(void)
{
    s_scan_running = false;
    s_scan_done_seen = true;
    s_scan_done_s = web_now_s();
}

/* ---- session and setup ---------------------------------------------------------------------------------- */

static esp_err_t h_session_get(web_req_t *r)
{
    web_admin_state_t admin = web_auth_admin_state();
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    wj_t *w = &resp.w;
    wj_obj_open(w, NULL);
    wj_int(w, "api", WEB_API_VERSION);
    wj_bool(w, "authenticated", r->sess >= 0);
    wj_bool(w, "admin_set", admin != WEB_ADMIN_NONE);
    wj_bool(w, "setup_done", read_u8(SETTINGS_NS_SYS, SETTINGS_KEY_SETUP_DONE, 0) != 0);
    wj_bool(w, "claim_allowed", admin == WEB_ADMIN_NONE && r->on_ap);
    wj_bool(w, "on_setup_ap", r->on_ap);
    wj_bool(w, "public_status", web_public_status());
    wj_bool(w, "settings_available", settings_ok());
    if (r->sess >= 0) {
        wj_uint(w, "idle_left_s", web_auth_session_left_s(r->sess));
    } else {
        wj_null(w, "idle_left_s");
    }
    wj_obj_close(w);
    return web_resp_end(&resp);
}

static esp_err_t h_session_post(web_req_t *r)
{
    web_admin_state_t admin = web_auth_admin_state();
    if (admin == WEB_ADMIN_NONE) {
        return web_send_err(r->req, WEB_ERR_ADMIN_NOT_SET);
    }
    if (admin == WEB_ADMIN_UNREADABLE) {
        return web_send_error(r->req, WEB_ERR_SETTINGS_UNAVAILABLE,
                              "The stored password cannot be read: hold the BOOT button to reset it", NULL, UINT32_MAX,
                              NULL, NULL);
    }
    uint32_t wait = web_auth_throttle_wait_s(r->req, r->on_ap);
    if (wait > 0) {
        return web_send_error(r->req, WEB_ERR_RATE_LIMITED, NULL, NULL, wait, NULL, NULL);
    }
    web_bad_t b = { 0 };
    const char *pw = NULL;
    if (!web_parse_password(r->body, "password", &pw, &b)) {
        return bad(r->req, &b);
    }
    if (!web_auth_check(r->req, r->on_ap, pw)) {
        return web_send_err(r->req, WEB_ERR_WRONG_PASSWORD);
    }
    if (r->sess >= 0) {
        web_auth_end_session(r->sess);   /* a fresh login replaces the one this browser had */
    }
    char cookie[96];
    (void)web_auth_start_session(r->req, cookie);
    char body[64];
    snprintf(body, sizeof(body), "{\"authenticated\":true,\"idle_timeout_s\":%u}", (unsigned)WEB_SESSION_IDLE_S);
    return web_send_json_literal(r->req, 200, body);
}

static esp_err_t h_session_delete(web_req_t *r)
{
    web_auth_logout(r->req, r->sess);
    return ok(r->req);
}

static esp_err_t h_setup_admin(web_req_t *r)
{
    if (web_auth_admin_state() != WEB_ADMIN_NONE) {
        return web_send_err(r->req, WEB_ERR_ADMIN_ALREADY_SET);
    }
    if (!r->on_ap) {
        return web_send_err(r->req, WEB_ERR_NOT_ON_SETUP_AP);
    }
    web_bad_t b = { 0 };
    const char *pw = NULL;
    if (!web_parse_password(r->body, "password", &pw, &b)) {
        return bad(r->req, &b);
    }
    if (!web_password_valid(pw)) {
        return web_send_error(r->req, WEB_ERR_INVALID_FIELD, "8-64 characters, no control characters", "password",
                              UINT32_MAX, NULL, NULL);
    }
    bool persisted = false;
    esp_err_t err = web_auth_set_password(pw, &persisted);
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGW(TAG, "admin password set over the setup Wi-Fi%s", persisted ? "" : " (this boot only)");
    char cookie[96];
    (void)web_auth_start_session(r->req, cookie);
    return web_send_json_literal(r->req, 200, persisted ? "{\"authenticated\":true,\"persisted\":true}"
                                                        : "{\"authenticated\":true,\"persisted\":false}");
}

static esp_err_t h_setup_done(web_req_t *r)
{
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    esp_err_t err = settings_set_u8(SETTINGS_NS_SYS, SETTINGS_KEY_SETUP_DONE, 1);
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    return web_send_json_literal(r->req, 200, "{\"setup_done\":true}");
}

static esp_err_t h_admin_password(web_req_t *r)
{
    uint32_t wait = web_auth_throttle_wait_s(r->req, r->on_ap);
    if (wait > 0) {
        return web_send_error(r->req, WEB_ERR_RATE_LIMITED, NULL, NULL, wait, NULL, NULL);
    }
    web_bad_t b = { 0 };
    const char *cur = NULL;
    const char *next = NULL;
    if (!web_parse_admin_password(r->body, &cur, &next, &b)) {
        return bad(r->req, &b);
    }
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    if (!web_auth_check(r->req, r->on_ap, cur)) {
        return web_send_err(r->req, WEB_ERR_WRONG_PASSWORD);
    }
    if (!web_password_valid(next)) {
        return web_send_error(r->req, WEB_ERR_INVALID_FIELD, "8-64 characters, no control characters", "new",
                              UINT32_MAX, NULL, NULL);
    }
    bool persisted = false;
    esp_err_t err = web_auth_set_password(next, &persisted);
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    web_auth_end_sessions(r->sess);
    ESP_LOGW(TAG, "admin password changed: other sessions ended");
    return ok(r->req);
}

/* ---- status ------------------------------------------------------------------------------------------------ */

static esp_err_t h_status(web_req_t *r)
{
    web_snap_status(&s_status);
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_status(&resp.w, &s_status, !r->admin);
    return web_resp_end(&resp);
}

/* ---- receiver ------------------------------------------------------------------------------------------------ */

static esp_err_t h_gnss(web_req_t *r)
{
    station_snapshot(&s_st);
    web_gnss_snap_t g;
    memset(&g, 0, sizeof(g));
    g.st = &s_st;
    if (gnss_link_get_stats(&g.link) != ESP_OK) {
        memset(&g.link, 0, sizeof(g.link));
        g.link.last_rx_age_ms = UINT32_MAX;
        g.link.last_rtcm_age_ms = UINT32_MAX;
    }
    g.autocfg = read_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_AUTOCFG, 1) != 0;
    g.recovery = stored_recovery();
    load_profile(&g.profile, NULL);
    if (gnss_obs_get_arp(&g.arp) != ESP_OK) {
        memset(&g.arp, 0, sizeof(g.arp));
    }
    esp_err_t e1033 = gnss_obs_get_1033(&g.rtcm1033);
    if (e1033 != ESP_OK && e1033 != ESP_ERR_NOT_FOUND) {
        memset(&g.rtcm1033, 0, sizeof(g.rtcm1033));   /* NOT_FOUND fills valid = false and the bad frames */
    }
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_gnss(&resp.w, &g);
    return web_resp_end(&resp);
}

/* Profile and the matching position record in one batch (fixed: the user's position; survey: none). */
static esp_err_t store_profile(const um98x_base_profile_t *p, const um98x_base_profile_t *old)
{
    uint8_t blob[UM98X_PROFILE_BLOB_MAX];
    size_t len = um98x_profile_encode(p, blob, sizeof(blob));
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    bool fixed = p->base_mode == UM98X_BASE_FIXED;
    bool moved = !(old->base_mode == UM98X_BASE_FIXED && old->fixed_lat_deg == p->fixed_lat_deg &&
                   old->fixed_lon_deg == p->fixed_lon_deg && old->fixed_height_m == p->fixed_height_m);
    uint8_t pblob[UM98X_POSITION_BLOB_MAX];
    size_t plen = 0;
    if (fixed && moved) {
        um98x_base_position_t pos = {
            .source = UM98X_POS_USER,
            .lat_deg = p->fixed_lat_deg,
            .lon_deg = p->fixed_lon_deg,
            .height_m = p->fixed_height_m,
            .sigma_m = 0.0f,
            .locked_unix = (time(NULL) > 1700000000) ? (int64_t)time(NULL) : 0,
        };
        plen = um98x_position_encode(&pos, pblob, sizeof(pblob));
        if (plen == 0) {
            return ESP_ERR_INVALID_ARG;
        }
    }
    settings_batch_t b;
    esp_err_t err = settings_batch_begin(SETTINGS_NS_GNSS, &b);
    if (err != ESP_OK) {
        return err;
    }
    err = settings_batch_set_blob(b, SETTINGS_KEY_GNSS_PROFILE, blob, len);
    if (err == ESP_OK && plen > 0) {
        err = settings_batch_set_blob(b, SETTINGS_KEY_GNSS_BASE_POS, pblob, plen);
    }
    if (err == ESP_OK) {
        err = settings_batch_commit(b);
    } else {
        settings_batch_abort(b);
    }
    if (err == ESP_OK && !fixed) {
        /* Surveying again: no locked position any more. */
        esp_err_t e = settings_erase_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BASE_POS);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            err = e;
        }
    }
    return err;
}

static esp_err_t h_gnss_profile(web_req_t *r)
{
    station_snapshot(&s_st);
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    um98x_base_profile_t stored;
    bool was_stored = false;
    load_profile(&stored, &was_stored);
    um98x_base_profile_t next;
    bool have_profile = false;
    bool autocfg = false;
    bool have_autocfg = false;
    web_bad_t b = { 0 };
    if (!web_parse_profile_put(r->body, &stored, &next, &have_profile, &autocfg, &have_autocfg, &b)) {
        return bad(r->req, &b);
    }
    bool have_recovery = false;
    health_recovery_mode_t recovery = HEALTH_RECOVERY_FULL;
    if (!web_parse_recovery(r->body, &have_recovery, &recovery, &b)) {
        return bad(r->req, &b);
    }
    /* The recovery mode is health's: it changes nothing on the receiver, so an open update session does not hold it. */
    if ((have_profile || have_autocfg) && journal_open_now(&s_st)) {
        return web_send_err(r->req, WEB_ERR_JOURNAL_OPEN);
    }
    bool changed = false;
    if (have_profile) {
        uint8_t a[UM98X_PROFILE_BLOB_MAX];
        uint8_t c[UM98X_PROFILE_BLOB_MAX];
        size_t la = um98x_profile_encode(&stored, a, sizeof(a));
        size_t lc = um98x_profile_encode(&next, c, sizeof(c));
        changed = !was_stored || la != lc || memcmp(a, c, lc) != 0;
        if (changed) {
            esp_err_t err = store_profile(&next, &stored);
            if (err != ESP_OK) {
                return web_send_internal(r->req, err);
            }
        }
    }
    if (have_autocfg) {
        esp_err_t err = settings_set_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_AUTOCFG, autocfg ? 1 : 0);
        if (err != ESP_OK) {
            return web_send_internal(r->req, err);
        }
    }
    if (have_recovery && recovery != stored_recovery()) {
        /* Health reads it every tick. */
        esp_err_t err = settings_set_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, (uint8_t)recovery);
        if (err != ESP_OK) {
            return web_send_internal(r->req, err);
        }
        ESP_LOGW(TAG, "automatic receiver recovery set to %s by the user", web_recovery_mode_name(recovery));
    }
    const char *apply = "unchanged";
    if (changed) {
        esp_err_t err = station_request_reconfigure();
        if (err == ESP_ERR_INVALID_STATE) {
            station_snapshot(&s_st);
        }
        apply = err == ESP_OK                ? "queued"
                : err == ESP_ERR_NOT_SUPPORTED ? "unsupported_model"
                : lifecycle_in_safe_mode()     ? "safe_mode"
                : s_st.rx_maintenance          ? "maintenance"
                                               : "not_running";
    }
    char body[96];
    snprintf(body, sizeof(body), "{\"accepted\":true,\"saved\":true,\"apply\":\"%s\"}", apply);
    return web_send_json_literal(r->req, 202, body);
}

/* The receiver actions of /api/gnss: errors in the contract's order. */
static esp_err_t station_action(web_req_t *r, esp_err_t (*fn)(void), bool lock)
{
    station_snapshot(&s_st);
    if (journal_open_now(&s_st)) {
        return web_send_err(r->req, WEB_ERR_JOURNAL_OPEN);
    }
    if (lifecycle_in_safe_mode()) {
        return web_send_err(r->req, WEB_ERR_SAFE_MODE);
    }
    if (s_st.rx_maintenance) {
        return web_send_err(r->req, WEB_ERR_RECEIVER_MAINTENANCE);
    }
    esp_err_t err = fn();
    switch (err) {
    case ESP_OK:
        return accepted(r->req);
    case ESP_ERR_NOT_SUPPORTED:
        return web_send_err(r->req, WEB_ERR_UNSUPPORTED_MODEL);
    case ESP_ERR_NOT_FOUND:
        return web_send_err(r->req, lock ? WEB_ERR_NO_POSITION_YET : WEB_ERR_BUSY);
    case ESP_ERR_INVALID_STATE:
        station_snapshot(&s_st);
        return web_send_err(r->req, journal_open_now(&s_st) ? WEB_ERR_JOURNAL_OPEN
                                    : s_st.rx_maintenance    ? WEB_ERR_RECEIVER_MAINTENANCE
                                                             : WEB_ERR_BUSY);
    default:
        return web_send_internal(r->req, err);
    }
}

static esp_err_t h_gnss_apply(web_req_t *r)
{
    return station_action(r, station_request_reconfigure, false);
}

static esp_err_t h_gnss_lock(web_req_t *r)
{
    return station_action(r, station_lock_position, true);
}

static esp_err_t h_gnss_unlock(web_req_t *r)
{
    return station_action(r, station_request_resurvey, false);
}

static esp_err_t h_gnss_signals(web_req_t *r)
{
    if (gnss_obs_get_summary(&s_obs) != ESP_OK) {
        memset(&s_obs, 0, sizeof(s_obs));
        s_obs.epoch_age_ms = UINT32_MAX;
    }
    size_t n = MSG_STATS_MAX;
    bool have_msgs = gnss_obs_get_msg_stats(s_msgs, &n) == ESP_OK;
    um98x_base_profile_t profile;
    load_profile(&profile, NULL);
    station_snapshot(&s_st);
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_signals(&resp.w, &s_obs, have_msgs ? s_msgs : NULL, have_msgs ? n : 0, &profile,
                      s_st.rtcm1230_supported);
    return web_resp_end(&resp);
}

static esp_err_t h_sky(web_req_t *r)
{
    size_t n = SKY_MAX;
    esp_err_t err = gnss_obs_get_sky(s_sky, &n);
    const char *reason = NULL;
    if (err != ESP_OK || n == 0) {
        um98x_base_profile_t profile;
        load_profile(&profile, NULL);
        if (!profile.sat_view_output) {
            reason = "sat_view_off";
        } else if (err != ESP_OK) {
            reason = "no_data";   /* on in the profile, but nothing arrived in the last 30 s */
        }
    }
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_sky(&resp.w, s_sky, err == ESP_OK ? n : 0, reason);
    return web_resp_end(&resp);
}

/* ---- receiver maintenance ------------------------------------------------------------------------------------ */

static esp_err_t send_maintenance(httpd_req_t *req)
{
    station_snapshot(&s_st);
    char body[96];
    snprintf(body, sizeof(body), "{\"on\":%s,\"left_s\":%u,\"chosen_s\":%u}", s_st.rx_maintenance ? "true" : "false",
             (unsigned)(s_st.rx_maintenance ? s_st.maintenance_left_s : 0),
             (unsigned)(s_st.rx_maintenance ? s_st.maintenance_chosen_s : 0));
    return web_send_json_literal(req, 200, body);
}

static esp_err_t h_maint_post(web_req_t *r)
{
    uint32_t duration = 0;
    web_bad_t b = { 0 };
    if (!web_parse_maintenance(r->body, &duration, &b)) {
        return bad(r->req, &b);
    }
    station_maint_why_t why = STATION_MAINT_OK;
    esp_err_t err = station_maintenance_start(duration, &why);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "receiver maintenance started or extended by the user (%u s)",
                 (unsigned)(duration != 0 ? duration : WEB_MAINT_DEFAULT_S));
        return send_maintenance(r->req);
    }
    const char *field = NULL;
    const char *reason = NULL;
    web_err_t e = web_maint_refusal(why, lifecycle_in_safe_mode(), &field, &reason);
    if (e == WEB_ERR_INTERNAL) {
        return web_send_internal(r->req, err);
    }
    const char *text = NULL;
    if (why == STATION_MAINT_REFUSED_SKY_OUTPUT) {
        text = "The live view's satellite output is turned off first: offer maintenance again in a few seconds";
    } else if (reason != NULL) {
        text = "A receiver session or job runs: offer maintenance again when it ends";
    }
    return web_send_error(r->req, e, text, field, UINT32_MAX, NULL, reason);
}

static esp_err_t h_maint_delete(web_req_t *r)
{
    esp_err_t err = station_maintenance_end();
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "receiver maintenance ended by the user");
        return accepted(r->req);
    }
    if (err == ESP_ERR_NOT_FOUND || (err == ESP_ERR_INVALID_STATE && !lifecycle_in_safe_mode())) {
        return web_send_err(r->req, WEB_ERR_NOT_IN_MAINTENANCE);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_err(r->req, WEB_ERR_SAFE_MODE);
    }
    return web_send_internal(r->req, err);
}

/* ---- live view -------------------------------------------------------------------------------------------- */

static esp_err_t send_live(httpd_req_t *req)
{
    station_snapshot(&s_st);
    size_t n = GNSS_OBS_MAX_LIVE;
    bool have = gnss_obs_get_live(s_live, &n, &s_live_totals) == ESP_OK;
    web_live_snap_t l = {
        .st = &s_st,
        .limits = { .default_min = CONFIG_STATION_LIVE_VIEW_MIN, .max_min = CONFIG_STATION_LIVE_VIEW_MAX_MIN,
                    .idle_s = CONFIG_STATION_LIVE_VIEW_IDLE_S },
        .sats = s_live,
        .count = have ? n : 0,
        .totals = have ? &s_live_totals : NULL,
    };
    web_resp_t resp;
    resp_begin(&resp, req, 200);
    web_build_live(&resp.w, &l);
    return web_resp_end(&resp);
}

static esp_err_t h_live_get(web_req_t *r)
{
    /* Every live-view request is an admin client asking for it: without one the window closes. */
    (void)station_live_view_touch();
    return send_live(r->req);
}

static esp_err_t h_live_post(web_req_t *r)
{
    uint32_t minutes = 0;
    web_bad_t b = { 0 };
    if (!web_parse_live(r->body, CONFIG_STATION_LIVE_VIEW_MAX_MIN, &minutes, &b)) {
        return bad(r->req, &b);
    }
    station_live_why_t why = STATION_LIVE_WHY_NONE;
    esp_err_t err = station_live_view_start(minutes, &why);
    if (err == ESP_ERR_INVALID_STATE) {
        web_err_t e = web_live_refusal(why, lifecycle_in_safe_mode());
        return e == WEB_ERR_INTERNAL ? web_send_internal(r->req, err) : web_send_err(r->req, e);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGI(TAG, "live view opened or extended by the user");
    return send_live(r->req);
}

static esp_err_t h_live_delete(web_req_t *r)
{
    esp_err_t err = station_live_view_stop();
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_err(r->req, WEB_ERR_LIVE_VIEW_CLOSED);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGI(TAG, "live view closed by the user");
    return ok(r->req);
}

/* ---- receiver update sessions ------------------------------------------------------------------------------ */

static void flash_ctx(const station_status_t *st, web_flash_ctx_t *c)
{
    bool safe = lifecycle_in_safe_mode();
    memset(c, 0, sizeof(*c));
    c->journal = web_journal_view(st, safe);
    c->safe_mode = safe;
    c->app_trial = lifecycle_app_trial_active();
    c->app_update = lifecycle_update_active() == LIFECYCLE_UPDATE_APP;
    c->receiver_in_loader = (st->alerts & STATION_ALERT_RECEIVER_IN_LOADER) != 0;
    c->station_running = !safe;
    c->flash_available = WEB_FLASH_AVAILABLE;
    c->maintenance = st->rx_maintenance;
}

static esp_err_t h_flash_get(web_req_t *r)
{
    station_snapshot(&s_st);
    web_flash_ctx_t ctx;
    flash_ctx(&s_st, &ctx);
    size_t n = station_get_flash_log(s_text, 4097);
    s_text[n < 4096 ? n : 4096] = '\0';
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_flash(&resp.w, &s_st, &ctx, s_text);
    return web_resp_end(&resp);
}

static esp_err_t flash_action(web_req_t *r, web_flash_action_t a, station_flash_action_t sa)
{
    web_bad_t b = { 0 };
    if (!web_parse_confirm(r->body, &b)) {
        return bad(r->req, &b);
    }
    station_snapshot(&s_st);
    web_flash_ctx_t ctx;
    flash_ctx(&s_st, &ctx);
    web_err_t e = WEB_ERR_INTERNAL;
    if (web_flash_action_why(&ctx, a, &e) != NULL) {
        return web_send_err(r->req, e);
    }
    esp_err_t err = station_flash_user_action(sa);
    switch (err) {
    case ESP_OK:
        if (a == WEB_FLASH_CLEAR) {
            web_staged_forget();
            ESP_LOGW(TAG, "receiver update session cleared by the user");
            return ok(r->req);
        }
        return accepted(r->req);
    case ESP_ERR_NOT_FOUND:
        return web_send_err(r->req, WEB_ERR_NO_JOURNAL);
    case ESP_ERR_INVALID_STATE:
        return web_send_err(r->req, web_rx_hands_off() ? WEB_ERR_RECEIVER_MAINTENANCE : WEB_ERR_NOT_WAITING);
    case ESP_ERR_NOT_SUPPORTED:
        return web_send_err(r->req, WEB_ERR_NOT_AVAILABLE_YET);
    default:
        return web_send_internal(r->req, err);
    }
}

static esp_err_t h_flash_retry(web_req_t *r)
{
    return flash_action(r, WEB_FLASH_RETRY, STATION_FLASH_ACTION_RETRY);
}

static esp_err_t h_flash_restart(web_req_t *r)
{
    return flash_action(r, WEB_FLASH_RESTART_RECEIVER, STATION_FLASH_ACTION_RESTART_RECEIVER);
}

static esp_err_t h_flash_clear(web_req_t *r)
{
    return flash_action(r, WEB_FLASH_CLEAR, STATION_FLASH_ACTION_CLEAR_JOURNAL);
}

/* ---- casters ---------------------------------------------------------------------------------------------- */

static size_t slot_count(void)
{
    size_t n = ntrip_server_slot_count();
    return n > WEB_MAX_SLOTS ? WEB_MAX_SLOTS : n;
}

/* Slot configs (without secrets); false when not loaded (safe mode). */
static bool load_configs(void)
{
    bool ok_all = true;
    for (size_t i = 0; i < slot_count(); i++) {
        if (ntrip_server_get_config(i, &s_cfg[i], false) != ESP_OK) {
            ok_all = false;
        }
        memset(s_cfg[i].password, 0, sizeof(s_cfg[i].password));
    }
    return ok_all;
}

static esp_err_t send_slot(httpd_req_t *req, size_t slot)
{
    bool have = ntrip_server_get_config(slot, &s_cfg[slot], false) == ESP_OK;
    memset(s_cfg[slot].password, 0, sizeof(s_cfg[slot].password));
    if (ntrip_server_get_status(slot, &s_cst[slot]) != ESP_OK) {
        memset(&s_cst[slot], 0, sizeof(s_cst[slot]));
    }
    web_resp_t resp;
    resp_begin(&resp, req, 200);
    web_build_caster_entry(&resp.w, slot, have ? &s_cfg[slot] : NULL, &s_cst[slot]);
    return web_resp_end(&resp);
}

static esp_err_t h_casters(web_req_t *r)
{
    bool have = load_configs();
    size_t n = slot_count();
    for (size_t i = 0; i < n; i++) {
        if (ntrip_server_get_status(i, &s_cst[i]) != ESP_OK) {
            memset(&s_cst[i], 0, sizeof(s_cst[i]));
        }
    }
    ntrip_caster_cfg_t preset;
    ntrip_server_onocoy_defaults(&preset);
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    wj_t *w = &resp.w;
    wj_obj_open(w, NULL);
    wj_uint(w, "max_slots", n);
    web_build_onocoy_preset(w, &preset, WEB_ONOCOY_TLS_PORT);
    wj_arr_open(w, "slots");
    for (size_t i = 0; i < n; i++) {
        web_build_caster_entry(w, i, have ? &s_cfg[i] : NULL, &s_cst[i]);
    }
    wj_arr_close(w);
    wj_obj_close(w);
    return web_resp_end(&resp);
}

/* "/api/casters/<n>[/ca]": slot index and whether it names the CA. */
static bool parse_slot_path(const char *uri, size_t *slot, bool *ca)
{
    static const char prefix[] = "/api/casters/";
    size_t plen = sizeof(prefix) - 1;
    if (strncmp(uri, prefix, plen) != 0) {
        return false;
    }
    const char *p = uri + plen;
    if (*p < '0' || *p > '9') {
        return false;
    }
    size_t v = 0;
    while (*p >= '0' && *p <= '9' && v < 100) {
        v = v * 10u + (size_t)(*p - '0');
        p++;
    }
    *ca = false;
    if (strncmp(p, "/ca", 3) == 0 && (p[3] == '\0' || p[3] == '?')) {
        *ca = true;
        p += 3;
    }
    if (*p != '\0' && *p != '?') {
        return false;
    }
    if (v >= slot_count()) {
        return false;
    }
    *slot = v;
    return true;
}

/* Reconnect a slot with its config unchanged (after its CA changed). */
static void reconnect_slot(size_t slot)
{
    ntrip_caster_cfg_t *c = &s_cfg[slot];
    if (ntrip_server_get_config(slot, c, false) == ESP_OK) {
        c->clear_password = false;
        memset(c->password, 0, sizeof(c->password));   /* empty + same host keeps the stored one */
        esp_err_t err = ntrip_server_set_config(slot, c, NULL);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "caster slot %u not reconnected: %s", (unsigned)slot, esp_err_to_name(err));
        }
    }
}

static esp_err_t caster_put(web_req_t *r, size_t slot)
{
    if (r->req->content_len > WEB_JSON_MAX) {
        return web_send_err(r->req, WEB_ERR_TOO_LARGE);
    }
    if (!load_configs()) {
        return web_send_err(r->req, WEB_ERR_SAFE_MODE);
    }
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    ntrip_caster_cfg_t next;
    web_bad_t b = { 0 };
    if (!web_parse_caster_put(r->body, &s_cfg[slot], &next, &b)) {
        return bad(r->req, &b);
    }
    ntrip_policy_t rule = NTRIP_POLICY_OK;
    esp_err_t err = ntrip_server_set_config(slot, &next, &rule);
    memset(next.password, 0, sizeof(next.password));
    /* The caster component owns the cross-slot rules and names the one it applied. */
    const char *reason = err == ESP_ERR_NOT_ALLOWED ? web_policy_reason(rule) : NULL;
    if (reason != NULL) {
        return web_send_error(r->req, WEB_ERR_CASTER_POLICY, NULL, NULL, UINT32_MAX, NULL, reason);
    }
    switch (err) {
    case ESP_OK:
        ESP_LOGI(TAG, "caster slot %u saved", (unsigned)slot);
        return send_slot(r->req, slot);
    case ESP_ERR_INVALID_ARG:
        return web_send_err(r->req, WEB_ERR_INVALID_FIELD);
    case ESP_ERR_INVALID_STATE:
        return web_send_err(r->req, WEB_ERR_SAFE_MODE);
    default:
        return web_send_internal(r->req, err);
    }
}

static esp_err_t caster_ca(web_req_t *r, size_t slot, bool store)
{
    if (!load_configs()) {
        return web_send_err(r->req, WEB_ERR_SAFE_MODE);
    }
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    char ns[16];
    snprintf(ns, sizeof(ns), SETTINGS_NS_CASTER_FMT, (unsigned)slot);
    esp_err_t err;
    if (store) {
        const char *pem = NULL;
        web_bad_t b = { 0 };
        if (!web_parse_ca_pem(r->body, &pem, &b)) {
            return bad(r->req, &b);
        }
        err = settings_set_blob(ns, SETTINGS_KEY_CST_CA_PEM, pem, strlen(pem));
    } else {
        err = settings_erase_key(ns, SETTINGS_KEY_CST_CA_PEM);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    reconnect_slot(slot);
    return send_slot(r->req, slot);
}

static esp_err_t h_caster_get(web_req_t *r)
{
    size_t slot = 0;
    bool ca = false;
    if (!parse_slot_path(r->req->uri, &slot, &ca)) {
        return web_send_err(r->req, WEB_ERR_NOT_FOUND);
    }
    if (ca) {
        return web_send_err(r->req, WEB_ERR_METHOD_NOT_ALLOWED);
    }
    return send_slot(r->req, slot);
}

static esp_err_t h_caster_put(web_req_t *r)
{
    size_t slot = 0;
    bool ca = false;
    if (!parse_slot_path(r->req->uri, &slot, &ca)) {
        return web_send_err(r->req, WEB_ERR_NOT_FOUND);
    }
    return ca ? caster_ca(r, slot, true) : caster_put(r, slot);
}

static esp_err_t h_caster_delete(web_req_t *r)
{
    size_t slot = 0;
    bool ca = false;
    if (!parse_slot_path(r->req->uri, &slot, &ca)) {
        return web_send_err(r->req, WEB_ERR_NOT_FOUND);
    }
    if (!ca) {
        return web_send_err(r->req, WEB_ERR_METHOD_NOT_ALLOWED);
    }
    return caster_ca(r, slot, false);
}

/* ---- Wi-Fi and system --------------------------------------------------------------------------------------- */

static bool ap_password_set(void)
{
    char pw[65];
    return settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_AP_PASS, pw, sizeof(pw)) == ESP_OK && pw[0] != '\0';
}

static esp_err_t h_wifi_get(web_req_t *r)
{
    if (network_get_status(&s_net) != ESP_OK) {
        memset(&s_net, 0, sizeof(s_net));
        s_net.probe_age_s = UINT32_MAX;
    }
    size_t n = 0;
    if (network_get_scan_results(s_scan, NETWORK_SCAN_MAX, &n) != ESP_OK) {
        n = 0;
    }
    web_wifi_snap_t s = {
        .net = &s_net,
        .scan = s_scan,
        .scan_count = n,
        .scan_age_s = s_scan_done_seen ? web_now_s() - s_scan_done_s : UINT32_MAX,
        .scan_running = s_scan_running,
        .ap_password_set = ap_password_set(),
    };
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_wifi(&resp.w, &s);
    return web_resp_end(&resp);
}

static esp_err_t h_wifi_put(web_req_t *r)
{
    web_wifi_put_t in;
    web_bad_t b = { 0 };
    if (!web_parse_wifi_put(r->body, &in, &b)) {
        return bad(r->req, &b);
    }
    bool persist = settings_ok();
    if (in.have_ap_password && !persist) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    /* Work out the station password before changing anything. */
    static char stored_pw[65];
    const char *pw = in.password;
    bool reconnect = in.have_ssid;
    if (in.have_ssid && !in.have_password) {
        char stored_ssid[33];
        bool same = settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_SSID, stored_ssid, sizeof(stored_ssid)) ==
                        ESP_OK &&
                    strcmp(stored_ssid, in.ssid) == 0;
        if (!same || settings_get_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_PASS, stored_pw, sizeof(stored_pw)) != ESP_OK) {
            return web_send_error(r->req, WEB_ERR_INVALID_FIELD, "A new network needs its password", "password",
                                  UINT32_MAX, NULL, NULL);
        }
        /* Same network, same password: nothing to change, so no reconnect
         * (a form that only sets the AP password may still send the SSID). */
        memset(stored_pw, 0, sizeof(stored_pw));
        reconnect = false;
    }
    if (in.have_ap_password) {
        esp_err_t err = settings_set_str(SETTINGS_NS_WIFI, SETTINGS_KEY_WIFI_AP_PASS, in.ap_password);
        if (err != ESP_OK) {
            memset(stored_pw, 0, sizeof(stored_pw));
            return web_send_internal(r->req, err);
        }
    }
    bool trial = false;
    if (reconnect) {
        esp_err_t err = network_set_credentials(in.ssid, pw);
        memset(stored_pw, 0, sizeof(stored_pw));
        if (err == ESP_ERR_INVALID_ARG) {
            return web_send_error(r->req, WEB_ERR_INVALID_FIELD, NULL, "password", UINT32_MAX, NULL, NULL);
        }
        if (err != ESP_OK) {
            return web_send_internal(r->req, err);
        }
        /* With credentials stored before, the new ones are pending: tried, and dropped back from if they get no IP. */
        trial = network_get_status(&s_net) == ESP_OK && s_net.cred_pending &&
                strncmp(s_net.pending_ssid, in.ssid, sizeof(s_net.pending_ssid)) == 0;
#if CONFIG_NETWORK_QEMU_ETHERNET
        ESP_LOGI(TAG, "Wi-Fi credentials changed: stored, not used (QEMU build: the emulated Ethernet stays the link)");
#else
        ESP_LOGI(TAG, "Wi-Fi credentials changed: %s",
                 trial ? "tried now, the previous ones come back unless they get an IP" : "the station connects with them");
#endif
    }
    char body[80];
    snprintf(body, sizeof(body), "{\"accepted\":true,\"persisted\":%s,\"trial\":%s}", persist ? "true" : "false",
             trial ? "true" : "false");
    return web_send_json_literal(r->req, 202, body);
}

static esp_err_t h_wifi_pending_retry(web_req_t *r)
{
    esp_err_t err = network_retry_pending_credentials();
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_err(r->req, WEB_ERR_NOTHING_PENDING);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGI(TAG, "pending Wi-Fi credentials tried again at the user's request");
    return accepted(r->req);
}

static esp_err_t h_wifi_pending_discard(web_req_t *r)
{
    esp_err_t err = network_discard_pending_credentials();
    if (err == ESP_ERR_NOT_FOUND) {
        return web_send_err(r->req, WEB_ERR_NOTHING_PENDING);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGI(TAG, "pending Wi-Fi credentials discarded by the user");
    return ok(r->req);
}

static esp_err_t h_wifi_scan(web_req_t *r)
{
    uint32_t now = web_now_s();
    if (s_scan_started && (uint32_t)(now - s_scan_started_s) < SCAN_MIN_INTERVAL_S) {
        return web_send_error(r->req, WEB_ERR_RATE_LIMITED, NULL, NULL,
                              SCAN_MIN_INTERVAL_S - (uint32_t)(now - s_scan_started_s), NULL, NULL);
    }
    esp_err_t err = network_scan_start();
    if (err == ESP_ERR_INVALID_STATE) {
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    s_scan_started = true;
    s_scan_started_s = now;
    s_scan_running = true;
    return accepted(r->req);
}

static esp_err_t send_system(httpd_req_t *req, int reboot_required)
{
    char stored[33] = "";
    if (settings_get_str(SETTINGS_NS_SYS, SETTINGS_KEY_HOSTNAME, stored, sizeof(stored)) != ESP_OK) {
        stored[0] = '\0';
    }
    const esp_app_desc_t *app = esp_app_get_description();
    uint8_t mac[6] = { 0 };
    bool have_mac = esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK;
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    uint32_t flash = 0;
    if (esp_flash_get_size(NULL, &flash) != ESP_OK) {
        flash = 0;
    }
    char project[33];
    char idf[33];
    strlcpy(project, app->project_name, sizeof(project));
    strlcpy(idf, app->idf_ver, sizeof(idf));
    web_system_snap_t s = {
        .hostname = stored,
        .hostname_active = web_hostname_active(),
        .public_status = web_public_status(),
        .setup_done = read_u8(SETTINGS_NS_SYS, SETTINGS_KEY_SETUP_DONE, 0) != 0,
        .settings_available = settings_ok(),
        .version = app->version,
        .project = project,
        .idf = idf,
        .mac = have_mac ? mac : NULL,
        .chip_model = CONFIG_IDF_TARGET,
        .chip_revision = chip.revision,
        .flash_mb = flash / (1024u * 1024u),
        .psram_mb = (uint32_t)(esp_psram_get_size() / (1024u * 1024u)),
        .reboot_required = reboot_required,
    };
    web_resp_t resp;
    resp_begin(&resp, req, 200);
    web_build_system(&resp.w, &s);
    return web_resp_end(&resp);
}

static esp_err_t h_system_get(web_req_t *r)
{
    return send_system(r->req, -1);
}

static esp_err_t h_system_put(web_req_t *r)
{
    web_system_put_t in;
    web_bad_t b = { 0 };
    if (!web_parse_system_put(r->body, &in, &b)) {
        return bad(r->req, &b);
    }
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    int reboot = -1;
    if (in.have_hostname) {
        esp_err_t err = settings_set_str(SETTINGS_NS_SYS, SETTINGS_KEY_HOSTNAME, in.hostname);
        if (err != ESP_OK) {
            return web_send_internal(r->req, err);
        }
        reboot = strcasecmp(in.hostname, web_hostname_active()) != 0 ? 1 : 0;
    }
    if (in.have_public_status) {
        esp_err_t err = settings_set_u8(SETTINGS_NS_WEB, SETTINGS_KEY_WEB_PUBLIC, in.public_status ? 1 : 0);
        if (err != ESP_OK) {
            return web_send_internal(r->req, err);
        }
        web_public_status_set_cached(in.public_status);
    }
    return send_system(r->req, reboot);
}

/* ---- updates ------------------------------------------------------------------------------------------------- */

static bool checks_enabled(void)
{
    return fw_update_checks_enabled();
}

static esp_err_t h_update_get(web_req_t *r)
{
    if (fw_update_get_status(&s_fw) != ESP_OK) {
        memset(&s_fw, 0, sizeof(s_fw));
    }
    station_snapshot(&s_st);
    web_last_result_t last;
    portENTER_CRITICAL(&s_res_lock);
    last = s_last;
    portEXIT_CRITICAL(&s_res_lock);
    static char rolled_back[FW_UPDATE_ROLLED_BACK_MAX][32];   /* server task only */
    size_t n_rolled_back = fw_update_get_rolled_back(rolled_back, FW_UPDATE_ROLLED_BACK_MAX);
    web_update_snap_t u = {
        .fw = &s_fw,
        .st = &s_st,
        .last = &last,
        .checks_enabled = checks_enabled(),
        .release_repo = fw_update_release_repo(),
        .signed_manifests = fw_update_signature_required(),
        .budget_left_s = web_budget_now(),
        .flash_available = WEB_FLASH_AVAILABLE,
        .rolled_back = (const char (*)[32])rolled_back,
        .rolled_back_count = n_rolled_back,
    };
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_update(&resp.w, &u);
    return web_resp_end(&resp);
}

static bool fw_busy(const fw_update_status_t *fw)
{
    return fw->state == FW_UPDATE_CHECKING || fw->state == FW_UPDATE_DOWNLOADING_APP ||
           fw->state == FW_UPDATE_DOWNLOADING_GNSS || fw->state == FW_UPDATE_UPLOADING_APP ||
           fw->state == FW_UPDATE_REBOOT_PENDING || web_upload_busy();
}

static esp_err_t h_update_check(web_req_t *r)
{
    if (!checks_enabled()) {
        return web_send_err(r->req, WEB_ERR_CHECKS_DISABLED);
    }
    uint32_t now = web_now_s();
    if (s_check_started && (uint32_t)(now - s_check_started_s) < CHECK_MIN_INTERVAL_S) {
        return web_send_error(r->req, WEB_ERR_RATE_LIMITED, NULL, NULL,
                              CHECK_MIN_INTERVAL_S - (uint32_t)(now - s_check_started_s), NULL, NULL);
    }
    esp_err_t err = fw_update_check();
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return web_send_err(r->req, WEB_ERR_CHECKS_DISABLED);
    }
    if (err == ESP_ERR_INVALID_STATE) {
        if (fw_update_get_status(&s_fw) == ESP_OK && s_fw.trial == FW_TRIAL_CONFIRMING) {
            return web_send_error(r->req, WEB_ERR_APP_TRIAL, NULL, NULL, s_fw.trial_left_s, NULL, NULL);
        }
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    s_check_started = true;
    s_check_started_s = now;
    web_note_job_started("check");
    return accepted(r->req);
}

static esp_err_t h_update_app(web_req_t *r)
{
    const char *version = NULL;
    bool ack = false;
    web_bad_t b = { 0 };
    if (!web_parse_update_app(r->body, &version, &ack, &b)) {
        return bad(r->req, &b);
    }
    if (fw_update_get_status(&s_fw) != ESP_OK) {
        return web_send_internal(r->req, ESP_FAIL);
    }
    if (s_fw.latest_version[0] == '\0') {
        return web_send_err(r->req, WEB_ERR_NOT_OFFERED);
    }
    if (strcmp(version, s_fw.latest_version) != 0) {
        return web_send_err(r->req, WEB_ERR_MANIFEST_CHANGED);
    }
    if (s_fw.latest_rolled_back && !ack) {
        return web_send_err(r->req, WEB_ERR_ROLLED_BACK_ACK_REQUIRED);
    }
    if (lifecycle_update_active() == LIFECYCLE_UPDATE_RECEIVER) {
        return web_send_err(r->req, WEB_ERR_JOURNAL_OPEN);
    }
    if (s_fw.trial == FW_TRIAL_CONFIRMING || s_fw.trial == FW_TRIAL_VERIFYING) {
        return web_send_error(r->req, WEB_ERR_APP_TRIAL, NULL, NULL, s_fw.trial_left_s, NULL, NULL);
    }
    if (fw_busy(&s_fw)) {
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    esp_err_t err = fw_update_install_app(version);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Everything the server can see passed: the version is not newer than the running one. */
        if (fw_update_get_status(&s_fw) == ESP_OK && fw_busy(&s_fw)) {
            return web_send_err(r->req, WEB_ERR_BUSY);
        }
        return web_send_err(r->req, WEB_ERR_NOT_OFFERED);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGW(TAG, "app update to %s started by the user", version);
    web_note_job_started("app_update");
    return accepted(r->req);
}

/* The receiver-update refusal for the current state, WEB_ERR_COUNT when none. */
static web_err_t gnss_block(const fw_update_status_t *fw, uint32_t *retry_in_s)
{
    *retry_in_s = UINT32_MAX;
    if (web_rx_hands_off()) {
        return WEB_ERR_RECEIVER_MAINTENANCE;   /* nothing is sent to the receiver */
    }
    switch (fw->gnss_blocked) {
    case FW_GNSS_BLOCK_APP_TRIAL:
        *retry_in_s = fw->gnss_blocked_left_s;
        return WEB_ERR_APP_TRIAL;
    case FW_GNSS_BLOCK_APP_UPDATE:
        return WEB_ERR_APP_UPDATE;
    case FW_GNSS_BLOCK_JOURNAL_OPEN:
        return WEB_ERR_JOURNAL_OPEN;
    case FW_GNSS_BLOCK_RECEIVER_NOT_STREAMING:
        return WEB_ERR_RECEIVER_NOT_STREAMING;
    case FW_GNSS_BLOCK_UNSUPPORTED_MODEL:
        return WEB_ERR_UNSUPPORTED_MODEL;
    case FW_GNSS_BLOCK_SAFE_MODE:
        return WEB_ERR_SAFE_MODE;
    default:
        break;
    }
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) == ESP_OK && info.pinned) {
        return WEB_ERR_PACKAGE_PINNED;
    }
    if (fw_busy(fw)) {
        return WEB_ERR_BUSY;
    }
    return WEB_ERR_COUNT;
}

static esp_err_t h_update_gnss(web_req_t *r)
{
    uint32_t build = 0;
    bool ack = false;
    web_bad_t b = { 0 };
    if (!web_parse_update_gnss(r->body, &build, &ack, &b)) {
        return bad(r->req, &b);
    }
    if (fw_update_get_status(&s_fw) != ESP_OK) {
        return web_send_internal(r->req, ESP_FAIL);
    }
    if (!s_fw.gnss_update_available || s_fw.gnss_latest_build != build) {
        return web_send_err(r->req, WEB_ERR_NOT_OFFERED);
    }
    uint32_t retry = UINT32_MAX;
    web_err_t e = gnss_block(&s_fw, &retry);
    if (e != WEB_ERR_COUNT) {
        return web_send_error(r->req, e, NULL, NULL, retry, NULL, NULL);
    }
    esp_err_t err = fw_update_install_gnss(build, ack);
    if (err == ESP_ERR_INVALID_STATE) {
        if (fw_update_get_status(&s_fw) == ESP_OK && (e = gnss_block(&s_fw, &retry)) != WEB_ERR_COUNT) {
            return web_send_error(r->req, e, NULL, NULL, retry, NULL, NULL);
        }
        return web_send_err(r->req, WEB_ERR_NOT_OFFERED);
    }
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGW(TAG, "receiver update to build %u started by the user", (unsigned)build);
    web_note_job_started("gnss_update");
    return accepted(r->req);
}

static esp_err_t h_update_rollback(web_req_t *r)
{
    web_bad_t b = { 0 };
    if (!web_parse_confirm(r->body, &b)) {
        return bad(r->req, &b);
    }
    if (lifecycle_update_active() == LIFECYCLE_UPDATE_RECEIVER) {
        return web_send_err(r->req, WEB_ERR_JOURNAL_OPEN);
    }
    if (fw_update_get_status(&s_fw) == ESP_OK && fw_busy(&s_fw)) {
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    esp_err_t err = fw_update_rollback();
    switch (err) {
    case ESP_OK:
        web_note_result("rollback", ESP_OK, "ok");
        ESP_LOGW(TAG, "rollback to the previous app requested by the user");
        return accepted(r->req);
    case ESP_ERR_NOT_FOUND:
        return web_send_err(r->req, WEB_ERR_NO_ROLLBACK_IMAGE);
    case ESP_ERR_INVALID_STATE: {
        const char *by = NULL;
        if (lifecycle_update_active() == LIFECYCLE_UPDATE_RECEIVER) {
            return web_send_err(r->req, WEB_ERR_JOURNAL_OPEN);
        }
        if (lifecycle_reboot_inhibited(&by)) {
            return web_send_error(r->req, WEB_ERR_REBOOT_INHIBITED, NULL, NULL, UINT32_MAX, by, NULL);
        }
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    default:
        return web_send_internal(r->req, err);
    }
}

/* ---- uploads ---------------------------------------------------------------------------------------------------- */

static esp_err_t send_staged(httpd_req_t *req, const um98x_flash_pkg_facts_t *facts, uint32_t expires_in_s)
{
    station_snapshot(&s_st);
    web_staged_snap_t s = {
        .facts = *facts,
        .expires_in_s = expires_in_s,
        .st = &s_st,
        .expected_downtime_s = station_expected_flash_downtime_s(facts->size),
        .budget_left_s = web_budget_now(),
        .flash_available = WEB_FLASH_AVAILABLE,
    };
    web_resp_t resp;
    resp_begin(&resp, req, 200);
    web_build_staged(&resp.w, &s);
    return web_resp_end(&resp);
}

static esp_err_t h_upload_gnss_get(web_req_t *r)
{
    um98x_flash_pkg_facts_t facts;
    uint32_t left = 0;
    if (!web_staged_get(&facts, &left)) {
        return web_send_err(r->req, WEB_ERR_NOTHING_STAGED);
    }
    return send_staged(r->req, &facts, left);
}

static esp_err_t h_upload_gnss_confirm(web_req_t *r)
{
    uint8_t sha[32];
    const char *fw = NULL;
    uint32_t build = 0;
    bool ack = false;
    web_bad_t b = { 0 };
    if (!web_parse_upload_confirm(r->body, sha, &fw, &build, &ack, &b)) {
        return bad(r->req, &b);
    }
    um98x_flash_pkg_facts_t facts;
    if (!web_staged_get(&facts, NULL)) {
        return web_send_err(r->req, WEB_ERR_NOTHING_STAGED);
    }
    if (memcmp(sha, facts.sha256, sizeof(sha)) != 0) {
        return web_send_err(r->req, WEB_ERR_SHA256_MISMATCH);
    }
    station_snapshot(&s_st);
    if (s_st.receiver.valid && s_st.receiver.build != 0 && build < s_st.receiver.build) {
        return web_send_err(r->req, WEB_ERR_DOWNGRADE);
    }
    if (fw_update_get_status(&s_fw) != ESP_OK) {
        return web_send_internal(r->req, ESP_FAIL);
    }
    uint32_t retry = UINT32_MAX;
    web_err_t e = gnss_block(&s_fw, &retry);
    if (e != WEB_ERR_COUNT && e != WEB_ERR_PACKAGE_PINNED) {
        return web_send_error(r->req, e, NULL, NULL, retry, NULL, NULL);
    }
    station_flash_request_t req;
    memset(&req, 0, sizeof(req));
    memcpy(req.sha256, sha, sizeof(req.sha256));
    req.size = facts.size;
    strlcpy(req.expected_fw, fw, sizeof(req.expected_fw));
    req.expected_build = build;
    req.cross_layout_confirmed = ack;
    esp_err_t err = station_request_flash(&req);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        /* Receiver flashing is not enabled in this build: the package is
         * checked against every rule instead (nothing reaches the receiver),
         * then released by the station. */
        err = station_request_package_check(&req);
        if (err == ESP_OK) {
            web_staged_forget();
            ESP_LOGI(TAG, "uploaded receiver package: checking it (flashing is not enabled in this build)");
            return web_send_json_literal(r->req, 202, "{\"accepted\":true,\"check_only\":true}");
        }
    }
    switch (err) {
    case ESP_OK:
        web_staged_forget();
        ESP_LOGW(TAG, "receiver update with the uploaded package started by the user");
        return accepted(r->req);
    case ESP_ERR_NOT_FOUND:
        web_staged_forget();
        return web_send_err(r->req, WEB_ERR_NOTHING_STAGED);
    case ESP_ERR_INVALID_STATE:
        if (fw_update_get_status(&s_fw) == ESP_OK && (e = gnss_block(&s_fw, &retry)) != WEB_ERR_COUNT) {
            return web_send_error(r->req, e, NULL, NULL, retry, NULL, NULL);
        }
        return web_send_err(r->req, WEB_ERR_BUSY);
    default:
        return web_send_internal(r->req, err);
    }
}

static esp_err_t h_upload_gnss_delete(web_req_t *r)
{
    if (!web_staged_get(NULL, NULL)) {
        return web_send_err(r->req, WEB_ERR_NOTHING_STAGED);
    }
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) == ESP_OK && info.pinned) {
        return web_send_err(r->req, WEB_ERR_PACKAGE_PINNED);
    }
    esp_err_t err = pkg_store_release();
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    web_staged_forget();
    ESP_LOGI(TAG, "uploaded receiver package cancelled");
    return ok(r->req);
}

static esp_err_t h_upload_app(web_req_t *r)
{
    return web_upload_app(r);
}

static esp_err_t h_upload_gnss(web_req_t *r)
{
    return web_upload_gnss(r);
}

/* ---- diagnostics ---------------------------------------------------------------------------------------------- */

static esp_err_t h_health(web_req_t *r)
{
    health_status_t health;
    if (health_get_status(&health) != ESP_OK) {
        memset(&health, 0, sizeof(health));
        health.rtcm_gap_ms = UINT32_MAX;
        health.factory_reset_refused_age_s = UINT32_MAX;
    }
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    size_t nboots = LIFECYCLE_BOOT_HISTORY_LEN;
    if (lifecycle_get_boot_history(s_boots, &nboots) != ESP_OK) {
        nboots = 0;
    }
    if (gnss_link_get_stats(&s_link) != ESP_OK) {
        memset(&s_link, 0, sizeof(s_link));
        s_link.last_rx_age_ms = UINT32_MAX;
        s_link.last_rtcm_age_ms = UINT32_MAX;
    }
    rtcm_hub_stats_t hub;
    rtcm_hub_get_stats(&hub);
    station_snapshot(&s_st);
    if (gnss_obs_get_summary(&s_obs) != ESP_OK) {
        memset(&s_obs, 0, sizeof(s_obs));
    }
    if (network_get_status(&s_net) != ESP_OK) {
        memset(&s_net, 0, sizeof(s_net));
        s_net.probe_age_s = UINT32_MAX;
    }
    diag_coredump_info_t cd;
    if (diag_coredump_info(&cd) != ESP_OK) {
        memset(&cd, 0, sizeof(cd));
    }
    diag_heap_info_t heap;
    diag_heap_stats(&heap);
    diag_journal_info_t journal;
    diag_journal_info(&journal);
    diag_log_stats_t log;
    diag_log_stats(&log);
    const char *by = NULL;
    if (!lifecycle_reboot_inhibited(&by)) {
        by = NULL;
    }
    web_health_snap_t h = {
        .health = &health,
        .boot = &boot,
        .uptime_s = web_now_s(),
        .history = s_boots,
        .history_count = nboots,
        .link = &s_link,
        .hub = &hub,
        .st = &s_st,
        .obs = &s_obs,
        .net = &s_net,
        .coredump = &cd,
        .heap = &heap,
        .journal = &journal,
        .log = &log,
        .inhibited_by = by,
        .update_slot = lifecycle_update_active(),
    };
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_health(&resp.w, &h);
    return web_resp_end(&resp);
}

static esp_err_t h_availability(web_req_t *r)
{
    bool have_cfg = load_configs();
    size_t n = slot_count();
    int prot = -1;
    for (size_t i = 0; have_cfg && i < n; i++) {
        if (s_cfg[i].protected_slot && prot < 0) {
            prot = (int)i;
        }
    }
    if (prot >= 0 && ntrip_server_get_status((size_t)prot, &s_cst[prot]) != ESP_OK) {
        memset(&s_cst[prot], 0, sizeof(s_cst[prot]));
    }
    ledger_day_t today;
    bool today_ok = ledger_get_today(&today) == ESP_OK;
    size_t days = CONFIG_LEDGER_HISTORY_DAYS < HISTORY_DAYS_MAX ? CONFIG_LEDGER_HISTORY_DAYS : HISTORY_DAYS_MAX;
    if (ledger_get_history(s_days, &days) != ESP_OK) {
        days = 0;
    }
    web_avail_snap_t a = {
        .clock = ledger_clock_source(),
        .second_of_day = ledger_utc_seconds_of_day(),
        .slots = n,
        .cfg = have_cfg ? s_cfg : NULL,
        .protected_slot = prot,
        .protected_status = prot >= 0 ? &s_cst[prot] : NULL,
        .down_today_s = ledger_protected_down_today_s(),
        .budget_left_s = web_budget_now(),
        .today_valid = today_ok,
        .today = &today,
        .history = s_days,
        .history_count = days,
    };
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_availability(&resp.w, &a);
    return web_resp_end(&resp);
}

static esp_err_t h_tasks(web_req_t *r)
{
    size_t n = TASKS_MAX;
    esp_err_t err = diag_task_stats(s_tasks, &n);
    if (err != ESP_OK && err != ESP_ERR_INVALID_SIZE) {   /* INVALID_SIZE: the first n tasks */
        n = 0;
    }
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_tasks(&resp.w, s_tasks, n);
    return web_resp_end(&resp);
}

static esp_err_t h_log_level(web_req_t *r)
{
    const char *tag = NULL;
    esp_log_level_t level = ESP_LOG_INFO;
    web_bad_t b = { 0 };
    if (!web_parse_log_level(r->body, &tag, &level, &b)) {
        return bad(r->req, &b);
    }
    esp_err_t err = diag_set_log_level(tag, level);
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    return ok(r->req);
}

static uint32_t query_u32(const char *query, const char *key)
{
    char v[12];
    if (!web_query_value(query, key, v, sizeof(v))) {
        return 0;
    }
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 10);
    return (end != v && *end == '\0') ? (uint32_t)n : 0;
}

static esp_err_t h_logs(web_req_t *r)
{
    char v[8];
    bool prev = web_query_value(r->query, "boot", v, sizeof(v)) && strcmp(v, "prev") == 0;
    if (web_query_value(r->query, "format", v, sizeof(v)) && strcmp(v, "text") == 0) {
        return web_download_log_text(r, prev);
    }
    uint32_t cursor = query_u32(r->query, "cursor");
    diag_log_read_info_t info = { 0 };
    diag_log_src_t src = prev ? DIAG_LOG_PREV_BOOT : DIAG_LOG_THIS_BOOT;
    size_t n = diag_log_read_ex(src, &cursor, s_text, sizeof(s_text), &info);
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_logs(&resp.w, cursor, info.more, info.gap, s_text, n);
    return web_resp_end(&resp);
}

static esp_err_t h_journal(web_req_t *r)
{
    uint32_t cursor = query_u32(r->query, "cursor");
    size_t n = JOURNAL_PAGE;
    if (diag_journal_read(&cursor, s_journal, &n) != ESP_OK) {
        n = 0;
    }
    diag_journal_info_t info;
    diag_journal_info(&info);
    bool more = n == JOURNAL_PAGE && info.newest_seq != 0 && (int32_t)(info.newest_seq - cursor) > 0;
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_journal(&resp.w, s_journal, n, cursor, more);
    return web_resp_end(&resp);
}

/*
 * The connection history: the newest WEB_HISTORY_MAX caster session ends and
 * returns, Wi-Fi drops and returns, read backwards through the journal one
 * page at a time.
 */
static esp_err_t h_connections(web_req_t *r)
{
    diag_journal_info_t info;
    diag_journal_info(&info);
    size_t n = 0;
    bool complete = info.available && info.newest_seq == 0;   /* an empty journal holds nothing more */
    if (info.available && info.newest_seq != 0 && info.oldest_seq != 0 && info.newest_seq >= info.oldest_seq) {
        uint32_t hi = info.newest_seq;
        for (;;) {
            uint32_t lo = hi - info.oldest_seq + 1u > JOURNAL_PAGE ? hi - (JOURNAL_PAGE - 1u) : info.oldest_seq;
            uint32_t cursor = lo - 1u;
            size_t got = hi - lo + 1u;
            if (diag_journal_read(&cursor, s_journal, &got) != ESP_OK) {
                break;
            }
            bool room = web_history_collect(s_journal, got, hi, s_hist, &n, WEB_HISTORY_MAX);
            if (lo <= info.oldest_seq) {
                complete = room;
                break;
            }
            if (!room) {
                break;
            }
            hi = lo - 1u;
        }
    }
    bool have_cfg = load_configs();
    web_resp_t resp;
    resp_begin(&resp, r->req, 200);
    web_build_connections(&resp.w, s_hist, n, have_cfg ? s_cfg : NULL, slot_count(), complete);
    return web_resp_end(&resp);
}

static esp_err_t h_coredump_get(web_req_t *r)
{
    return web_download_coredump(r);
}

static esp_err_t h_flash_capture(web_req_t *r)
{
    return web_download_flash_capture(r);
}

static esp_err_t h_coredump_delete(web_req_t *r)
{
    diag_coredump_info_t cd;
    if (diag_coredump_info(&cd) != ESP_OK || !cd.present) {
        return web_send_err(r->req, WEB_ERR_NO_COREDUMP);
    }
    esp_err_t err = diag_coredump_erase();
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    return ok(r->req);
}

/* ---- system actions -------------------------------------------------------------------------------------------- */

static esp_timer_handle_t s_reboot_timer;
static lifecycle_reboot_reason_t s_reboot_reason;

static void on_reboot_timer(void *arg)
{
    (void)arg;
    (void)lifecycle_request_reboot(s_reboot_reason);
}

static esp_err_t schedule_reboot(lifecycle_reboot_reason_t reason)
{
    if (s_reboot_timer == NULL) {
        const esp_timer_create_args_t args = { .callback = on_reboot_timer, .name = "web_reboot" };
        esp_err_t err = esp_timer_create(&args, &s_reboot_timer);
        if (err != ESP_OK) {
            return err;
        }
    }
    s_reboot_reason = reason;
    (void)esp_timer_stop(s_reboot_timer);
    return esp_timer_start_once(s_reboot_timer, (uint64_t)REBOOT_DELAY_MS * 1000u);
}

static esp_err_t h_reboot(web_req_t *r)
{
    const char *by = NULL;
    if (lifecycle_reboot_inhibited(&by)) {
        return web_send_error(r->req, WEB_ERR_REBOOT_INHIBITED, NULL, NULL, UINT32_MAX, by, NULL);
    }
    esp_err_t err = schedule_reboot(LIFECYCLE_REBOOT_USER);
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGW(TAG, "reboot requested by the user");
    char body[64];
    snprintf(body, sizeof(body), "{\"accepted\":true,\"reboot_in_ms\":%u}", (unsigned)REBOOT_DELAY_MS);
    return web_send_json_literal(r->req, 202, body);
}

static esp_err_t h_factory_reset(web_req_t *r)
{
    web_bad_t b = { 0 };
    if (!web_parse_confirm(r->body, &b)) {
        return bad(r->req, &b);
    }
    station_snapshot(&s_st);
    if (web_journal_view(&s_st, lifecycle_in_safe_mode()) == WEB_JOURNAL_RUNNING) {
        return web_send_err(r->req, WEB_ERR_JOURNAL_OPEN);   /* allowed while it waits for the user */
    }
    const char *by = NULL;
    if (lifecycle_reboot_inhibited(&by)) {
        return web_send_error(r->req, WEB_ERR_REBOOT_INHIBITED, NULL, NULL, UINT32_MAX, by, NULL);
    }
    if (!settings_ok()) {
        return web_send_err(r->req, WEB_ERR_SETTINGS_UNAVAILABLE);
    }
    esp_err_t err = settings_factory_reset();
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGW(TAG, "factory reset by the user: rebooting into first setup");
    err = schedule_reboot(LIFECYCLE_REBOOT_FACTORY_RESET);
    if (err != ESP_OK) {
        (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_FACTORY_RESET);
    }
    return accepted(r->req);
}

static esp_err_t h_safe_mode_clear(web_req_t *r)
{
    if (!lifecycle_in_safe_mode()) {
        return web_send_err(r->req, WEB_ERR_NOT_IN_SAFE_MODE);
    }
    const char *by = NULL;
    if (lifecycle_reboot_inhibited(&by)) {
        return web_send_error(r->req, WEB_ERR_REBOOT_INHIBITED, NULL, NULL, UINT32_MAX, by, NULL);
    }
    esp_err_t err = web_worker_clear_safe_mode();
    if (err != ESP_OK) {
        return web_send_internal(r->req, err);
    }
    ESP_LOGW(TAG, "leaving safe mode at the user's request");
    return accepted(r->req);
}

/* ---- routes ------------------------------------------------------------------------------------------------------ */

#define ROUTE(u, m, a, b, f) { .uri = (u), .method = (m), .access = (a), .body = (b), .max_body = 0, .fn = (f) }

const web_route_t web_api_routes[] = {
    ROUTE("/api/session", HTTP_GET, WEB_ACC_PUB, WEB_BODY_NONE, h_session_get),
    ROUTE("/api/session", HTTP_POST, WEB_ACC_PUB, WEB_BODY_JSON, h_session_post),
    ROUTE("/api/session", HTTP_DELETE, WEB_ACC_ADMIN, WEB_BODY_NONE, h_session_delete),
    ROUTE("/api/setup/admin", HTTP_POST, WEB_ACC_CLAIM, WEB_BODY_JSON, h_setup_admin),
    ROUTE("/api/setup/done", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_setup_done),
    ROUTE("/api/admin-password", HTTP_PUT, WEB_ACC_ADMIN, WEB_BODY_JSON, h_admin_password),
    ROUTE("/api/status", HTTP_GET, WEB_ACC_PUBSTAR, WEB_BODY_NONE, h_status),
    ROUTE("/api/gnss", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_gnss),
    ROUTE("/api/gnss/profile", HTTP_PUT, WEB_ACC_ADMIN, WEB_BODY_JSON, h_gnss_profile),
    ROUTE("/api/gnss/apply", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_gnss_apply),
    ROUTE("/api/gnss/position/lock", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_gnss_lock),
    ROUTE("/api/gnss/position/lock", HTTP_DELETE, WEB_ACC_ADMIN, WEB_BODY_NONE, h_gnss_unlock),
    ROUTE("/api/gnss/signals", HTTP_GET, WEB_ACC_PUBSTAR, WEB_BODY_NONE, h_gnss_signals),
    ROUTE("/api/sky", HTTP_GET, WEB_ACC_PUBSTAR, WEB_BODY_NONE, h_sky),
    ROUTE("/api/gnss/maintenance", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_maint_post),
    ROUTE("/api/gnss/maintenance", HTTP_DELETE, WEB_ACC_ADMIN, WEB_BODY_NONE, h_maint_delete),
    ROUTE("/api/gnss/live", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_live_get),
    ROUTE("/api/gnss/live", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_live_post),
    ROUTE("/api/gnss/live", HTTP_DELETE, WEB_ACC_ADMIN, WEB_BODY_NONE, h_live_delete),
    ROUTE("/api/gnss/flash", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_flash_get),
    ROUTE("/api/gnss/flash/retry", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_flash_retry),
    ROUTE("/api/gnss/flash/restart-receiver", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_flash_restart),
    ROUTE("/api/gnss/flash/clear", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_flash_clear),
    ROUTE("/api/casters", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_casters),
    ROUTE("/api/casters/*", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_caster_get),
    { .uri = "/api/casters/*", .method = HTTP_PUT, .access = WEB_ACC_ADMIN, .body = WEB_BODY_JSON,
      .max_body = WEB_JSON_MAX_CA, .fn = h_caster_put },
    ROUTE("/api/casters/*", HTTP_DELETE, WEB_ACC_ADMIN, WEB_BODY_NONE, h_caster_delete),
    ROUTE("/api/wifi", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_wifi_get),
    ROUTE("/api/wifi", HTTP_PUT, WEB_ACC_ADMIN, WEB_BODY_JSON, h_wifi_put),
    ROUTE("/api/wifi/scan", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_wifi_scan),
    ROUTE("/api/wifi/pending/retry", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_wifi_pending_retry),
    ROUTE("/api/wifi/pending/discard", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_wifi_pending_discard),
    ROUTE("/api/system", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_system_get),
    ROUTE("/api/system", HTTP_PUT, WEB_ACC_ADMIN, WEB_BODY_JSON, h_system_put),
    ROUTE("/api/update", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_update_get),
    ROUTE("/api/update/check", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_update_check),
    ROUTE("/api/update/app", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_update_app),
    ROUTE("/api/update/gnss", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_update_gnss),
    ROUTE("/api/update/rollback", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_update_rollback),
    ROUTE("/api/upload/app", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_UPLOAD, h_upload_app),
    ROUTE("/api/upload/gnss", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_UPLOAD, h_upload_gnss),
    ROUTE("/api/upload/gnss", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_upload_gnss_get),
    ROUTE("/api/upload/gnss", HTTP_DELETE, WEB_ACC_ADMIN, WEB_BODY_NONE, h_upload_gnss_delete),
    ROUTE("/api/upload/gnss/confirm", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_upload_gnss_confirm),
    ROUTE("/api/health", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_health),
    ROUTE("/api/availability", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_availability),
    ROUTE("/api/diag/tasks", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_tasks),
    ROUTE("/api/diag/flash-capture", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_flash_capture),
    ROUTE("/api/diag/log-level", HTTP_PUT, WEB_ACC_ADMIN, WEB_BODY_JSON, h_log_level),
    ROUTE("/api/logs", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_logs),
    ROUTE("/api/journal", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_journal),
    ROUTE("/api/connections", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_connections),
    ROUTE("/api/coredump", HTTP_GET, WEB_ACC_ADMIN, WEB_BODY_NONE, h_coredump_get),
    ROUTE("/api/coredump", HTTP_DELETE, WEB_ACC_ADMIN, WEB_BODY_NONE, h_coredump_delete),
    ROUTE("/api/system/reboot", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_reboot),
    ROUTE("/api/system/factory-reset", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_factory_reset),
    ROUTE("/api/system/safe-mode/clear", HTTP_POST, WEB_ACC_ADMIN, WEB_BODY_JSON, h_safe_mode_clear),
};

const size_t web_api_route_count = sizeof(web_api_routes) / sizeof(web_api_routes[0]);

bool web_api_path_known(const char *path)
{
    size_t slot = 0;
    bool ca = false;
    if (parse_slot_path(path, &slot, &ca)) {
        return true;
    }
    for (size_t i = 0; i < web_api_route_count; i++) {
        if (strcmp(web_api_routes[i].uri, path) == 0) {
            return true;
        }
    }
    return strcmp(path, "/ws") == 0;
}
