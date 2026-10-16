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
    /* Every entry is read, the ones past max too: the start times follow the last one. */
    size_t whole = 0;
    boot_history_entry_t spare;
    for (size_t i = 0; i < n && blob_reader_ok(&r); i++) {
        boot_history_entry_t *e = (*count < max) ? &out[*count] : &spare;
        e->cls = blob_get_u8(&r);
        e->raw = blob_get_u8(&r);
        e->reboot_reason = blob_get_u16(&r);
        e->boot_no = blob_get_u32(&r);
        e->prev_uptime_s = blob_get_u32(&r);
        blob_get_str(&r, e->version, sizeof(e->version));
        e->start_unix = 0;
        if (blob_reader_ok(&r)) {
            whole++;
            if (e != &spare) {
                (*count)++;
            }
        }
    }
    if (whole == n && blob_reader_remaining(&r) >= 4 * n) {
        for (size_t i = 0; i < n; i++) {
            uint32_t t = blob_get_u32(&r);
            if (i < *count) {
                out[i].start_unix = t;
            }
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
        blob_put_u32(&w, entries[i].prev_uptime_s);
        blob_put_str(&w, entries[i].version);
    }
    /* The start times only while every v1 reader still holds the blob (long versions leave no room). */
    size_t room = (cap < BOOT_HISTORY_BLOB_MAX) ? cap : BOOT_HISTORY_BLOB_MAX;
    if (blob_writer_ok(&w) && w.len + 4 * count <= room) {
        for (size_t i = 0; i < count; i++) {
            blob_put_u32(&w, entries[i].start_unix);
        }
    }
    return blob_writer_ok(&w) ? w.len : 0;
}
