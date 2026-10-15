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

#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_APPLY_CMD_MAX    128u
#define UM98X_APPLY_REASON_MAX 64u

typedef struct {
    bool force_mode;   /**< send MODE BASE even when the read-back matches: a lock or a re-survey */
    /**
     * The caller has a signal group to go back to if the receiver refuses
     * the profile's (its last successful apply's group, not the profile's):
     * a plain refusal is then not rolled back with RESET, as when the
     * session read the running group back (um98x_exec.h).
     */
    bool sg_fallback;
} um98x_apply_opts_t;

/** A signal-group pair as a session read it back. */
typedef struct {
    bool known;        /**< the configuration dump named it */
    uint8_t master;
    uint8_t slave;
} um98x_sg_pair_t;

/** What one apply session did. */
typedef struct {
    char firmware[48];          /**< receiver firmware of this session (its version log) */
    bool signal_group_sent;     /**< CONFIG SIGNALGROUP was sent: the receiver restarted and saved it by itself */
    /**
     * The receiver refused the profile's signal group: a negative reply to
     * CONFIG SIGNALGROUP, or a read-back after its restart that names
     * another group (failed_cmd and reason say which). A read-back that names
     * no group, twice, is no refusal (an ordinary failure). The session
     * failed and was rolled back like any other failure.
     */
    bool signal_group_rejected;
    /**
     * That refusal was a negative reply to CONFIG SIGNALGROUP: the receiver
     * changed nothing and did not restart. Not rolled back with RESET when
     * there is a group to go back to (sg_before known, or opts.sg_fallback):
     * the caller applies that group again at once.
     */
    bool signal_group_refused_plain;
    um98x_sg_pair_t sg_before;  /**< the group this session read back before changing it */
    um98x_sg_pair_t sg_read;    /**< the group the session's latest read-back showed (after a change, the new one) */
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

    /** The configuration dump was read in this session (settings is valid). */
    bool settings_read;
    /**
     * What the receiver runs as far as this session knows: the latest
     * read-back, with the antenna descriptor and B1C/B2a as sent when the
     * session saved them (a failed session's RESET brings back the read
     * values).
     */
    um98x_rx_settings_t settings;
    bool antenna_sent;          /**< the antenna descriptor was sent (the read-back differed or was unknown) */
    bool antenna_ok;            /**< ... and accepted */
    char antenna_reply[UM98X_APPLY_REASON_MAX];   /**< its rejection text, or "timeout" ("" when accepted) */
    bool b1cb2a_sent;           /**< BeiDou B1C/B2a in RTCM was turned on (it read back off) */
    bool b1cb2a_ok;             /**< ... and accepted */
    char b1cb2a_reply[UM98X_APPLY_REASON_MAX];    /**< its rejection text, or "timeout" ("" when accepted) */

    /**
     * The signal-group query of this session (ANT1GROUPA, after the
     * signal-group step): what the running group configures on the main
     * antenna input. bands.state UNKNOWN when the session did not get that
     * far (or no answer); REFUSED with the receiver's reply in bands_reply.
     */
    bool bands_asked;
    um98x_bands_t bands;
    char bands_reply[UM98X_APPLY_REASON_MAX];     /**< refusal text, "timeout" or "link error" ("" when read) */
} um98x_apply_result_t;

/* ---- apply preview: what a session would send, for the user to confirm ------------- */

#define UM98X_PREVIEW_TEXT_MAX UM98X_APPLY_CMD_MAX
/** Most steps of a preview: the fixed head and tail plus one per RTCM message. */
#define UM98X_PREVIEW_MAX      (UM98X_MAX_RTCM_MSGS + 13u)

/** um98x_preview_step_t.kind */
typedef enum {
    UM98X_PREVIEW_QUERY = 0,   /**< reads the receiver, changes nothing */
    UM98X_PREVIEW_WRITE,       /**< changes the receiver's running configuration */
} um98x_preview_kind_t;

/** um98x_preview_step_t.when */
typedef enum {
    UM98X_PREVIEW_ALWAYS = 0,  /**< sent in every session */
    UM98X_PREVIEW_IF_DIFFERS,  /**< only when the read-back differs from the profile */
    UM98X_PREVIEW_IF_OFF,      /**< only when the read-back shows the setting off */
} um98x_preview_when_t;

/** One command of the preview, as the session writes it. */
typedef struct {
    char text[UM98X_PREVIEW_TEXT_MAX];
    uint8_t kind;              /**< um98x_preview_kind_t */
    uint8_t when;              /**< um98x_preview_when_t */
    bool saved;                /**< the command itself saves to the receiver's stored configuration */
} um98x_preview_step_t;

#ifdef __cplusplus
}
#endif
