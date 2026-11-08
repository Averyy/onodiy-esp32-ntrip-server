/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_shell_build.h"

#include <stdio.h>
#include <string.h>

#include "web_shell_enums.h"

/* A bounded char array as a string (its terminator, or the array's end). */
void web_build_str_arr_or_null(wj_t *w, const char *key, const char *s, size_t cap)
{
    size_t n = strnlen(s, cap);
    if (n == 0) {
        wj_null(w, key);
    } else {
        wj_strn(w, key, s, n);
    }
}

void web_build_ladder_triggers(wj_t *w, const char *key, uint32_t bits)
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
void web_build_cred_fields(wj_t *w, const network_status_t *n)
{
    wj_bool(w, "cred_trial", n->cred_trial);
    wj_uint(w, "cred_trial_left_s", n->cred_trial ? n->cred_trial_left_s : 0);
    wj_bool(w, "cred_pending", n->cred_pending);
    web_build_str_arr_or_null(w, "pending_ssid", (n->cred_pending || n->cred_trial) ? n->pending_ssid : "", sizeof(n->pending_ssid));
    wj_bool(w, "cred_reverted", n->cred_reverted);
    wj_str(w, "cred_reverted_why", cred_revert_name(n));
}

/* Seconds until the AP closes: null while it is down or nothing is scheduled. */
void web_build_ap_close_in(wj_t *w, const char *key, const network_status_t *n)
{
    wj_u32_or_null(w, key, n->setup_ap_active ? n->ap_close_in_s : UINT32_MAX);
}

void web_build_ap_reasons(wj_t *w, const char *key, bool active, uint32_t reasons)
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

void web_build_wifi(wj_t *w, const web_wifi_snap_t *s)
{
    const network_status_t *n = s->net;
    wj_obj_open(w, NULL);
    web_build_str_arr_or_null(w, "mdns_host", n->mdns_host, sizeof(n->mdns_host));
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
    char bssid[18] = "";
    if (n->associated && n->bssid_known) {
        snprintf(bssid, sizeof(bssid), "%02X:%02X:%02X:%02X:%02X:%02X", n->bssid[0], n->bssid[1], n->bssid[2],
                 n->bssid[3], n->bssid[4], n->bssid[5]);
    }
    wj_str_or_null(w, "bssid", bssid);
    if (n->associated && n->tx_power_known) {
        wj_double(w, "tx_power_dbm", (double)n->tx_power_qdbm / 4.0, 2);
    } else {
        wj_null(w, "tx_power_dbm");
    }
    if (n->ap_steering == NETWORK_AP_STEERING_NEVER) {
        /* left out: this build never reads it (null would read as "not yet") */
    } else if (n->associated && n->ap_steering >= 0) {
        wj_bool(w, "ap_steering", n->ap_steering > 0);
    } else {
        wj_null(w, "ap_steering");
    }
    wj_bool(w, "time_synced", n->time_synced);
    wj_str(w, "upstream", web_probe_name(n->probe));
    wj_u32_or_null(w, "probe_age_s", n->probe_age_s);
    wj_uint(w, "reconnects", n->reconnects);
    wj_uint(w, "connect_failures", n->connect_failures);
    wj_uint(w, "roams", n->roams);
    wj_uint(w, "offline_s", n->offline_s);
    wj_uint(w, "retry_in_s", n->retry_in_s);
    if (n->last_disconnect_reason != 0) {
        wj_uint(w, "last_disconnect_reason", n->last_disconnect_reason);
    } else {
        wj_null(w, "last_disconnect_reason");
    }
    wj_bool(w, "fallback_dns", n->fallback_dns);
    wj_uint(w, "assoc_no_ip_s", n->assoc_no_ip_s);
    web_build_cred_fields(w, n);
    wj_obj_open(w, "ladder");
    wj_bool(w, "active", n->ladder_active);
    wj_uint(w, "rung", n->ladder_rung);
    wj_str(w, "withheld", web_ladder_why_name(n->ladder_withheld));
    web_build_ladder_triggers(w, "triggers", n->ladder_triggers);
    wj_uint(w, "actions", n->ladder_actions);
    wj_obj_close(w);
    wj_obj_open(w, "ap");
    wj_bool(w, "active", n->setup_ap_active);
    wj_str_or_null(w, "ssid", n->ap_ssid);
    web_build_ap_reasons(w, "reasons", n->setup_ap_active, n->ap_reasons);
    wj_uint(w, "clients", n->ap_clients);
    wj_uint(w, "recovery_left_s", n->recovery_ap_left_s);
    web_build_ap_close_in(w, "close_in_s", n);
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

void web_shell_build_session(wj_t *w, const web_shell_session_snap_t *s)
{
    wj_obj_open(w, NULL);
    wj_int(w, "api", WEB_SHELL_API_VERSION);
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
    wj_str(w, "setup_step", s->authenticated && !s->setup_done ? s->setup_step : NULL);
    wj_obj_close(w);
}
