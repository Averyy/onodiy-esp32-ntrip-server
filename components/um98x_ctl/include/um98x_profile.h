/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Base profile and base position: defaults, validation, persistence format
 * (pure logic, no IDF includes, host-testable).
 *
 * Blobs are explicit little-endian with a leading version byte (see
 * settings_codec.h); never raw structs.
 *
 * Fields the base mode does not use (fixed coordinates in SURVEY_IN,
 * survey_in_s in FIXED) are cleared by encode/decode and ignored by the hash,
 * so two profiles that configure the receiver the same way always look the
 * same. NaN or infinity is invalid in every field, used or not.
 *
 * The profile hash (gnss/applied_hash) covers only what reaches the receiver,
 * in its own fixed serialisation without the blob version byte: padding,
 * compiler changes or a new blob layout never make an applied profile look
 * different. Its format is frozen; changing it reconfigures every station
 * once.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UM98X_PROFILE_BLOB_MAX  128u
#define UM98X_POSITION_BLOB_MAX 48u

/** Accepted ellipsoidal height range for fixed and locked positions (metres). */
#define UM98X_HEIGHT_MIN_M (-1000.0)
#define UM98X_HEIGHT_MAX_M 10000.0

/**
 * Onocoy default profile: signal group 3/6 (adds Galileo E6 on the UM982),
 * survey-in 60 s, RTCM 1005/1033/1230 every 10 s and MSM7
 * 1077/1087/1097/1117/1127/1137 every 1 s.
 */
void um98x_profile_default(um98x_base_profile_t *out);

/** Plausibility check (ranges, message count, coordinates for FIXED mode, no NaN anywhere). */
bool um98x_profile_valid(const um98x_base_profile_t *p);

/** Encode (unused fields cleared); returns the blob length, or 0 if cap is too small or p is invalid. */
size_t um98x_profile_encode(const um98x_base_profile_t *p, uint8_t *buf, size_t cap);

/** Decode a blob written by any version of um98x_profile_encode(); false if unusable. */
bool um98x_profile_decode(const uint8_t *buf, size_t len, um98x_base_profile_t *out);

/** Stable hash of what the profile configures ("already applied" check); 0 if invalid. */
uint32_t um98x_profile_hash(const um98x_base_profile_t *p);

/** Plausibility check: known source, lat/lon in range, height in the range above, finite sigma >= 0. */
bool um98x_position_valid(const um98x_base_position_t *pos);

/** Encode a valid position; 0 if invalid or cap is too small (so nothing is saved that decode would reject). */
size_t um98x_position_encode(const um98x_base_position_t *pos, uint8_t *buf, size_t cap);
/** Decode; false for a damaged, foreign or invalid record (same check as encode). */
bool um98x_position_decode(const uint8_t *buf, size_t len, um98x_base_position_t *out);

#ifdef __cplusplus
}
#endif
