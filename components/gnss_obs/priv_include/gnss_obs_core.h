/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observation summary core (pure logic, no IDF or RTOS includes, injected
 * millisecond clock, host-testable).
 *
 * Fed with decoded MSM headers (gnss_rtcm3_msm_decode) and their per-band
 * CNR (obs_msm_cnr), with every RTCM frame's message number, and with parsed
 * satellite-in-view sentences (obs_gsv_parse). The caller decodes outside
 * its lock; every feed here is bounded (no allocation, at most a pass over
 * a fixed table).
 *
 * Signals and bands: per constellation it remembers when each signal ID was
 * last seen, so the signal mask and the band count describe the last
 * OBS_SIGNAL_WINDOW_MS: a band that stops arriving drops out of the count
 * after that long, and a single missing epoch does not. Satellites are
 * counted over all MSMs of the constellation's latest epoch (a
 * constellation may be split over several MSMs with the same epoch time).
 *
 * Band self-check: the best constellation is the one with the most bands
 * (ties go to the lower gnss_sys_t value). Galileo E6 that was seen since
 * the last reset and is now missing is reported as lost: E6 depends on the
 * receiver's signal group, so it is only expected once it has been seen.
 *
 * CNR: the cells of one constellation's epoch (all its MSMs with the same
 * epoch time) are averaged per band. The epoch is finished by the next MSM
 * with the multiple-message bit clear, or by the constellation's next epoch
 * time, whichever comes first; the view shows the mean of the latest
 * finished epoch, for bands present in the window, as long as it is not
 * older than the window.
 *
 * Epochs: an MSM with the multiple-message bit clear closes an epoch; the
 * core counts those and the time since the latest one.
 *
 * Epoch gaps, against the configured period (obs_core_set_expected: the
 * shortest period of the constellation's MSM types in the profile; nothing
 * is counted for a constellation without one). When a constellation's
 * epoch time advances by d, round(d / period) - 1 epochs are missing
 * (none for an advance shorter than 1.5 periods). Epoch times are compared
 * within one constellation only, modulo its week (GLONASS: day of week and
 * time of day; modulo a day while the day is unknown). An advance of more
 * than OBS_EPOCH_GAP_MAX_MS (or four periods, if that is longer), or a step
 * back, is an outage or a time reset, not a run of missing epochs: the
 * count restarts from there (a "jump").
 * The station total counts each missing epoch once: only gaps found on the
 * reference constellation count, i.e. the lowest gnss_sys_t with a
 * configured period that is present, so a lost epoch (missing from every
 * constellation) is not counted once per constellation. Per-constellation
 * counts are kept too.
 *
 * Messages: per message number, frames and messages (an MSM type counts one
 * message per epoch time, however many frames carry it), the time of the
 * latest frame and the mean of the last OBS_MSG_INTERVALS intervals between
 * messages. The profile's messages are always in the table (with their
 * configured period), seen or not; other numbers take free entries, and a
 * full table gives up the entry of a number not seen for OBS_MSG_STALE_MS
 * (else the new number is not tracked and counted in msg_dropped).
 *
 * Sky view: satellites from satellite-in-view sentences with azimuth and
 * elevation, kept for OBS_SKY_WINDOW_MS after they were last reported
 * (several times the receiver's 10 s output period, so one lost sentence
 * does not blank a satellite). A satellite reported for several signals in
 * one output burst keeps the highest CNR of that burst.
 *
 * The clock is any monotonic millisecond count (the caller uses esp_timer).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "gnss_obs_parse.h"
#include "gnss_rtcm3.h"
#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OBS_SIGNAL_WINDOW_MS 10000u   /**< how long a signal (and its CNR) counts after it was last seen */
#define OBS_EPOCH_GAP_MAX_MS 60000u   /**< a longer advance of the epoch time is a jump, not gaps */
#define OBS_MSG_MAX          32u      /**< message numbers tracked */
#define OBS_MSG_INTERVALS    8u       /**< intervals averaged for a message's period */
#define OBS_MSG_INTERVAL_MAX 3600000u /**< longer intervals are counted as this (ms) */
#define OBS_MSG_STALE_MS     60000u   /**< a full table gives up a number not seen for this long */
#define OBS_EXPECT_MAX       UM98X_MAX_RTCM_MSGS
#define OBS_SKY_MAX          80u      /**< satellites kept for the sky view */
#define OBS_SKY_WINDOW_MS    30000u   /**< a satellite stays this long after its last report */
#define OBS_SKY_BURST_MS     2000u    /**< reports of one satellite this close together are one burst */

#define OBS_NEVER UINT32_MAX          /**< "never" / "unknown" in views */

/** One message number. */
typedef struct {
    uint16_t msg;
    uint16_t expected_period_s;        /**< in the profile with this period; 0 = not in the profile */
    bool epoch_valid;                  /**< MSM: epoch holds the epoch time last counted */
    uint32_t epoch;
    uint64_t count;                    /**< messages (MSM: epochs) */
    uint64_t frames;
    uint64_t last_ms;                  /**< latest frame (valid with frames > 0) */
    uint64_t counted_ms;               /**< latest counted message */
    uint32_t iv[OBS_MSG_INTERVALS];    /**< latest intervals between messages, ring */
    uint32_t iv_sum;
    uint8_t iv_n;
    uint8_t iv_next;
} obs_msg_t;

/** One satellite of the sky view. */
typedef struct {
    uint8_t sys;                       /**< gnss_sys_t */
    uint8_t sat;                       /**< MSM satellite number */
    int8_t elevation_deg;
    uint16_t azimuth_deg;
    uint8_t cnr_dbhz;                  /**< 0 = not tracked */
    uint64_t seen_ms;                  /**< latest report with azimuth and elevation */
    uint64_t cnr_ms;                   /**< start of the burst cnr_dbhz belongs to */
} obs_sky_entry_t;

/** Configuration (survives obs_core_reset). */
typedef struct {
    uint8_t expect_count;
    um98x_rtcm_out_t expect[OBS_EXPECT_MAX];
    uint32_t period_ms[GNSS_SYS_COUNT];            /**< expected MSM period, 0 = not configured */
} obs_core_cfg_t;

typedef struct {
    obs_core_cfg_t cfg;

    /* signals and epochs */
    uint64_t sig_seen_ms[GNSS_SYS_COUNT][32];      /**< when each signal ID (index id-1) was last seen */
    uint32_t sig_ever[GNSS_SYS_COUNT];             /**< signal IDs seen at least once (bit id-1) */
    uint64_t msm_seen_ms[GNSS_SYS_COUNT];          /**< latest MSM of the constellation */
    uint32_t epoch_time[GNSS_SYS_COUNT];           /**< epoch time of that MSM */
    uint64_t epoch_sats[GNSS_SYS_COUNT];           /**< satellites in that epoch (union over its MSMs) */
    uint8_t msm_ever;                              /**< constellations with at least one MSM (bit per gnss_sys_t) */
    bool e6_seen;                                  /**< Galileo E6 seen since the last reset */
    bool epoch_ever;
    uint64_t epochs;                               /**< MSMs that closed an epoch */
    uint64_t epoch_end_ms;                         /**< when the latest of them arrived */

    /* CNR */
    uint32_t cnr_acc_q4[GNSS_SYS_COUNT][GNSS_BAND_COUNT];   /**< epoch in progress: sum, 1/16 dB-Hz */
    uint16_t cnr_acc_n[GNSS_SYS_COUNT][GNSS_BAND_COUNT];    /**< epoch in progress: cells */
    uint16_t cnr_q4[GNSS_SYS_COUNT][GNSS_BAND_COUNT];       /**< mean of the latest finished epoch */
    uint64_t cnr_ms[GNSS_SYS_COUNT][GNSS_BAND_COUNT];       /**< when it was finished */
    uint8_t cnr_valid[GNSS_SYS_COUNT];                      /**< bands with a finished mean (bit per gnss_band_t) */
    uint8_t cnr_open;                                       /**< constellations with an epoch in progress */

    /* epoch gaps */
    uint64_t sys_gaps[GNSS_SYS_COUNT];
    uint64_t sys_jumps[GNSS_SYS_COUNT];
    uint64_t epoch_gaps;                           /**< station total (reference constellation) */
    uint64_t epoch_jumps;

    /* messages */
    uint8_t msg_count;
    uint32_t msg_dropped;                          /**< frames of numbers the full table could not take */
    obs_msg_t msg[OBS_MSG_MAX];

    /* sky view */
    bool gsv_ever;
    uint64_t gsv_ms;                               /**< latest satellite-in-view sentence */
    uint8_t sky_count;
    uint32_t sky_dropped;                          /**< reports the full table could not take */
    obs_sky_entry_t sky[OBS_SKY_MAX];
} obs_core_t;

typedef struct {
    bool present;              /**< an MSM of this constellation within the window */
    uint8_t satellites;        /**< in its latest epoch (0 when not present) */
    uint32_t signal_mask;      /**< signal IDs seen within the window (bit id-1) */
    uint8_t band_mask;         /**< bands within the window (bit per gnss_band_t) */
    uint8_t bands;             /**< number of bits in band_mask */
    uint8_t cnr_dbhz[GNSS_BAND_COUNT];   /**< mean CNR per band, rounded (at least 1), 0 = not available */
    uint64_t epoch_gaps;       /**< missing epochs of this constellation */
} obs_core_sys_t;

typedef struct {
    obs_core_sys_t sys[GNSS_SYS_COUNT];
    gnss_sys_t best_sys;       /**< most bands (GNSS_SYS_GPS with 0 bands when nothing is present) */
    uint8_t best_bands;
    bool e6_lost;              /**< Galileo E6 seen since the last reset, but not within the window */
    uint64_t epochs;
    uint32_t epoch_age_ms;     /**< since the latest epoch-closing MSM, OBS_NEVER = never */
    uint32_t epoch_period_ms;  /**< shortest configured MSM period, 0 = none configured */
    uint64_t epoch_gaps;       /**< station total */
    uint64_t epoch_jumps;
    uint32_t msg_dropped;
} obs_core_view_t;

/** Message statistics as of a view. */
typedef struct {
    uint16_t msg;
    uint16_t expected_period_s;  /**< 0 = not in the profile */
    uint64_t count;              /**< messages (MSM: epochs) */
    uint64_t frames;
    uint32_t period_ms;          /**< mean interval, OBS_NEVER = fewer than two messages */
    uint32_t age_ms;             /**< since the latest frame, OBS_NEVER = never */
} obs_msg_stat_t;

/** One satellite of the sky view. */
typedef struct {
    gnss_sys_t sys;
    uint8_t sat;
    int8_t elevation_deg;
    uint16_t azimuth_deg;
    uint8_t cnr_dbhz;            /**< 0 = not tracked */
} obs_sat_t;

/** Forget everything, configuration included (a zeroed struct is the same). */
void obs_core_init(obs_core_t *c);

/** Forget every observation (also the "E6 seen" memory); keep the configuration. */
void obs_core_reset(obs_core_t *c);

/**
 * The profile's RTCM messages and periods: listed in the message table
 * from now on, and the source of the expected epoch period per
 * constellation. At most OBS_EXPECT_MAX entries (more are ignored); a
 * number listed twice keeps its shortest period. Observations are kept.
 */
void obs_core_set_expected(obs_core_t *c, const um98x_rtcm_out_t *rtcm, size_t count);

/**
 * One decoded MSM header received at now_ms, with its per-band CNR (NULL
 * when the MSM carries none or its cells did not decode).
 */
void obs_core_feed_msm(obs_core_t *c, const gnss_rtcm3_msm_t *h, const obs_cnr_t *cnr, uint64_t now_ms);

/**
 * One RTCM frame of message number msg (0 is ignored). has_epoch /
 * epoch_time: the MSM epoch time when the frame is an MSM whose header
 * decoded; frames of a type with the epoch time already counted for it are
 * part of the same message.
 */
void obs_core_feed_msg(obs_core_t *c, uint16_t msg, bool has_epoch, uint32_t epoch_time, uint64_t now_ms);

/** One parsed satellite-in-view sentence received at now_ms. */
void obs_core_feed_gsv(obs_core_t *c, const obs_gsv_t *g, uint64_t now_ms);

/** Summary as of now_ms. */
void obs_core_view(const obs_core_t *c, uint64_t now_ms, obs_core_view_t *out);

/** Message statistics, sorted by message number; returns the entries written (at most cap). */
size_t obs_core_msgs(const obs_core_t *c, uint64_t now_ms, obs_msg_stat_t *out, size_t cap);

/** Whether a satellite-in-view sentence arrived within OBS_SKY_WINDOW_MS. */
bool obs_core_sky_available(const obs_core_t *c, uint64_t now_ms);

/**
 * Satellites of the sky view, sorted by constellation and satellite number;
 * returns the entries written (at most cap).
 */
size_t obs_core_sky(const obs_core_t *c, uint64_t now_ms, obs_sat_t *out, size_t cap);

#ifdef __cplusplus
}
#endif
