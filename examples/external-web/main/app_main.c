/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#include "diag.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lifecycle.h"
#include "network.h"
#include "nvs_flash.h"
#include "product_web.h"
#include "settings.h"
#include "web_shell.h"

void app_main(void)
{
    lifecycle_early_boot();
    ESP_ERROR_CHECK(diag_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    /* Never erase NVS on init failure. Recovery remains available. */
    esp_err_t err = nvs_flash_init();
    if (err == ESP_OK) err = settings_init();
    if (err != ESP_OK) ESP_LOGW("product", "settings unavailable: %s", esp_err_to_name(err));
    err = lifecycle_init();
    if (err != ESP_OK) ESP_LOGW("product", "lifecycle: %s", esp_err_to_name(err));
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(network_init());
    ESP_ERROR_CHECK(product_web_init());
    ESP_ERROR_CHECK(network_start());
    ESP_ERROR_CHECK(web_shell_start());
#if !CONFIG_NETWORK_QEMU_ETHERNET
    /* BOOT recovery is owned by this task, with no receiver dependency. */
    const gpio_config_t button = { .pin_bit_mask = 1ULL << GPIO_NUM_0,
        .mode = GPIO_MODE_INPUT, .pull_up_en = GPIO_PULLUP_ENABLE };
    ESP_ERROR_CHECK(gpio_config(&button));
    unsigned held = 0;
    for (;;) {
        if (gpio_get_level(GPIO_NUM_0)) held = 0;
        else if (held < 100 && ++held == 100) {
            product_web_recover();
            ESP_ERROR_CHECK(network_start_setup_ap(true));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
#endif
}
