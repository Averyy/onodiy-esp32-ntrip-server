/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Boot history blob (life/boots) codec (pure logic, no IDF includes,
 * host-testable).
 *
 * v1 layout (settings_codec.h, little-endian): u8 version, u8 count, then
 * count x {u8 reset class, u8 raw reset reason, u16 planned-reboot reason,
 * u32 boots since power-on, u32 previous session uptime (s, UINT32_MAX =
 * unknown), str app version}, newest first; then, when it fits, count x u32
 * start time (UTC s, 0 = not known) in the same order. Builds that predate
 * the start times read the entries and stop before them, and drop them when
 * they record a boot: every start then reads as not dated. They are written
 * only while the blob stays within BOOT_HISTORY_BLOB_MAX, which every v1
 * reader holds, and are read all or none.
 *
 * A blob that is not v1 (a newer app's layout, after a rollback) is reported
 * as such and must be left untouched: "life" survives factory reset, so
 * overwriting it would destroy the device's history.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_HISTORY_VERSION  1
#define BOOT_HISTORY_LEN      16
#define BOOT_HISTORY_VER_MAX  31    /**< app version characters kept (esp_app_desc_t.version) */
/** Largest v1 blob (the start times included); a stored blob longer than this was written by a newer layout. */
#define BOOT_HISTORY_BLOB_MAX (2 + BOOT_HISTORY_LEN * (12 + 1 + BOOT_HISTORY_VER_MAX))
/** Previous session uptime not known (power-on, brownout, other resets). */
#define BOOT_HISTORY_UPTIME_UNKNOWN UINT32_MAX

typedef struct {
    uint8_t cls;                /**< lifecycle_reset_class_t */
    uint8_t raw;                /**< esp_reset_reason_t */
    uint16_t reboot_reason;     /**< lifecycle_reboot_reason_t the previous boot planned */
    uint32_t boot_no;           /**< boots since power-on */
    uint32_t prev_uptime_s;     /**< uptime of the session before this boot, BOOT_HISTORY_UPTIME_UNKNOWN if unknown */
    char version[BOOT_HISTORY_VER_MAX + 1];
    uint32_t start_unix;        /**< UTC second this boot started, 0 = not known (no trusted clock yet, an older build) */
} boot_history_entry_t;

/**
 * Decode a v1 history into out[0..max). Entries after a damaged one are
 * dropped; start times that are missing or cut short read as 0.
 * @return false if buf is not a v1 history (newer or foreign layout: leave it alone).
 */
bool boot_history_decode(const uint8_t *buf, size_t len, boot_history_entry_t *out, size_t max, size_t *count);

/** Encode at most BOOT_HISTORY_LEN entries, newest first. @return blob length, 0 if it does not fit. */
size_t boot_history_encode(const boot_history_entry_t *entries, size_t count, uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif
