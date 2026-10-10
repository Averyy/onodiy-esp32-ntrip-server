/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation summary: what the receiver is actually sending.
 *
 * Fed from the RTCM path (every CRC-valid frame, including the ones the hub
 * drops) and from the receiver's text lines (only its satellite-in-view
 * sentences are used). It decodes only public structures: the RTCM 10403
 * MSM header (epoch time, multiple-message bit, satellite/signal/cell
 * masks), the per-cell carrier-to-noise values of MSM4 to MSM7, the
 * antenna reference point of 1005/1006, and NMEA satellite-in-view
 * sentences. From that it maintains:
 *
 *   - per constellation: satellites in the latest epoch, signals (bands)
 *     present, and the mean CNR per band over the cells of the latest
 *     epoch -> the web UI's signal view without extra receiver output;
 *   - per message number: frames, messages (an MSM type counts once per
 *     epoch however many frames carry it), the mean of the last 8
 *     intervals and the time since the latest frame -> the observed message
 *     set and rates, listed next to the profile's messages and periods
 *     (gnss_obs_set_expected), seen or not;
 *   - "bands on the best constellation": Onocoy rewards 4 bands on one
 *     constellation (Galileo E1/E5a/E5b/E6 via signal group 3/6). Fewer than
 *     4 is reported as DEGRADED with an alert even while everything else
 *     looks fine;
 *   - epoch gaps against the configured period: an MSM epoch time that
 *     advances by more than the profile's period counts the missing epochs
 *     as lost data, independent of byte counters. Each missing epoch counts
 *     once for the station (on the lowest present constellation with a
 *     configured period), and once per constellation in its own count. An
 *     advance of more than 60 s (or four periods) or a step back is an
 *     outage or time reset ("jump"), counted separately, not as gaps;
 *   - the sky view: satellites with azimuth, elevation and CNR from the
 *     satellite-in-view sentences, which the receiver sends only when the
 *     profile's satellite-view output is on (every 10 s). Satellites are
 *     numbered as in the MSM satellite mask: GPS, Galileo, BeiDou and NavIC
 *     by PRN, GLONASS by slot, QZSS as PRN - 192, SBAS as PRN - 119.
 *
 * Feeding runs in the GNSS RX task: decoding happens outside the lock, the
 * update under a short spinlock, no allocation, no logging, never blocks.
 * Readers get snapshots under the same lock. The state lives in PSRAM.
 *
 * Bands are the buckets of gnss_rtcm3.h (L1, L2, L5, L6, E5 AltBOC). A signal
 * (and its CNR) counts for 10 s after it was last seen, so one missing
 * epoch does not change the count. Galileo E6 is only expected once it has
 * been seen since the last gnss_obs_reset() (it depends on the receiver's
 * signal group); losing it after that is reported in e6_lost.
 *
 * Counters (epochs, gaps, jumps, message counts) run since the last
 * gnss_obs_reset(), which the station calls after every successful receiver
 * configuration: a reader that takes deltas must allow them to go down.
 *
 * Getters work before gnss_obs_init() (safe mode skips it): they report
 * nothing seen.
 *
 * Status: implemented, the pure parts host-tested. PROVISIONAL, to be
 * confirmed on hardware: the receiver's satellite-in-view talkers and
 * satellite numbering, and the band buckets (gnss_rtcm3.h).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_rtcm3.h"   /* gnss_sys_t, gnss_band_t */
#include "gnss_types.h"   /* um98x_rtcm_out_t */

#ifdef __cplusplus
extern "C" {
#endif

#define GNSS_OBS_MAX_MSG_TYPES 32   /**< message numbers tracked (the profile's always among them) */
#define GNSS_OBS_MAX_SKY       80   /**< satellites kept for the sky view */

/** Per-constellation summary: signals of the last 10 s, satellites of the latest epoch. */
typedef struct {
    bool present;
    uint8_t satellites;
    uint32_t signal_mask;          /**< MSM signal IDs seen in the last 10 s (bit id-1) */
    uint8_t band_mask;             /**< bands present (bit per gnss_band_t) */
    uint8_t bands;                 /**< distinct frequency bands present */
    uint8_t cnr_dbhz[8];           /**< mean CNR per band, index gnss_band_t (MSM4-7 only), 0 = n/a */
    uint64_t epoch_gaps;           /**< epochs of this constellation missing from its configured cadence */
} gnss_obs_sys_t;

typedef struct {
    gnss_obs_sys_t sys[GNSS_SYS_COUNT];
    gnss_sys_t best_sys;           /**< constellation with the most bands */
    uint8_t best_bands;            /**< fewer than 4: DEGRADED with an alert */
    bool e6_lost;                  /**< Galileo E6 seen since the last reset, now missing: alert */
    uint32_t epoch_age_ms;         /**< since the last complete epoch, UINT32_MAX = never */
    uint64_t epochs;
    uint64_t epoch_gaps;           /**< epochs missing from the expected cadence (station total) */
    uint64_t epoch_jumps;          /**< epoch time jumps (outage over 60 s, time reset): not counted as gaps */
    uint32_t epoch_period_ms;      /**< shortest configured MSM period, 0 = none (no gaps counted) */
} gnss_obs_summary_t;

/** Observed RTCM message statistics. */
typedef struct {
    uint16_t msg;
    uint16_t expected_period_s;    /**< period in the profile, 0 = not in the profile */
    uint64_t count;                /**< messages (MSM: epochs); 0 = never seen (a profile entry) */
    uint64_t frames;               /**< frames (an MSM epoch may span several) */
    uint32_t period_ms;            /**< mean of the last 8 intervals, UINT32_MAX = fewer than two messages */
    uint32_t age_ms;               /**< since last seen, UINT32_MAX = never */
} gnss_obs_msg_stat_t;

/** Antenna reference point from the latest CRC-valid 1005 or 1006 (public RTCM 10403). */
typedef struct {
    bool valid;
    uint16_t station_id;           /**< reference station ID of that frame */
    int64_t ecef_01mm[3];          /**< X, Y, Z in 0.1 mm units */
    uint32_t age_ms;               /**< since that frame, UINT32_MAX = never */
} gnss_obs_arp_t;

/** One satellite for the sky view (from satellite-in-view sentences). */
typedef struct {
    gnss_sys_t sys;
    uint8_t prn;                   /**< satellite number as in the MSM satellite mask (see above) */
    int8_t elevation_deg;          /**< -90..90 */
    uint16_t azimuth_deg;          /**< 0..359, true north */
    uint8_t cnr_dbhz;              /**< highest of its signals in the latest report, 0 = not tracked */
} gnss_obs_sat_t;

esp_err_t gnss_obs_init(void);

/** Every CRC-valid RTCM3 frame (GNSS RX task; never blocks). */
void gnss_obs_feed_rtcm(const uint8_t *frame, size_t len, uint16_t msg_type);

/** Every checksum-valid text line (GNSS RX task; never blocks): satellite-in-view sentences are used. */
void gnss_obs_feed_line(const char *line, size_t len);

/**
 * The profile's RTCM messages and periods (the station, whenever it loads
 * the profile): listed in the message statistics, seen or not, and the
 * expected epoch period per constellation for the gap count (the shortest
 * period of its MSM types; constellations without one count no gaps). At
 * most UM98X_MAX_RTCM_MSGS entries. Kept across gnss_obs_reset().
 */
void gnss_obs_set_expected(const um98x_rtcm_out_t *rtcm, size_t count);

esp_err_t gnss_obs_get_summary(gnss_obs_summary_t *out);

/**
 * Message statistics sorted by message number. *count in = capacity of out
 * (GNSS_OBS_MAX_MSG_TYPES holds them all), out = filled. ESP_ERR_INVALID_ARG
 * without count, or without out for a non-zero capacity.
 */
esp_err_t gnss_obs_get_msg_stats(gnss_obs_msg_stat_t *out, size_t *count);

/**
 * Satellites in view, sorted by constellation and satellite number; only
 * those reported with azimuth and elevation within the last 30 s. *count
 * in = capacity, out = filled (the lowest constellation/satellite numbers
 * when more are in view). ESP_ERR_NOT_FOUND (count 0) when no
 * satellite-in-view sentence arrived within 30 s: the receiver does not send
 * them (satellite-view output off) or has not yet. ESP_ERR_INVALID_ARG as
 * for gnss_obs_get_msg_stats.
 */
esp_err_t gnss_obs_get_sky(gnss_obs_sat_t *out, size_t *count);

/** Latest antenna reference point (the base position shown and locked). ESP_ERR_NOT_FOUND before the first 1005/1006. */
esp_err_t gnss_obs_get_arp(gnss_obs_arp_t *out);

/**
 * GPS time of the latest GPS MSM epoch: its epoch time as sent (ms of the
 * GPS week) and the time since that MSM arrived. ESP_ERR_NOT_FOUND when no
 * GPS MSM arrived within the last 10 s; ESP_ERR_INVALID_ARG without both
 * outputs.
 */
esp_err_t gnss_obs_get_gps_time(uint32_t *tow_ms, uint32_t *age_ms);

/** Forget every observation (after a receiver reconfigure or flash); the expected messages stay. */
void gnss_obs_reset(void);

#ifdef __cplusplus
}
#endif
