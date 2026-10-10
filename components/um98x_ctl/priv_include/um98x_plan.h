/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Configuration planner (pure logic, no IDF includes, host-testable): from
 * the profile and the receiver's read-back to the ordered list of
 * configuring commands, sent only where something differs.
 *
 * Session order (the executor runs it): CR LF; UNLOG COM1 (quiet port);
 * CONFIG and MODE read-back; then this plan:
 *   1. CONFIG SIGNALGROUP <master> <slave>   only if the read-back differs
 *      (the receiver restarts and saves it by itself; sent first so its
 *      restart loses no unsaved setting and its ack is read on a quiet port)
 *   2. CONFIG COM1 <baud>                    only if the profile's baud is
 *      not the link's current baud
 *   3. MODE BASE ...                         only if the base-mode read-back
 *      differs, or for a lock / re-survey (um98x_base.h)
 *   4. RTCM<msg> COM1 <period>               every profile message, in
 *      profile order (1230 non-fatal: some builds reject it)
 *   5. GPGSV COM1 10                         only with satellite-view output
 *                                            (non-fatal)
 *   6. SAVECONFIG                            the only save of the session
 *
 * Never sent: anything for COM2/COM3 (the user's ports), the vendor RTCM
 * encryption settings, FRESET, comma-separated command forms. Commands are
 * the manual's space-separated upper-case form.
 *
 * Detection candidates: the stored baud, then 115200, 460800, 921600,
 * 230400 (short list); the full list adds 57600, 38400, 19200, 9600;
 * duplicates skipped.
 *
 * Status: order and conditions PROVISIONAL, to be confirmed on hardware.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_types.h"
#include "um98x_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_CMD_UNLOG_COM1 "UNLOG COM1"
#define UM98X_CMD_CONFIG     "CONFIG"
#define UM98X_CMD_MODE       "MODE"
#define UM98X_CMD_VERSION    "VERSIONA"
#define UM98X_CMD_SAVE       "SAVECONFIG"
#define UM98X_CMD_RESET      "RESET"

/** The only receiver model this layer configures (exact match). */
#define UM98X_SUPPORTED_MODEL "UM982"

/** Message whose rejection is not fatal (unsupported on some builds). */
#define UM98X_RTCM_OPTIONAL 1230u

#define UM98X_DETECT_MAX 8u

typedef enum {
    UM98X_STEP_SIGNALGROUP = 0,
    UM98X_STEP_BAUD,
    UM98X_STEP_MODE,
    UM98X_STEP_RTCM,
    UM98X_STEP_SAT_VIEW,
    UM98X_STEP_SAVE,
} um98x_step_kind_t;

typedef struct {
    uint8_t kind;        /**< um98x_step_kind_t */
    bool fatal;          /**< a failure stops the session */
    uint16_t msg;        /**< RTCM: message number */
    uint16_t period_s;   /**< RTCM: period */
} um98x_step_t;

#define UM98X_PLAN_MAX (UM98X_MAX_RTCM_MSGS + 5)

typedef struct {
    uint8_t count;
    um98x_step_t step[UM98X_PLAN_MAX];
} um98x_plan_t;

/** What the session read back before configuring. */
typedef struct {
    bool config_ok;                       /**< the dump was read */
    um98x_config_readback_t config;
    bool mode_ok;                         /**< the base-mode log was read */
    char mode_body[UM98X_MODE_BODY_MAX];
} um98x_readback_t;

/** True for the supported model (exactly "UM982"; UM982C, UM980, ... are not). */
bool um98x_model_supported(const char *model);

/** True if the read-back shows the profile's signal group (unknown = different). */
bool um98x_signal_group_matches(const um98x_base_profile_t *p, const um98x_readback_t *rb);

/**
 * Build the plan for a valid profile.
 * @param link_baud  the baud the receiver answers at now
 * @param force_mode send MODE BASE even if the read-back matches (lock, re-survey)
 * @return false for an invalid profile.
 */
bool um98x_plan_build(const um98x_base_profile_t *p, const um98x_readback_t *rb, uint32_t link_baud,
                      bool force_mode, um98x_plan_t *out);

/** Only the profile's RTCM outputs (restoring output after a failed session; no MODE, baud or save). */
bool um98x_plan_rtcm_only(const um98x_base_profile_t *p, um98x_plan_t *out);

/** Command text of one step. @return length, 0 if it does not fit or the step is invalid. */
size_t um98x_step_command(const um98x_base_profile_t *p, const um98x_step_t *s, char *buf, size_t cap);

/** Detection candidates (see above). @return count written. */
size_t um98x_detect_candidates(uint32_t stored_baud, bool full, uint32_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
