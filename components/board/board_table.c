/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "board_table.h"

#include <stdio.h>
#include <string.h>

#define NONE (-1)

static const board_entry_t s_boards[] = {
    /* UM982 + ESP32-S3-WROOM-1 N16R8 dev board: the receiver's COM1 on GPIO1/2, the BOOT button. The firmware's
     * Kconfig defaults (CONFIG_BOARD_*) are these values; a build with others uses its own for this board. */
    {
        .id = BOARD_ID_DEFAULT,
        .name = "UM982 board (AliExpress)",
        .flash_mb = 16,
        .uart_num = 1,
        .rx_gpio = 1,
        .tx_gpio = 2,
        .port = 1,
        .reset_gpio = NONE,
        .button_gpio = 0,
        .status_led_gpio = NONE,
        .rgb_gpio = { NONE, NONE, NONE },
        .rgb_invert = false,
        .wifi_led_gpio = NONE,
        .gnss_led_gpio = NONE,
        .gnss_status_gpio = NONE,
        .keep_undriven = { NONE, NONE, NONE, NONE },
    },
    /* GNS NTRIP-X (ESP32-S3-WROOM-1U-N8R8, UM980): the receiver's COM1 on GPIO16/15, the case's button on GPIO0,
     * an RGB LED lit low, a Wi-Fi LED and a GNSS LED that follows the receiver's status output on GPIO11. GPIO1/2
     * (a flow-control pair, unused) and GPIO40/41 reach parts of the board whose use is not known. */
    {
        .id = "um980-ntrx",
        .name = "GNS NTRIP-X",
        .flash_mb = 8,
        .uart_num = 1,
        .rx_gpio = 16,
        .tx_gpio = 15,
        .port = 1,
        .reset_gpio = NONE,
        .button_gpio = 0,
        .status_led_gpio = NONE,
        .rgb_gpio = { 6, 5, 4 },
        .rgb_invert = true,
        .wifi_led_gpio = 7,
        .gnss_led_gpio = 8,
        .gnss_status_gpio = 11,
        .keep_undriven = { 1, 2, 40, 41 },
    },
};

size_t board_table_count(void)
{
    return sizeof(s_boards) / sizeof(s_boards[0]);
}

const board_entry_t *board_table_at(size_t i)
{
    return i < board_table_count() ? &s_boards[i] : NULL;
}

const board_entry_t *board_table_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (size_t i = 0; i < board_table_count(); i++) {
        if (strcmp(s_boards[i].id, id) == 0) {
            return &s_boards[i];
        }
    }
    return NULL;
}

const char *board_marker_id(const char *label)
{
    const size_t n = sizeof(BOARD_MARKER_PREFIX) - 1u;
    if (label == NULL || strncmp(label, BOARD_MARKER_PREFIX, n) != 0 || label[n] == '\0') {
        return NULL;
    }
    return label + n;
}

void board_choose(const board_entry_t *dflt, const char *const *markers, size_t n_markers, uint32_t flash_bytes,
                  board_choice_t *out)
{
    memset(out, 0, sizeof(*out));
    out->entry = dflt;
    out->flash_mb = flash_bytes / (1024u * 1024u);
    out->trusted = true;

    if (n_markers == 0) {
        out->from = BOARD_FROM_DEFAULT;
        if (out->flash_mb != 0 && out->flash_mb != dflt->flash_mb) {
            out->issue = BOARD_NOTE_FLASH;
        }
        return;
    }
    out->from = BOARD_FROM_MARKER;
    out->trusted = false;
    if (n_markers > 1) {
        out->issue = BOARD_BAD_TWO;
        snprintf(out->why, sizeof(out->why), "The partition table marks %u boards. Install the firmware for this board again.",
                 (unsigned)n_markers);
        return;
    }
    const char *id = board_marker_id(markers[0]);
    const board_entry_t *e = board_table_find(id);
    if (e == NULL) {
        out->issue = BOARD_BAD_UNKNOWN;
        snprintf(out->why, sizeof(out->why), "This firmware does not know the board \"%.10s\". Update it or install it again.",
                 id != NULL ? id : "");
        return;
    }
    out->entry = e;
    if (out->flash_mb != 0 && out->flash_mb != e->flash_mb) {
        out->issue = BOARD_BAD_FLASH;
        snprintf(out->why, sizeof(out->why), "The install is for the %s (%u MB flash), but this chip has %u MB.", e->name,
                 (unsigned)e->flash_mb, (unsigned)out->flash_mb);
        return;
    }
    out->trusted = true;
}
