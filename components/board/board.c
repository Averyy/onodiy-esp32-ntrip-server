/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "board.h"

#include <stddef.h>
#include <string.h>

#include "board_pins.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "sdkconfig.h"

static const char *TAG = "board";

/* The receiver link's pins and port come from the saved wiring when it passes the check (board_init). */
static board_desc_t s_board = {
    .name = "UM982 + ESP32-S3-WROOM-1 N16R8",
    .gnss_uart_num = CONFIG_BOARD_GNSS_UART_NUM,
    .gnss_rx_gpio = CONFIG_BOARD_GNSS_RX_GPIO,
    .gnss_tx_gpio = CONFIG_BOARD_GNSS_TX_GPIO,
    .gnss_port = CONFIG_BOARD_GNSS_PORT,
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
static bool s_have_saved;    /* board_use_wiring() gave one */
static board_wiring_t s_saved;
static char s_link_off[64];  /* why the receiver link stays closed this start ("" = it opens) */

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

/* The board's pins besides the receiver link's (for the wiring check). */
static size_t other_pins(board_pin_use_t *out)
{
    const board_desc_t *b = &s_board;
    size_t n = 0;
    out[n++] = (board_pin_use_t){ "button", b->button_gpio };
    out[n++] = (board_pin_use_t){ "status LED", b->status_led_gpio };
    out[n++] = (board_pin_use_t){ "receiver reset", b->gnss_reset_gpio };
    out[n++] = (board_pin_use_t){ "SD MISO", b->sd_miso_gpio };
    out[n++] = (board_pin_use_t){ "SD MOSI", b->sd_mosi_gpio };
    out[n++] = (board_pin_use_t){ "SD SCK", b->sd_sck_gpio };
    out[n++] = (board_pin_use_t){ "SD CS", b->sd_cs_gpio };
    return n;
}

#define OTHER_PINS_MAX 7u

bool board_wiring_check(const board_wiring_t *w, char *why, size_t cap)
{
    board_pin_use_t uses[OTHER_PINS_MAX];
    size_t n = other_pins(uses);
    return !board_wiring_problem(w, uses, n, why, cap);
}

void board_wiring_default(board_wiring_t *out)
{
    out->rx_gpio = (int8_t)CONFIG_BOARD_GNSS_RX_GPIO;
    out->tx_gpio = (int8_t)CONFIG_BOARD_GNSS_TX_GPIO;
    out->port = (uint8_t)CONFIG_BOARD_GNSS_PORT;
}

void board_wiring_active(board_wiring_t *out)
{
    out->rx_gpio = (int8_t)s_board.gnss_rx_gpio;
    out->tx_gpio = (int8_t)s_board.gnss_tx_gpio;
    out->port = (uint8_t)s_board.gnss_port;
}

void board_use_wiring(const board_wiring_t *w)
{
    if (s_valid) {
        ESP_LOGW(TAG, "receiver wiring given after the pins were checked: used from the next start");
        return;
    }
    s_have_saved = w != NULL;
    if (w != NULL) {
        s_saved = *w;
    }
}

const char *board_link_off_reason(void)
{
    return s_link_off[0] != '\0' ? s_link_off : NULL;
}

size_t board_link_pins(int8_t *out, size_t cap)
{
    board_pin_use_t uses[OTHER_PINS_MAX];
    size_t n_uses = other_pins(uses);
    size_t n = 0;
    for (int g = 0; g <= 48 && n < cap; g++) {
        bool taken = false;
        for (size_t i = 0; i < n_uses; i++) {
            taken = taken || uses[i].gpio == g;
        }
        if (board_pin_for_link(g) && !taken) {
            out[n++] = (int8_t)g;
        }
    }
    return n;
}

/* The saved wiring replaces the build's when it passes the check. One that
 * does not (a later build may reserve one of its pins) is left stored and
 * shown, and the receiver link stays closed: the board is wired otherwise, so
 * the build's pins may reach something else, and nothing is sent there. */
static void take_saved_wiring(void)
{
    if (!s_have_saved) {
        return;
    }
    const board_wiring_t w = s_saved;
    char why[64];
    if (!board_wiring_check(&w, why, sizeof(why))) {
        strlcpy(s_link_off, why, sizeof(s_link_off));
        ESP_LOGE(TAG, "saved receiver wiring (RX GPIO%d, TX GPIO%d, COM%u) not usable: %s The receiver link stays closed.",
                 w.rx_gpio, w.tx_gpio, (unsigned)w.port, why);
        return;
    }
    s_board.gnss_rx_gpio = w.rx_gpio;
    s_board.gnss_tx_gpio = w.tx_gpio;
    s_board.gnss_port = w.port;
    ESP_LOGI(TAG, "saved receiver wiring: RX GPIO%d, TX GPIO%d, COM%u", w.rx_gpio, w.tx_gpio, (unsigned)w.port);
}

esp_err_t board_init(void)
{
    if (s_valid) {
        return ESP_OK;   /* an early boot step may have validated the pins already */
    }
    take_saved_wiring();
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
    ESP_LOGI(TAG, "%s: GNSS on UART%d (RX GPIO%d, TX GPIO%d) to the receiver's COM%d", b->name, b->gnss_uart_num,
             b->gnss_rx_gpio, b->gnss_tx_gpio, b->gnss_port);
    return ESP_OK;
}
