/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Base mode and base position (pure logic, no IDF includes, host-testable):
 * the base-mode command, the comparison with the receiver's base-mode
 * read-back, survey tracking, and the lock / re-survey / verify rules.
 *
 * Base-mode command (Unicore N4 syntax, spaces, upper case):
 *   survey-in  "MODE BASE <id> TIME <survey_in_s> 0"   (distance 0: always
 *              use the new average)
 *   fixed      "MODE BASE <id> <X> <Y> <Z>"             ECEF metres with 4
 *              decimals, converted from the profile's WGS84 latitude,
 *              longitude and ellipsoidal height (no height-reference
 *              ambiguity, unlike latitude/longitude/height input).
 *
 * Re-entering a base mode restarts it on the receiver (a survey starts over,
 * and a redundant fixed position has been seen to stall RTCM output for
 * seconds), so the command is sent only when the read-back differs (or for a
 * lock or a re-survey). The read-back is the body of the receiver's
 * base-mode log; it is "the same" when it starts with MODE BASE and:
 *  - survey-in: has a TIME token followed by the profile's seconds (a
 *    distance after it, if present, must be 0);
 *  - fixed: carries three numbers, each within 0.5 mm of the profile's ECEF
 *    coordinates (latitude/longitude/height numbers never match);
 *  - in both, a station ID echoed right after BASE must equal the profile's.
 * Anything else, an empty or unparsable body included, is "different".
 * PROVISIONAL, to be confirmed on hardware: the exact read-back text.
 *
 * Survey window: from the base-mode acknowledgement (or a receiver restart
 * seen while the profile surveys) until the survey is complete or
 * survey_in_s + 120 s have passed. Complete = the base-mode read-back no
 * longer reports a TIME survey, or (fallback) at least survey_in_s seconds
 * passed and the 1005 reference point stayed within 1 mm per axis over 60
 * consecutive frames. PROVISIONAL, to be confirmed on hardware: the
 * receiver's text after a finished survey.
 *
 * Lock: the latest 1005 reference point (ECEF, 0.1 mm units) becomes the
 * profile's fixed position (stored as WGS84), source "survey". After the
 * apply, the next 1005 must be within 5 mm per axis of it, else the station
 * raises its position-mismatch alert.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_MODE_CMD_MAX            80u     /**< base-mode command buffer */
#define UM98X_MODE_MATCH_TOL_M        0.0005  /**< fixed read-back tolerance per axis */
#define UM98X_LOCK_VERIFY_TOL_M       0.005   /**< lock check: next 1005 vs the stored position, per axis */
#define UM98X_SURVEY_GRACE_S          120u    /**< survey window beyond survey_in_s */
#define UM98X_SURVEY_STABLE_FRAMES    60u     /**< fallback: consecutive 1005 frames without movement */
#define UM98X_SURVEY_STABLE_TOL_01MM  10      /**< ... within 1 mm per axis (0.1 mm units) */
#define UM98X_SURVEY_MODE_POLL_S      60u     /**< base-mode query period while surveying */

/** Base-mode command for the profile (see above). @return length, 0 if the profile is invalid or cap too small. */
size_t um98x_mode_command(const um98x_base_profile_t *p, char *buf, size_t cap);

/** True if the base-mode read-back already configures what the profile asks for (see above). */
bool um98x_mode_matches(const um98x_base_profile_t *p, const char *body);

/** True if the read-back reports a running survey-in ("MODE BASE ... TIME ..."). */
bool um98x_mode_is_survey(const char *body);

/** True if the read-back reports a fixed base (MODE BASE with coordinates, no TIME). */
bool um98x_mode_is_fixed_base(const char *body);

/** Survey-in progress, fed with the base-mode read-back and the 1005 reference points. */
typedef struct {
    bool active;              /**< a survey window was started */
    bool complete;            /**< the read-back no longer reports a TIME survey */
    int64_t start_ms;         /**< window start (monotonic ms) */
    uint32_t survey_s;
    bool have_ref;
    int64_t ref_01mm[3];      /**< first point of the current stable run */
    uint32_t stable_frames;   /**< consecutive 1005 frames within the tolerance of ref_01mm */
} um98x_survey_t;

/** Start (or restart) the window at now_ms for a survey of survey_s seconds. */
void um98x_survey_start(um98x_survey_t *s, int64_t now_ms, uint32_t survey_s);

/** Base-mode read-back while surveying: a base-mode body without TIME means the survey ended. */
void um98x_survey_on_mode(um98x_survey_t *s, const char *body);

/** A CRC-valid 1005 reference point (ECEF, 0.1 mm units). */
void um98x_survey_on_arp(um98x_survey_t *s, const int64_t ecef_01mm[3]);

/** True once complete by the read-back, or by the fallback rule at now_ms. */
bool um98x_survey_done(const um98x_survey_t *s, int64_t now_ms);

/** True inside the survey window (active, not done, before survey_s + 120 s). */
bool um98x_survey_window_open(const um98x_survey_t *s, int64_t now_ms);

/** Seconds left in the window (0 when closed). */
uint32_t um98x_survey_window_left_s(const um98x_survey_t *s, int64_t now_ms);

/**
 * Lock: the profile with the reference point as its fixed position, and the
 * position record to store (source survey, locked at now_unix).
 * @return false if the point is not a valid position or the resulting
 *         profile is invalid (next and pos untouched then).
 */
bool um98x_lock_profile(const um98x_base_profile_t *cur, const int64_t ecef_01mm[3], int64_t now_unix,
                        um98x_base_profile_t *next, um98x_base_position_t *pos);

/** Re-survey: the profile back to survey-in (its survey time, or the default 600 s). */
void um98x_resurvey_profile(const um98x_base_profile_t *cur, um98x_base_profile_t *next);

/**
 * True if the reference point lies within tol_m per axis of the fixed
 * profile's position (ECEF). False for a survey-in profile.
 */
bool um98x_arp_near_profile(const um98x_base_profile_t *p, const int64_t ecef_01mm[3], double tol_m);

/** Reference point (ECEF, 0.1 mm) to a WGS84 position record (source survey, not locked). */
bool um98x_arp_to_position(const int64_t ecef_01mm[3], um98x_base_position_t *out);

#ifdef __cplusplus
}
#endif
