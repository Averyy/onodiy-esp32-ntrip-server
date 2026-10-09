/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "boot_info.h"

#include <inttypes.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_psram.h"

static const char *TAG = "boot";

const char *boot_reset_reason_str(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_EXT:       return "external pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "panic/exception";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "other watchdog";
    case ESP_RST_DEEPSLEEP: return "deep-sleep wake";
    case ESP_RST_BROWNOUT:  return "brownout";
    case ESP_RST_SDIO:      return "SDIO";
    case ESP_RST_USB:       return "USB";
    case ESP_RST_JTAG:      return "JTAG";
    case ESP_RST_EFUSE:     return "eFuse error";
    case ESP_RST_PWR_GLITCH: return "power glitch";
    case ESP_RST_CPU_LOCKUP: return "CPU lockup";
    case ESP_RST_UNKNOWN:
    default:                return "unknown";
    }
}

void boot_info_log(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    esp_reset_reason_t reason = esp_reset_reason();

    esp_chip_info_t chip;
    esp_chip_info(&chip);

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        flash_size = 0;
    }

    ESP_LOGI(TAG, "%s %s (built %s %s)", app->project_name, app->version, app->date, app->time);
    ESP_LOGI(TAG, "ESP-IDF %s, chip rev v%u.%u, %u core(s)", esp_get_idf_version(),
             (unsigned)(chip.revision / 100), (unsigned)(chip.revision % 100), (unsigned)chip.cores);
    ESP_LOGI(TAG, "reset reason: %s (%d)", boot_reset_reason_str(reason), (int)reason);
    ESP_LOGI(TAG, "flash %" PRIu32 " KB, PSRAM %u KB", flash_size / 1024,
             (unsigned)(esp_psram_get_size() / 1024));
    ESP_LOGI(TAG, "free heap: internal %u KB, PSRAM %u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
}
