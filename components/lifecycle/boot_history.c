/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "boot_history.h"

#include "settings_codec.h"

bool boot_history_decode(const uint8_t *buf, size_t len, boot_history_entry_t *out, size_t max, size_t *count)
{
    *count = 0;
    blob_reader_t r;
    blob_reader_init(&r, buf, len);
    uint8_t version = blob_get_u8(&r);
    if (!blob_reader_ok(&r) || version != BOOT_HISTORY_VERSION) {
        return false;
    }
    size_t n = blob_get_u8(&r);
    for (size_t i = 0; i < n && *count < max && blob_reader_ok(&r); i++) {
        boot_history_entry_t *e = &out[*count];
        e->cls = blob_get_u8(&r);
        e->raw = blob_get_u8(&r);
        e->reboot_reason = blob_get_u16(&r);
        e->boot_no = blob_get_u32(&r);
        blob_get_str(&r, e->version, sizeof(e->version));
        if (blob_reader_ok(&r)) {
            (*count)++;
        }
    }
    return true;
}

size_t boot_history_encode(const boot_history_entry_t *entries, size_t count, uint8_t *buf, size_t cap)
{
    if (count > BOOT_HISTORY_LEN) {
        count = BOOT_HISTORY_LEN;
    }
    blob_writer_t w;
    blob_writer_init(&w, buf, cap);
    blob_put_u8(&w, BOOT_HISTORY_VERSION);
    blob_put_u8(&w, (uint8_t)count);
    for (size_t i = 0; i < count; i++) {
        blob_put_u8(&w, entries[i].cls);
        blob_put_u8(&w, entries[i].raw);
        blob_put_u16(&w, entries[i].reboot_reason);
        blob_put_u32(&w, entries[i].boot_no);
        blob_put_str(&w, entries[i].version);
    }
    return blob_writer_ok(&w) ? w.len : 0;
}
