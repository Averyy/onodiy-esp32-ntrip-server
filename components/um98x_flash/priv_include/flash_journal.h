/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Receiver-flash journal codec and recovery-table decisions (pure logic, no
 * IDF includes, host-testable).
 *
 * v1 layout (settings_codec.h, little-endian): u8 version, u8 state, u8 phase,
 * sha256[32], u32 size, str expected_fw, u32 expected_build, str pre_model,
 * str pre_fw, u32 pre_build, u32 app_baud, u32 boot_baud, u8 transfers,
 * u8 entry_attempts, u16 last_acked_block, u8 failure_class, i64 changed_unix.
 *
 * Decoding is strict about what decides recovery: an unknown version, a
 * truncated record, a state or phase outside the known values, or an
 * over-long string make the journal unusable, and the caller then treats it
 * as open and waiting for the user (send nothing). Trailing bytes after a v1
 * record (a newer writer that appended fields) are ignored.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FLASH_JOURNAL_VERSION  1u
/** Largest v1 record. */
#define FLASH_JOURNAL_V1_MAX   (3u + 32u + 4u + 48u + 4u + 16u + 48u + 4u + 4u + 4u + 1u + 1u + 2u + 1u + 8u)
/** Read buffer: a v1 record plus room for fields a newer writer may append. */
#define FLASH_JOURNAL_READ_MAX 256u

typedef enum {
    FLASH_JOURNAL_OK = 0,
    FLASH_JOURNAL_UNKNOWN_VERSION,   /**< written by a newer schema */
    FLASH_JOURNAL_DAMAGED,           /**< truncated or out-of-range values */
} flash_journal_result_t;

/** Encode a journal (always as FLASH_JOURNAL_VERSION). @return length, 0 if invalid or cap too small. */
size_t flash_journal_encode(const um98x_flash_journal_t *j, uint8_t *buf, size_t cap);

/** Decode a stored journal; out is cleared unless FLASH_JOURNAL_OK. */
flash_journal_result_t flash_journal_decode(const uint8_t *buf, size_t len, um98x_flash_journal_t *out);

/** True if state and phase hold known values and phase is a main (session-order) state. */
bool flash_journal_valid(const um98x_flash_journal_t *j);

/** See um98x_flash_journal_needs_race(). */
bool flash_journal_needs_race(const um98x_flash_journal_t *j, bool receiver_was_reset);

#ifdef __cplusplus
}
#endif
