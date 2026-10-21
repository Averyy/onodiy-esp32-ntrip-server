/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "board.h"

#include <stddef.h>
#include <string.h>

#include "board_pins.h"
#include "board_table.h"
#include "driver/gpio.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "sdkconfig.h"

static const char *TAG = "board";

/* Filled by pick_board() from the board table; the receiver link's pins and port come from the saved wiring when
 * it passes the check (board_init). */
static board_desc_t s_board;
static board_entry_t s_original;     /* the original board with this build's Kconfig pins */
static board_choice_t s_choice;
static bool s_picked;

static bool s_valid;
static bool s_have_saved;    /* board_use_wiring() gave one */
static board_wiring_t s_saved;
static char s_link_off[64];  /* why the saved wiring is refused this start ("" = it is not) */

/* The original board is the build's: its Kconfig pins replace the table's (a build for a board wired otherwise). */
static void original_from_kconfig(board_entry_t *e)
{
    *e = *board_table_find(BOARD_ID_DEFAULT);
    e->uart_num = (int8_t)CONFIG_BOARD_GNSS_UART_NUM;
    e->rx_gpio = (int8_t)CONFIG_BOARD_GNSS_RX_GPIO;
    e->tx_gpio = (int8_t)CONFIG_BOARD_GNSS_TX_GPIO;
    e->port = (uint8_t)CONFIG_BOARD_GNSS_PORT;
    e->reset_gpio = (int8_t)CONFIG_BOARD_GNSS_RESET_GPIO;
    e->button_gpio = (int8_t)CONFIG_BOARD_BUTTON_GPIO;
    e->status_led_gpio = (int8_t)CONFIG_BOARD_STATUS_LED_GPIO;
}

#define MARKERS_MAX 4u

/* The board.<id> marker partitions' labels (at most `cap` kept). @return how many the table has, up to MARKERS_MAX. */
static size_t find_markers(char labels[][BOARD_LABEL_MAX + 1], size_t cap)
{
    size_t n = 0;
    esp_partition_iterator_t it =
        esp_partition_find((esp_partition_type_t)BOARD_MARKER_TYPE, (esp_partition_subtype_t)BOARD_MARKER_SUBTYPE, NULL);
    while (it != NULL && n < MARKERS_MAX) {
        const esp_partition_t *p = esp_partition_get(it);
        if (board_marker_id(p->label) != NULL) {
            if (n < cap) {
                strlcpy(labels[n], p->label, BOARD_LABEL_MAX + 1);
            }
            n++;
        }
        it = esp_partition_next(it);
    }
    esp_partition_iterator_release(it);
    return n;
}

/* Which board this is, once (the first board_*() call): the partition table's marker, else the original. */
static void pick_board(void)
{
    if (s_picked) {
        return;
    }
    /* The first call is app_main's (board_init, or an early boot step before it), before any other task asks. */
    original_from_kconfig(&s_original);
    char labels[MARKERS_MAX][BOARD_LABEL_MAX + 1];
    const char *marks[MARKERS_MAX];
    size_t n = find_markers(labels, MARKERS_MAX);
    for (size_t i = 0; i < n; i++) {
        marks[i] = labels[i];
    }
    uint32_t flash = 0;
    if (esp_flash_get_physical_size(NULL, &flash) != ESP_OK) {
        flash = 0;   /* not known: not checked */
    }
    board_choose(&s_original, marks, n, flash, &s_choice);

    const board_entry_t *e = s_choice.entry;
    const bool t = s_choice.trusted;   /* else nothing but the button is driven or read */
    s_board = (board_desc_t){
        .id = e->id,
        .name = e->name,
        .antenna = e->antenna,
        .source = s_choice.from == BOARD_FROM_MARKER ? BOARD_SOURCE_MARKER : BOARD_SOURCE_DEFAULT,
        .flash_mb = e->flash_mb,
        .gnss_uart_num = e->uart_num,
        .gnss_rx_gpio = e->rx_gpio,
        .gnss_tx_gpio = e->tx_gpio,
        .gnss_port = e->port,
        .gnss_reset_gpio = t ? e->reset_gpio : -1,
        .button_gpio = e->button_gpio,
        .status_led_gpio = t ? e->status_led_gpio : -1,
        .rgb_gpio = { t ? e->rgb_gpio[0] : -1, t ? e->rgb_gpio[1] : -1, t ? e->rgb_gpio[2] : -1 },
        .rgb_invert = e->rgb_invert,
        .wifi_led_gpio = t ? e->wifi_led_gpio : -1,
        .gnss_led_gpio = t ? e->gnss_led_gpio : -1,
        .gnss_status_gpio = t ? e->gnss_status_gpio : -1,
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
    _Static_assert(BOARD_UNDRIVEN_MAX == BOARD_KEEP_MAX, "the undriven pins of board.h and board_table.h");
    for (size_t i = 0; i < BOARD_KEEP_MAX; i++) {
        s_board.undriven_gpio[i] = e->keep_undriven[i];
    }
    s_picked = true;   /* only once filled in */
}

const board_desc_t *board_get(void)
{
    pick_board();
    return &s_board;
}

bool board_is_valid(void)
{
    return s_valid;
}

const char *board_problem(void)
{
    pick_board();
    return s_choice.trusted ? NULL : s_choice.why;
}

bool board_wiring_refused(void)
{
    return s_link_off[0] != '\0';
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
    const board_desc_t *b = board_get();
    size_t n = 0;
    out[n++] = (board_pin_use_t){ "button", b->button_gpio };
    out[n++] = (board_pin_use_t){ "status LED", b->status_led_gpio };
    out[n++] = (board_pin_use_t){ "red LED", b->rgb_gpio[0] };
    out[n++] = (board_pin_use_t){ "green LED", b->rgb_gpio[1] };
    out[n++] = (board_pin_use_t){ "blue LED", b->rgb_gpio[2] };
    out[n++] = (board_pin_use_t){ "Wi-Fi LED", b->wifi_led_gpio };
    out[n++] = (board_pin_use_t){ "GNSS LED", b->gnss_led_gpio };
    out[n++] = (board_pin_use_t){ "receiver status", b->gnss_status_gpio };
    out[n++] = (board_pin_use_t){ "receiver reset", b->gnss_reset_gpio };
    out[n++] = (board_pin_use_t){ "SD MISO", b->sd_miso_gpio };
    out[n++] = (board_pin_use_t){ "SD MOSI", b->sd_mosi_gpio };
    out[n++] = (board_pin_use_t){ "SD SCK", b->sd_sck_gpio };
    out[n++] = (board_pin_use_t){ "SD CS", b->sd_cs_gpio };
    for (size_t i = 0; i < BOARD_UNDRIVEN_MAX; i++) {
        out[n++] = (board_pin_use_t){ NULL, b->undriven_gpio[i] };   /* wired to something unknown */
    }
    return n;
}

#define OTHER_PINS_MAX (13u + BOARD_UNDRIVEN_MAX)

bool board_wiring_check(const board_wiring_t *w, char *why, size_t cap)
{
    board_pin_use_t uses[OTHER_PINS_MAX];
    size_t n = other_pins(uses);
    return !board_wiring_problem(w, uses, n, why, cap);
}

void board_wiring_default(board_wiring_t *out)
{
    pick_board();
    const board_entry_t *e = s_choice.entry;
    out->rx_gpio = e->rx_gpio;
    out->tx_gpio = e->tx_gpio;
    out->port = e->port;
}

void board_wiring_active(board_wiring_t *out)
{
    pick_board();
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
    const char *bad = board_problem();
    if (bad != NULL) {
        return bad;
    }
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

bool board_journal_note(int32_t *a, int32_t *b)
{
    pick_board();
    *a = (int32_t)s_choice.issue;
    *b = (int32_t)((s_choice.flash_mb & 0xFFu) | ((s_board.flash_mb & 0xFFu) << 8));
    return s_choice.issue != BOARD_OK;
}

/* The board choice in the log (once per start). */
static void report_choice(void)
{
    static bool s_reported;
    if (s_reported) {
        return;
    }
    s_reported = true;
    const board_desc_t *b = &s_board;
    switch (s_choice.issue) {
    case BOARD_OK:
        ESP_LOGI(TAG, "board: %s (%s)", b->name,
                 b->source == BOARD_SOURCE_MARKER ? "marked in the partition table" : "no board marker");
        return;
    case BOARD_NOTE_FLASH:
        ESP_LOGW(TAG, "board: %s (no board marker), but the chip has %u MB flash, not %u MB", b->name,
                 (unsigned)s_choice.flash_mb, b->flash_mb);
        return;
    default:
        ESP_LOGE(TAG, "board: %s The receiver link stays closed and the board's LEDs are not used.", s_choice.why);
        return;
    }
}

esp_err_t board_init(void)
{
    if (s_valid) {
        return ESP_OK;   /* an early boot step may have validated the pins already */
    }
    pick_board();
    report_choice();
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
        { "red LED", b->rgb_gpio[0], true, true, false },
        { "green LED", b->rgb_gpio[1], true, true, false },
        { "blue LED", b->rgb_gpio[2], true, true, false },
        { "Wi-Fi LED", b->wifi_led_gpio, true, true, false },
        { "GNSS LED", b->gnss_led_gpio, true, true, false },
        { "receiver status", b->gnss_status_gpio, false, true, false },
        { "undriven", b->undriven_gpio[0], false, true, false },
        { "undriven", b->undriven_gpio[1], false, true, false },
        { "undriven", b->undriven_gpio[2], false, true, false },
        { "undriven", b->undriven_gpio[3], false, true, false },
        { "SD MISO", b->sd_miso_gpio, false, true, false },
        { "SD MOSI", b->sd_mosi_gpio, true, true, false },
        { "SD SCK", b->sd_sck_gpio, true, true, false },
        { "SD CS", b->sd_cs_gpio, true, true, false },
    };
    _Static_assert(BOARD_UNDRIVEN_MAX == 4, "list every undriven pin above");
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
    if (b->gnss_status_gpio >= 0) {
        const gpio_config_t status = {
            .pin_bit_mask = 1ULL << b->gnss_status_gpio,
            .mode = GPIO_MODE_INPUT,
            .pull_up_en = GPIO_PULLUP_DISABLE,
            .pull_down_en = GPIO_PULLDOWN_ENABLE,   /* low (not good) while the receiver does not drive it */
            .intr_type = GPIO_INTR_DISABLE,
        };
        esp_err_t err = gpio_config(&status);
        if (err != ESP_OK) {
            return err;
        }
    }

    /* TODO: drive the reset line inactive and the single status LED off once their polarity is confirmed on
     * hardware. Neither is known to be wired on the original board. */

    s_valid = true;
    ESP_LOGI(TAG, "%s: GNSS on UART%d (RX GPIO%d, TX GPIO%d) to the receiver's COM%d", b->name, b->gnss_uart_num,
             b->gnss_rx_gpio, b->gnss_tx_gpio, b->gnss_port);
    return ESP_OK;
}
