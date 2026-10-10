/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_enums.h"

#include "diag.h"

#include <string.h>

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static const char *pick(const char *const *table, size_t count, int v)
{
    return (v >= 0 && (size_t)v < count && table[v] != NULL) ? table[v] : "unknown";
}

/* Name of the lowest set bit of `bit` in a table indexed by bit number. */
static const char *bit_name(const char *const *table, size_t count, uint32_t bit)
{
    for (size_t i = 0; i < count && i < 32; i++) {
        if (bit == (1u << i)) {
            return table[i];
        }
    }
    return NULL;
}

static bool lookup(const char *const *table, size_t count, const char *s, int *out)
{
    if (s == NULL) {
        return false;
    }
    for (size_t i = 0; i < count; i++) {
        if (table[i] != NULL && strcmp(table[i], s) == 0) {
            *out = (int)i;
            return true;
        }
    }
    return false;
}

static const char *const s_station_state[] = {
    [STATION_STATE_IDLE] = "idle",
    [STATION_STATE_DETECTING] = "detecting",
    [STATION_STATE_CONFIGURING] = "configuring",
    [STATION_STATE_STREAMING] = "streaming",
    [STATION_STATE_FLASHING] = "flashing",
    [STATION_STATE_FLASH_RECOVERY] = "flash_recovery",
    [STATION_STATE_RECOVERING] = "recovering",
    [STATION_STATE_NO_RECEIVER] = "no_receiver",
};

const char *web_station_state_name(station_state_t v)
{
    return pick(s_station_state, COUNT(s_station_state), (int)v);
}

static const char *const s_station_alert[] = {
    "unsupported_model", "config_failed", "position_mismatch", "antenna_moved", "receiver_attention",
    "receiver_in_loader",
};
_Static_assert(STATION_ALERT_UNSUPPORTED_MODEL == 1u << 0 && STATION_ALERT_RECEIVER_IN_LOADER == 1u << 5,
               "station alert bits moved: update the name table");

const char *web_station_alert_name(uint32_t bit)
{
    return bit_name(s_station_alert, COUNT(s_station_alert), bit);
}

static const char *const s_health_level[] = {
    [HEALTH_OK] = "ok",
    [HEALTH_DEGRADED] = "degraded",
    [HEALTH_RECOVERING] = "recovering",
    [HEALTH_FAILED] = "failed",
};

const char *web_health_level_name(health_level_t v)
{
    return pick(s_health_level, COUNT(s_health_level), (int)v);
}

static const char *const s_health_step[] = {
    [HEALTH_STEP_NONE] = "none",
    [HEALTH_STEP_RESYNC] = "resync",
    [HEALTH_STEP_LINK_RESTART] = "link_restart",
    [HEALTH_STEP_RESCAN] = "rescan",
    [HEALTH_STEP_RECONFIGURE] = "reconfigure",
    [HEALTH_STEP_RECEIVER_RESET] = "receiver_reset",
    [HEALTH_STEP_REBOOT] = "reboot",
};

const char *web_health_step_name(health_step_t v)
{
    return pick(s_health_step, COUNT(s_health_step), (int)v);
}

static const char *const s_health_alert[] = {
    "power_supply", "receiver_power", "bands", "profile_drift", "heap_fragmented",
    "safe_mode", "settings_degraded", "no_upstream", "link_loss", "power_lost",
};
_Static_assert(HEALTH_ALERT_POWER_SUPPLY == 1u << 0 && HEALTH_ALERT_LINK_LOSS == 1u << 8 &&
               HEALTH_ALERT_POWER_LOST == 1u << 9,
               "health alert bits moved: update the name table");

const char *web_health_alert_name(uint32_t bit)
{
    return bit_name(s_health_alert, COUNT(s_health_alert), bit);
}

static const char *const s_ntrip_state[] = {
    [NTRIP_STATE_DISABLED] = "disabled",
    [NTRIP_STATE_WAIT_NETWORK] = "wait_network",
    [NTRIP_STATE_WAIT_DATA] = "wait_data",
    [NTRIP_STATE_CONNECTING] = "connecting",
    [NTRIP_STATE_STREAMING] = "streaming",
    [NTRIP_STATE_BACKOFF] = "backoff",
    [NTRIP_STATE_AUTH_FAILED] = "auth_failed",
    [NTRIP_STATE_PAUSED] = "paused",
    [NTRIP_STATE_CONFIG_ERROR] = "config_error",
    [NTRIP_STATE_BANNED] = "banned",
};

const char *web_ntrip_state_name(ntrip_state_t v)
{
    return pick(s_ntrip_state, COUNT(s_ntrip_state), (int)v);
}

static const char *const s_ntrip_fail[] = {
    [NTRIP_FAIL_NONE] = "none",
    [NTRIP_FAIL_DNS] = "dns",
    [NTRIP_FAIL_CONNECT] = "connect",
    [NTRIP_FAIL_TLS] = "tls",
    [NTRIP_FAIL_TIMEOUT] = "timeout",
    [NTRIP_FAIL_REMOTE_CLOSE] = "remote_close",
    [NTRIP_FAIL_WRITE_STALL] = "write_stall",
    [NTRIP_FAIL_KEEPALIVE] = "keepalive",
    [NTRIP_FAIL_AUTH] = "auth",
    [NTRIP_FAIL_MOUNT_BUSY] = "mount_busy",
    [NTRIP_FAIL_CONFIG] = "config",
    [NTRIP_FAIL_BANNED] = "banned",
};
_Static_assert(COUNT(s_ntrip_fail) == NTRIP_FAIL_CLASS_COUNT, "caster failure classes moved: update the table");

const char *web_ntrip_fail_name(ntrip_fail_class_t v)
{
    return pick(s_ntrip_fail, COUNT(s_ntrip_fail), (int)v);
}

static const char *const s_trust[] = {
    [NTRIP_TRUST_BUNDLE] = "bundle",
    [NTRIP_TRUST_CUSTOM_CA] = "custom_ca",
    [NTRIP_TRUST_INSECURE] = "insecure",
};

const char *web_ntrip_trust_name(ntrip_trust_t v)
{
    return pick(s_trust, COUNT(s_trust), (int)v);
}

static const char *const s_backoff[] = {
    [NTRIP_BACKOFF_FAST] = "fast",
    [NTRIP_BACKOFF_STANDARD] = "standard",
    [NTRIP_BACKOFF_POLITE] = "polite",
};

const char *web_ntrip_backoff_name(ntrip_backoff_profile_t v)
{
    return pick(s_backoff, COUNT(s_backoff), (int)v);
}

static const char *const s_base_mode[] = {
    [UM98X_BASE_SURVEY_IN] = "survey_in",
    [UM98X_BASE_FIXED] = "fixed",
};

const char *web_base_mode_name(um98x_base_mode_t v)
{
    return pick(s_base_mode, COUNT(s_base_mode), (int)v);
}

static const char *const s_pos_source[] = {
    [UM98X_POS_NONE] = "none",
    [UM98X_POS_SURVEY] = "survey",
    [UM98X_POS_AVERAGED] = "averaged",
    [UM98X_POS_USER] = "user",
};

const char *web_pos_source_name(um98x_pos_source_t v)
{
    return pick(s_pos_source, COUNT(s_pos_source), (int)v);
}

static const char *const s_flash_phase[] = {
    [UM98X_FLASH_IDLE] = "idle",
    [UM98X_FLASH_CHECKING] = "checking",
    [UM98X_FLASH_ENTERING] = "entering",
    [UM98X_FLASH_TRANSFERRING] = "transferring",
    [UM98X_FLASH_COMMITTING] = "committing",
    [UM98X_FLASH_FINISHING] = "finishing",
    [UM98X_FLASH_DONE] = "done",
    [UM98X_FLASH_FAILED] = "failed",
    [UM98X_FLASH_NEEDS_USER] = "needs_user",
};

const char *web_flash_phase_name(um98x_flash_phase_t v)
{
    return pick(s_flash_phase, COUNT(s_flash_phase), (int)v);
}

static const char *const s_journal_state[] = {
    [UM98X_JOURNAL_PREPARED] = "prepared",
    [UM98X_JOURNAL_BAUD_CHANGING] = "baud_changing",
    [UM98X_JOURNAL_RESET_SENT] = "reset_sent",
    [UM98X_JOURNAL_MENU2_SENT] = "menu2_sent",
    [UM98X_JOURNAL_TRANSFERRING] = "transferring",
    [UM98X_JOURNAL_EOT_SENT] = "eot_sent",
    [UM98X_JOURNAL_VERIFYING] = "verifying",
    [UM98X_JOURNAL_FAILED] = "failed",
    [UM98X_JOURNAL_NEEDS_USER] = "needs_user",
};

const char *web_journal_state_name(uint8_t v)
{
    return pick(s_journal_state, COUNT(s_journal_state), (int)v);
}

static const char *const s_flash_failure[] = {
    [UM98X_FLASH_FAIL_NONE] = "none",
    [UM98X_FLASH_FAIL_TRANSPORT] = "transport",
    [UM98X_FLASH_FAIL_PACKAGE] = "package",
    [UM98X_FLASH_FAIL_UNCONFIRMED] = "unconfirmed",
    [UM98X_FLASH_FAIL_NO_ANSWER] = "no_answer",
    [UM98X_FLASH_FAIL_APP_NOT_STARTING] = "app_not_starting",
    [UM98X_FLASH_FAIL_ENTRY] = "entry",
    [UM98X_FLASH_FAIL_JOURNAL] = "journal",
    [UM98X_FLASH_FAIL_RECEIVER] = "receiver",
};

static const char *const s_flash_outcome[] = {
    [STATION_FLASH_OUT_NONE] = "none",
    [STATION_FLASH_OUT_SUCCESS] = "success",
    [STATION_FLASH_OUT_OLD_FIRMWARE] = "old_firmware",
    [STATION_FLASH_OUT_UNEXPECTED] = "unexpected_build",
    [STATION_FLASH_OUT_NOT_DONE] = "not_done",
    [STATION_FLASH_OUT_REJECTED] = "rejected",
    [STATION_FLASH_OUT_NOT_STARTED] = "not_started",
    [STATION_FLASH_OUT_NEEDS_USER] = "needs_user",
    [STATION_FLASH_OUT_CHECKED] = "checked",
};

const char *web_flash_outcome_name(uint8_t v)
{
    return pick(s_flash_outcome, COUNT(s_flash_outcome), (int)v);
}

const char *web_flash_outcome_result(uint8_t v)
{
    switch (v) {
    case STATION_FLASH_OUT_SUCCESS:
    case STATION_FLASH_OUT_CHECKED:
        return "ok";
    case STATION_FLASH_OUT_REJECTED:
        return "package_rejected";
    case STATION_FLASH_OUT_NONE:
        return "failed";
    default:
        return web_flash_outcome_name(v);
    }
}

const char *web_flash_failure_name(uint8_t v)
{
    return pick(s_flash_failure, COUNT(s_flash_failure), (int)v);
}

static const char *const s_fw_state[] = {
    [FW_UPDATE_IDLE] = "idle",
    [FW_UPDATE_CHECKING] = "checking",
    [FW_UPDATE_DOWNLOADING_APP] = "downloading_app",
    [FW_UPDATE_DOWNLOADING_GNSS] = "downloading_gnss",
    [FW_UPDATE_REBOOT_PENDING] = "reboot_pending",
    [FW_UPDATE_FAILED] = "failed",
    [FW_UPDATE_UPLOADING_APP] = "uploading_app",
};

const char *web_fw_state_name(fw_update_state_t v)
{
    return pick(s_fw_state, COUNT(s_fw_state), (int)v);
}

static const char *const s_fw_trial[] = {
    [FW_TRIAL_NONE] = "none",
    [FW_TRIAL_CONFIRMING] = "confirming",
    [FW_TRIAL_VERIFYING] = "verifying",
    [FW_TRIAL_WATCHING] = "watching",
};

const char *web_fw_trial_name(fw_update_trial_t v)
{
    return pick(s_fw_trial, COUNT(s_fw_trial), (int)v);
}

static const char *const s_gnss_block[] = {
    [FW_GNSS_BLOCK_NONE] = "none",
    [FW_GNSS_BLOCK_APP_TRIAL] = "app_trial",
    [FW_GNSS_BLOCK_JOURNAL_OPEN] = "journal_open",
    [FW_GNSS_BLOCK_UNSUPPORTED_MODEL] = "unsupported_model",
    [FW_GNSS_BLOCK_SAFE_MODE] = "safe_mode",
    [FW_GNSS_BLOCK_APP_UPDATE] = "app_update",
    [FW_GNSS_BLOCK_RECEIVER_NOT_STREAMING] = "receiver_not_streaming",
};

const char *web_gnss_block_name(fw_update_gnss_block_t v)
{
    return pick(s_gnss_block, COUNT(s_gnss_block), (int)v);
}

static const char *const s_probe[] = {
    [NETWORK_PROBE_NONE] = "none",
    [NETWORK_PROBE_OK] = "ok",
    [NETWORK_PROBE_NO_IP] = "no_ip",
    [NETWORK_PROBE_DNS_FAILED] = "dns_failed",
    [NETWORK_PROBE_NO_UPSTREAM] = "no_upstream",
};

const char *web_probe_name(network_probe_t v)
{
    return pick(s_probe, COUNT(s_probe), (int)v);
}

static const char *const s_ap_reason[] = { "setup", "fallback", "linger", "recovery" };
_Static_assert(NETWORK_AP_SETUP == 1u << 0 && NETWORK_AP_RECOVERY == 1u << 3, "AP reason bits moved: update the table");

const char *web_ap_reason_name(uint32_t bit)
{
    return bit_name(s_ap_reason, COUNT(s_ap_reason), bit);
}

static const char *const s_ladder_why[] = {
    [NETWORK_LADDER_WHY_NONE] = "none",
    [NETWORK_LADDER_WHY_UPSTREAM_OK] = "upstream_ok",
    [NETWORK_LADDER_WHY_NO_UPSTREAM] = "no_upstream",
    [NETWORK_LADDER_WHY_JOURNAL_OPEN] = "journal_open",
    [NETWORK_LADDER_WHY_INTERVAL] = "interval",
    [NETWORK_LADDER_WHY_INHIBITED] = "inhibited",
    [NETWORK_LADDER_WHY_AP_IN_USE] = "ap_in_use",
    [NETWORK_LADDER_WHY_NO_REBOOT_TRIGGER] = "no_reboot_trigger",
    [NETWORK_LADDER_WHY_AP_OVERRIDDEN] = "ap_overridden",
};
_Static_assert(COUNT(s_ladder_why) == (size_t)NETWORK_LADDER_WHY_AP_OVERRIDDEN + 1u, "ladder reasons added: update the table");

const char *web_ladder_why_name(uint8_t v)
{
    return pick(s_ladder_why, COUNT(s_ladder_why), (int)v);
}

static const char *const s_ladder_trigger[] = { "protected", "assoc_no_ip", "unreached" };
_Static_assert(NETWORK_LADDER_TRIG_PROTECTED == 1u << 0 && NETWORK_LADDER_TRIG_UNREACHED == 1u << 2,
               "network ladder trigger bits moved: update the table");

const char *web_ladder_trigger_name(uint32_t bit)
{
    return bit_name(s_ladder_trigger, COUNT(s_ladder_trigger), bit);
}

static const char *const s_link_mode[] = { "off", "stream", "command", "raw" };

const char *web_link_mode_name(int v)
{
    return pick(s_link_mode, COUNT(s_link_mode), v);
}

static const char *const s_reset_class[] = {
    [LIFECYCLE_RESET_POWER_ON] = "power_on",
    [LIFECYCLE_RESET_PLANNED] = "planned",
    [LIFECYCLE_RESET_CRASH] = "crash",
    [LIFECYCLE_RESET_BROWNOUT] = "brownout",
    [LIFECYCLE_RESET_OTHER] = "other",
};

const char *web_reset_class_name(lifecycle_reset_class_t v)
{
    return pick(s_reset_class, COUNT(s_reset_class), (int)v);
}

static const char *const s_reboot_reason[] = {
    [LIFECYCLE_REBOOT_NONE] = "none",
    [LIFECYCLE_REBOOT_USER] = "user",
    [LIFECYCLE_REBOOT_FACTORY_RESET] = "factory_reset",
    [LIFECYCLE_REBOOT_ADMIN_RESET] = "admin_reset",
    [LIFECYCLE_REBOOT_APP_UPDATE] = "app_update",
    [LIFECYCLE_REBOOT_APP_REVERT] = "app_revert",
    [LIFECYCLE_REBOOT_GNSS_LADDER] = "gnss_ladder",
    [LIFECYCLE_REBOOT_NETWORK_LADDER] = "network_ladder",
    [LIFECYCLE_REBOOT_LOW_HEAP] = "low_heap",
    [LIFECYCLE_REBOOT_SAFE_MODE_RETRY] = "safe_mode_retry",
    [LIFECYCLE_REBOOT_SAFE_MODE_CLEARED] = "safe_mode_cleared",
    [LIFECYCLE_REBOOT_NVS_SLOT_SWITCH] = "nvs_slot_switch",
    [LIFECYCLE_REBOOT_BOOT_FAILURE] = "boot_failure",
    [LIFECYCLE_REBOOT_APP_ROLLBACK] = "app_rollback",
};

_Static_assert(COUNT(s_reboot_reason) == (size_t)LIFECYCLE_REBOOT_APP_ROLLBACK + 1u,
               "reboot reasons moved: update the table");

const char *web_reboot_reason_name(lifecycle_reboot_reason_t v)
{
    return pick(s_reboot_reason, COUNT(s_reboot_reason), (int)v);
}

static const char *const s_reset_reason[] = {
    [ESP_RST_UNKNOWN] = "unknown",
    [ESP_RST_POWERON] = "poweron",
    [ESP_RST_EXT] = "ext",
    [ESP_RST_SW] = "sw",
    [ESP_RST_PANIC] = "panic",
    [ESP_RST_INT_WDT] = "int_wdt",
    [ESP_RST_TASK_WDT] = "task_wdt",
    [ESP_RST_WDT] = "wdt",
    [ESP_RST_DEEPSLEEP] = "deepsleep",
    [ESP_RST_BROWNOUT] = "brownout",
    [ESP_RST_SDIO] = "sdio",
    [ESP_RST_USB] = "usb",
    [ESP_RST_JTAG] = "jtag",
    [ESP_RST_EFUSE] = "efuse",
    [ESP_RST_PWR_GLITCH] = "pwr_glitch",
    [ESP_RST_CPU_LOCKUP] = "cpu_lockup",
};

const char *web_reset_reason_name(esp_reset_reason_t v)
{
    return pick(s_reset_reason, COUNT(s_reset_reason), (int)v);
}

static const char *const s_clock[] = {
    [LEDGER_CLOCK_NONE] = "none",
    [LEDGER_CLOCK_SNTP] = "sntp",
    [LEDGER_CLOCK_GNSS] = "gnss",
};

const char *web_clock_name(ledger_clock_t v)
{
    return pick(s_clock, COUNT(s_clock), (int)v);
}

static const char *const s_sys[] = {
    [GNSS_SYS_GPS] = "gps",
    [GNSS_SYS_GLONASS] = "glonass",
    [GNSS_SYS_GALILEO] = "galileo",
    [GNSS_SYS_SBAS] = "sbas",
    [GNSS_SYS_QZSS] = "qzss",
    [GNSS_SYS_BEIDOU] = "beidou",
    [GNSS_SYS_NAVIC] = "navic",
};
_Static_assert(COUNT(s_sys) == GNSS_SYS_COUNT, "constellations moved: update the table");

const char *web_sys_name(gnss_sys_t v)
{
    return pick(s_sys, COUNT(s_sys), (int)v);
}

static const char *const s_band[] = {
    [GNSS_BAND_L1] = "l1",
    [GNSS_BAND_L2] = "l2",
    [GNSS_BAND_L5] = "l5",
    [GNSS_BAND_L6] = "l6",
    [GNSS_BAND_E5AB] = "e5ab",
};
_Static_assert(COUNT(s_band) == GNSS_BAND_COUNT, "bands moved: update the table");

const char *web_band_name(gnss_band_t v)
{
    return pick(s_band, COUNT(s_band), (int)v);
}

/* By the stored kind value: the values are not contiguous (gaps stay NULL = "unknown"). */
static const char *const s_diag_event[] = {
    [DIAG_EV_BOOT] = "boot",
    [DIAG_EV_REBOOT] = "reboot",
    [DIAG_EV_LADDER_STEP] = "ladder_step",
    [DIAG_EV_APP_UPDATE] = "app_update",
    [DIAG_EV_APP_REVERT] = "app_revert",
    [DIAG_EV_GNSS_FLASH] = "gnss_flash",
    [DIAG_EV_SAFE_MODE] = "safe_mode",
    [DIAG_EV_WIFI_DISCONNECT] = "wifi_disconnect",
    [DIAG_EV_DATA_LOSS] = "data_loss",
    [DIAG_EV_SESSION] = "session",
    [DIAG_EV_CASTER_DOWN] = "caster_down",
    [DIAG_EV_CASTER_UP] = "caster_up",
    [DIAG_EV_WIFI_UP] = "wifi_up",
    [DIAG_EV_RX_MAINT] = "rx_maintenance",
    [DIAG_EV_LIVE_VIEW] = "live_view",
};
_Static_assert(COUNT(s_diag_event) == (size_t)DIAG_EV_LIVE_VIEW + 1u, "journal kinds added: update the table");

const char *web_diag_event_name(uint16_t kind)
{
    return pick(s_diag_event, COUNT(s_diag_event), (int)kind);
}

static const char *const s_update_slot[] = {
    [LIFECYCLE_UPDATE_NONE] = "none",
    [LIFECYCLE_UPDATE_APP] = "app",
    [LIFECYCLE_UPDATE_RECEIVER] = "receiver",
};

const char *web_update_slot_name(lifecycle_update_t v)
{
    return pick(s_update_slot, COUNT(s_update_slot), (int)v);
}

static const char *const s_recovery_mode[] = {
    [HEALTH_RECOVERY_FULL] = "full",
    [HEALTH_RECOVERY_NO_REBOOT] = "no_reboot",
    [HEALTH_RECOVERY_ALERTS] = "alerts_only",
};
_Static_assert(COUNT(s_recovery_mode) == (size_t)HEALTH_RECOVERY_ALERTS + 1u, "recovery modes moved: update the table");

const char *web_recovery_mode_name(health_recovery_mode_t v)
{
    return pick(s_recovery_mode, COUNT(s_recovery_mode), (int)v);
}

static const char *const s_setting[] = {
    [UM98X_SETTING_UNKNOWN] = "unknown",
    [UM98X_SETTING_ON] = "on",
    [UM98X_SETTING_OFF] = "off",
};

const char *web_setting_name(uint8_t v)
{
    return pick(s_setting, COUNT(s_setting), (int)v);
}

static const char *const s_live_sky[] = {
    [STATION_LIVE_SKY_OFF] = "off",
    [STATION_LIVE_SKY_PENDING] = "pending",
    [STATION_LIVE_SKY_ON] = "on",
    [STATION_LIVE_SKY_ON_PROFILE] = "on_profile",
    [STATION_LIVE_SKY_BLOCKED] = "blocked",
    [STATION_LIVE_SKY_STOPPING] = "stopping",
};
_Static_assert(COUNT(s_live_sky) == (size_t)STATION_LIVE_SKY_STOPPING + 1u, "live view output states moved: update the table");

const char *web_live_sky_name(uint8_t v)
{
    return pick(s_live_sky, COUNT(s_live_sky), (int)v);
}

static const char *const s_live_why[] = {
    [STATION_LIVE_WHY_NONE] = "none",
    [STATION_LIVE_WHY_BUSY] = "busy",
    [STATION_LIVE_WHY_NOT_STREAMING] = "not_streaming",
    [STATION_LIVE_WHY_MAINTENANCE] = "maintenance",
    [STATION_LIVE_WHY_AUTOCFG_OFF] = "autocfg_off",
    [STATION_LIVE_WHY_UNSUPPORTED] = "unsupported",
    [STATION_LIVE_WHY_RECONFIGURED] = "reconfigured",
    [STATION_LIVE_WHY_REJECTED] = "rejected",
    [STATION_LIVE_WHY_NO_ANSWER] = "no_answer",
    [STATION_LIVE_WHY_FLASH] = "flash",
    [STATION_LIVE_WHY_NOT_RUNNING] = "not_running",
};
_Static_assert(COUNT(s_live_why) == (size_t)STATION_LIVE_WHY_NOT_RUNNING + 1u, "live view reasons moved: update the table");

const char *web_live_why_name(uint8_t v)
{
    return pick(s_live_why, COUNT(s_live_why), (int)v);
}

static const char *const s_live_end[] = {
    [STATION_LIVE_END_NONE] = "none",
    [STATION_LIVE_END_USER] = "user",
    [STATION_LIVE_END_EXPIRED] = "expired",
    [STATION_LIVE_END_IDLE] = "idle",
    [STATION_LIVE_END_FLASH] = "flash",
};
_Static_assert(COUNT(s_live_end) == (size_t)STATION_LIVE_END_FLASH + 1u, "live view end reasons moved: update the table");

const char *web_live_end_name(uint8_t v)
{
    return pick(s_live_end, COUNT(s_live_end), (int)v);
}

/* ---- reverse lookups -------------------------------------------------------------- */

bool web_base_mode_parse(const char *s, um98x_base_mode_t *out)
{
    int v;
    if (!lookup(s_base_mode, COUNT(s_base_mode), s, &v)) {
        return false;
    }
    *out = (um98x_base_mode_t)v;
    return true;
}

bool web_ntrip_trust_parse(const char *s, ntrip_trust_t *out)
{
    int v;
    if (!lookup(s_trust, COUNT(s_trust), s, &v)) {
        return false;
    }
    *out = (ntrip_trust_t)v;
    return true;
}

bool web_ntrip_backoff_parse(const char *s, ntrip_backoff_profile_t *out)
{
    int v;
    if (!lookup(s_backoff, COUNT(s_backoff), s, &v)) {
        return false;
    }
    *out = (ntrip_backoff_profile_t)v;
    return true;
}

bool web_recovery_mode_parse(const char *s, health_recovery_mode_t *out)
{
    int v;
    if (!lookup(s_recovery_mode, COUNT(s_recovery_mode), s, &v)) {
        return false;
    }
    *out = (health_recovery_mode_t)v;
    return true;
}

bool web_log_level_parse(const char *s, esp_log_level_t *out)
{
    static const char *const levels[] = {
        [ESP_LOG_NONE] = "none",   [ESP_LOG_ERROR] = "error", [ESP_LOG_WARN] = "warn",
        [ESP_LOG_INFO] = "info",   [ESP_LOG_DEBUG] = "debug", [ESP_LOG_VERBOSE] = "verbose",
    };
    int v;
    if (!lookup(levels, COUNT(levels), s, &v)) {
        return false;
    }
    *out = (esp_log_level_t)v;
    return true;
}
