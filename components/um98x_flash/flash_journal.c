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

uint8_t flash_journal_effective_state(const um98x_flash_journal_t *j)
{
    if (j->transfers > 0 && j->state >= UM98X_JOURNAL_PREPARED && j->state <= UM98X_JOURNAL_MENU2_SENT) {
        /* A retry after the point of no return records the entry and menu
         * states again; the receiver's flash is presumed modified all the
         * same, so the loader is never left with "6" here. */
        return UM98X_JOURNAL_FAILED;
    }
    return j->state;
}

fl_plan_t flash_journal_plan(fj_read_t read, const um98x_flash_journal_t *j, bool receiver_was_reset)
{
    switch (read) {
    case FJ_NONE:
        return FL_PLAN_NONE;
    case FJ_UNKNOWN:
        return FL_PLAN_UNKNOWN;
    case FJ_OPEN:
        break;
    default:
        return FL_PLAN_WAIT_USER;   /* unreadable or newer: send nothing */
    }
    if (j == NULL || !flash_journal_valid(j)) {
        return FL_PLAN_WAIT_USER;
    }
    bool esp_only = !receiver_was_reset;
    switch (flash_journal_effective_state(j)) {
    case UM98X_JOURNAL_PREPARED:
    case UM98X_JOURNAL_BAUD_CHANGING:
        return esp_only ? FL_PLAN_RESTORE_CHECK : FL_PLAN_APP_CHECK;
    case UM98X_JOURNAL_RESET_SENT:
        return esp_only ? FL_PLAN_LOADER_PEEK : FL_PLAN_APP_CHECK;
    case UM98X_JOURNAL_MENU2_SENT:
        return esp_only ? FL_PLAN_LOADER_PEEK : FL_PLAN_RACE_LEAVE;
    case UM98X_JOURNAL_TRANSFERRING:
    case UM98X_JOURNAL_FAILED:
        return esp_only ? FL_PLAN_LISTEN_TRANSFER : FL_PLAN_RACE_RETRY;
    case UM98X_JOURNAL_EOT_SENT:
        return esp_only ? FL_PLAN_LISTEN_EOT : FL_PLAN_RACE_RETRY;
    case UM98X_JOURNAL_VERIFYING:
        return esp_only ? FL_PLAN_VERIFY_CHECK : FL_PLAN_RACE_VERIFY;
    case UM98X_JOURNAL_NEEDS_USER:
        if (esp_only) {
            return FL_PLAN_WAIT_USER;
        }
        /* Past the menu, or after any transfer (a post-PoNR re-entry that
         * stopped in reset-sent): the loader is the way back to a receiver
         * whose flash was modified, and the race is its only window. */
        return (j->phase >= UM98X_JOURNAL_MENU2_SENT || j->transfers > 0) ? FL_PLAN_RACE_STOP : FL_PLAN_CHECK_WAIT;
    default:
        return FL_PLAN_WAIT_USER;
    }
}

bool flash_journal_plan_races(fl_plan_t p)
{
    return p == FL_PLAN_RACE_LEAVE || p == FL_PLAN_RACE_RETRY || p == FL_PLAN_RACE_VERIFY || p == FL_PLAN_RACE_STOP;
}

const char *flash_journal_plan_text(fl_plan_t p)
{
    switch (p) {
    case FL_PLAN_NONE:
        return "no session: normal start";
    case FL_PLAN_WAIT_USER:
        return "send nothing and wait for the user";
    case FL_PLAN_UNKNOWN:
        return "settings unavailable: send nothing";
    case FL_PLAN_RESTORE_CHECK:
        return "restore COM1 to the application baud (config com1 at the loader baud, then at both), then listen "
               "for the application and query its version";
    case FL_PLAN_APP_CHECK:
        return "listen for the application at its baud, query its version, re-apply the profile, close";
    case FL_PLAN_LOADER_PEEK:
        return "listen 3 s at the loader baud (CAN CAN on a start byte), one \"\\r\\n\"; at the prompt \"6\", else "
               "listen for the application";
    case FL_PLAN_RACE_LEAVE:
        return "stop key at the loader baud from boot; at the prompt \"6\"; then listen for the application";
    case FL_PLAN_LISTEN_TRANSFER:
        return "listen only at the loader baud (start bytes: CAN CAN); at the prompt send the same package again "
               "within the caps; else listen for the application";
    case FL_PLAN_RACE_RETRY:
        return "stop key at the loader baud from boot; at the prompt send the same package again within the caps; "
               "else listen for the application";
    case FL_PLAN_LISTEN_EOT:
        return "listen only at the loader baud until the receiver reports the outcome of the transfer";
    case FL_PLAN_VERIFY_CHECK:
        return "listen for the application and query its version; else listen for the loader prompt";
    case FL_PLAN_RACE_VERIFY:
        return "stop key at the loader baud from boot; at the prompt \"6\" and the version check; else listen for "
               "the application";
    case FL_PLAN_RACE_STOP:
        return "stop key at the loader baud from boot, stop at the prompt and wait for the user";
    case FL_PLAN_CHECK_WAIT:
        return "listen for the application and query its version, then wait for the user";
    default:
        return "?";
    }
}

const char *flash_journal_state_name(uint8_t state)
{
    static const char *const k[] = { "?",           "prepared",  "baud-changing", "reset-sent", "menu-2-sent",
                                     "transferring", "eot-sent", "verifying",     "failed",     "needs-user" };
    return state < sizeof(k) / sizeof(k[0]) ? k[state] : "?";
}

bool flash_journal_needs_race(const um98x_flash_journal_t *j, bool receiver_was_reset)
{
    if (j == NULL || !receiver_was_reset) {
        return false;   /* an ESP32-only reset leaves the receiver where it was */
    }
    /* Before the loader menu (and without a transfer): the application is checked instead. */
    return flash_journal_plan_races(flash_journal_plan(FJ_OPEN, j, true));
}
