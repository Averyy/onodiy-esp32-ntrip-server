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

/** How the journal read at boot came out. */
typedef enum {
    FJ_NONE = 0,       /**< no journal: normal start */
    FJ_OPEN,           /**< a readable journal */
    FJ_UNREADABLE,     /**< damaged or unreadable: open, waiting for the user */
    FJ_NEWER,          /**< written by a newer schema: open, waiting for the user */
    FJ_UNKNOWN,        /**< settings unavailable: state unknown */
} fj_read_t;

/**
 * Boot-time recovery table: what the first bytes, bauds and timeouts are,
 * by journal state and reset kind ("ESP-only": the receiver did not reset;
 * "power-on": a power-on or brownout reset both chips). APP-CHECK is
 * listen-only at the journalled application baud, the version query only
 * after the application answered there, then the exit checks. RACE is the
 * stop key at the loader baud for a few seconds as the first UART action.
 */
typedef enum {
    FL_PLAN_NONE = 0,          /**< no journal: normal start, no probe of any kind */
    FL_PLAN_WAIT_USER,         /**< send nothing, wait for the user */
    FL_PLAN_UNKNOWN,           /**< settings unavailable: send nothing, nothing to clear */
    FL_PLAN_RESTORE_CHECK,     /**< prepared / baud-changing, ESP-only: the baud restore ladder, APP-CHECK, re-apply, close */
    FL_PLAN_APP_CHECK,         /**< APP-CHECK, re-apply, close */
    FL_PLAN_LOADER_PEEK,       /**< reset-sent / menu-2-sent, ESP-only: listen 3 s at the loader baud (start bytes -> CAN CAN), one "\r\n"; prompt -> "6"; else APP-CHECK */
    FL_PLAN_RACE_LEAVE,        /**< menu-2-sent, power-on: RACE; prompt -> "6"; then APP-CHECK */
    FL_PLAN_LISTEN_TRANSFER,   /**< transferring / failed, ESP-only: listen-only at the loader baud (T2; 10 s for failed); prompt -> retry within the caps */
    FL_PLAN_RACE_RETRY,        /**< transferring / failed / eot-sent, power-on: RACE; prompt -> retry within the caps; else APP-CHECK */
    FL_PLAN_LISTEN_EOT,        /**< eot-sent, ESP-only: listen up to T2 with the outcome table */
    FL_PLAN_VERIFY_CHECK,      /**< verifying, ESP-only: APP-CHECK; else the prompt means "the application does not start" */
    FL_PLAN_RACE_VERIFY,       /**< verifying, power-on: RACE; prompt -> "6" and the exit checks; else APP-CHECK */
    FL_PLAN_RACE_STOP,         /**< needs-user, power-on, past the menu: RACE, stop at the prompt, wait for the user */
    FL_PLAN_CHECK_WAIT,        /**< needs-user, power-on, before the menu: APP-CHECK, then wait for the user */
} fl_plan_t;

fl_plan_t flash_journal_plan(fj_read_t read, const um98x_flash_journal_t *j, bool receiver_was_reset);

/** True when the plan starts with the early race into the loader. */
bool flash_journal_plan_races(fl_plan_t p);

/** What the plan sends, in words (report-only log). */
const char *flash_journal_plan_text(fl_plan_t p);

/** Short name of a journal state. */
const char *flash_journal_state_name(uint8_t state);

#ifdef __cplusplus
}
#endif
