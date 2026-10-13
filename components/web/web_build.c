/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_build.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "um98x_profile.h"
#include "web_enums.h"
#include "web_parse.h"

/* esp_ota_ops.h is not part of the host build; web.c checks this value. */
#define WEB_ESP_ERR_OTA_VALIDATE_FAILED 0x1503

/* ---- session ------------------------------------------------------------------------------ */

void web_build_session(wj_t *w, const web_session_snap_t *s)
{
    wj_obj_open(w, NULL);
    wj_int(w, "api", WEB_API_VERSION);
    wj_bool(w, "authenticated", s->authenticated);
    wj_bool(w, "admin_set", s->admin_set);
    wj_bool(w, "setup_done", s->setup_done);
    wj_bool(w, "claim_allowed", s->claim_allowed);
    wj_bool(w, "on_setup_ap", s->on_setup_ap);
    wj_bool(w, "public_status", s->public_status);
    wj_bool(w, "settings_available", s->settings_available);
    if (s->authenticated) {
        wj_uint(w, "idle_left_s", s->idle_left_s);
    } else {
        wj_null(w, "idle_left_s");
    }
    /* Not new disclosures: DHCP, mDNS and the AP's beacon carry them anyway. */
    wj_str(w, "hostname", s->hostname != NULL ? s->hostname : "");
    wj_str_or_null(w, "mdns_host", s->mdns_host);
    wj_str_or_null(w, "ap_ssid", s->ap_ssid);
    wj_str(w, "setup_step", s->authenticated && !s->setup_done ? web_setup_step_name(s->setup_step) : NULL);
    wj_obj_close(w);
}

/* ---- receiver update session view ---------------------------------------------------- */

web_journal_t web_journal_view(const station_status_t *st, bool safe_mode)
{
    if (st->flash_journal_settings_unavailable) {
        return WEB_JOURNAL_UNKNOWN;
    }
    if (!st->flash_journal_open) {
        return WEB_JOURNAL_NONE;
    }
    if (!st->flash_journal_readable) {
        return WEB_JOURNAL_UNREADABLE;
    }
    if (st->flash_session_running && !safe_mode) {
        /* A retry, restart or recovery was accepted: its record still says
         * "needs user" until the session's first write. */
        return WEB_JOURNAL_RUNNING;
    }
    if (safe_mode || st->flash_journal.state == UM98X_JOURNAL_NEEDS_USER || st->flash.phase == UM98X_FLASH_NEEDS_USER) {
        return WEB_JOURNAL_WAITING;
    }
    return WEB_JOURNAL_RUNNING;
}

const char *web_journal_name(web_journal_t j)
{
    static const char *const names[] = { "none", "running", "waiting", "unreadable", "unknown" };
    return (unsigned)j < sizeof(names) / sizeof(names[0]) ? names[j] : "unknown";
}

static const char *refuse(web_err_t *err, web_err_t e, const char *why)
{
    if (err != NULL) {
        *err = e;
    }
    return why;
}

const char *web_flash_action_why(const web_flash_ctx_t *c, web_flash_action_t a, web_err_t *err)
{
    switch (c->journal) {
    case WEB_JOURNAL_NONE:
        if (a == WEB_FLASH_RESTART_RECEIVER && c->receiver_in_loader) {
            if (c->safe_mode) {
                return refuse(err, WEB_ERR_SAFE_MODE, "safe_mode");
            }
            if (!c->station_running) {
                return refuse(err, WEB_ERR_NOT_WAITING, "not_waiting");
            }
            if (c->maintenance) {
                return refuse(err, WEB_ERR_RECEIVER_MAINTENANCE, "receiver_maintenance");
            }
            return NULL;
        }
        return refuse(err, WEB_ERR_NO_JOURNAL, "no_journal");
    case WEB_JOURNAL_UNKNOWN:
        return refuse(err, WEB_ERR_SETTINGS_UNAVAILABLE, "settings_unavailable");
    case WEB_JOURNAL_RUNNING:
        return refuse(err, WEB_ERR_NOT_WAITING, "not_waiting");
    case WEB_JOURNAL_UNREADABLE:
        if (a == WEB_FLASH_CLEAR) {
            return NULL;
        }
        return refuse(err, WEB_ERR_JOURNAL_OPEN, "unreadable");
    case WEB_JOURNAL_WAITING:
    default:
        break;
    }
    if (a == WEB_FLASH_CLEAR) {
        return NULL;
    }
    if (c->safe_mode) {
        return refuse(err, WEB_ERR_SAFE_MODE, "safe_mode");
    }
    if (c->maintenance) {
        return refuse(err, WEB_ERR_RECEIVER_MAINTENANCE, "receiver_maintenance");
    }
    if (a == WEB_FLASH_RETRY) {
        if (c->app_trial) {
            return refuse(err, WEB_ERR_APP_TRIAL, "app_trial");
        }
        if (c->app_update) {
            return refuse(err, WEB_ERR_APP_UPDATE, "app_update");
        }
    }
    if (!c->flash_available) {
        return refuse(err, WEB_ERR_NOT_AVAILABLE_YET, "not_available_yet");
    }
    return NULL;
}

/* ---- shared pieces --------------------------------------------------------------------- */

static void err_name(wj_t *w, const char *key, esp_err_t err)
{
    if (err == ESP_OK) {
        wj_null(w, key);
    } else {
        wj_str(w, key, esp_err_to_name(err));
    }
}

static bool dev_build(const char *version)
{
    return version == NULL || version[0] == '\0' || strchr(version, '+') != NULL;
}

static void receiver_info(wj_t *w, const char *key, const um98x_info_t *r)
{
    wj_obj_open(w, key);
    wj_bool(w, "valid", r->valid);
    if (r->valid) {
        wj_str(w, "model", r->model);
        wj_str_or_null(w, "firmware", r->firmware);
        if (r->build != 0) {
            wj_uint(w, "build", r->build);
        } else {
            wj_null(w, "build");
        }
        wj_str_or_null(w, "serial", r->serial);
        wj_str_or_null(w, "pn", r->pn);
        wj_str_or_null(w, "efuse_id", r->efuse_id);
        wj_str_or_null(w, "compile_date", r->compile_date);
    }
    wj_obj_close(w);
}

static void station_alerts(wj_t *w, const char *key, uint32_t alerts)
{
    wj_arr_open(w, key);
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((alerts & bit) != 0) {
            const char *name = web_station_alert_name(bit);
            if (name != NULL) {
                wj_str(w, NULL, name);
            }
        }
    }
    wj_arr_close(w);
}

/* A bounded char array as a string (its terminator, or the array's end). */
static void str_arr(wj_t *w, const char *key, const char *s, size_t cap)
{
    wj_strn(w, key, s, strnlen(s, cap));
}

static void str_arr_or_null(wj_t *w, const char *key, const char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    if (n == 0) {
        wj_null(w, key);
    } else {
        wj_strn(w, key, s, n);
    }
}

static void ladder_triggers(wj_t *w, const char *key, uint32_t bits)
{
    wj_arr_open(w, key);
    for (uint32_t bit = 1; bit != 0 && bit < 0x100; bit <<= 1) {
        if ((bits & bit) != 0) {
            const char *name = web_ladder_trigger_name(bit);
            if (name != NULL) {
                wj_str(w, NULL, name);
            }
        }
    }
    wj_arr_close(w);
}

/* "no_ip", "auth_failed", or NULL (null) when the credentials were not reverted. */
static const char *cred_revert_name(const network_status_t *n)
{
    if (!n->cred_reverted) {
        return NULL;
    }
    switch (n->cred_reverted_why) {
    case NETWORK_CRED_REVERT_NO_IP:
        return "no_ip";
    case NETWORK_CRED_REVERT_AUTH:
        return "auth_failed";
    default:
        return NULL;
    }
}

/* Changed Wi-Fi credentials on trial, pending, or reverted (status network section and /api/wifi). */
static void cred_fields(wj_t *w, const network_status_t *n)
{
    wj_bool(w, "cred_trial", n->cred_trial);
    wj_uint(w, "cred_trial_left_s", n->cred_trial ? n->cred_trial_left_s : 0);
    wj_bool(w, "cred_pending", n->cred_pending);
    str_arr_or_null(w, "pending_ssid", (n->cred_pending || n->cred_trial) ? n->pending_ssid : "", sizeof(n->pending_ssid));
    wj_bool(w, "cred_reverted", n->cred_reverted);
    wj_str(w, "cred_reverted_why", cred_revert_name(n));
}

/* Seconds until the AP closes: null while it is down or nothing is scheduled. */
static void ap_close_in(wj_t *w, const char *key, const network_status_t *n)
{
    wj_u32_or_null(w, key, n->setup_ap_active ? n->ap_close_in_s : UINT32_MAX);
}

/* ---- status ------------------------------------------------------------------------------- */

static const char *const s_section_names[WEB_SEC_COUNT] = {
    [WEB_SEC_DEVICE] = "device",
    [WEB_SEC_SAFE_MODE] = "safe_mode",
    [WEB_SEC_NETWORK] = "network",
    [WEB_SEC_RECEIVER] = "receiver",
    [WEB_SEC_HEALTH] = "health",
    [WEB_SEC_ALERTS] = "alerts",
    [WEB_SEC_AVAILABILITY] = "availability",
    [WEB_SEC_UPDATE] = "update",
};

const char *web_section_name(web_section_t sec)
{
    return (unsigned)sec < WEB_SEC_COUNT ? s_section_names[sec] : "unknown";
}

bool web_section_public(web_section_t sec)
{
    return sec == WEB_SEC_DEVICE || sec == WEB_SEC_RECEIVER || sec == WEB_SEC_HEALTH;
}

/* Index of the enabled protected slot that is set up (Onocoy: mountpoint, username, password), -1 if none. */
static int protected_slot(const web_status_snap_t *s)
{
    if (!s->cfg_valid) {
        return -1;
    }
    for (size_t i = 0; i < s->slots && i < WEB_MAX_SLOTS; i++) {
        if (s->cfg[i].enabled && s->cfg[i].protected_slot && ntrip_server_onocoy_ready(&s->cfg[i])) {
            return (int)i;
        }
    }
    return -1;
}

/* The protected slot's day figures (status summary and caster status): null unless they mean something. */
static void day_figures(wj_t *w, const ntrip_caster_status_t *t, bool prot)
{
    bool v = prot && t->day_metrics_valid;
    if (v) {
        wj_uint(w, "graced_uptime_permille", t->graced_uptime_permille);
        wj_bool(w, "streak_ok", t->streak_ok);
    } else {
        wj_null(w, "graced_uptime_permille");
        wj_null(w, "streak_ok");
    }
    if (v && t->so_far_valid) {
        wj_uint(w, "graced_uptime_so_far_permille", t->graced_uptime_so_far_permille);
    } else {
        wj_null(w, "graced_uptime_so_far_permille");
    }
    if (v && t->data_rate_valid) {
        wj_uint(w, "data_rate_permille", t->data_rate_permille);
    } else {
        wj_null(w, "data_rate_permille");
    }
    if (v) {
        wj_uint(w, "budget_s", t->budget_s);
        wj_uint(w, "budget_left_s", t->budget_left_s);
    } else {
        wj_null(w, "budget_s");
        wj_null(w, "budget_left_s");
    }
}

uint32_t web_budget_left_s(const web_status_snap_t *s)
{
    if (s->clock == LEDGER_CLOCK_NONE) {
        return UINT32_MAX;
    }
    int p = protected_slot(s);
    if (p < 0) {
        return UINT32_MAX;
    }
    if (s->cst[p].state != NTRIP_STATE_DISABLED) {
        return s->cst[p].budget_left_s;
    }
    return s->ledger_budget_left_s;
}

static void sec_device(wj_t *w, const web_status_snap_t *s, bool pub)
{
    wj_obj_open(w, "device");
    wj_str(w, "hostname", s->hostname);
    if (!pub) {
        wj_str(w, "version", s->version);
        wj_bool(w, "dev_build", dev_build(s->version));
        if (s->clock == LEDGER_CLOCK_NONE) {
            wj_null(w, "time_unix");
        } else {
            wj_unix(w, "time_unix", s->time_unix);
        }
        wj_str(w, "time_source", web_clock_name(s->clock));
        wj_bool(w, "settings_available", s->settings_available);
    }
    wj_obj_close(w);
}

static void sec_safe_mode(wj_t *w, const web_status_snap_t *s)
{
    wj_obj_open(w, "safe_mode");
    wj_bool(w, "active", s->boot.safe_mode);
    if (s->boot.safe_mode) {
        uint32_t at = s->boot.safe_mode_retry_s;
        wj_uint(w, "retry_in_s", at > s->uptime_s ? at - s->uptime_s : 0);
    } else {
        wj_null(w, "retry_in_s");
    }
    wj_uint(w, "crashes", s->boot.crashes);
    wj_obj_close(w);
}

static void ap_reasons(wj_t *w, const char *key, bool active, uint32_t reasons)
{
    wj_arr_open(w, key);
    if (active) {
        for (uint32_t bit = 1; bit != 0 && bit < 0x100; bit <<= 1) {
            if ((reasons & bit) != 0) {
                const char *name = web_ap_reason_name(bit);
                if (name != NULL) {
                    wj_str(w, NULL, name);
                }
            }
        }
    }
    wj_arr_close(w);
}

static void sec_network(wj_t *w, const web_status_snap_t *s)
{
    const network_status_t *n = &s->net;
    wj_obj_open(w, "network");
    wj_bool(w, "online", n->online);
    wj_str_or_null(w, "ssid", n->sta_configured ? n->ssid : NULL);
    wj_str_or_null(w, "ip", n->online ? n->ip : NULL);
    if (n->associated) {
        wj_int(w, "rssi", n->rssi);
    } else {
        wj_null(w, "rssi");
    }
    wj_str(w, "upstream", web_probe_name(n->probe));
    /* In whole minutes: the section (sent when it changes) then changes at most once a minute. */
    wj_u32_or_null(w, "probe_age_s", n->probe_age_s == UINT32_MAX ? UINT32_MAX : n->probe_age_s / 60u * 60u);
    wj_uint(w, "offline_s", n->offline_s);
    wj_bool(w, "ap_active", n->setup_ap_active);
    ap_reasons(w, "ap_reasons", n->setup_ap_active, n->ap_reasons);
    ap_close_in(w, "ap_close_in_s", n);
    str_arr_or_null(w, "mdns_host", n->mdns_host, sizeof(n->mdns_host));
    wj_uint(w, "assoc_no_ip_s", n->assoc_no_ip_s);
    ladder_triggers(w, "ladder_triggers", n->ladder_triggers);
    cred_fields(w, n);
    wj_obj_close(w);
}

static void sg_pair(wj_t *w, const char *key, station_sg_t g)
{
    wj_obj_open(w, key);
    wj_uint(w, "master", g.master);
    wj_uint(w, "slave", g.slave);
    wj_obj_close(w);
}

/* receiver.signal_test: a running comparison, or one that ended this boot
 * less than a day ago (its outcome); null otherwise. The countdowns are
 * rounded (10 s, 60 s), so the section changes rarely. */
#define WEB_SGT_SHOW_ENDED_S 86400u

static void signal_test_brief(wj_t *w, const station_sgt_brief_t *b)
{
    if (b->state == STATION_SGT_IDLE && (b->outcome == STATION_SGT_OUT_NONE || b->ended_age_s >= WEB_SGT_SHOW_ENDED_S)) {
        wj_null(w, "signal_test");
        return;
    }
    wj_obj_open(w, "signal_test");
    wj_str(w, "state", web_sgt_state_name(b->state));
    if (b->phase >= 0) {
        wj_uint(w, "phase", (uint64_t)b->phase);
    } else {
        wj_null(w, "phase");
    }
    if (b->have_running) {
        sg_pair(w, "group", b->running);
    } else {
        wj_null(w, "group");
    }
    wj_u32_or_null(w, "left_s", b->left_s);
    wj_u32_or_null(w, "decide_left_s", b->decide_left_s);
    wj_str(w, "outcome", web_sgt_outcome_name(b->outcome));
    wj_obj_close(w);
}

static void sec_receiver(wj_t *w, const web_status_snap_t *s, bool pub)
{
    const station_status_t *st = &s->st;
    bool have_best = s->obs_valid && s->best_bands > 0;
    wj_obj_open(w, "receiver");
    wj_str(w, "state", web_station_state_name(st->state));
    if (!pub) {
        wj_str_or_null(w, "model", st->receiver.valid ? st->receiver.model : NULL);
        wj_str_or_null(w, "firmware", st->receiver.valid ? st->receiver.firmware : NULL);
        wj_bool(w, "responding", st->receiver_responding);
        wj_bool(w, "identity_confirmed", st->identity_confirmed);
        wj_bool(w, "profile_applied", st->profile_applied);
        wj_bool(w, "position_locked", st->position_locked);
        wj_bool(w, "surveying", st->surveying);
        wj_uint(w, "survey_window_left_s", st->survey_window_left_s);
        wj_u32_or_null(w, "rtcm_age_ms", s->rtcm_age_ms);
        wj_str(w, "best_sys", have_best ? web_sys_name(s->best_sys) : NULL);
    }
    wj_uint(w, "best_bands", s->obs_valid ? s->best_bands : 0);
    if (!pub) {
        wj_str(w, "journal", web_journal_name(web_journal_view(st, s->boot.safe_mode)));
        wj_bool(w, "uart_closed", s->uart_closed);
        wj_bool(w, "rx_maintenance", st->rx_maintenance);
        wj_uint(w, "maintenance_left_s", st->rx_maintenance ? st->maintenance_left_s : 0);
        wj_uint(w, "maintenance_chosen_s", st->rx_maintenance ? st->maintenance_chosen_s : 0);
        wj_u32_or_null(w, "next_detect_in_s", st->next_detect_in_s);
        wj_bool(w, "first_apply_held", st->first_apply_held);
        signal_test_brief(w, &st->sg_test);
    }
    wj_obj_close(w);
}

/* Why the GNSS ladder's ESP32 restart waits now, when the last of those
 * reasons ends at the latest (null: setup holds, which has no limit), and
 * the reboot cap's wait (status health and /api/health). Health reports
 * them only during a ladder episode. */
static void reboot_hold_fields(wj_t *w, const health_status_t *h)
{
    static const char *const names[] = { "setup", "ap_client", "admin", "power_on" };
    wj_arr_open(w, "reboot_hold");
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if ((h->gnss_reboot_hold & (1u << i)) != 0) {
            wj_str(w, NULL, names[i]);
        }
    }
    wj_arr_close(w);
    wj_u32_or_null(w, "reboot_hold_left_s", h->gnss_reboot_hold != 0 ? h->gnss_reboot_hold_left_s : UINT32_MAX);
    wj_u32_or_null(w, "reboot_cap_wait_s", h->gnss_reboot_cap_wait_s);
    /* Which of the cap's limits sets that wait (the later one when both hold). */
    const char *limit = NULL;
    if (h->gnss_reboot_cap_wait_s != UINT32_MAX) {
        limit = h->gnss_reboot_cap_limit == HEALTH_CAP_DAY ? "day" : "hour";
    }
    wj_str(w, "reboot_cap_limit", limit);
}

static void sec_health(wj_t *w, const web_status_snap_t *s, bool pub)
{
    wj_obj_open(w, "health");
    wj_str(w, "level", web_health_level_name(s->health.level));
    if (!pub) {
        wj_str(w, "last_step", web_health_step_name(s->health.last_step));
        wj_bool(w, "ladder_suspended", s->health.gnss_ladder_suspended);
        /* A bool (not the age), so the section only changes when it flips. */
        wj_bool(w, "factory_reset_refused", s->health.factory_reset_refused_age_s < WEB_FACTORY_REFUSED_SHOW_S);
        wj_str(w, "recovery_mode", web_recovery_mode_name(s->health.recovery_mode));
        wj_bool(w, "reboot_recheck", s->health.gnss_reboot_recheck);
        reboot_hold_fields(w, &s->health);
    }
    wj_obj_close(w);
}

static void sec_alerts(wj_t *w, const web_status_snap_t *s)
{
    wj_arr_open(w, "alerts");
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((s->health.alerts & bit) != 0) {
            const char *name = web_health_alert_name(bit);
            if (name != NULL) {
                wj_obj_open(w, NULL);
                wj_str(w, "id", name);
                wj_str(w, "source", "health");
                wj_obj_close(w);
            }
        }
    }
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((s->st.alerts & bit) != 0) {
            const char *name = web_station_alert_name(bit);
            if (name != NULL) {
                wj_obj_open(w, NULL);
                wj_str(w, "id", name);
                wj_str(w, "source", "station");
                if (bit == STATION_ALERT_CONFIG_FAILED) {
                    wj_uint(w, "retry_in_s", s->st.config_retry_in_s);
                    wj_bool(w, "auto_retry", s->st.config_auto_retry);
                } else if (bit == STATION_ALERT_NO_RECEIVER) {
                    wj_bool(w, "saw_bytes", s->st.detect_saw_bytes);
                    wj_u32_or_null(w, "next_detect_in_s", s->st.next_detect_in_s);
                }
                wj_obj_close(w);
            }
        }
    }
    wj_arr_close(w);
}

static void sec_availability(wj_t *w, const web_status_snap_t *s)
{
    wj_obj_open(w, "availability");
    wj_uint(w, "protected_down_today_s", s->protected_down_today_s);
    wj_u32_or_null(w, "budget_left_s", web_budget_left_s(s));
    wj_obj_close(w);
}

static void sec_update(wj_t *w, const web_status_snap_t *s)
{
    const fw_update_status_t *f = &s->fw;
    wj_obj_open(w, "update");
    wj_bool(w, "app_available", f->app_update_available);
    wj_str_or_null(w, "app_version", f->latest_version);
    wj_str(w, "app_reverted_version", f->reverted_version);
    wj_bool(w, "app_reverted_in_stage1", f->reverted_in_stage1);
    wj_bool(w, "gnss_available", f->gnss_update_available);
    if (f->gnss_latest_build != 0) {
        wj_uint(w, "gnss_build", f->gnss_latest_build);
    } else {
        wj_null(w, "gnss_build");
    }
    wj_str_or_null(w, "gnss_version", f->gnss_latest);
    wj_str(w, "gnss_blocked_reason", web_gnss_block_name(f->gnss_blocked));
    wj_uint(w, "gnss_blocked_left_s", f->gnss_blocked_left_s);
    wj_bool(w, "gnss_flash_available", s->flash_available);
    wj_str(w, "gnss_flash_reason", s->flash_available ? NULL : "not_enabled_in_build");
    wj_bool(w, "checking", f->state == FW_UPDATE_CHECKING);
    wj_unix(w, "last_check_unix", f->last_check_unix);
    wj_obj_close(w);
}

void web_build_section(wj_t *w, const web_status_snap_t *s, web_section_t sec, bool pub)
{
    if (pub && !web_section_public(sec)) {
        return;
    }
    switch (sec) {
    case WEB_SEC_DEVICE:
        sec_device(w, s, pub);
        break;
    case WEB_SEC_SAFE_MODE:
        sec_safe_mode(w, s);
        break;
    case WEB_SEC_NETWORK:
        sec_network(w, s);
        break;
    case WEB_SEC_RECEIVER:
        sec_receiver(w, s, pub);
        break;
    case WEB_SEC_HEALTH:
        sec_health(w, s, pub);
        break;
    case WEB_SEC_ALERTS:
        sec_alerts(w, s);
        break;
    case WEB_SEC_AVAILABILITY:
        sec_availability(w, s);
        break;
    case WEB_SEC_UPDATE:
        sec_update(w, s);
        break;
    default:
        break;
    }
}

bool web_caster_slot_empty(const ntrip_caster_cfg_t *c)
{
    return c != NULL && c->host[0] == '\0' && !c->enabled;
}

void web_build_caster_summary(wj_t *w, const web_status_snap_t *s, size_t slot, bool pub)
{
    if (slot >= WEB_MAX_SLOTS) {
        return;
    }
    const ntrip_caster_cfg_t *c = s->cfg_valid ? &s->cfg[slot] : NULL;
    const ntrip_caster_status_t *t = &s->cst[slot];
    bool prot = c != NULL && c->protected_slot;
    bool empty = web_caster_slot_empty(c);
    if (pub && empty) {
        return;   /* the public form lists configured slots only */
    }
    wj_obj_open(w, NULL);
    wj_uint(w, "slot", slot);
    if (empty) {
        wj_str(w, "name", "");   /* whatever name a removed slot still has means nothing */
    } else {
        wj_str_or_null(w, "name", c != NULL ? c->name : NULL);
    }
    wj_str(w, "state", web_ntrip_state_name(t->state));
    if (!pub) {
        wj_bool(w, "empty", empty);
        wj_bool(w, "enabled", c != NULL && c->enabled);
        wj_bool(w, "protected", prot);
        wj_str_or_null(w, "host", c != NULL ? c->host : NULL);
        wj_str(w, "reason", web_ntrip_fail_name(t->reason));
        wj_uint(w, "state_age_s", t->state_age_s);
        wj_uint(w, "connected_s", t->connected_s);
        wj_uint(w, "today_online_s", t->today_online_s);
        wj_uint(w, "today_offline_s", t->today_offline_s);
        day_figures(w, t, prot);
        wj_bool(w, "onocoy_ready", c != NULL && ntrip_server_onocoy_ready(c));
        wj_uint(w, "bytes_sent", t->bytes_sent);
        wj_uint(w, "lag_ms_now", t->lag_ms_now);
        wj_uint(w, "next_retry_s", t->next_retry_s);
        wj_uint(w, "unreached_s", t->unreached_s);
        wj_str(w, "hint", t->hint);
        wj_str(w, "stop_reason", web_ntrip_stop_name(t->stop_reason));
    }
    wj_obj_close(w);
}

void web_build_status(wj_t *w, const web_status_snap_t *s, bool pub)
{
    wj_obj_open(w, NULL);
    wj_int(w, "api", WEB_API_VERSION);
    wj_bool(w, "public", pub);
    wj_uint(w, "uptime_s", s->uptime_s);
    web_build_section(w, s, WEB_SEC_DEVICE, pub);
    web_build_section(w, s, WEB_SEC_SAFE_MODE, pub);
    web_build_section(w, s, WEB_SEC_NETWORK, pub);
    web_build_section(w, s, WEB_SEC_RECEIVER, pub);
    wj_arr_open(w, "casters");
    for (size_t i = 0; i < s->slots && i < WEB_MAX_SLOTS; i++) {
        web_build_caster_summary(w, s, i, pub);
    }
    wj_arr_close(w);
    web_build_section(w, s, WEB_SEC_HEALTH, pub);
    web_build_section(w, s, WEB_SEC_ALERTS, pub);
    web_build_section(w, s, WEB_SEC_AVAILABILITY, pub);
    web_build_section(w, s, WEB_SEC_UPDATE, pub);
    wj_obj_close(w);
}

/* ---- receiver --------------------------------------------------------------------------------- */

void web_build_profile(wj_t *w, const char *key, const um98x_base_profile_t *p)
{
    bool fixed = p->base_mode == UM98X_BASE_FIXED;
    wj_obj_open(w, key);
    wj_uint(w, "signal_group_master", p->signal_group_master);
    wj_uint(w, "signal_group_slave", p->signal_group_slave);
    wj_str(w, "base_mode", web_base_mode_name(p->base_mode));
    wj_uint(w, "survey_in_s", fixed ? 0 : p->survey_in_s);
    if (fixed) {
        wj_obj_open(w, "fixed");
        wj_double(w, "lat_deg", p->fixed_lat_deg, 9);
        wj_double(w, "lon_deg", p->fixed_lon_deg, 9);
        wj_double(w, "height_m", p->fixed_height_m, 4);
        wj_obj_close(w);
    } else {
        wj_null(w, "fixed");
    }
    wj_uint(w, "station_id", p->station_id);
    wj_uint(w, "com1_baud", p->com1_baud);
    wj_arr_open(w, "rtcm");
    for (size_t i = 0; i < p->rtcm_count && i < UM98X_MAX_RTCM_MSGS; i++) {
        wj_obj_open(w, NULL);
        wj_uint(w, "msg", p->rtcm[i].msg);
        wj_uint(w, "period_s", p->rtcm[i].period_s);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_bool(w, "sat_view_output", p->sat_view_output);
    str_arr(w, "antenna_model", p->antenna_model, sizeof(p->antenna_model));
    str_arr(w, "antenna_serial", p->antenna_serial, sizeof(p->antenna_serial));
    wj_uint(w, "antenna_setup_id", p->antenna_setup_id);
    wj_obj_close(w);
}

/* The receiver's settings as read back at the last apply session this boot (unknown before one). */
static void rx_settings(wj_t *w, const station_status_t *st)
{
    const um98x_rx_settings_t *r = &st->rx_settings;
    bool v = st->rx_settings_valid;
    wj_obj_open(w, "rx_settings");
    wj_bool(w, "valid", v);
    if (v) {
        wj_uint(w, "age_s", st->rx_settings_age_s);
    } else {
        wj_null(w, "age_s");
    }
    wj_unix(w, "read_unix", v ? st->rx_settings_unix : 0);
    wj_str(w, "b1cb2a", web_setting_name(v ? r->b1cb2a : (uint8_t)UM98X_SETTING_UNKNOWN));
    wj_str(w, "mmp", web_setting_name(v ? r->mmp : (uint8_t)UM98X_SETTING_UNKNOWN));
    if (v && r->mask_known) {
        wj_double(w, "mask_deg", (double)r->mask_ddeg / 10.0, 1);
    } else {
        wj_null(w, "mask_deg");
    }
    if (v && r->antenna_known) {
        wj_obj_open(w, "antenna");
        str_arr(w, "model", r->antenna_model, sizeof(r->antenna_model));
        str_arr(w, "serial", r->antenna_serial, sizeof(r->antenna_serial));
        wj_uint(w, "setup_id", r->antenna_setup_id);
        wj_obj_close(w);
    } else {
        wj_null(w, "antenna");
    }
    wj_obj_close(w);
}

static void apply_step(wj_t *w, const char *key, bool sent, bool ok, const char *reply, size_t cap)
{
    wj_obj_open(w, key);
    wj_bool(w, "sent", sent);
    wj_bool(w, "ok", sent && ok);
    str_arr_or_null(w, "reply", (sent && !ok) ? reply : "", cap);
    wj_obj_close(w);
}

/* The latest 1033 the receiver published, as decoded (nothing rewritten). */
static void rtcm1033(wj_t *w, const gnss_obs_1033_t *d)
{
    wj_obj_open(w, "rtcm1033");
    wj_bool(w, "valid", d->valid);
    if (d->valid) {
        wj_uint(w, "station_id", d->station_id);
        str_arr(w, "antenna", d->antenna, sizeof(d->antenna));
        wj_uint(w, "setup_id", d->setup_id);
        str_arr(w, "antenna_serial", d->antenna_serial, sizeof(d->antenna_serial));
        str_arr(w, "receiver_type", d->receiver_type, sizeof(d->receiver_type));
        str_arr(w, "receiver_fw", d->receiver_fw, sizeof(d->receiver_fw));
        str_arr(w, "receiver_serial", d->receiver_serial, sizeof(d->receiver_serial));
        wj_u32_or_null(w, "age_ms", d->age_ms);
    }
    wj_uint(w, "bad_frames", d->bad_frames);
    wj_obj_close(w);
}

static void maintenance(wj_t *w, const station_status_t *st)
{
    static const uint32_t choices[] = WEB_MAINT_CHOICES_S;
    wj_obj_open(w, "maintenance");
    wj_bool(w, "on", st->rx_maintenance);
    wj_uint(w, "left_s", st->rx_maintenance ? st->maintenance_left_s : 0);
    wj_uint(w, "chosen_s", st->rx_maintenance ? st->maintenance_chosen_s : 0);
    wj_arr_open(w, "choices_s");
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); i++) {
        wj_uint(w, NULL, choices[i]);
    }
    wj_arr_close(w);
    wj_uint(w, "default_s", WEB_MAINT_DEFAULT_S);
    wj_uint(w, "max_left_s", WEB_MAINT_MAX_LEFT_S);
    wj_obj_close(w);
}

void web_build_gnss(wj_t *w, const web_gnss_snap_t *g)
{
    const station_status_t *st = g->st;
    wj_obj_open(w, NULL);
    wj_str(w, "state", web_station_state_name(st->state));
    receiver_info(w, "receiver", &st->receiver);
    wj_bool(w, "responding", st->receiver_responding);
    wj_bool(w, "identity_confirmed", st->identity_confirmed);
    station_alerts(w, "alerts", st->alerts);
    wj_bool(w, "profile_applied", st->profile_applied);
    if (st->rtcm1230_known) {
        wj_bool(w, "rtcm1230_supported", st->rtcm1230_supported);
    } else {
        wj_null(w, "rtcm1230_supported");   /* never applied: neither accepted nor refused */
    }
    wj_bool(w, "config_failed", st->config_failed);
    wj_uint(w, "config_retry_in_s", st->config_retry_in_s);
    wj_bool(w, "config_auto_retry", st->config_auto_retry);
    wj_bool(w, "reconfigure_refused", st->reconfigure_refused);
    wj_uint(w, "detect_failures", st->detect_failures);

    wj_obj_open(w, "link");
    wj_uint(w, "baud", g->link.baud);
    wj_str(w, "mode", web_link_mode_name((int)g->link.mode));
    wj_uint(w, "rx_bytes", g->link.rx_bytes);
    wj_u32_or_null(w, "rx_age_ms", g->link.last_rx_age_ms);
    wj_u32_or_null(w, "rtcm_age_ms", g->link.last_rtcm_age_ms);
    wj_bool(w, "open", g->link.link_open);
    wj_uint(w, "open_failures", g->link.open_failures);
    wj_bool(w, "hands_off", g->link.hands_off);
    wj_obj_close(w);

    wj_bool(w, "autocfg", g->autocfg);
    wj_str(w, "recovery", web_recovery_mode_name(g->recovery));
    wj_bool(w, "first_apply_held", st->first_apply_held);
    web_build_profile(w, "profile", &g->profile);
    rx_settings(w, st);
    wj_obj_open(w, "apply_steps");
    apply_step(w, "antenna", st->antenna_sent, st->antenna_ok, st->antenna_reply, sizeof(st->antenna_reply));
    apply_step(w, "b1cb2a", st->b1cb2a_sent, st->b1cb2a_ok, st->b1cb2a_reply, sizeof(st->b1cb2a_reply));
    wj_obj_close(w);
    rtcm1033(w, &g->rtcm1033);
    maintenance(w, st);
    if (st->rx_sg_valid) {
        sg_pair(w, "rx_signal_group", st->rx_sg);
    } else {
        wj_null(w, "rx_signal_group");
    }
    if (st->sg_refused) {
        wj_obj_open(w, "signal_group_refused");
        wj_uint(w, "master", st->sg_refused_group.master);
        wj_uint(w, "slave", st->sg_refused_group.slave);
        wj_strn(w, "reply", st->sg_refused_reply, strnlen(st->sg_refused_reply, sizeof(st->sg_refused_reply)));
        if (st->sg_back_valid) {
            sg_pair(w, "back_to", st->sg_back);
        } else {
            wj_null(w, "back_to");
        }
        wj_obj_close(w);
    } else {
        wj_null(w, "signal_group_refused");
    }

    const um98x_base_position_t *pos = &st->position;
    bool have_pos = pos->source != UM98X_POS_NONE;
    wj_obj_open(w, "position");
    wj_bool(w, "locked", st->position_locked);
    wj_str(w, "source", web_pos_source_name(pos->source));
    if (have_pos) {
        wj_double(w, "lat_deg", pos->lat_deg, 9);
        wj_double(w, "lon_deg", pos->lon_deg, 9);
        wj_double(w, "height_m", pos->height_m, 4);
        wj_double(w, "sigma_m", pos->sigma_m, 4);
    } else {
        wj_null(w, "lat_deg");
        wj_null(w, "lon_deg");
        wj_null(w, "height_m");
        wj_null(w, "sigma_m");
    }
    wj_unix(w, "locked_unix", pos->locked_unix);
    wj_obj_close(w);

    wj_obj_open(w, "survey");
    wj_bool(w, "active", st->surveying);
    wj_uint(w, "window_left_s", st->survey_window_left_s);
    wj_obj_close(w);

    wj_obj_open(w, "arp");
    wj_bool(w, "valid", g->arp.valid);
    if (g->arp.valid) {
        double xyz[3];
        for (int i = 0; i < 3; i++) {
            xyz[i] = (double)g->arp.ecef_01mm[i] / 10000.0;
        }
        wj_uint(w, "station_id", g->arp.station_id);
        wj_arr_open(w, "ecef_m");
        for (int i = 0; i < 3; i++) {
            wj_double(w, NULL, xyz[i], 4);
        }
        wj_arr_close(w);
        double lat = NAN;
        double lon = NAN;
        double h = NAN;
        bool llh = um98x_ecef_to_llh(xyz, &lat, &lon, &h);
        wj_double(w, "lat_deg", llh ? lat : NAN, 9);
        wj_double(w, "lon_deg", llh ? lon : NAN, 9);
        wj_double(w, "height_m", llh ? h : NAN, 4);
        wj_u32_or_null(w, "age_ms", g->arp.age_ms);
        if (st->position_locked && have_pos) {
            double ref[3];
            um98x_llh_to_ecef(pos->lat_deg, pos->lon_deg, pos->height_m, ref);
            double dx = xyz[0] - ref[0];
            double dy = xyz[1] - ref[1];
            double dz = xyz[2] - ref[2];
            wj_double(w, "offset_m", sqrt(dx * dx + dy * dy + dz * dz), 4);
        } else {
            wj_null(w, "offset_m");
        }
    }
    wj_obj_close(w);
    wj_obj_close(w);
}

/* Epoch-gap counters: gaps are only counted against a configured MSM period. */
static void obs_gaps(wj_t *w, const gnss_obs_summary_t *sum)
{
    if (sum->epoch_period_ms != 0) {
        wj_uint(w, "epoch_gaps", sum->epoch_gaps);
        wj_uint(w, "epoch_period_ms", sum->epoch_period_ms);
    } else {
        wj_null(w, "epoch_gaps");
        wj_null(w, "epoch_period_ms");
    }
    wj_uint(w, "epoch_jumps", sum->epoch_jumps);
}

static int profile_period(const um98x_base_profile_t *p, uint16_t msg)
{
    for (size_t i = 0; p != NULL && i < p->rtcm_count && i < UM98X_MAX_RTCM_MSGS; i++) {
        if (p->rtcm[i].msg == msg) {
            return p->rtcm[i].period_s;
        }
    }
    return -1;
}

static void msg_entry(wj_t *w, uint16_t msg, const gnss_obs_msg_stat_t *stat, int expected)
{
    wj_obj_open(w, NULL);
    wj_uint(w, "msg", msg);
    wj_uint(w, "count", stat != NULL ? stat->count : 0);
    wj_uint(w, "frames", stat != NULL ? stat->frames : 0);
    if (stat != NULL) {
        wj_u32_or_null(w, "period_ms", stat->period_ms);
        wj_u32_or_null(w, "age_ms", stat->age_ms);
    } else {
        wj_null(w, "period_ms");
        wj_null(w, "age_ms");
    }
    if (expected >= 0) {
        wj_int(w, "expected_period_s", expected);
    } else {
        wj_null(w, "expected_period_s");
    }
    wj_obj_close(w);
}

void web_build_signals(wj_t *w, const gnss_obs_summary_t *sum, const gnss_obs_msg_stat_t *stats, size_t nstats,
                       const um98x_base_profile_t *profile, int rtcm1230_supported)
{
    wj_obj_open(w, NULL);
    wj_str(w, "best_sys", sum->best_bands > 0 ? web_sys_name(sum->best_sys) : NULL);
    wj_uint(w, "best_bands", sum->best_bands);
    wj_bool(w, "e6_lost", sum->e6_lost);
    wj_u32_or_null(w, "epoch_age_ms", sum->epoch_age_ms);
    wj_uint(w, "epochs", sum->epochs);
    obs_gaps(w, sum);
    wj_arr_open(w, "systems");
    for (int s = 0; s < GNSS_SYS_COUNT; s++) {
        const gnss_obs_sys_t *o = &sum->sys[s];
        if (!o->present) {
            continue;
        }
        wj_obj_open(w, NULL);
        wj_str(w, "sys", web_sys_name((gnss_sys_t)s));
        wj_uint(w, "satellites", o->satellites);
        wj_arr_open(w, "bands");
        for (int b = 0; b < GNSS_BAND_COUNT; b++) {
            if ((o->band_mask & (1u << b)) != 0) {
                wj_str(w, NULL, web_band_name((gnss_band_t)b));
            }
        }
        wj_arr_close(w);
        wj_arr_open(w, "signal_ids");
        for (unsigned id = 1; id <= 32; id++) {
            if ((o->signal_mask & (1u << (id - 1))) != 0) {
                wj_uint(w, NULL, id);
            }
        }
        wj_arr_close(w);
        wj_obj_open(w, "cnr_dbhz");
        for (int b = 0; b < GNSS_BAND_COUNT; b++) {
            if ((o->band_mask & (1u << b)) == 0) {
                continue;
            }
            if (o->cnr_dbhz[b] != 0) {
                wj_uint(w, web_band_name((gnss_band_t)b), o->cnr_dbhz[b]);
            } else {
                wj_null(w, web_band_name((gnss_band_t)b));
            }
        }
        wj_obj_close(w);
        wj_obj_close(w);
    }
    wj_arr_close(w);

    if (stats == NULL) {
        wj_null(w, "messages");
    } else {
        wj_arr_open(w, "messages");
        /* The profile's messages first, in its order, then anything else observed. */
        for (size_t i = 0; profile != NULL && i < profile->rtcm_count && i < UM98X_MAX_RTCM_MSGS; i++) {
            uint16_t msg = profile->rtcm[i].msg;
            const gnss_obs_msg_stat_t *found = NULL;
            for (size_t k = 0; k < nstats; k++) {
                if (stats[k].msg == msg) {
                    found = &stats[k];
                    break;
                }
            }
            msg_entry(w, msg, found, profile->rtcm[i].period_s);
        }
        for (size_t k = 0; k < nstats; k++) {
            if (profile_period(profile, stats[k].msg) < 0) {
                msg_entry(w, stats[k].msg, &stats[k], -1);
            }
        }
        wj_arr_close(w);
    }
    if (rtcm1230_supported < 0) {
        wj_null(w, "rtcm1230_supported");
    } else {
        wj_bool(w, "rtcm1230_supported", rtcm1230_supported != 0);
    }
    wj_obj_close(w);
}

/* The satellite's own PRN: gnss_obs numbers QZSS and SBAS as in the MSM
 * satellite mask (PRN - 192, PRN - 119); GLONASS stays its slot number. */
unsigned web_sky_prn(gnss_sys_t sys, uint8_t prn)
{
    switch (sys) {
    case GNSS_SYS_QZSS:
        return prn + 192u;
    case GNSS_SYS_SBAS:
        return prn + 119u;
    default:
        return prn;
    }
}

/* ---- signal-group comparison ------------------------------------------------------------------ */

static void sgt_result(wj_t *w, const station_sgt_result_t *r)
{
    wj_obj_open(w, NULL);
    sg_pair(w, "group", r->group);
    wj_str(w, "status", web_sgt_result_name(r->status));
    wj_uint(w, "measured_s", r->measured_s);
    wj_uint(w, "samples", r->samples);
    wj_uint(w, "epoch_gaps", r->epoch_gaps);
    wj_str(w, "best_sys", r->best_sys < GNSS_SYS_COUNT ? web_sys_name((gnss_sys_t)r->best_sys) : NULL);
    wj_uint(w, "best_bands", r->best_bands);
    wj_arr_open(w, "systems");
    for (unsigned i = 0; i < r->nsys && i < STATION_SGT_SYS; i++) {
        const station_sgt_sys_t *sy = &r->sys[i];
        wj_obj_open(w, NULL);
        wj_str(w, "sys", web_sys_name((gnss_sys_t)sy->sys));
        wj_double(w, "satellites", sy->sats_x10 / 10.0, 1);
        wj_arr_open(w, "bands");
        for (unsigned b = 0; b < sy->nband && b < STATION_SGT_BANDS; b++) {
            wj_str(w, NULL, web_band_name((gnss_band_t)sy->band[b].band));
        }
        wj_arr_close(w);
        wj_arr_open(w, "per_band");
        for (unsigned b = 0; b < sy->nband && b < STATION_SGT_BANDS; b++) {
            const station_sgt_band_t *e = &sy->band[b];
            wj_obj_open(w, NULL);
            wj_str(w, "band", web_band_name((gnss_band_t)e->band));
            wj_double(w, "satellites", e->sats_x10 / 10.0, 1);
            if (e->cnr_x10 != 0) {
                wj_double(w, "cnr_dbhz", e->cnr_x10 / 10.0, 1);
            } else {
                wj_null(w, "cnr_dbhz");
            }
            wj_arr_open(w, "signal_ids");
            for (unsigned id = 1; id <= 32; id++) {
                if ((e->signal_mask & (1u << (id - 1u))) != 0) {
                    wj_uint(w, NULL, id);
                }
            }
            wj_arr_close(w);
            wj_obj_close(w);
        }
        wj_arr_close(w);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

void web_build_signal_test(wj_t *w, const station_sg_test_t *v)
{
    wj_obj_open(w, NULL);
    wj_str(w, "state", web_sgt_state_name(v->state));
    if (v->have_test) {
        wj_arr_open(w, "groups");
        sg_pair(w, NULL, v->groups[0]);
        sg_pair(w, NULL, v->groups[1]);
        wj_arr_close(w);
        sg_pair(w, "before", v->before);
    } else {
        wj_null(w, "groups");
        wj_null(w, "before");
    }
    if (v->phase >= 0) {
        wj_uint(w, "phase", (uint64_t)v->phase);
    } else {
        wj_null(w, "phase");
    }
    wj_uint(w, "phase_s", v->phase_s);
    wj_uint(w, "settle_s", v->settle_s);
    wj_u32_or_null(w, "left_s", v->left_s);
    wj_u32_or_null(w, "decide_left_s", v->decide_left_s);
    if (v->have_running) {
        sg_pair(w, "running_group", v->running);
    } else {
        wj_null(w, "running_group");
    }
    if (v->have_test) {
        wj_arr_open(w, "results");
        sgt_result(w, &v->results[0]);
        sgt_result(w, &v->results[1]);
        wj_arr_close(w);
    } else {
        wj_null(w, "results");
    }
    wj_str(w, "outcome", web_sgt_outcome_name(v->outcome));
    if (v->have_kept) {
        sg_pair(w, "kept", v->kept);
    } else {
        wj_null(w, "kept");
    }
    wj_strn(w, "reason", v->reason, strnlen(v->reason, sizeof(v->reason)));
    wj_unix(w, "ended_unix", v->ended_unix);
    wj_obj_close(w);
}

void web_build_sky(wj_t *w, const gnss_obs_sat_t *sats, size_t count, const char *reason)
{
    wj_obj_open(w, NULL);
    wj_bool(w, "available", reason == NULL);
    wj_str(w, "reason", reason);
    wj_arr_open(w, "satellites");
    for (size_t i = 0; reason == NULL && sats != NULL && i < count && i < 64; i++) {
        wj_obj_open(w, NULL);
        wj_str(w, "sys", web_sys_name(sats[i].sys));
        wj_uint(w, "prn", web_sky_prn(sats[i].sys, sats[i].prn));
        wj_int(w, "el_deg", sats[i].elevation_deg);
        wj_uint(w, "az_deg", sats[i].azimuth_deg);
        if (sats[i].cnr_dbhz != 0) {
            wj_uint(w, "cnr_dbhz", sats[i].cnr_dbhz);
        } else {
            wj_null(w, "cnr_dbhz");
        }
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

/* The flash counters (progress messages and GET /api/gnss/flash). */
static void flash_counters(wj_t *w, const um98x_flash_progress_t *p)
{
    wj_uint(w, "bytes_sent", p->bytes_sent);
    wj_uint(w, "bytes_total", p->bytes_total);
    wj_uint(w, "retries", p->retries);
    wj_uint(w, "transfers", p->transfers);
    wj_str(w, "failure", web_flash_failure_name(p->failure_class));
    wj_uint(w, "frames_acked", p->frames_acked);
    wj_uint(w, "frames_total", p->frames_total);
    wj_uint(w, "frames_sent", p->frames_sent);
    wj_uint(w, "entry_attempts", p->entry_attempts);
    if (p->journal_state != 0) {
        wj_str(w, "journal_state", web_journal_state_name(p->journal_state));
    } else {
        wj_null(w, "journal_state");
    }
}

static void action(wj_t *w, const char *key, const web_flash_ctx_t *c, web_flash_action_t a)
{
    const char *why = web_flash_action_why(c, a, NULL);
    wj_obj_open(w, key);
    wj_bool(w, "allowed", why == NULL);
    wj_str(w, "why", why);
    wj_obj_close(w);
}

void web_build_flash(wj_t *w, const station_status_t *st, const web_flash_ctx_t *ctx, const char *receiver_text)
{
    wj_obj_open(w, NULL);
    wj_str(w, "journal", web_journal_name(ctx->journal));
    if (ctx->journal == WEB_JOURNAL_RUNNING || ctx->journal == WEB_JOURNAL_WAITING) {
        const um98x_flash_journal_t *j = &st->flash_journal;
        wj_obj_open(w, "record");
        wj_str(w, "state", web_journal_state_name(j->state));
        wj_str(w, "phase", web_journal_state_name(j->phase));
        wj_hex(w, "sha256", j->sha256, sizeof(j->sha256));
        wj_uint(w, "size", j->size);
        wj_str_or_null(w, "expected_fw", j->expected_fw);
        wj_uint(w, "expected_build", j->expected_build);
        wj_str_or_null(w, "pre_model", j->pre_model);
        wj_str_or_null(w, "pre_fw", j->pre_fw);
        wj_uint(w, "pre_build", j->pre_build);
        wj_uint(w, "app_baud", j->app_baud);
        wj_uint(w, "boot_baud", j->boot_baud);
        wj_uint(w, "transfers", j->transfers);
        wj_uint(w, "entry_attempts", j->entry_attempts);
        wj_uint(w, "last_acked_block", j->last_acked_block);
        wj_str(w, "failure", web_flash_failure_name(j->failure_class));
        wj_unix(w, "changed_unix", j->changed_unix);
        wj_obj_close(w);
    } else {
        wj_null(w, "record");
    }
    const um98x_flash_progress_t *p = &st->flash;
    wj_obj_open(w, "progress");
    wj_str(w, "phase", web_flash_phase_name(p->phase));
    flash_counters(w, p);
    err_name(w, "last_err", (esp_err_t)p->last_err);
    wj_obj_close(w);
    wj_bool(w, "receiver_in_loader", ctx->receiver_in_loader);
    wj_str(w, "receiver_text", receiver_text != NULL ? receiver_text : "");
    wj_obj_open(w, "actions");
    action(w, "retry", ctx, WEB_FLASH_RETRY);
    action(w, "restart_receiver", ctx, WEB_FLASH_RESTART_RECEIVER);
    action(w, "clear", ctx, WEB_FLASH_CLEAR);
    wj_obj_close(w);
    const station_flash_last_t *l = &st->flash_last;
    if (l->outcome == STATION_FLASH_OUT_NONE) {
        wj_null(w, "last");
    } else {
        wj_obj_open(w, "last");
        wj_str(w, "outcome", web_flash_outcome_name(l->outcome));
        wj_uint(w, "failed_rule", l->failed_rule);
        wj_bool(w, "cross_layout", l->cross_layout);
        wj_bool(w, "check_only", l->check_only);
        wj_bool(w, "reapply_ok", l->reapply_ok);
        wj_str(w, "firmware", l->firmware);
        wj_uint(w, "build", l->build);
        wj_str(w, "reason", l->reason);
        wj_unix(w, "ended_unix", l->ended_unix);
        if (l->ended_age_s == UINT32_MAX) {
            wj_null(w, "ended_age_s");
        } else {
            wj_uint(w, "ended_age_s", l->ended_age_s);
        }
        wj_obj_close(w);
    }
    wj_bool(w, "check_running", st->pkg_check_running);
    wj_bool(w, "flash_available", ctx->flash_available);
    wj_str(w, "flash_reason", ctx->flash_available ? NULL : "not_enabled_in_build");
    wj_obj_close(w);
}

web_err_t web_maint_refusal(station_maint_why_t why, bool safe_mode, const char **field, const char **reason)
{
    const char *f = NULL;
    const char *r = NULL;
    web_err_t e;
    switch (why) {
    case STATION_MAINT_REFUSED_FLASH:
        e = WEB_ERR_JOURNAL_OPEN;
        break;
    case STATION_MAINT_REFUSED_SESSION:
        e = WEB_ERR_BUSY;
        r = "receiver_session";
        break;
    case STATION_MAINT_REFUSED_SKY_OUTPUT:
        e = WEB_ERR_BUSY;
        r = "live_view_output";
        break;
    case STATION_MAINT_REFUSED_DURATION:
        e = WEB_ERR_INVALID_FIELD;
        f = "duration_s";
        break;
    case STATION_MAINT_NOT_ON:
        e = WEB_ERR_NOT_IN_MAINTENANCE;
        break;
    case STATION_MAINT_REFUSED_NOT_RUNNING:
    default:
        e = safe_mode ? WEB_ERR_SAFE_MODE : WEB_ERR_INTERNAL;
        break;
    }
    if (field != NULL) {
        *field = f;
    }
    if (reason != NULL) {
        *reason = r;
    }
    return e;
}

web_err_t web_live_refusal(station_live_why_t why, bool safe_mode)
{
    if (why == STATION_LIVE_WHY_FLASH) {
        return WEB_ERR_JOURNAL_OPEN;
    }
    return safe_mode ? WEB_ERR_SAFE_MODE : WEB_ERR_INTERNAL;
}

/* ---- live view ------------------------------------------------------------------------------------ */

void web_build_live_window(wj_t *w, const char *key, const station_live_status_t *win, const web_live_limits_t *lim)
{
    wj_obj_open(w, key);
    wj_bool(w, "open", win->open);
    wj_uint(w, "left_s", win->open ? win->left_s : 0);
    wj_uint(w, "idle_left_s", win->open ? win->idle_left_s : 0);
    wj_uint(w, "open_s", win->open ? win->open_s : 0);
    wj_uint(w, "max_s", win->max_s);
    wj_uint(w, "default_min", lim->default_min);
    wj_uint(w, "max_min", lim->max_min);
    wj_uint(w, "idle_s", lim->idle_s);
    wj_str(w, "sky", web_live_sky_name(win->sky));
    wj_str(w, "sky_why", web_live_why_name(win->sky_why));
    wj_uint(w, "sky_period_s", win->sky_period_s);
    wj_str(w, "last_end", web_live_end_name(win->last_end));
    str_arr_or_null(w, "sky_reply", win->sky_reply, sizeof(win->sky_reply));
    wj_obj_close(w);
}

static void live_sat(wj_t *w, const gnss_obs_live_sat_t *s)
{
    wj_obj_open(w, NULL);
    wj_str(w, "sys", web_sys_name(s->sys));
    wj_uint(w, "prn", web_sky_prn(s->sys, s->prn));
    wj_bool(w, "in_msm", s->in_msm);
    wj_u32_or_null(w, "msm_age_ms", s->msm_age_ms);
    wj_arr_open(w, "signal_ids");
    for (unsigned id = 1; id <= 32; id++) {
        if ((s->signal_mask & (1u << (id - 1))) != 0) {
            wj_uint(w, NULL, id);
        }
    }
    wj_arr_close(w);
    wj_obj_open(w, "cnr_dbhz");
    for (int b = 0; b < GNSS_BAND_COUNT; b++) {
        if ((s->band_mask & (1u << b)) == 0) {
            continue;
        }
        if (s->cnr_dbhz[b] != 0) {
            wj_uint(w, web_band_name((gnss_band_t)b), s->cnr_dbhz[b]);
        } else {
            wj_null(w, web_band_name((gnss_band_t)b));
        }
    }
    wj_obj_close(w);
    if (s->has_pos) {
        wj_int(w, "el_deg", s->elevation_deg);
        wj_uint(w, "az_deg", s->azimuth_deg);
    } else {
        wj_null(w, "el_deg");
        wj_null(w, "az_deg");
    }
    if (s->sky_cnr_dbhz != 0) {
        wj_uint(w, "sky_cnr_dbhz", s->sky_cnr_dbhz);
    } else {
        wj_null(w, "sky_cnr_dbhz");
    }
    wj_u32_or_null(w, "pos_age_ms", s->has_pos ? s->pos_age_ms : UINT32_MAX);
    wj_obj_close(w);
}

static void live_totals(wj_t *w, const gnss_obs_live_t *t)
{
    wj_obj_open(w, "totals");
    wj_uint(w, "satellites", t->satellites);
    wj_uint(w, "tracked", t->tracked);
    wj_uint(w, "signals", t->signals);
    wj_uint(w, "with_pos", t->with_pos);
    wj_uint(w, "dropped", t->dropped);
    if (t->cnr_dbhz != 0) {
        wj_uint(w, "cnr_dbhz", t->cnr_dbhz);
    } else {
        wj_null(w, "cnr_dbhz");
    }
    wj_u32_or_null(w, "msm_age_ms", t->msm_age_ms);
    wj_u32_or_null(w, "sky_age_ms", t->sky_age_ms);
    wj_obj_open(w, "bands");
    for (int b = 0; b < GNSS_BAND_COUNT; b++) {
        if (t->band_sats[b] == 0) {
            continue;
        }
        wj_obj_open(w, web_band_name((gnss_band_t)b));
        wj_uint(w, "satellites", t->band_sats[b]);
        if (t->band_cnr_dbhz[b] != 0) {
            wj_uint(w, "cnr_dbhz", t->band_cnr_dbhz[b]);
        } else {
            wj_null(w, "cnr_dbhz");
        }
        wj_obj_close(w);
    }
    wj_obj_close(w);
    wj_arr_open(w, "systems");
    for (int sys = 0; sys < GNSS_SYS_COUNT; sys++) {
        if (t->sys_sats[sys] == 0) {
            continue;
        }
        wj_obj_open(w, NULL);
        wj_str(w, "sys", web_sys_name((gnss_sys_t)sys));
        wj_uint(w, "satellites", t->sys_sats[sys]);
        wj_obj_open(w, "bands");
        for (int b = 0; b < GNSS_BAND_COUNT; b++) {
            if (t->sys_band_sats[sys][b] != 0) {
                wj_uint(w, web_band_name((gnss_band_t)b), t->sys_band_sats[sys][b]);
            }
        }
        wj_obj_close(w);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

void web_build_live(wj_t *w, const web_live_snap_t *l)
{
    const station_status_t *st = l->st;
    wj_obj_open(w, NULL);
    web_build_live_window(w, "window", &st->live, &l->limits);
    wj_str(w, "receiver_state", web_station_state_name(st->state));
    wj_bool(w, "rx_maintenance", st->rx_maintenance);
    const um98x_base_position_t *pos = &st->position;
    bool have_pos = pos->source != UM98X_POS_NONE;
    wj_obj_open(w, "position");
    wj_bool(w, "locked", st->position_locked);
    wj_str(w, "source", web_pos_source_name(pos->source));
    wj_double(w, "lat_deg", have_pos ? pos->lat_deg : NAN, 9);
    wj_double(w, "lon_deg", have_pos ? pos->lon_deg : NAN, 9);
    wj_double(w, "height_m", have_pos ? pos->height_m : NAN, 4);
    wj_obj_close(w);
    if (l->totals != NULL) {
        live_totals(w, l->totals);
    } else {
        wj_null(w, "totals");
    }
    wj_arr_open(w, "satellites");
    for (size_t i = 0; l->totals != NULL && l->sats != NULL && i < l->count; i++) {
        live_sat(w, &l->sats[i]);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

/* ---- casters ------------------------------------------------------------------------------------ */

void web_build_caster_config(wj_t *w, const char *key, const ntrip_caster_cfg_t *c)
{
    if (c == NULL) {
        wj_null(w, key);
        return;
    }
    wj_obj_open(w, key);
    wj_bool(w, "enabled", c->enabled);
    wj_str(w, "name", c->name);
    wj_str(w, "host", c->host);
    wj_uint(w, "port", c->port);
    wj_int(w, "ntrip_version", c->proto == NTRIP_PROTO_V2 ? 2 : 1);
    wj_bool(w, "tls", c->tls);
    wj_str(w, "trust", web_ntrip_trust_name(c->trust_mode));
    wj_bool(w, "ca_pem_set", c->ca_pem_set);
    wj_str(w, "mountpoint", c->mountpoint);
    wj_str(w, "username", c->username);
    wj_bool(w, "password_set", c->password_set);
    wj_str(w, "label", c->label);
    wj_bool(w, "v2_raw_body", c->v2_raw_body);
    wj_str(w, "backoff_profile", web_ntrip_backoff_name(c->backoff_profile));
    wj_bool(w, "protected", c->protected_slot);
    wj_bool(w, "dup_stream_ack", c->dup_stream_ack);
    wj_bool(w, "test_onocoy", c->test_onocoy && ntrip_test_onocoy_allowed());
    wj_obj_close(w);
}

void web_build_caster_status(wj_t *w, const char *key, const ntrip_caster_status_t *t, bool prot)
{
    wj_obj_open(w, key);
    wj_str(w, "state", web_ntrip_state_name(t->state));
    wj_str(w, "reason", web_ntrip_fail_name(t->reason));
    wj_uint(w, "state_age_s", t->state_age_s);
    wj_uint(w, "progress_age_ms", t->progress_age_ms);
    wj_uint(w, "connects", t->connects);
    wj_uint(w, "failures", t->failures);
    wj_uint(w, "disconnects", t->disconnects);
    wj_uint(w, "restarts", t->restarts);
    wj_obj_open(w, "failures_by_class");
    for (int i = NTRIP_FAIL_NONE + 1; i < NTRIP_FAIL_CLASS_COUNT; i++) {
        wj_uint(w, web_ntrip_fail_name((ntrip_fail_class_t)i), t->failures_by_class[i]);
    }
    wj_obj_close(w);
    wj_unix(w, "connected_since_unix", t->connected_since);
    wj_uint(w, "connected_s", t->connected_s);
    wj_uint(w, "today_online_s", t->today_online_s);
    wj_uint(w, "today_offline_s", t->today_offline_s);
    day_figures(w, t, prot);
    wj_uint(w, "bytes_sent", t->bytes_sent);
    wj_uint(w, "frames_sent", t->frames_sent);
    wj_uint(w, "epochs_sent", t->epochs_sent);
    wj_uint(w, "epochs_sent_today", t->epochs_sent_today);
    wj_uint(w, "frames_dropped", t->frames_dropped);
    wj_uint(w, "bytes_dropped", t->bytes_dropped);
    wj_uint(w, "epochs_dropped", t->epochs_dropped);
    wj_uint(w, "epochs_dropped_today", t->epochs_dropped_today);
    wj_uint(w, "frames_skipped_idle", t->frames_skipped_idle);
    wj_uint(w, "lag_drops", t->lag_drops);
    wj_uint(w, "laps", t->laps);
    wj_uint(w, "lag_ms_now", t->lag_ms_now);
    wj_uint(w, "max_lag_ms", t->max_lag_ms);
    wj_uint(w, "backoff_attempt", t->backoff_attempt);
    wj_uint(w, "next_retry_s", t->next_retry_s);
    wj_int(w, "last_status_code", t->last_status_code);
    wj_str(w, "last_status_line", t->last_status_line);
    wj_str(w, "server_header", t->server_header);
    wj_str_or_null(w, "resolved_ip", t->resolved_ip);
    wj_uint(w, "dns_cached_used", t->dns_cached_used);
    wj_uint(w, "unreached_s", t->unreached_s);
    wj_str_or_null(w, "tls_version", t->tls_version);
    wj_str_or_null(w, "tls_cipher", t->tls_cipher);
    wj_str(w, "hint", t->hint);
    err_name(w, "last_err", t->last_err);
    wj_str(w, "stop_reason", web_ntrip_stop_name(t->stop_reason));
    wj_uint(w, "login_rejects", t->login_rejects);
    wj_uint(w, "login_blocks", t->login_blocks);
    wj_uint(w, "kicked_sessions", t->kicked_sessions);
    wj_strn(w, "last_session_text", t->last_session_text, strnlen(t->last_session_text, sizeof(t->last_session_text)));
    wj_uint(w, "bytes_sent_today", t->bytes_sent_today);
    wj_obj_close(w);
}

void web_build_caster_entry(wj_t *w, size_t slot, const ntrip_caster_cfg_t *cfg, const ntrip_caster_status_t *st)
{
    wj_obj_open(w, NULL);
    wj_uint(w, "slot", slot);
    web_build_caster_config(w, "config", cfg);
    web_build_caster_status(w, "status", st, cfg != NULL && cfg->protected_slot);
    /* An Onocoy slot with the console's mountpoint, username and password (false for every other slot). */
    wj_bool(w, "onocoy_ready", cfg != NULL && ntrip_server_onocoy_ready(cfg));
    wj_obj_close(w);
}

void web_build_onocoy_preset(wj_t *w, const ntrip_caster_cfg_t *p, uint16_t tls_port)
{
    wj_obj_open(w, "onocoy_preset");
    wj_str(w, "host", p->host);
    wj_uint(w, "port", p->port);
    wj_uint(w, "tls_port", tls_port);
    wj_int(w, "ntrip_version", p->proto == NTRIP_PROTO_V2 ? 2 : 1);
    wj_str(w, "backoff_profile", web_ntrip_backoff_name(p->backoff_profile));
    wj_obj_close(w);
}

/* ---- Wi-Fi, system, updates --------------------------------------------------------------------- */

void web_build_wifi(wj_t *w, const web_wifi_snap_t *s)
{
    const network_status_t *n = s->net;
    wj_obj_open(w, NULL);
    str_arr_or_null(w, "mdns_host", n->mdns_host, sizeof(n->mdns_host));
    wj_bool(w, "configured", n->sta_configured);
    wj_bool(w, "credentials_unreadable", n->sta_credentials_unreadable);
    wj_str_or_null(w, "ssid", n->sta_configured ? n->ssid : NULL);
    wj_bool(w, "online", n->online);
    wj_bool(w, "associated", n->associated);
    wj_str_or_null(w, "ip", n->online ? n->ip : NULL);
    if (n->associated) {
        wj_int(w, "rssi", n->rssi);
        wj_uint(w, "channel", n->channel);
    } else {
        wj_null(w, "rssi");
        wj_null(w, "channel");
    }
    wj_bool(w, "time_synced", n->time_synced);
    wj_str(w, "upstream", web_probe_name(n->probe));
    wj_u32_or_null(w, "probe_age_s", n->probe_age_s);
    wj_uint(w, "reconnects", n->reconnects);
    wj_uint(w, "connect_failures", n->connect_failures);
    wj_uint(w, "offline_s", n->offline_s);
    wj_uint(w, "retry_in_s", n->retry_in_s);
    if (n->last_disconnect_reason != 0) {
        wj_uint(w, "last_disconnect_reason", n->last_disconnect_reason);
    } else {
        wj_null(w, "last_disconnect_reason");
    }
    wj_bool(w, "fallback_dns", n->fallback_dns);
    wj_uint(w, "assoc_no_ip_s", n->assoc_no_ip_s);
    cred_fields(w, n);
    wj_obj_open(w, "ladder");
    wj_bool(w, "active", n->ladder_active);
    wj_uint(w, "rung", n->ladder_rung);
    wj_str(w, "withheld", web_ladder_why_name(n->ladder_withheld));
    ladder_triggers(w, "triggers", n->ladder_triggers);
    wj_uint(w, "actions", n->ladder_actions);
    wj_obj_close(w);
    wj_obj_open(w, "ap");
    wj_bool(w, "active", n->setup_ap_active);
    wj_str_or_null(w, "ssid", n->ap_ssid);
    ap_reasons(w, "reasons", n->setup_ap_active, n->ap_reasons);
    wj_uint(w, "clients", n->ap_clients);
    wj_uint(w, "recovery_left_s", n->recovery_ap_left_s);
    ap_close_in(w, "close_in_s", n);
    wj_bool(w, "password_set", s->ap_password_set);
    wj_obj_close(w);
    wj_obj_open(w, "scan");
    wj_bool(w, "running", s->scan_running);
    wj_u32_or_null(w, "age_s", s->scan_age_s);
    wj_arr_open(w, "results");
    for (size_t i = 0; s->scan != NULL && i < s->scan_count && i < NETWORK_SCAN_MAX; i++) {
        wj_obj_open(w, NULL);
        wj_str(w, "ssid", s->scan[i].ssid);
        wj_int(w, "rssi", s->scan[i].rssi);
        wj_uint(w, "channel", s->scan[i].channel);
        wj_bool(w, "secured", s->scan[i].secured);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_obj_close(w);
    wj_obj_close(w);
}

void web_build_system(wj_t *w, const web_system_snap_t *s)
{
    char mac[18] = "";
    if (s->mac != NULL) {
        snprintf(mac, sizeof(mac), "%02X:%02X:%02X:%02X:%02X:%02X", s->mac[0], s->mac[1], s->mac[2], s->mac[3],
                 s->mac[4], s->mac[5]);
    }
    char rev[24];
    snprintf(rev, sizeof(rev), "v%u.%u", s->chip_revision / 100u, s->chip_revision % 100u);
    wj_obj_open(w, NULL);
    wj_str(w, "hostname", (s->hostname != NULL && s->hostname[0] != '\0') ? s->hostname : s->hostname_active);
    wj_str(w, "hostname_active", s->hostname_active);
    wj_bool(w, "public_status", s->public_status);
    wj_bool(w, "setup_done", s->setup_done);
    wj_bool(w, "settings_available", s->settings_available);
    wj_str(w, "version", s->version);
    wj_str(w, "project", s->project);
    wj_str(w, "idf", s->idf);
    wj_str_or_null(w, "mac", mac);
    wj_obj_open(w, "chip");
    wj_str(w, "model", s->chip_model);
    wj_str(w, "revision", rev);
    wj_uint(w, "flash_mb", s->flash_mb);
    wj_uint(w, "psram_mb", s->psram_mb);
    wj_obj_close(w);
    if (s->reboot_required >= 0) {
        wj_bool(w, "reboot_required", s->reboot_required != 0);
    }
    wj_obj_close(w);
}

static bool is_gnss_job(const char *job)
{
    return job != NULL && strncmp(job, "gnss_", 5) == 0;
}

const char *web_result_code(const char *job, esp_err_t err)
{
    bool app = job != NULL && strncmp(job, "app_", 4) == 0;
    bool check = job != NULL && strcmp(job, "check") == 0;
    switch (err) {
    case ESP_OK:
        return "ok";
    case ESP_ERR_NOT_SUPPORTED:
        return is_gnss_job(job) ? "not_available_yet" : "failed";
    case ESP_ERR_INVALID_STATE:
        return "refused";
    case ESP_ERR_INVALID_CRC:
        return check ? "signature_failed" : "checksum_mismatch";
    case ESP_ERR_NOT_ALLOWED:
        /* fw_update: a hop off the repository's releases on GitHub's release hosts
         * (or off https), refused before connecting. */
        return (check || strcmp(job != NULL ? job : "", "app_update") == 0 ||
                strcmp(job != NULL ? job : "", "gnss_update") == 0)
                   ? "untrusted_host"
                   : "failed";
    case ESP_ERR_INVALID_SIZE:
        return "size_mismatch";
    case ESP_ERR_INVALID_VERSION:
    case WEB_ESP_ERR_OTA_VALIDATE_FAILED:
        return app ? "invalid_image" : (is_gnss_job(job) ? "package_rejected" : "failed");
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_INVALID_RESPONSE:
    case ESP_FAIL:
        return (check || strcmp(job != NULL ? job : "", "app_update") == 0 ||
                strcmp(job != NULL ? job : "", "gnss_update") == 0)
                   ? "download_failed"
                   : "failed";
    default:
        return "failed";
    }
}

void web_build_update(wj_t *w, const web_update_snap_t *u)
{
    const fw_update_status_t *f = u->fw;
    const station_status_t *st = u->st;
    wj_obj_open(w, NULL);
    wj_str(w, "state", web_fw_state_name(f->state));
    wj_uint(w, "progress_pct", f->progress_pct);
    wj_bool(w, "checks_enabled", u->checks_enabled);
    wj_str_or_null(w, "release_repo", u->release_repo);
    wj_bool(w, "signed_manifests", u->signed_manifests);
    wj_unix(w, "last_check_unix", f->last_check_unix);
    /* The last check attempt, successful or not (last_check_unix: the last success). */
    wj_unix(w, "last_attempt_unix", f->last_attempt_unix);
    wj_str(w, "last_check_result", web_check_result_name(f->last_check_result));
    if (f->last_http_status != 0) {
        wj_uint(w, "last_http_status", f->last_http_status);
    } else {
        wj_null(w, "last_http_status");
    }
    wj_u32_or_null(w, "next_check_in_s", f->next_check_in_s);
    if (u->app_max_bytes != 0) {
        wj_uint(w, "app_max_bytes", u->app_max_bytes);
    } else {
        wj_null(w, "app_max_bytes");
    }
    if (u->pkg_max_bytes != 0) {
        wj_uint(w, "pkg_max_bytes", u->pkg_max_bytes);
    } else {
        wj_null(w, "pkg_max_bytes");
    }
    if (u->running_elf_sha256 != NULL) {
        wj_hex(w, "running_elf_sha256", u->running_elf_sha256, 32);
    } else {
        wj_null(w, "running_elf_sha256");
    }
    if (u->last != NULL && u->last->valid) {
        wj_obj_open(w, "last_result");
        wj_str(w, "job", u->last->job);
        wj_str(w, "result", u->last->result);
        err_name(w, "message", u->last->err);
        if (strcmp(u->last->result, "package_rejected") == 0) {
            wj_uint(w, "failed_rule", u->last->failed_rule);
            wj_bool(w, "cross_layout", u->last->cross_layout);
        }
        wj_obj_close(w);
    } else {
        wj_null(w, "last_result");
    }

    wj_obj_open(w, "app");
    wj_str(w, "running", f->running_version);
    wj_bool(w, "dev_build", dev_build(f->running_version));
    wj_str_or_null(w, "latest", f->latest_version);
    wj_bool(w, "available", f->app_update_available);
    wj_bool(w, "latest_rolled_back", f->latest_rolled_back);
    wj_arr_open(w, "rolled_back");
    for (size_t i = 0; u->rolled_back != NULL && i < u->rolled_back_count; i++) {
        wj_str(w, NULL, u->rolled_back[i]);
    }
    wj_arr_close(w);
    wj_str(w, "reverted", f->reverted_version);
    wj_bool(w, "reverted_in_stage1", f->reverted_in_stage1);
    wj_obj_open(w, "trial");
    wj_str(w, "stage", web_fw_trial_name(f->trial));
    wj_uint(w, "left_s", f->trial == FW_TRIAL_NONE ? 0 : f->trial_left_s);
    wj_obj_close(w);
    wj_bool(w, "rollback_available", f->rollback_available);
    wj_obj_close(w);

    bool rx = st != NULL && st->receiver.valid;
    wj_obj_open(w, "gnss");
    wj_str_or_null(w, "model", rx ? st->receiver.model : NULL);
    wj_str_or_null(w, "running", f->gnss_running);
    if (rx && st->receiver.build != 0) {
        wj_uint(w, "running_build", st->receiver.build);
    } else {
        wj_null(w, "running_build");
    }
    wj_str_or_null(w, "latest", f->gnss_latest);
    if (f->gnss_latest_build != 0) {
        wj_uint(w, "latest_build", f->gnss_latest_build);
    } else {
        wj_null(w, "latest_build");
    }
    wj_bool(w, "available", f->gnss_update_available);
    wj_str(w, "blocked", web_gnss_block_name(f->gnss_blocked));
    wj_uint(w, "blocked_left_s", f->gnss_blocked_left_s);
    if (f->gnss_update_available) {
        wj_uint(w, "expected_downtime_s", f->gnss_expected_downtime_s);
    } else {
        wj_null(w, "expected_downtime_s");
    }
    wj_u32_or_null(w, "budget_left_s", u->budget_left_s);
    wj_bool(w, "flash_available", u->flash_available);
    wj_str(w, "flash_reason", u->flash_available ? NULL : "not_enabled_in_build");
    wj_obj_close(w);
    wj_obj_close(w);
}

/* ---- uploads -------------------------------------------------------------------------------------- */

void web_build_staged(wj_t *w, const web_staged_snap_t *s)
{
    const um98x_flash_pkg_facts_t *f = &s->facts;
    wj_obj_open(w, NULL);
    wj_obj_open(w, "staged");
    wj_uint(w, "size", f->size);
    wj_hex(w, "sha256", f->sha256, sizeof(f->sha256));
    wj_str_or_null(w, "model", f->model);
    wj_int(w, "layout", f->layout);
    wj_uint(w, "layout_build", f->layout_build);
    wj_arr_open(w, "stamps");
    for (size_t i = 0; i < 5; i++) {
        wj_uint(w, NULL, f->stamps[i]);
    }
    wj_arr_close(w);
    wj_uint(w, "expires_in_s", s->expires_in_s);
    wj_obj_close(w);
    bool rx = s->st != NULL && s->st->receiver.valid;
    wj_obj_open(w, "receiver");
    wj_str_or_null(w, "model", rx ? s->st->receiver.model : NULL);
    wj_str_or_null(w, "firmware", rx ? s->st->receiver.firmware : NULL);
    if (rx && s->st->receiver.build != 0) {
        wj_uint(w, "build", s->st->receiver.build);
    } else {
        wj_null(w, "build");
    }
    wj_obj_close(w);
    wj_uint(w, "expected_downtime_s", s->expected_downtime_s);
    wj_u32_or_null(w, "budget_left_s", s->budget_left_s);
    wj_bool(w, "flash_available", s->flash_available);
    wj_str(w, "flash_reason", s->flash_available ? NULL : "not_enabled_in_build");
    wj_obj_close(w);
}

/* ---- diagnostics ------------------------------------------------------------------------------------ */

static void framing_stats(wj_t *w, const gnss_framing_stats_t *f)
{
    wj_obj_open(w, "framing");
    wj_uint(w, "bytes_in", f->bytes_in);
    wj_uint(w, "bytes_discarded", f->bytes_discarded);
    wj_uint(w, "rtcm_frames", f->rtcm_frames);
    wj_uint(w, "rtcm_crc_errors", f->rtcm_crc_errors);
    wj_uint(w, "rtcm_reserved_nonzero", f->rtcm_reserved_nonzero);
    wj_uint(w, "rtcm_bad_length", f->rtcm_bad_length);
    wj_uint(w, "text_lines", f->text_lines);
    wj_uint(w, "text_rejected", f->text_rejected);
    wj_uint(w, "text_aborts", f->text_aborts);
    wj_uint(w, "text_overflows", f->text_overflows);
    wj_uint(w, "bin_skipped", f->bin_skipped);
    wj_uint(w, "resyncs", f->resyncs);
    wj_uint(w, "prompts", f->prompts);
    wj_uint(w, "prompts_ignored", f->prompts_ignored);
    wj_obj_close(w);
}

void web_build_health(wj_t *w, const web_health_snap_t *h)
{
    const health_status_t *hs = h->health;
    wj_obj_open(w, NULL);
    wj_str(w, "level", web_health_level_name(hs->level));
    wj_arr_open(w, "alerts");
    for (uint32_t bit = 1; bit != 0; bit <<= 1) {
        if ((hs->alerts & bit) != 0) {
            const char *name = web_health_alert_name(bit);
            if (name != NULL) {
                wj_str(w, NULL, name);
            }
        }
    }
    wj_arr_close(w);
    wj_str(w, "last_step", web_health_step_name(hs->last_step));
    wj_bool(w, "ladder_suspended", hs->gnss_ladder_suspended);
    wj_uint(w, "uptime_s", h->uptime_s);
    wj_uint(w, "gnss_stalls", hs->gnss_stalls);
    wj_uint(w, "recoveries", hs->recoveries);
    wj_uint(w, "caster_restarts", hs->caster_restarts);
    wj_uint(w, "network_recoveries", hs->network_recoveries);
    wj_u32_or_null(w, "rtcm_gap_ms", hs->rtcm_gap_ms);
    if (hs->gnss_retry_in_s != 0) {
        wj_uint(w, "gnss_retry_in_s", hs->gnss_retry_in_s);
    } else {
        wj_null(w, "gnss_retry_in_s");
    }
    wj_u32_or_null(w, "factory_reset_refused_age_s", hs->factory_reset_refused_age_s);
    wj_uint(w, "admin_resets", hs->admin_resets);
    wj_str(w, "recovery_mode", web_recovery_mode_name(hs->recovery_mode));
    wj_bool(w, "reboot_recheck", hs->gnss_reboot_recheck);
    reboot_hold_fields(w, hs);
    wj_obj_open(w, "heap");
    wj_uint(w, "internal_free", hs->heap_internal_free);
    wj_uint(w, "internal_min", hs->heap_internal_min);
    wj_uint(w, "internal_largest", hs->heap_internal_largest);
    wj_uint(w, "psram_free", hs->heap_psram_free);
    if (h->heap != NULL) {
        wj_uint(w, "psram_min", h->heap->psram_min);
        wj_uint(w, "psram_largest", h->heap->psram_largest);
    }
    wj_obj_close(w);
    wj_str(w, "inhibited_by", h->inhibited_by);
    wj_str(w, "update_slot", web_update_slot_name(h->update_slot));

    const lifecycle_boot_info_t *b = h->boot;
    wj_obj_open(w, "boot");
    wj_str(w, "reset_reason", web_reset_reason_name(b->reset_reason));
    wj_str(w, "reset_class", web_reset_class_name(b->reset_class));
    wj_str(w, "prev_reboot_reason", web_reboot_reason_name(b->prev_reboot_reason));
    wj_uint(w, "boots_since_power_on", b->boots_since_power_on);
    wj_uint(w, "crashes", b->crashes);
    wj_uint(w, "brownouts_since_power_on", b->brownouts_since_power_on);
    wj_bool(w, "safe_mode", b->safe_mode);
    if (b->safe_mode) {
        wj_uint(w, "safe_mode_retry_in_s", b->safe_mode_retry_s > h->uptime_s ? b->safe_mode_retry_s - h->uptime_s : 0);
    } else {
        wj_null(w, "safe_mode_retry_in_s");
    }
    wj_bool(w, "stable", b->stable);
    wj_u32_or_null(w, "prev_session_uptime_s", b->prev_session_uptime_s);
    wj_obj_close(w);

    wj_arr_open(w, "boot_history");
    for (size_t i = 0; h->history != NULL && i < h->history_count; i++) {
        const lifecycle_boot_record_t *r = &h->history[i];
        wj_obj_open(w, NULL);
        wj_str(w, "reset_class", web_reset_class_name(r->reset_class));
        wj_str(w, "reset_reason", web_reset_reason_name(r->reset_reason));
        wj_str(w, "planned_reason", web_reboot_reason_name(r->planned_reason));
        wj_uint(w, "boot_no", r->boot_no);
        wj_u32_or_null(w, "prev_uptime_s", r->prev_uptime_s);
        wj_str(w, "version", r->version);
        wj_obj_close(w);
    }
    wj_arr_close(w);

    const gnss_link_stats_t *l = h->link;
    wj_obj_open(w, "link");
    wj_str(w, "mode", web_link_mode_name((int)l->mode));
    wj_uint(w, "baud", l->baud);
    wj_uint(w, "rx_bytes", l->rx_bytes);
    wj_uint(w, "tx_bytes", l->tx_bytes);
    wj_uint(w, "hw_fifo_overflows", l->hw_fifo_overflows);
    wj_uint(w, "rx_buffer_full", l->rx_buffer_full);
    wj_uint(w, "rx_events_dropped", l->rx_events_dropped);
    wj_uint(w, "line_errors", l->line_errors);
    wj_uint(w, "baud_switch_bytes", l->baud_switch_bytes);
    wj_uint(w, "rtcm_held", l->rtcm_held);
    wj_uint(w, "flushed_bytes", l->flushed_bytes);
    wj_u32_or_null(w, "last_rx_age_ms", l->last_rx_age_ms);
    wj_u32_or_null(w, "last_rtcm_age_ms", l->last_rtcm_age_ms);
    wj_bool(w, "open", l->link_open);
    wj_uint(w, "open_failures", l->open_failures);
    wj_bool(w, "hands_off", l->hands_off);
    framing_stats(w, &l->framing);
    wj_obj_close(w);

    const rtcm_hub_stats_t *hub = h->hub;
    wj_obj_open(w, "hub");
    wj_uint(w, "frames_in", hub->frames_in);
    wj_uint(w, "bytes_in", hub->bytes_in);
    wj_uint(w, "epochs_in", hub->epochs_in);
    wj_uint(w, "epochs_by_mmb", hub->epochs_by_mmb);
    wj_uint(w, "epochs_by_time", hub->epochs_by_time);
    wj_uint(w, "epochs_by_gap", hub->epochs_by_gap);
    wj_uint(w, "publish_failures", hub->publish_failures);
    wj_uint(w, "consumers", hub->consumers);
    wj_uint(w, "capacity", hub->capacity);
    wj_u32_or_null(w, "last_frame_age_ms", hub->last_frame_age_ms);
    wj_obj_close(w);

    const station_status_t *st = h->st;
    wj_obj_open(w, "station");
    wj_str(w, "state", web_station_state_name(st->state));
    wj_uint(w, "rtcm_excluded", st->rtcm_excluded);
    wj_uint(w, "detect_failures", st->detect_failures);
    wj_bool(w, "detect_saw_bytes", st->detect_saw_bytes);
    wj_bool(w, "lease_held", st->lease_held);
    wj_u32_or_null(w, "since_lease_release_ms", st->since_lease_release_ms);
    wj_bool(w, "rx_maintenance", st->rx_maintenance);
    wj_obj_close(w);

    wj_obj_open(w, "obs");
    wj_uint(w, "epochs", h->obs->epochs);
    obs_gaps(w, h->obs);
    wj_obj_close(w);

    const network_status_t *n = h->net;
    wj_obj_open(w, "network");
    wj_uint(w, "reconnects", n->reconnects);
    wj_uint(w, "connect_failures", n->connect_failures);
    wj_uint(w, "offline_s", n->offline_s);
    if (n->last_disconnect_reason != 0) {
        wj_uint(w, "last_disconnect_reason", n->last_disconnect_reason);
    } else {
        wj_null(w, "last_disconnect_reason");
    }
    wj_bool(w, "fallback_dns", n->fallback_dns);
    wj_str(w, "probe", web_probe_name(n->probe));
    wj_u32_or_null(w, "probe_age_s", n->probe_age_s);
    wj_uint(w, "assoc_no_ip_s", n->assoc_no_ip_s);
    wj_obj_open(w, "ladder");
    wj_bool(w, "active", n->ladder_active);
    wj_uint(w, "rung", n->ladder_rung);
    wj_str(w, "withheld", web_ladder_why_name(n->ladder_withheld));
    ladder_triggers(w, "triggers", n->ladder_triggers);
    wj_uint(w, "actions", n->ladder_actions);
    wj_obj_close(w);
    wj_obj_close(w);

    const diag_journal_info_t *j = h->journal;
    if (j != NULL) {
        wj_obj_open(w, "journal");
        wj_bool(w, "available", j->available);
        wj_uint(w, "capacity", j->capacity);
        wj_uint(w, "oldest_seq", j->oldest_seq);
        wj_uint(w, "newest_seq", j->newest_seq);
        wj_uint(w, "sector_cycles", j->sector_cycles);
        wj_uint(w, "written", j->written);
        wj_uint(w, "queued", j->queued);
        wj_uint(w, "refused_kind", j->refused_kind);
        wj_uint(w, "refused_budget", j->refused_budget);
        wj_uint(w, "queue_full", j->queue_full);
        wj_uint(w, "io_errors", j->io_errors);
        wj_obj_close(w);
    } else {
        wj_null(w, "journal");
    }
    const diag_log_stats_t *lg = h->log;
    if (lg != NULL) {
        wj_obj_open(w, "log");
        wj_uint(w, "lines", lg->lines);
        wj_uint(w, "ring_overwritten", lg->ring_overwritten);
        wj_uint(w, "cut", lg->cut);
        wj_uint(w, "console_dropped", lg->console_dropped);
        wj_uint(w, "console_dropped_bytes", lg->console_dropped_bytes);
        wj_bool(w, "prev_boot_available", lg->prev_boot_available);
        wj_uint(w, "prev_boot_lines", lg->prev_boot_lines);
        wj_obj_close(w);
    } else {
        wj_null(w, "log");
    }

    const diag_coredump_info_t *cd = h->coredump;
    wj_obj_open(w, "coredump");
    wj_bool(w, "present", cd->present);
    wj_uint(w, "size", cd->present ? cd->size : 0);
    wj_str(w, "task", cd->present ? cd->task : "");
    if (cd->present) {
        wj_addr(w, "pc", cd->pc);
    } else {
        wj_null(w, "pc");
    }
    wj_arr_open(w, "backtrace");
    for (size_t i = 0; cd->present && i < cd->backtrace_depth && i < 16; i++) {
        wj_addr(w, NULL, cd->backtrace[i]);
    }
    wj_arr_close(w);
    wj_obj_close(w);
    wj_bool(w, "byte_capture_built", h->byte_capture_built);
    wj_obj_close(w);
}

static void day(wj_t *w, const char *key, const ledger_day_t *d, size_t slots)
{
    char date[11];
    web_utc_date(d->utc_day, date);
    wj_obj_open(w, key);
    wj_int(w, "utc_day", d->utc_day);
    wj_str(w, "date", date);
    wj_arr_open(w, "down_s");
    for (size_t i = 0; i < slots && i < LEDGER_MAX_SLOTS; i++) {
        wj_uint(w, NULL, d->down_s[i]);
    }
    wj_arr_close(w);
    wj_arr_open(w, "planned_s");
    for (size_t i = 0; i < slots && i < LEDGER_MAX_SLOTS; i++) {
        wj_uint(w, NULL, d->planned_s[i]);
    }
    wj_arr_close(w);
    wj_arr_open(w, "reconnects");
    for (size_t i = 0; i < slots && i < LEDGER_MAX_SLOTS; i++) {
        wj_uint(w, NULL, d->reconnects[i]);
    }
    wj_arr_close(w);
    wj_arr_open(w, "up_s");
    for (size_t i = 0; i < slots && i < LEDGER_MAX_SLOTS; i++) {
        wj_uint(w, NULL, d->up_s[i]);
    }
    wj_arr_close(w);
    wj_uint(w, "power_off_s", d->power_off_s);
    wj_uint(w, "rx_overflows", d->rx_overflows);
    wj_obj_close(w);
}

int web_avail_protected_slot(const ntrip_caster_cfg_t *cfg, size_t slots)
{
    int any = -1;
    for (size_t i = 0; cfg != NULL && i < slots && i < WEB_MAX_SLOTS; i++) {
        if (!cfg[i].protected_slot) {
            continue;
        }
        if (cfg[i].enabled) {
            return (int)i;
        }
        if (any < 0) {
            any = (int)i;
        }
    }
    return any;
}

void web_build_availability(wj_t *w, const web_avail_snap_t *a)
{
    wj_obj_open(w, NULL);
    wj_str(w, "clock", web_clock_name(a->clock));
    if (a->second_of_day >= 0 && a->clock != LEDGER_CLOCK_NONE) {
        wj_int(w, "utc_second_of_day", a->second_of_day);
    } else {
        wj_null(w, "utc_second_of_day");
    }
    wj_arr_open(w, "slots");
    for (size_t i = 0; i < a->slots && i < WEB_MAX_SLOTS; i++) {
        const ntrip_caster_cfg_t *c = a->cfg != NULL ? &a->cfg[i] : NULL;
        bool empty = web_caster_slot_empty(c);
        wj_obj_open(w, NULL);
        wj_uint(w, "slot", i);
        wj_bool(w, "empty", empty);
        if (empty) {
            wj_str(w, "name", "");
        } else {
            wj_str_or_null(w, "name", c != NULL ? c->name : NULL);
        }
        wj_bool(w, "protected", c != NULL && c->protected_slot);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    if (a->protected_slot >= 0 && a->protected_status != NULL) {
        const ntrip_caster_status_t *p = a->protected_status;
        const ntrip_caster_cfg_t *pc = a->cfg != NULL ? &a->cfg[a->protected_slot] : NULL;
        bool ready = pc != NULL && ntrip_server_onocoy_ready(pc);
        bool counted = ready && pc->enabled && p->day_metrics_valid;
        wj_obj_open(w, "protected");
        wj_bool(w, "enabled", pc != NULL && pc->enabled);
        wj_bool(w, "onocoy_ready", ready);
        wj_uint(w, "down_today_s", a->down_today_s);
        wj_u32_or_null(w, "budget_left_s", counted ? a->budget_left_s : UINT32_MAX);
        wj_u32_or_null(w, "budget_s", counted ? p->budget_s : UINT32_MAX);
        if (counted && p->data_rate_valid) {
            wj_uint(w, "data_rate_permille", p->data_rate_permille);
        } else {
            wj_null(w, "data_rate_permille");
        }
        if (counted) {
            wj_uint(w, "graced_uptime_permille", p->graced_uptime_permille);
            wj_bool(w, "streak_ok", p->streak_ok);
        } else {
            wj_null(w, "graced_uptime_permille");
            wj_null(w, "streak_ok");
        }
        if (counted && p->so_far_valid) {
            wj_uint(w, "graced_uptime_so_far_permille", p->graced_uptime_so_far_permille);
        } else {
            wj_null(w, "graced_uptime_so_far_permille");
        }
        wj_obj_close(w);
    } else {
        wj_null(w, "protected");
    }
    if (a->today_valid && a->today != NULL) {
        day(w, "today", a->today, a->slots);
    } else {
        wj_null(w, "today");
    }
    wj_arr_open(w, "history");
    for (size_t i = 0; a->history != NULL && i < a->history_count; i++) {
        day(w, NULL, &a->history[i], a->slots);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

/* A Wi-Fi drop's RSSI (its journal b) is a reading only from -127 to -1 dBm:
 * this build records 0 for "unknown", earlier builds recorded the driver's
 * -128, and both stay in a journal across updates. */
static bool drop_rssi_known(int32_t b)
{
    return b >= -127 && b <= -1;
}

void web_build_journal(wj_t *w, const diag_journal_entry_t *e, size_t count, uint32_t cursor, bool more)
{
    wj_obj_open(w, NULL);
    wj_uint(w, "cursor", cursor);
    wj_bool(w, "more", more);
    wj_arr_open(w, "entries");
    for (size_t i = 0; e != NULL && i < count; i++) {
        wj_obj_open(w, NULL);
        wj_uint(w, "seq", e[i].seq);
        wj_unix(w, "time_unix", e[i].unix_s);
        wj_uint(w, "uptime_s", e[i].uptime_s);
        wj_str(w, "kind", web_diag_event_name(e[i].kind));
        wj_int(w, "a", e[i].a);
        if (e[i].kind == DIAG_EV_WIFI_DISCONNECT && !drop_rssi_known(e[i].b)) {
            wj_null(w, "b");
        } else {
            wj_int(w, "b", e[i].b);
        }
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

bool web_history_kind(uint16_t kind)
{
    return kind == DIAG_EV_CASTER_DOWN || kind == DIAG_EV_CASTER_UP || kind == DIAG_EV_CASTER_STOP ||
           kind == DIAG_EV_WIFI_UP || kind == DIAG_EV_WIFI_DISCONNECT;
}

bool web_history_collect(const diag_journal_entry_t *page, size_t count, uint32_t hi, diag_journal_entry_t *out,
                      size_t *n, size_t max)
{
    for (size_t i = count; i > 0 && *n < max; i--) {
        const diag_journal_entry_t *e = &page[i - 1];
        if (e->seq <= hi && web_history_kind(e->kind)) {
            out[(*n)++] = *e;
        }
    }
    return *n < max;
}

static void conn_entry(wj_t *w, const diag_journal_entry_t *e, const ntrip_caster_cfg_t *cfg, size_t slots)
{
    bool stop = e->kind == DIAG_EV_CASTER_STOP;
    bool caster = e->kind == DIAG_EV_CASTER_DOWN || e->kind == DIAG_EV_CASTER_UP || stop;
    uint32_t a = (uint32_t)e->a;
    wj_obj_open(w, NULL);
    wj_uint(w, "seq", e->seq);
    wj_unix(w, "time_unix", e->unix_s);
    wj_uint(w, "uptime_s", e->uptime_s);
    wj_str(w, "kind", web_diag_event_name(e->kind));
    if (caster) {
        size_t slot = a & 0xFFu;
        wj_uint(w, "slot", slot);
        if (cfg != NULL && slot < slots) {
            str_arr_or_null(w, "slot_name", cfg[slot].name, sizeof(cfg[slot].name));
        } else {
            wj_null(w, "slot_name");
        }
        if (e->kind == DIAG_EV_CASTER_DOWN) {
            wj_str(w, "class", web_ntrip_fail_name((ntrip_fail_class_t)((a >> 8) & 0xFFu)));
        } else {
            wj_null(w, "class");
        }
        if (stop) {
            wj_null(w, "count");
        } else {
            wj_uint(w, "count", (a >> 16) & 0x7FFFu);
        }
    } else {
        wj_null(w, "slot");
        wj_null(w, "slot_name");
        wj_null(w, "class");
        if (e->kind == DIAG_EV_WIFI_UP) {
            wj_uint(w, "count", a);
        } else {
            wj_null(w, "count");
        }
    }
    if (e->kind == DIAG_EV_WIFI_DISCONNECT || stop) {
        wj_null(w, "duration_s");
    } else {
        wj_uint(w, "duration_s", e->b < 0 ? 0u : (uint32_t)e->b);
    }
    if (e->kind == DIAG_EV_WIFI_DISCONNECT) {
        wj_uint(w, "reason", a);
        if (drop_rssi_known(e->b)) {
            wj_int(w, "rssi", e->b);
        } else {
            wj_null(w, "rssi");
        }
    } else {
        wj_null(w, "reason");
        wj_null(w, "rssi");
    }
    if (stop) {
        wj_str(w, "stop_reason", web_ntrip_stop_name((ntrip_stop_t)((a >> 8) & 0xFFu)));
    } else {
        wj_null(w, "stop_reason");
    }
    wj_obj_close(w);
}

void web_build_connections(wj_t *w, const diag_journal_entry_t *e, size_t count, const ntrip_caster_cfg_t *cfg,
                           size_t slots, bool complete)
{
    wj_obj_open(w, NULL);
    wj_bool(w, "complete", complete);
    wj_arr_open(w, "entries");
    for (size_t i = 0; e != NULL && i < count; i++) {
        conn_entry(w, &e[i], cfg, slots);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

void web_build_tasks(wj_t *w, const diag_task_info_t *t, size_t count)
{
    wj_obj_open(w, NULL);
    wj_arr_open(w, "tasks");
    for (size_t i = 0; t != NULL && i < count; i++) {
        wj_obj_open(w, NULL);
        wj_strn(w, "name", t[i].name, strnlen(t[i].name, sizeof(t[i].name)));
        wj_uint(w, "priority", t[i].priority);
        if (t[i].core < 0) {
            wj_null(w, "core");
        } else {
            wj_int(w, "core", t[i].core);
        }
        wj_uint(w, "stack_free_min", t[i].stack_free_min);
        wj_uint(w, "cpu_pct", t[i].cpu_pct);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    wj_obj_close(w);
}

void web_build_logs(wj_t *w, uint32_t cursor, bool more, bool gap, const char *text, size_t len,
                    const char *level_max)
{
    wj_obj_open(w, NULL);
    wj_uint(w, "cursor", cursor);
    wj_bool(w, "more", more);
    wj_bool(w, "gap", gap);
    wj_str(w, "level_max", level_max != NULL ? level_max : "info");
    wj_strn(w, "text", text != NULL ? text : "", text != NULL ? len : 0);
    wj_obj_close(w);
}

/* ---- errors and WebSocket messages ------------------------------------------------------------------- */

void web_build_error(wj_t *w, web_err_t err, const char *message, const char *field, uint32_t retry_in_s,
                     const char *by, const char *reason)
{
    wj_obj_open(w, NULL);
    wj_obj_open(w, "error");
    wj_str(w, "code", web_err_code(err));
    wj_str(w, "message", message != NULL ? message : web_err_message(err));
    wj_str(w, "field", field);
    wj_u32_or_null(w, "retry_in_s", retry_in_s);
    wj_str(w, "by", by);
    if (reason != NULL) {
        wj_str(w, "reason", reason);
    }
    wj_obj_close(w);
    wj_obj_close(w);
}

void web_flash_progress_msg(const um98x_flash_progress_t *p, bool final, bool check_only,
                            const station_flash_last_t *last, web_progress_t *out)
{
    const char *result = NULL;
    if (final) {
        result = last != NULL ? web_flash_outcome_result(last->outcome) : (p->phase == UM98X_FLASH_DONE ? "ok" : "failed");
    }
    bool ok = final && strcmp(result, "ok") == 0;
    memset(out, 0, sizeof(*out));
    out->job = check_only ? "gnss_check" : "gnss_flash";
    out->phase = check_only && !final ? "checking" : web_flash_phase_name(p->phase);
    out->pct = p->bytes_total > 0 && !check_only ? (int)((uint64_t)p->bytes_sent * 100u / p->bytes_total) : -1;
    out->bytes = -1;
    out->done = final;
    out->result = result;
    out->err = final && !ok ? (esp_err_t)p->last_err : ESP_OK;
    out->flash = check_only ? NULL : p;
    out->last = final ? last : NULL;
}

void web_build_progress(wj_t *w, const web_progress_t *p)
{
    wj_obj_open(w, NULL);
    wj_str(w, "type", "progress");
    wj_str(w, "job", p->job);
    wj_str(w, "phase", p->phase);
    if (p->pct >= 0) {
        wj_int(w, "pct", p->pct > 100 ? 100 : p->pct);
    }
    if (p->bytes >= 0) {
        wj_int(w, "bytes", p->bytes);
        wj_int(w, "total", p->total);
    }
    if (p->flash != NULL) {
        flash_counters(w, p->flash);
    }
    wj_bool(w, "done", p->done);
    if (p->done) {
        wj_str(w, "result", p->result != NULL ? p->result : web_result_code(p->job, p->err));
        if (p->err != ESP_OK) {
            wj_str(w, "message", esp_err_to_name(p->err));
        }
        if (p->last != NULL) {
            wj_str(w, "outcome", web_flash_outcome_name(p->last->outcome));
            wj_str(w, "firmware", p->last->firmware);
            wj_uint(w, "build", p->last->build);
            wj_uint(w, "failed_rule", p->last->failed_rule);
            wj_str(w, "reason", p->last->reason);
        }
    }
    wj_obj_close(w);
}

/* ---- receiver: apply preview ------------------------------------------------------------ */

void web_build_apply_preview(wj_t *w, const um98x_preview_step_t *steps, size_t count)
{
    wj_obj_open(w, NULL);
    wj_arr_open(w, "steps");
    for (size_t i = 0; i < count; i++) {
        const um98x_preview_step_t *s = &steps[i];
        wj_obj_open(w, NULL);
        str_arr(w, "text", s->text, sizeof(s->text));
        wj_str(w, "kind", web_preview_kind_name(s->kind));
        wj_str(w, "when", web_preview_when_name(s->when));
        wj_bool(w, "saved", s->saved);
        wj_obj_close(w);
    }
    wj_arr_close(w);
    bool save = count > 0 && steps[count - 1].kind == UM98X_PREVIEW_WRITE && steps[count - 1].saved;
    wj_bool(w, "saveconfig", save);
    wj_obj_close(w);
}
