/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation summary: what the receiver is actually sending.
 *
 * Fed from the RTCM path (every published frame) and from satellite-in-view
 * text lines. It decodes only public RTCM 10403 structures: the MSM header
 * (epoch time, multiple-message bit, satellite/signal/cell masks) and, for
 * MSM7, the per-cell carrier-to-noise values. From that it maintains:
 *
 *   - per constellation: satellites tracked, signals (bands) present, mean
 *     CNR per band -> the web UI's signal view without extra receiver
 *     output (satellite-in-view sentences are only needed for azimuth and
 *     elevation, at a low rate);
 *   - the observed message set and per-message rates -> compared with the
 *     stored profile; a mismatch means the receiver was reconfigured or
 *     reset behind our back, so the station re-applies the profile;
 *   - "bands on the best constellation": Onocoy rewards 4 bands on one
 *     constellation (Galileo E1/E5a/E5b/E6 via signal group 3/6). Fewer than
 *     4 is reported as DEGRADED with an alert even while everything else
 *     looks fine;
 *   - epoch gaps: an MSM epoch time that does not advance by the configured
 *     period counts as lost data, independent of byte counters.
 *
 * Feeding is cheap (header decode only, no allocation, no logging) and runs
 * in the GNSS RX task; readers get snapshots under a short lock.
 *
 * Bands are the buckets of gnss_rtcm3.h (L1, L2, L5, L6, E5 AltBOC). A signal
 * counts for 10 s after it was last seen, so one missing epoch does not
 * change the count. Galileo E6 is only expected once it has been seen
 * since the last gnss_obs_reset() (it depends on the receiver's signal
 * group); losing it after that is reported in e6_lost.
 *
 * Status: per-constellation satellites, signal masks, bands, best
 * constellation, E6 loss, epoch count and age, and the antenna reference
 * point are implemented (pure core host-tested). Not yet: per-band CNR
 * (cnr_dbhz stays 0), epoch gaps, message statistics and the sky view
 * (ESP_ERR_NOT_SUPPORTED).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_rtcm3.h"   /* gnss_sys_t, gnss_band_t */

#ifdef __cplusplus
extern "C" {
#endif

/** Per-constellation summary: signals of the last 10 s, satellites of the latest epoch. */
typedef struct {
    bool present;
    uint8_t satellites;
    uint32_t signal_mask;          /**< MSM signal IDs seen in the last 10 s (bit id-1) */
    uint8_t band_mask;             /**< bands present (bit per gnss_band_t) */
    uint8_t bands;                 /**< distinct frequency bands present */
    uint8_t cnr_dbhz[8];           /**< mean CNR per band, index gnss_band_t (MSM7 only), 0 = n/a */
} gnss_obs_sys_t;

typedef struct {
    gnss_obs_sys_t sys[GNSS_SYS_COUNT];
    gnss_sys_t best_sys;           /**< constellation with the most bands */
    uint8_t best_bands;            /**< fewer than 4: DEGRADED with an alert */
    bool e6_lost;                  /**< Galileo E6 seen since the last reset, now missing: alert */
    uint32_t epoch_age_ms;         /**< since the last complete epoch, UINT32_MAX = never */
    uint64_t epochs;
    uint64_t epoch_gaps;           /**< epochs missing from the expected cadence */
} gnss_obs_summary_t;

/** Observed RTCM message statistics. */
typedef struct {
    uint16_t msg;
    uint64_t count;
    uint32_t period_ms;            /**< measured average interval */
    uint32_t age_ms;               /**< since last seen */
} gnss_obs_msg_stat_t;

/** Antenna reference point from the latest CRC-valid 1005 or 1006 (public RTCM 10403). */
typedef struct {
    bool valid;
    uint16_t station_id;           /**< reference station ID of that frame */
    int64_t ecef_01mm[3];          /**< X, Y, Z in 0.1 mm units */
    uint32_t age_ms;               /**< since that frame, UINT32_MAX = never */
} gnss_obs_arp_t;

/** One satellite for the sky view (from satellite-in-view lines). */
typedef struct {
    gnss_sys_t sys;
    uint8_t prn;
    int8_t elevation_deg;
    uint16_t azimuth_deg;
    uint8_t cnr_dbhz;
} gnss_obs_sat_t;

esp_err_t gnss_obs_init(void);

/** Every published RTCM3 frame (GNSS RX task; never blocks). */
void gnss_obs_feed_rtcm(const uint8_t *frame, size_t len, uint16_t msg_type);

/** Satellite-in-view text lines (GNSS RX task; never blocks). */
void gnss_obs_feed_line(const char *line, size_t len);

esp_err_t gnss_obs_get_summary(gnss_obs_summary_t *out);

/** *count in = capacity, out = filled (up to 32 message types). */
esp_err_t gnss_obs_get_msg_stats(gnss_obs_msg_stat_t *out, size_t *count);

/** *count in = capacity, out = filled. */
esp_err_t gnss_obs_get_sky(gnss_obs_sat_t *out, size_t *count);

/** Latest antenna reference point (the base position shown and locked). ESP_ERR_NOT_FOUND before the first 1005/1006. */
esp_err_t gnss_obs_get_arp(gnss_obs_arp_t *out);

/** Forget everything (after a receiver reconfigure or flash). */
void gnss_obs_reset(void);

#ifdef __cplusplus
}
#endif
