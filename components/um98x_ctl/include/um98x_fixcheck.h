/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Fixed-position check (pure logic, no IDF includes, host-testable): is the
 * profile's fixed base position where the receiver is?
 *
 * A fixed base sends the position it was given in every 1005, wherever the
 * antenna is, so a wrong value (a typo, swapped digits, a position from
 * another site) makes every correction the station sends wrong. Nothing in
 * the RTCM stream shows it: the 1005 is that value. The receiver keeps
 * computing its own single-point solution as a fixed base, though, and two
 * one-shot logs report it (no rate: output once; Unicore N4 command set):
 *
 *   BASEPOSA  the base's real-time position while the receiver works as a
 *             fixed base (the manual's purpose: see whether the base moved)
 *   SPPNAVA   the pseudorange (single-point) solution, any mode
 *
 * The station asks BASEPOSA and falls back to SPPNAVA when the receiver
 * refuses it, only echoes the set position (FIXEDPOS) or gives nothing
 * usable three times. Both bodies start alike: solution status, position type,
 * latitude, longitude, height above mean sea level, geoid separation,
 * datum, the latitude, longitude and height standard deviations, base ID
 * (quoted), differential age, solution age, satellites tracked, satellites
 * used, ... (um98x_own_fix_parse reads those 15 fields).
 *
 * A solution counts (um98x_own_fix_usable) only when computed, of a
 * single-point type (single, pseudorange differential or SBAS; a FIXEDPOS
 * answer only echoes a set position), from at least 6 satellites, with the
 * horizontal standard deviation (latitude and longitude together) at most
 * 5 m and the height's at most 10 m, finite, and with a plausible height
 * and geoid separation (|separation| <= 150 m).
 *
 * Limits: 50 m horizontally and 50 m vertically. The UM982's single-point
 * accuracy is 1.5 m horizontal and 2.5 m vertical (RMS, its datasheet), so
 * a receiver that is where the fixed position says stays far inside them:
 * 50 m is over 30 times its horizontal and 20 times its vertical RMS, and
 * still 10 and 5 times the largest deviations a counted solution may
 * report. A slip in the third decimal of a degree (about 110 m), swapped
 * digits or another site's coordinates are well beyond. Tighter limits
 * would start to read the multipath of a poor sky (single-point errors of
 * 10 to 20 m) as a wrong position. Beyond about 300 m the receiver stops
 * its RTCM output altogether (the antenna-moved alert); the check then
 * says how far off the position is.
 *
 * Height: the logs give the height above mean sea level and the geoid
 * separation (positive where the geoid lies above the ellipsoid); the
 * ellipsoidal height, which the fixed position holds, is their sum. The
 * manual's example lines print the separation with both signs for the same
 * place, so the sum was checked on real data: a UM981's single-point and
 * ECEF solutions of one epoch (test/fixtures/unicore/pyunigps_um981.log)
 * agree to the millimetre with the sum, and are about 100 m apart with the
 * difference. A fixed height entered as height above sea level is off by
 * the separation. PROVISIONAL, to be confirmed on hardware: that BASEPOSA
 * reports the receiver's own solution while it works as a fixed base.
 *
 * Height above sea level typed in (um98x_fix_sea_level): a common mistake,
 * since maps and survey marks give that height. It puts the fixed
 * height off by the separation, which is under 50 m in most places, so the
 * limit alone misses it. The receiver prints its own height above sea
 * level, so the mistake shows directly: the fixed height is about that
 * height (within 5 m) while it is more than 10 m (and four of the
 * solutions' own height deviations) from the receiver's ellipsoidal
 * height, and the horizontal distance is within the limit (the height
 * alone is wrong). Reading the sea-level height as printed, not the
 * sum, leaves the separation's sign out of it.
 *   5 m: twice the receiver's 2.5 m vertical RMS. The decision takes the
 *   medians of three solutions 20 s apart: one stray solution cannot move
 *   them, but solutions that close share most of their error (the
 *   atmosphere and multipath change slowly), so the median scatters about
 *   as much as one solution, and 2 RMS covers about 95 % of them. The
 *   difference between the receiver's geoid model and the one behind the
 *   typed height comes on top; a wider band would take more of the other
 *   wrong heights for this one.
 *   10 m, or four times the height deviation the solutions report (their
 *   median) where that is more. 10 m is four times the vertical RMS and
 *   the largest height deviation a counted solution may report. A right
 *   height reads that far off only with an unsure solution (a poor sky's
 *   10 to 20 m); where the separation is about that error, the right
 *   height would read as a sea-level height and the advice would break
 *   it. With the solutions' own deviation it takes an error beyond four of
 *   them: a solution that reports 8 m needs the fixed height more than
 *   32 m off. Where the separation is about 10 m or less, a typed
 *   sea-level height is too close to the right one to tell them apart:
 *   nothing is said there (the 50 m limit still holds).
 * Once raised, it holds while the medians stay within 10 m of the
 * receiver's sea-level height and more than 5 m from its ellipsoidal
 * height (each 2 RMS looser), so an hourly check does not flip it on the
 * receiver's scatter; it clears with a run outside that, or at once when
 * the fixed position changes.
 *
 * Verdict (um98x_fixchk_feed): three usable samples in a row that agree.
 * Far: the horizontal or the vertical distance beyond its limit in all
 * three. Near: both within in all three; once a warning was decided (far
 * or sea level), both within 40 m (no flapping around the limit). Samples
 * that disagree decide nothing. The distances reported are the medians of
 * the three, per component. A run that decided far or near is a sea-level
 * height instead when its medians say so (above).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_FIX_LIMIT_M      50.0    /**< horizontal and vertical limit */
#define UM98X_FIX_CLEAR_M      40.0    /**< a far verdict clears below this (both) */
#define UM98X_FIX_SAMPLES      3u      /**< samples in a row that decide */
#define UM98X_FIX_MAX_SD_H_M   5.0     /**< horizontal standard deviation cap (latitude and longitude together) */
#define UM98X_FIX_MAX_SD_V_M   10.0    /**< height standard deviation cap */
#define UM98X_FIX_MIN_SATS     6u      /**< satellites used, at least */
#define UM98X_FIX_MAX_SEP_M    150.0   /**< plausible geoid separation (the geoid stays within about 110 m of the ellipsoid) */
#define UM98X_FIX_SEA_BAND_M   5.0     /**< a sea-level height: the fixed height within this of the receiver's sea-level height */
#define UM98X_FIX_SEA_FLOOR_M  10.0    /**< ... and more than this from its ellipsoidal height, */
#define UM98X_FIX_SEA_SD_K     4.0     /**< ... and more than this many of the height deviations the solutions report */
#define UM98X_FIX_SEA_HOLD_BAND_M  10.0   /**< once raised: it holds within this of the sea-level height */
#define UM98X_FIX_SEA_HOLD_FLOOR_M 5.0    /**< ... and beyond this from the ellipsoidal height */

/** The receiver's position logs. */
#define UM98X_CMD_BASEPOS "BASEPOSA"
#define UM98X_CMD_SPPNAV  "SPPNAVA"

/** Position type of a solution, as far as the check cares. */
typedef enum {
    UM98X_OWN_POS_NONE = 0,    /**< no solution */
    UM98X_OWN_POS_FIXED,       /**< FIXEDPOS: a set position, not computed */
    UM98X_OWN_POS_SINGLE,      /**< single point */
    UM98X_OWN_POS_PSRDIFF,     /**< pseudorange differential */
    UM98X_OWN_POS_SBAS,        /**< SBAS-aided single point */
    UM98X_OWN_POS_OTHER,       /**< anything else (RTK, PPP, INS, unknown words) */
} um98x_own_pos_type_t;

/** One reported solution. */
typedef struct {
    bool computed;             /**< solution status SOL_COMPUTED */
    uint8_t pos_type;          /**< um98x_own_pos_type_t */
    double lat_deg;
    double lon_deg;
    double msl_height_m;       /**< height above mean sea level */
    double separation_m;       /**< geoid separation as printed (its sign is not trusted, see above) */
    double sd_lat_m;
    double sd_lon_m;
    double sd_hgt_m;
    uint8_t sats_tracked;
    uint8_t sats_used;
} um98x_own_fix_t;

/**
 * Parse a log body (the text between the header's ';' and the checksum's
 * '*'). False when one of the first 15 fields is missing or not a number
 * where a number belongs; the status and type words never fail (unknown
 * words read as not computed / OTHER).
 */
bool um98x_own_fix_parse(const char *body, size_t len, um98x_own_fix_t *out);

/** The solution counts for the check (see above). */
bool um98x_own_fix_usable(const um98x_own_fix_t *f);

/** Where the fixed position is, seen from the receiver's own solution. */
typedef struct {
    double horizontal_m;       /**< >= 0 */
    double up_m;               /**< fixed height minus the receiver's: > 0 the fixed position is above it */
    double distance_m;         /**< 3-D */
    double up_msl_m;           /**< fixed height minus the receiver's height above sea level (as printed) */
    double sd_hgt_m;           /**< the solution's own height standard deviation (a run's median in shown) */
} um98x_fix_offset_t;

/**
 * The fixed position (WGS84 latitude, longitude, ellipsoidal height) against
 * a usable solution. False for an unusable solution or a fixed position
 * that is not finite or out of range.
 */
bool um98x_fix_offset(double fixed_lat_deg, double fixed_lon_deg, double fixed_height_m, const um98x_own_fix_t *f,
                      um98x_fix_offset_t *out);

/** The horizontal or the vertical distance is beyond limit_m. */
bool um98x_fix_beyond(const um98x_fix_offset_t *o, double limit_m);

/** Off by its height alone: the horizontal distance within UM98X_FIX_LIMIT_M (the warning names the height). */
bool um98x_fix_height_only(const um98x_fix_offset_t *o);

/**
 * The fixed height is the receiver's height above sea level, not its
 * ellipsoidal height (see above): within the band of that height, and
 * beyond the floor and four of the solution's height deviations from its
 * ellipsoidal height. held: a sea-level verdict stands (the looser hold
 * band and floor).
 */
bool um98x_fix_sea_level(const um98x_fix_offset_t *o, bool held);

typedef enum {
    UM98X_FIX_NONE = 0,        /**< no verdict */
    UM98X_FIX_NEAR,            /**< within the limits */
    UM98X_FIX_FAR,             /**< beyond them */
    UM98X_FIX_SEA_LEVEL,       /**< the fixed height is a height above sea level (um98x_fix_sea_level) */
} um98x_fix_verdict_t;

typedef struct {
    uint8_t verdict;           /**< um98x_fix_verdict_t: the latest decision */
    uint8_t n;                 /**< samples in the current run */
    um98x_fix_offset_t run[UM98X_FIX_SAMPLES];
    um98x_fix_offset_t shown;  /**< medians of the run that decided (valid with a verdict) */
} um98x_fixchk_t;

/** No verdict, no samples. */
void um98x_fixchk_init(um98x_fixchk_t *c);

/** A new run of samples (a new check): the verdict and its distances stay until the run decides. */
void um98x_fixchk_restart(um98x_fixchk_t *c);

/**
 * One usable sample. Kept in a run of the latest UM98X_FIX_SAMPLES; once the
 * run is full and agrees, the verdict is decided (and shown updated).
 * @return the verdict this sample decided (it may equal the previous one),
 *         UM98X_FIX_NONE when the run decided nothing.
 */
uint8_t um98x_fixchk_feed(um98x_fixchk_t *c, const um98x_fix_offset_t *o);

#ifdef __cplusplus
}
#endif
