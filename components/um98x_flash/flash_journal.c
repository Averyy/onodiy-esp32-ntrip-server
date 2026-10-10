/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "flash_journal.h"

#include <string.h>

#include "settings_codec.h"

static bool main_state(uint8_t s)
{
    return s >= UM98X_JOURNAL_PREPARED && s <= UM98X_JOURNAL_VERIFYING;
}

bool flash_journal_valid(const um98x_flash_journal_t *j)
{
    if (j == NULL || !main_state(j->phase)) {
        return false;
    }
    if (main_state(j->state)) {
        return j->state == j->phase;   /* phase follows the main state */
    }
    return j->state == UM98X_JOURNAL_FAILED || j->state == UM98X_JOURNAL_NEEDS_USER;
}

size_t flash_journal_encode(const um98x_flash_journal_t *j, uint8_t *buf, size_t cap)
{
    if (!flash_journal_valid(j)) {
        return 0;
    }
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, FLASH_JOURNAL_VERSION);
    blob_put_u8(&w, j->state);
    blob_put_u8(&w, j->phase);
    for (size_t i = 0; i < sizeof(j->sha256); i++) {
        blob_put_u8(&w, j->sha256[i]);
    }
    blob_put_u32(&w, j->size);
    blob_put_str(&w, j->expected_fw);
    blob_put_u32(&w, j->expected_build);
    blob_put_str(&w, j->pre_model);
    blob_put_str(&w, j->pre_fw);
    blob_put_u32(&w, j->pre_build);
    blob_put_u32(&w, j->app_baud);
    blob_put_u32(&w, j->boot_baud);
    blob_put_u8(&w, j->transfers);
    blob_put_u8(&w, j->entry_attempts);
    blob_put_u16(&w, j->last_acked_block);
    blob_put_u8(&w, j->failure_class);
    blob_put_i64(&w, j->changed_unix);
    return blob_writer_ok(&w) ? w.len : 0;
}

flash_journal_result_t flash_journal_decode(const uint8_t *buf, size_t len, um98x_flash_journal_t *out)
{
    memset(out, 0, sizeof(*out));
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    uint8_t version = blob_get_u8(&r);
    if (!blob_reader_ok(&r) || version == 0) {
        return FLASH_JOURNAL_DAMAGED;
    }
    if (version > FLASH_JOURNAL_VERSION) {
        return FLASH_JOURNAL_UNKNOWN_VERSION;
    }

    um98x_flash_journal_t j;
    memset(&j, 0, sizeof(j));
    j.version = version;
    j.state = blob_get_u8(&r);
    j.phase = blob_get_u8(&r);
    for (size_t i = 0; i < sizeof(j.sha256); i++) {
        j.sha256[i] = blob_get_u8(&r);
    }
    j.size = blob_get_u32(&r);
    blob_get_str(&r, j.expected_fw, sizeof(j.expected_fw));
    j.expected_build = blob_get_u32(&r);
    blob_get_str(&r, j.pre_model, sizeof(j.pre_model));
    blob_get_str(&r, j.pre_fw, sizeof(j.pre_fw));
    j.pre_build = blob_get_u32(&r);
    j.app_baud = blob_get_u32(&r);
    j.boot_baud = blob_get_u32(&r);
    j.transfers = blob_get_u8(&r);
    j.entry_attempts = blob_get_u8(&r);
    j.last_acked_block = blob_get_u16(&r);
    j.failure_class = blob_get_u8(&r);
    j.changed_unix = blob_get_i64(&r);
    /* Trailing bytes from a newer writer of the same version are ignored. */
    if (!blob_reader_ok(&r) || !flash_journal_valid(&j)) {
        return FLASH_JOURNAL_DAMAGED;
    }
    *out = j;
    return FLASH_JOURNAL_OK;
}

bool flash_journal_needs_race(const um98x_flash_journal_t *j, bool receiver_was_reset)
{
    if (j == NULL || !receiver_was_reset) {
        return false;   /* an ESP32-only reset leaves the receiver where it was */
    }
    switch (j->state) {
    case UM98X_JOURNAL_MENU2_SENT:
    case UM98X_JOURNAL_TRANSFERRING:
    case UM98X_JOURNAL_EOT_SENT:
    case UM98X_JOURNAL_VERIFYING:
    case UM98X_JOURNAL_FAILED:
        return true;
    case UM98X_JOURNAL_NEEDS_USER:
        return j->phase >= UM98X_JOURNAL_MENU2_SENT && j->phase <= UM98X_JOURNAL_VERIFYING;
    default:
        return false;   /* before the loader menu: check the application instead */
    }
}
