/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * GNSS firmware package staging area (raw flash partition "gnss_pkg").
 *
 * Holds at most one receiver firmware package (.pkg), and only while a
 * user-initiated receiver update is in progress: it is written after the
 * user confirmed an update (download) or uploaded a file, and released when
 * that session ends. Nothing is staged in advance; the daily update check
 * never writes here. Format-agnostic: it stores bytes, checks size and
 * SHA-256, and records metadata. Understanding the package (model check,
 * what to send) is the flasher's job.
 *
 * Layout: sector 0 holds the header, a small log of CRC-checked records
 * {magic, layout version, state, pinned, sequence, size, sha256, version
 * label}; the payload starts at the next 4 KB boundary. Every metadata change
 * (committed, pinned, unpinned, released) appends a record, so none of them
 * needs an erase and a power cut during one leaves the previous state in
 * force (a torn record fails its CRC). pkg_store_begin() erases the header
 * sector first and the commit record is written last, so a power cut
 * mid-download leaves the slot EMPTY, never "valid but truncated".
 * Releasing or invalidating writes only a header record; the payload sectors
 * are erased by the next write, one 4 KB sector ahead of the data. The
 * partition is reserved for this and nothing else uses it.
 *
 * Pinning: while a receiver flash is open, the staged package is the only
 * copy that can finish or recover it. The station pins it before the first
 * journal write and releases it (unpin + empty, pkg_store_release) when the
 * journal closes, whatever the outcome; a journal waiting for the user keeps
 * it pinned. A boot that finds no open journal releases any package left
 * over (pinned or not). The pin is stored in the header, so it survives
 * reboots and power cuts, and while it is set pkg_store_begin() refuses to
 * overwrite the slot. The web upload path also releases the slot when the
 * user cancels or does not confirm an upload within 15 minutes, and so does
 * a failed pre-flight validation.
 *
 * Integrity: SHA-256 is computed with PSA Crypto (psa_hash_*, hardware SHA)
 * while writing. Before a flash the flasher reads the package into RAM and
 * checks the SHA-256 of that copy, which is what gets sent;
 * pkg_store_verify() re-hashes the flash copy (e.g. for the upload page).
 *
 * Writers: fw_update (download after the user's click) and web (manual
 * upload). Reader: um98x_flash. One writer at a time; begin() fails with
 * ESP_ERR_INVALID_STATE otherwise. Do not call from a task whose stack is in
 * PSRAM (flash writes).
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

/**
 * Locate the partition and read the header (once at boot). A write that a
 * reset cut short is forgotten: the slot is then empty. A partition table
 * without the partition (a board with 8 MB flash) is not an error: ESP_OK,
 * pkg_store_capacity() 0, and every other call refuses (ESP_ERR_NOT_FOUND,
 * ESP_ERR_INVALID_STATE).
 */
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

/**
 * Append payload bytes (erases flash ahead of the write position, sector by
 * sector). ESP_ERR_INVALID_SIZE beyond the size given to begin().
 */
esp_err_t pkg_store_write(pkg_store_writer_t w, const void *data, size_t len);

/**
 * Verify size and SHA-256, then commit the header. Closes the writer in
 * every case (no pkg_store_abort() afterwards).
 * @return ESP_ERR_INVALID_SIZE if fewer bytes than announced were written,
 *         ESP_ERR_INVALID_CRC on digest mismatch (slot stays empty in both cases).
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

/** Mark the slot empty (header only; the payload is erased by the next write). Refused while pinned. */
esp_err_t pkg_store_invalidate(void);

/**
 * Release the staged package: unpin and mark the slot empty in one header
 * write (the payload is erased by the next write). For the end of a receiver
 * flash session (journal closed), a failed validation, a cancelled or
 * unconfirmed upload, and a boot without an open journal. ESP_OK also when
 * nothing is staged; ESP_ERR_INVALID_STATE without the partition.
 */
esp_err_t pkg_store_release(void);

#ifdef __cplusplus
}
#endif
