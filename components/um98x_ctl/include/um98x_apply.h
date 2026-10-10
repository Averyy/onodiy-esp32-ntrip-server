/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Profile apply: options and result (types only, no IDF includes), shared by
 * the command layer's public API and its host-tested executor.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_APPLY_CMD_MAX    96u
#define UM98X_APPLY_REASON_MAX 64u

typedef struct {
    bool force_mode;   /**< send MODE BASE even when the read-back matches: a lock or a re-survey */
} um98x_apply_opts_t;

/** What one apply session did. */
typedef struct {
    char firmware[48];          /**< receiver firmware of this session (its version log) */
    bool signal_group_sent;     /**< CONFIG SIGNALGROUP was sent: the receiver restarted and saved it by itself */
    bool mode_sent;             /**< MODE BASE was sent and accepted (a survey restarts) */
    int64_t mode_ack_ms;        /**< monotonic ms (esp_timer) of that acceptance: start of a survey window */
    bool baud_changed;          /**< the COM1 baud was changed in this session */
    uint32_t baud;              /**< link baud when the session ended */
    uint32_t found_baud;        /**< receiver found again at this baud by a detection inside the session (0 = none ran) */
    /** where the receiver last answered after that detection or the rollback's
     *  RESET (0 = not known): kept as gnss/baud when the session fails */
    uint32_t answer_baud;
    bool rtcm1230_ok;           /**< RTCM1230 accepted (false if rejected or not in the profile) */
    bool saved;                 /**< SAVECONFIG accepted: the receiver runs the profile */
    bool persisted;             /**< applied hash, firmware, 1230 result (and baud) written in one settings batch */
    bool rolled_back;           /**< RESET sent after a failure (the receiver goes back to its saved configuration) */
    bool rollback_answered;     /**< the receiver answered the version query after that RESET */
    bool receiver_lost;         /**< the receiver could not be found again: nothing more was sent */
    char failed_cmd[UM98X_APPLY_CMD_MAX];     /**< the command that failed ("" on success) */
    char reason[UM98X_APPLY_REASON_MAX];      /**< the receiver's reply text, or "timeout", "link error", ... */
} um98x_apply_result_t;

#ifdef __cplusplus
}
#endif
