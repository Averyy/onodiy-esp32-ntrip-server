/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation summary core (pure logic, no IDF or RTOS includes, injected
 * millisecond clock, host-testable).
 *
 * Fed with decoded MSM headers (gnss_rtcm3_msm_decode). Per constellation it
 * remembers when each signal ID was last seen, so the signal mask and the
 * band count describe the last OBS_SIGNAL_WINDOW_MS: a band that stops
 * arriving drops out of the count after that long, and a single missing
 * epoch does not. Satellites are counted over all MSMs of the
 * constellation's latest epoch (a constellation may be split over several
 * MSMs with the same epoch time).
 *
 * Band self-check: the best constellation is the one with the most bands
 * (ties go to the lower gnss_sys_t value). Galileo E6 that was seen since
 * the last reset and is now missing is reported as lost: E6 depends on the
 * receiver's signal group, so it is only expected once it has been seen.
 *
 * Epochs: an MSM with the multiple-message bit clear closes an epoch; the
 * core counts those and the time since the latest one.
 *
 * The clock is any monotonic millisecond count (the caller uses esp_timer).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gnss_rtcm3.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBS_SIGNAL_WINDOW_MS 10000u   /**< how long a signal counts after it was last seen */

typedef struct {
    uint64_t sig_seen_ms[GNSS_SYS_COUNT][32];  /**< when each signal ID (index id-1) was last seen */
    uint32_t sig_ever[GNSS_SYS_COUNT];         /**< signal IDs seen at least once (bit id-1) */
    uint64_t msm_seen_ms[GNSS_SYS_COUNT];      /**< latest MSM of the constellation */
    uint32_t epoch_time[GNSS_SYS_COUNT];       /**< epoch time of that MSM */
    uint64_t epoch_sats[GNSS_SYS_COUNT];       /**< satellites in that epoch (union over its MSMs) */
    uint8_t msm_ever;                          /**< constellations with at least one MSM (bit per gnss_sys_t) */
    bool e6_seen;                              /**< Galileo E6 seen since the last reset */
    bool epoch_ever;
    uint64_t epochs;                           /**< MSMs that closed an epoch */
    uint64_t epoch_end_ms;                     /**< when the latest of them arrived */
} obs_core_t;

typedef struct {
    bool present;              /**< an MSM of this constellation within the window */
    uint8_t satellites;        /**< in its latest epoch (0 when not present) */
    uint32_t signal_mask;      /**< signal IDs seen within the window (bit id-1) */
    uint8_t band_mask;         /**< bands within the window (bit per gnss_band_t) */
    uint8_t bands;             /**< number of bits in band_mask */
} obs_core_sys_t;

typedef struct {
    obs_core_sys_t sys[GNSS_SYS_COUNT];
    gnss_sys_t best_sys;       /**< most bands (GNSS_SYS_GPS with 0 bands when nothing is present) */
    uint8_t best_bands;
    bool e6_lost;              /**< Galileo E6 seen since the last reset, but not within the window */
    uint64_t epochs;
    uint32_t epoch_age_ms;     /**< since the latest epoch-closing MSM, UINT32_MAX = never */
} obs_core_view_t;

/** Forget everything (also the "E6 seen" memory). */
void obs_core_reset(obs_core_t *c);

/** One decoded MSM header received at now_ms. */
void obs_core_feed_msm(obs_core_t *c, const gnss_rtcm3_msm_t *h, uint64_t now_ms);

/** Summary as of now_ms. */
void obs_core_view(const obs_core_t *c, uint64_t now_ms, obs_core_view_t *out);

#ifdef __cplusplus
}
#endif
