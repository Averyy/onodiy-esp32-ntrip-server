/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_shell_enums.h"

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
