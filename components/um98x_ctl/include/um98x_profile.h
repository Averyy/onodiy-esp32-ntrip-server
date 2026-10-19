/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Base profile and base position: defaults, validation, persistence format,
 * WGS84 <-> ECEF conversion (pure logic, no IDF includes, host-testable).
 *
 * Blobs are explicit little-endian with a leading version byte (see
 * settings_codec.h); never raw structs.
 *
 * Fields the base mode does not use (fixed coordinates in SURVEY_IN,
 * survey_in_s in FIXED) are cleared by encode/decode and ignored by the hash,
 * so two profiles that configure the receiver the same way always look the
 * same; likewise the antenna serial and setup ID without an antenna model,
 * and trailing spaces of the antenna strings. NaN or infinity is invalid in
 * every field, used or not.
 *
 * The antenna fields are appended to the blob (a blob without them decodes
 * with an empty antenna) and to the hash serialisation only when a model is
 * set, so a profile without one keeps the hash it had before they existed.
 *
 * After them comes the defaults the blob was written under (3 since the sky
 * map's output is on by default; 2 since 1137 joined the default message
 * set; a blob without it was written under 1). What a later default changed
 * is read into a blob written before it, where it still holds the old
 * default (a value nobody chose), so the station and the UI show and apply
 * today's:
 *  - written under 1 with exactly the old default set (1005, 1033, 1077,
 *    1087, 1097, 1117, 1127 and 1230, each once, in any order, every one
 *    every 1 s): today's default list, Onocoy's, instead of a "custom" one;
 *  - written under 1 or 2 with sat_view_output off (its default then):
 *    on, today's default.
 * Any other value, and any blob written since (the old set or the output
 * off chosen again on purpose, or a defaults byte this firmware does not
 * know), decodes as stored. Nothing is written for it: the next save stores
 * the values with today's byte, and an image rolled back to still reads the
 * old ones (its own defaults byte check ignores a newer one). The hash then
 * differs from the applied one, so with automatic setup the receiver is
 * configured with it once, as after a save.
 *
 * The profile hash (gnss/applied_hash) covers only what reaches the receiver,
 * in its own fixed serialisation without the blob version byte: padding,
 * compiler changes or a new blob layout never make an applied profile look
 * different. Its format is frozen once released; changing it reconfigures
 * every station once.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_PROFILE_BLOB_MAX  192u
#define UM98X_POSITION_BLOB_MAX 48u

/** Accepted ellipsoidal height range for fixed and locked positions (metres). */
#define UM98X_HEIGHT_MIN_M (-1000.0)
#define UM98X_HEIGHT_MAX_M 10000.0

#define UM98X_STATION_ID_MAX     4095u    /**< reference station ID is 12 bits */
#define UM98X_SURVEY_MAX_S       3600u    /**< longest survey-in the receiver accepts */
#define UM98X_DEFAULT_SURVEY_S   600u
#define UM98X_DEFAULT_COM1_BAUD  115200u

/**
 * Onocoy default profile: signal group 3/6 (adds Galileo E6 on the UM982),
 * survey-in 600 s, COM1 at 115200, RTCM 1005, 1033, MSM7
 * 1077/1087/1097/1117/1127/1137 and 1230, all every 1 s: the list Onocoy's
 * station connection guide asks for (1005 at 1 s too, which it allows).
 * 1137 (NavIC) and 1230 may be refused by a receiver firmware without them;
 * the apply tolerates that (um98x_rtcm_optional). NavIC and QZSS are
 * regional: outside their area no MSM of theirs arrives, which the station
 * never holds against the profile. Satellite-in-view sentences are on (every
 * 10 s, saved in the receiver): the UI's sky map places the satellites with
 * them; the signal view comes from MSM data either way.
 *
 * The 600 s survey assumes the receiver keeps sending MSM observations
 * while it surveys; PROVISIONAL, to be confirmed on hardware (if it holds
 * them back, the default goes back to 60 s). 460800 becomes the COM1 default
 * only once confirmed on hardware.
 *
 * @param station_id reference station ID, normally um98x_station_id_from_mac()
 *                   of the base MAC (stable per device).
 */
void um98x_profile_default(um98x_base_profile_t *out, uint16_t station_id);

/** Station ID from a MAC address: its low 12 bits. */
uint16_t um98x_station_id_from_mac(const uint8_t mac[6]);

/** True for the COM1 baud rates the receiver supports (9600 ... 921600). */
bool um98x_baud_supported(uint32_t baud);

/**
 * Plausibility check (ranges, message count, coordinates for FIXED mode,
 * station ID <= 4095, a supported COM1 baud, survey 1..3600 s, no NaN
 * anywhere, antenna strings per um98x_antenna_str_valid()).
 */
bool um98x_profile_valid(const um98x_base_profile_t *p);

/**
 * An antenna descriptor or serial number the profile can hold: terminated
 * within UM98X_ANTENNA_STR_MAX characters, printable ASCII (0x20..0x7E)
 * without '"' (the receiver command quotes it). Empty is valid.
 */
bool um98x_antenna_str_valid(const char *s, size_t cap);

/** Length of s without its trailing spaces (the stored form). */
size_t um98x_antenna_str_len(const char *s);

/**
 * Two antenna descriptors (or serial numbers) name the same thing: equal
 * once leading and trailing spaces are dropped and every run of spaces
 * inside reads as one space. UM982 build 25164 stores the descriptor with
 * its runs of spaces collapsed ("TRM59800.00     SCIS", the IGS name padded
 * to 16 characters before the radome, reads back as "TRM59800.00 SCIS"), so
 * an exact comparison would never match and every apply would send the
 * antenna again. NULL reads as "".
 */
bool um98x_antenna_str_same(const char *a, const char *b);

/** Encode (unused fields cleared); returns the blob length, or 0 if cap is too small or p is invalid. */
size_t um98x_profile_encode(const um98x_base_profile_t *p, uint8_t *buf, size_t cap);

/**
 * Decode a blob written by any version of um98x_profile_encode(), an old
 * default read as today's (see above); false if unusable.
 */
bool um98x_profile_decode(const uint8_t *buf, size_t len, um98x_base_profile_t *out);

/** What um98x_profile_decode_ex() read as today's default (bits of *upgraded). */
#define UM98X_PROFILE_UPGRADED_RTCM 0x01u   /**< the old default message set, read as Onocoy's list */
#define UM98X_PROFILE_UPGRADED_SKY  0x02u   /**< satellite-in-view output off from before it was the default, read as on */

/** um98x_profile_decode(); *upgraded (may be NULL) says which old defaults were read as today's (0 = none). */
bool um98x_profile_decode_ex(const uint8_t *buf, size_t len, um98x_base_profile_t *out, unsigned *upgraded);

/** Stable hash of what the profile configures ("already applied" check); 0 if invalid. */
uint32_t um98x_profile_hash(const um98x_base_profile_t *p);

/** Plausibility check: known source, lat/lon in range, height in the range above, finite sigma >= 0. */
bool um98x_position_valid(const um98x_base_position_t *pos);

/** Encode a valid position; 0 if invalid or cap is too small (so nothing is saved that decode would reject). */
size_t um98x_position_encode(const um98x_base_position_t *pos, uint8_t *buf, size_t cap);
/** Decode; false for a damaged, foreign or invalid record (same check as encode). */
bool um98x_position_decode(const uint8_t *buf, size_t len, um98x_base_position_t *out);

/**
 * WGS84 latitude/longitude (degrees) and ellipsoidal height (metres) to
 * ECEF metres. Fixed positions go to the receiver in this form.
 */
void um98x_llh_to_ecef(double lat_deg, double lon_deg, double height_m, double xyz[3]);

/**
 * ECEF metres (e.g. the antenna reference point of a 1005 message) to WGS84
 * latitude/longitude/ellipsoidal height. Round trip with um98x_llh_to_ecef()
 * within 0.1 mm.
 * @return false for non-finite input or a point within 100 km of the Earth's
 *         centre.
 */
bool um98x_ecef_to_llh(const double xyz[3], double *lat_deg, double *lon_deg, double *height_m);

#ifdef __cplusplus
}
#endif
