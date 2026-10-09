/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS firmware package staging area (raw flash partition "gnss_pkg").
 *
 * Holds one receiver firmware package (.pkg) between "downloaded/uploaded" and
 * "flashed to the receiver". Format-agnostic: it stores bytes, checks size and
 * SHA-256, and records metadata. Understanding the package (model check, what
 * to send) is the flasher's job.
 *
 * Layout: sector 0 holds a small header {magic, layout version, state, size,
 * sha256, version label, pinned}; the payload starts at the next 4 KB
 * boundary. The header is written last, so a power cut mid-download leaves
 * the slot EMPTY, never "valid but truncated".
 *
 * Pinning: while a receiver flash is open (started and not yet confirmed by
 * the receiver reporting the new version), the staged package is the only
 * copy that can recover the receiver. The station pins it before the first
 * byte goes to the receiver and unpins it after the post-flash version check.
 * The pin is stored in the header, so it survives reboots and power cuts,
 * and while it is set pkg_store_begin() refuses to overwrite the slot.
 *
 * Integrity: SHA-256 is computed with PSA Crypto (psa_hash_*, hardware SHA)
 * while writing, and recomputed by pkg_store_verify() right before every
 * flash run, because a package may sit in flash for days.
 *
 * Writers: fw_update (download) and web (manual upload). Reader: um98x_flash.
 * One writer at a time; begin() fails with ESP_ERR_INVALID_STATE otherwise.
 * Do not call from a task whose stack is in PSRAM (flash writes).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Metadata of the staged package. */
typedef struct {
    bool valid;            /**< complete and SHA-256 verified at write time */
    bool pinned;           /**< a receiver flash depends on it; cannot be replaced */
    uint32_t size;         /**< payload bytes */
    uint8_t sha256[32];
    char version[32];      /**< label given at finish (e.g. "R4.10Build18975") */
} pkg_store_info_t;

/** Opaque in-progress write. */
typedef struct pkg_store_writer *pkg_store_writer_t;

/** Locate the partition and read the header. */
esp_err_t pkg_store_init(void);

/** Payload capacity in bytes (partition size minus header sector). */
size_t pkg_store_capacity(void);

/** Staged package metadata. ESP_ERR_NOT_FOUND if the slot is empty/invalid. */
esp_err_t pkg_store_get_info(pkg_store_info_t *out);

/**
 * Start writing a new package. Invalidates the current one immediately.
 *
 * @param size            exact payload size; ESP_ERR_INVALID_SIZE if > capacity.
 * @param expected_sha256 digest to verify at finish, or NULL to accept any
 *                        (manual upload: digest is computed and shown instead).
 * @return ESP_ERR_INVALID_STATE if another write is open or the staged
 *         package is pinned.
 */
esp_err_t pkg_store_begin(uint32_t size, const uint8_t expected_sha256[32], pkg_store_writer_t *out);

/** Append payload bytes (erases flash ahead of the write position, sector by sector). */
esp_err_t pkg_store_write(pkg_store_writer_t w, const void *data, size_t len);

/**
 * Verify size and SHA-256, then commit the header.
 * @return ESP_ERR_INVALID_CRC on digest mismatch (slot stays empty).
 */
esp_err_t pkg_store_finish(pkg_store_writer_t w, const char *version, pkg_store_info_t *out);

/** Abandon a write; the slot stays empty. NULL is ignored. */
void pkg_store_abort(pkg_store_writer_t w);

/** Read payload bytes of the valid package (offset 0 = first package byte). */
esp_err_t pkg_store_read(uint32_t offset, void *buf, size_t len);

/**
 * Re-hash the whole staged payload and compare with the header digest
 * (~3 MB: about a second with hardware SHA). Feeds the task watchdog.
 * @return ESP_ERR_INVALID_CRC on mismatch, ESP_ERR_NOT_FOUND if nothing staged.
 */
esp_err_t pkg_store_verify(void);

/** Pin or unpin the staged package (persisted in the header). */
esp_err_t pkg_store_set_pinned(bool pinned);

/** Mark the slot empty (header only; the payload is erased on next begin). Refused while pinned. */
esp_err_t pkg_store_invalidate(void);

#ifdef __cplusplus
}
#endif
