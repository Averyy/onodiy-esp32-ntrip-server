/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "board.h"

#include <stddef.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "board";

static const board_desc_t s_board = {
    .name = "UM982 + ESP32-S3-WROOM-1 N16R8",
    .gnss_uart_num = CONFIG_BOARD_GNSS_UART_NUM,
    .gnss_rx_gpio = CONFIG_BOARD_GNSS_RX_GPIO,
    .gnss_tx_gpio = CONFIG_BOARD_GNSS_TX_GPIO,
    .gnss_reset_gpio = CONFIG_BOARD_GNSS_RESET_GPIO,
    .button_gpio = CONFIG_BOARD_BUTTON_GPIO,
    .status_led_gpio = CONFIG_BOARD_STATUS_LED_GPIO,
#if CONFIG_BOARD_SD_ENABLE
    .sd_present = true,
    .sd_miso_gpio = CONFIG_BOARD_SD_MISO_GPIO,
    .sd_mosi_gpio = CONFIG_BOARD_SD_MOSI_GPIO,
    .sd_sck_gpio = CONFIG_BOARD_SD_SCK_GPIO,
    .sd_cs_gpio = CONFIG_BOARD_SD_CS_GPIO,
#else
    .sd_present = false,
    .sd_miso_gpio = -1,
    .sd_mosi_gpio = -1,
    .sd_sck_gpio = -1,
    .sd_cs_gpio = -1,
#endif
};

static bool s_valid;

const board_desc_t *board_get(void)
{
    return &s_board;
}

bool board_is_valid(void)
{
    return s_valid;
}

static esp_err_t check_pin(const char *what, int gpio, bool output, bool optional, bool strapping_ok)
{
    if (gpio < 0) {
        if (optional) {
            return ESP_OK;
        }
        ESP_LOGE(TAG, "%s: no GPIO configured", what);
        return ESP_ERR_INVALID_ARG;
    }
    bool valid = output ? GPIO_IS_VALID_OUTPUT_GPIO(gpio) : GPIO_IS_VALID_GPIO(gpio);
    if (!valid) {
        ESP_LOGE(TAG, "%s: GPIO%d is not a valid %s pin", what, gpio, output ? "output" : "input");
        return ESP_ERR_INVALID_ARG;
    }
    const char *owner = board_pin_owner(gpio);
    if (owner != NULL) {
        ESP_LOGE(TAG, "%s: GPIO%d is used by %s", what, gpio, owner);
        return ESP_ERR_INVALID_ARG;
    }
    if (board_pin_strapping(gpio) && !strapping_ok) {
        ESP_LOGW(TAG, "%s: GPIO%d is a strapping pin", what, gpio);
    }
    return ESP_OK;
}

esp_err_t board_init(void)
{
    if (s_valid) {
        return ESP_OK;   /* an early boot step may have validated the pins already */
    }
    const board_desc_t *b = &s_board;
    const struct {
        const char *what;
        int gpio;
        bool output;
        bool optional;
        bool strapping_ok;     /* the BOOT button sits on a strapping pin by design */
    } pins[] = {
        { "GNSS RX", b->gnss_rx_gpio, false, false, false },
        { "GNSS TX", b->gnss_tx_gpio, true, false, false },
        { "GNSS reset", b->gnss_reset_gpio, true, true, false },
        { "button", b->button_gpio, false, true, true },
        { "status LED", b->status_led_gpio, true, true, false },
        { "SD MISO", b->sd_miso_gpio, false, true, false },
        { "SD MOSI", b->sd_mosi_gpio, true, true, false },
        { "SD SCK", b->sd_sck_gpio, true, true, false },
        { "SD CS", b->sd_cs_gpio, true, true, false },
    };
    const size_t n = sizeof(pins) / sizeof(pins[0]);

    for (size_t i = 0; i < n; i++) {
        esp_err_t err = check_pin(pins[i].what, pins[i].gpio, pins[i].output, pins[i].optional,
                                  pins[i].strapping_ok);
        if (err != ESP_OK) {
            return err;
        }
        for (size_t j = 0; j < i; j++) {
            if (pins[i].gpio >= 0 && pins[i].gpio == pins[j].gpio) {
                ESP_LOGE(TAG, "%s and %s both use GPIO%d", pins[j].what, pins[i].what, pins[i].gpio);
                return ESP_ERR_INVALID_ARG;
            }
        }
    }

    if (b->button_gpio >= 0) {
        const gpio_config_t button = {
            .pin_bit_mask = 1ULL << b->button_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_ENABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&button);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* TODO: drive the reset line inactive and the LED off once their polarity
     * is confirmed on hardware. Neither is known to be wired on the dev board. */

    s_valid = true;
    ESP_LOGI(TAG, "%s: GNSS on UART%d (RX GPIO%d, TX GPIO%d)", b->name, b->gnss_uart_num,
             b->gnss_rx_gpio, b->gnss_tx_gpio);
    return ESP_OK;
}
