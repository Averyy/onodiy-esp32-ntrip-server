/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "leds.h"

#include <stdbool.h>
#include <stddef.h>

#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "led_core.h"

static const char *TAG = "leds";

#define TICK_US     25000
#define LEDC_MODE   LEDC_LOW_SPEED_MODE
#define LEDC_TIMER  LEDC_TIMER_0
#define LEDC_RES    LEDC_TIMER_8_BIT   /* duty 0-255, as led_core's levels */
#define LEDC_FREQ   5000
#define CH_RED      LEDC_CHANNEL_0
#define CH_GREEN    LEDC_CHANNEL_1
#define CH_BLUE     LEDC_CHANNEL_2
#define CH_WIFI     LEDC_CHANNEL_3

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static led_state_t s_state;    /* s_lock */
static bool s_have_state;      /* s_lock */

/* The timer's own (only the timer callback touches them after leds_init). */
static esp_timer_handle_t s_timer;
static led_seq_t s_seq;
static bool s_rgb;
static bool s_wifi;
static int s_gnss_led = -1;
static int s_gnss_in = -1;
static uint32_t s_duty[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };   /* last written, per channel */

static void set_duty(ledc_channel_t ch, uint32_t duty)
{
    if (s_duty[ch] == duty) {
        return;
    }
    s_duty[ch] = duty;
    (void)ledc_set_duty(LEDC_MODE, ch, duty);
    (void)ledc_update_duty(LEDC_MODE, ch);
}

static void tick(void *arg)
{
    (void)arg;
    led_state_t st;
    bool have;
    portENTER_CRITICAL(&s_lock);
    st = s_state;
    have = s_have_state;
    portEXIT_CRITICAL(&s_lock);
    int64_t now = esp_timer_get_time() / 1000;

    if (s_rgb) {
        led_rgb_t c = led_seq_color(&s_seq, have ? &st : NULL, now);
        set_duty(CH_RED, c.r);
        set_duty(CH_GREEN, c.g);
        set_duty(CH_BLUE, c.b);
    }
    if (s_wifi) {
        set_duty(CH_WIFI, have ? led_wifi_level(st.online, st.rssi) : 0);
    }
    if (s_gnss_led >= 0) {
        (void)gpio_set_level((gpio_num_t)s_gnss_led, s_gnss_in >= 0 ? gpio_get_level((gpio_num_t)s_gnss_in) : 0);
    }
}

/* One LEDC channel on `gpio`, dark (with `invert` the pin idles high). */
static esp_err_t channel(ledc_channel_t ch, int gpio, bool invert)
{
    const ledc_channel_config_t cfg = {
        .gpio_num = gpio,
        .speed_mode = LEDC_MODE,
        .channel = ch,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = invert ? 1u : 0u,
    };
    s_duty[ch] = 0;
    return ledc_channel_config(&cfg);
}

esp_err_t leds_init(void)
{
    if (s_timer != NULL) {
        return ESP_OK;
    }
    const board_desc_t *b = board_get();
    if (!board_is_valid()) {
        return ESP_ERR_INVALID_STATE;   /* the pins were not checked */
    }
    s_rgb = b->rgb_gpio[0] >= 0 && b->rgb_gpio[1] >= 0 && b->rgb_gpio[2] >= 0;
    s_wifi = b->wifi_led_gpio >= 0;
    s_gnss_led = b->gnss_led_gpio;
    s_gnss_in = b->gnss_status_gpio;
    if (!s_rgb && !s_wifi && s_gnss_led < 0) {
        return ESP_OK;   /* nothing to drive on this board */
    }

    esp_err_t err = ESP_OK;
    if (s_rgb || s_wifi) {
        const ledc_timer_config_t timer = {
            .speed_mode = LEDC_MODE,
            .duty_resolution = LEDC_RES,
            .timer_num = LEDC_TIMER,
            .freq_hz = LEDC_FREQ,
            .clk_cfg = LEDC_AUTO_CLK,
        };
        err = ledc_timer_config(&timer);
    }
    if (err == ESP_OK && s_rgb) {
        err = channel(CH_RED, b->rgb_gpio[0], b->rgb_invert);
        if (err == ESP_OK) {
            err = channel(CH_GREEN, b->rgb_gpio[1], b->rgb_invert);
        }
        if (err == ESP_OK) {
            err = channel(CH_BLUE, b->rgb_gpio[2], b->rgb_invert);
        }
    }
    if (err == ESP_OK && s_wifi) {
        err = channel(CH_WIFI, b->wifi_led_gpio, false);
    }
    if (err == ESP_OK && s_gnss_led >= 0) {
        const gpio_config_t out = {
            .pin_bit_mask = 1ULL << s_gnss_led,
            .mode = GPIO_MODE_OUTPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        err = gpio_set_level((gpio_num_t)s_gnss_led, 0);
        if (err == ESP_OK) {
            err = gpio_config(&out);
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED pins not configured: %s", esp_err_to_name(err));
        return err;
    }

    led_seq_init(&s_seq, esp_timer_get_time() / 1000);
    const esp_timer_create_args_t args = {
        .callback = tick,
        .name = "leds",
        .dispatch_method = ESP_TIMER_TASK,
    };
    err = esp_timer_create(&args, &s_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_timer, TICK_US);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LED timer not started: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "%s: status LED %s, Wi-Fi LED %s, GNSS LED %s", b->name, s_rgb ? "on" : "none", s_wifi ? "on" : "none",
             s_gnss_led >= 0 ? "on" : "none");
    return ESP_OK;
}

void leds_set(const led_state_t *s)
{
    if (s == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    s_state = *s;
    s_have_state = true;
    portEXIT_CRITICAL(&s_lock);
}
