/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Configuration planner (pure logic, no IDF includes, host-testable): from
 * the profile and the receiver's read-back to the ordered list of
 * configuring commands, sent only where something differs.
 *
 * COM1 below stands for the receiver port the station is wired to
 * (um98x_link_port(): COM1 on the dev board, COM2 or COM3 on a board wired
 * so); every command that names a port names that one.
 *
 * Session order (the executor runs it): CR LF; UNLOG COM1 (quiet port);
 * CONFIG and MODE read-back; then this plan (with the signal-group query,
 * ANT1GROUPA, between step 1 and step 2, sent in every session):
 *   1. CONFIG SIGNALGROUP <master> <slave>   only if the read-back differs
 *      (the receiver restarts and saves it by itself; sent first so its
 *      restart loses no unsaved setting and its ack is read on a quiet port)
 *   2. CONFIG COM1 <baud>                    only if the profile's baud is
 *      not the link's current baud
 *   3. MODE BASE ...                         only if the base-mode read-back
 *      differs, or for a lock / re-survey (um98x_base.h)
 *   4. CONFIG BASEANTENNAMODEL "<name>" "<serial>" <setup id> USER
 *                                            only with an antenna model in
 *      the profile and a read-back that differs or was not understood
 *      (non-fatal: a rejection is recorded and shown)
 *   5. CONFIG RTCMB1CB2A ENABLE              only if the read-back shows
 *      BeiDou B1C/B2a in RTCM turned off; unknown = not sent (non-fatal).
 *      Not a profile field: B1C/B2a is always wanted
 *   6. MASK <deg>                            always (non-fatal: a rejection
 *      is recorded and shown). The UM982 has no MASK item in its
 *      configuration dump (build 25164 answers it only to its own query),
 *      so there is nothing to compare with; sent again it changes nothing
 *   7. RTCM<msg> COM1 <period>               every profile message, in
 *      profile order (the optional ones non-fatal: um98x_rtcm_optional)
 *   8. GPGSV COM1 10                         only with satellite-view output
 *                                            (non-fatal)
 *   9. SAVECONFIG                            the only save of the session
 *
 * Never sent: anything for the receiver's other ports (the user's), the vendor RTCM
 * encryption settings, multipath mitigation (read back and shown only), a
 * mask of a constellation, frequency or satellite, FRESET, comma-separated
 * command forms. Commands are the manual's space-separated upper-case form.
 *
 * Satellite-in-view output outside the profile (the live view, never
 * saved): "GPGSV COM1 <period>" on, "UNLOG COM1 GPGSV" off
 * (um98x_sat_view_command).
 *
 * Detection candidates: the stored baud, then 115200, 460800, 921600,
 * 230400 (short list); the full list adds 57600, 38400, 19200, 9600;
 * duplicates skipped.
 *
 * Status: order and conditions PROVISIONAL, to be confirmed on hardware
 * (also whether the B1C/B2a command restarts the receiver, and the antenna
 * command's form).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_types.h"
#include "um98x_apply.h"
#include "um98x_reply.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_CMD_CONFIG     "CONFIG"
#define UM98X_CMD_MODE       "MODE"
#define UM98X_CMD_VERSION    "VERSIONA"
#define UM98X_CMD_SAVE       "SAVECONFIG"
#define UM98X_CMD_RESET      "RESET"
/** Signal-group query, main antenna input: one-shot, never with a rate (um98x_bands.h). */
#define UM98X_CMD_GROUPS     "ANT1GROUPA"

/**
 * The receiver port the link is wired to, 1 (COM1) to 3 (COM3): set once at
 * start, before the first session (um98x_ctl_set_port); COM1 until then. A
 * value outside 1-3 is ignored.
 */
void um98x_link_port_set(uint8_t port);
uint8_t um98x_link_port(void);

/** "COM1" ... "COM3": the link's port as commands and the configuration dump name it. */
const char *um98x_link_port_name(void);

/** "UNLOG COM<n>": every output of the link's port off (the quiet port). */
const char *um98x_cmd_unlog(void);

/**
 * What the "applied" record stores (settings gnss/applied_hash) for `p` on
 * the link's port: the profile's hash (um98x_profile_hash) on COM1, as it was
 * stored before the port was a choice; on COM2 or COM3 that hash mixed with
 * the port, so a receiver wired to another port since its last configuration
 * (its outputs are on the old one) is configured again. 0 if `p` is invalid.
 */
uint32_t um98x_applied_hash(const um98x_base_profile_t *p);

/** The only receiver model this layer configures (exact match). */
#define UM98X_SUPPORTED_MODEL "UM982"

/** GLONASS code-phase biases: optional, its answer stored (gnss/rtcm1230_ok). */
#define UM98X_RTCM_GLO_BIASES 1230u

/**
 * RTCM messages whose refusal is not fatal: a receiver firmware without them
 * refuses them, and the rest of the profile is applied all the same (the
 * refusal is listed in um98x_apply_result_t.rtcm_refused). 1230 (GLONASS
 * biases, refused by some builds) and the NavIC MSMs 1131-1137 (in Onocoy's
 * list; refused by a firmware without NavIC). Every other message is fatal.
 */
bool um98x_rtcm_optional(uint16_t msg);

#define UM98X_DETECT_MAX 8u

typedef enum {
    UM98X_STEP_SIGNALGROUP = 0,
    UM98X_STEP_BAUD,
    UM98X_STEP_MODE,
    UM98X_STEP_RTCM,
    UM98X_STEP_SAT_VIEW,
    UM98X_STEP_SAVE,
    UM98X_STEP_ANTENNA,     /**< base antenna descriptor (non-fatal) */
    UM98X_STEP_B1CB2A,      /**< BeiDou B1C/B2a in RTCM on (non-fatal) */
    UM98X_STEP_MASK,        /**< elevation mask (non-fatal) */
} um98x_step_kind_t;

typedef struct {
    uint8_t kind;        /**< um98x_step_kind_t */
    bool fatal;          /**< a failure stops the session */
    uint16_t msg;        /**< RTCM: message number */
    uint16_t period_s;   /**< RTCM: period */
} um98x_step_t;

#define UM98X_PLAN_MAX (UM98X_MAX_RTCM_MSGS + 8)

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
 * True if the read-back shows the profile's antenna: descriptor and serial
 * number the same by um98x_antenna_str_same() (runs of spaces read as one,
 * as the receiver stores them), same setup ID. Unknown (or no dump) =
 * different. A profile without a model never needs the step: true.
 */
bool um98x_antenna_matches(const um98x_base_profile_t *p, const um98x_readback_t *rb);

/** Satellite-in-view output period for the live view (s): 1..60. */
#define UM98X_SAT_VIEW_PERIOD_MAX 60u

/**
 * Command that turns the satellite-in-view output on ("GPGSV COM1
 * <period_s>", period 1..UM98X_SAT_VIEW_PERIOD_MAX) or off ("UNLOG COM1
 * GPGSV"). Never saved. PROVISIONAL, to be confirmed on hardware: the
 * off form, and whether either interrupts the RTCM output.
 * @return length, 0 for a period out of range or a buffer too small.
 */
size_t um98x_sat_view_command(bool on, uint16_t period_s, char *buf, size_t cap);

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

/**
 * What an apply session would send, without a read-back (nothing is
 * sent): the session's quiet-port command, its queries, then every step
 * the plan can hold, each marked with when it goes out. In session order:
 *   UNLOG COM1                     write, always (not saved by itself)
 *   VERSIONA                       query, always (the session's identity)
 *   CONFIG, MODE                   queries, always (the read-back)
 *   CONFIG SIGNALGROUP m s         write, if the read-back differs (the
 *                                  receiver saves it and restarts)
 *   ANT1GROUPA                     query, always (what the running group
 *                                  configures on the main antenna input)
 *   CONFIG COM1 <baud>             write, if the profile's baud is not
 *                                  link_baud
 *   MODE BASE ...                  write, if the base-mode read-back
 *                                  differs (a fixed position as ECEF)
 *   CONFIG BASEANTENNAMODEL ...    write, only with an antenna model, if
 *                                  the read-back differs
 *   CONFIG RTCMB1CB2A ENABLE       write, if the read-back shows it off
 *   MASK <deg>                     write, always
 *   RTCM<msg> COM1 <period>        write, always, one per profile message
 *   GPGSV COM1 10                  write, always, only with satellite-view
 *                                  output in the profile
 *   SAVECONFIG                     write, always, saved: the receiver's whole
 *                                  running configuration
 * The texts come from the plan builder's own formatter (um98x_step_command).
 * @return steps written (at most cap; 0 for an invalid profile).
 */
size_t um98x_plan_preview(const um98x_base_profile_t *p, uint32_t link_baud, um98x_preview_step_t *out, size_t cap);

/** Detection candidates (see above). @return count written. */
size_t um98x_detect_candidates(uint32_t stored_baud, bool full, uint32_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
