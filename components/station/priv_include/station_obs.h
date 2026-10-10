/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Observed-set tracker (pure logic, no IDF or RTOS includes, injected
 * millisecond clock, host-testable): does the receiver send what the profile
 * asks for?
 *
 * The station feeds it every RTCM frame that reaches the hub (from the GNSS
 * RX task, under the station's lock) and evaluates it from its own task.
 * One window at a time: armed with the profile's message list, it starts at
 * the first RTCM frame after arming (so output that resumes a few seconds
 * after a configuration session is not held against it) and lasts
 * STN_OBS_WINDOW_MS, or twice the longest period of a checked message if
 * that is longer.
 *
 * Verdict at the end of the window:
 *  - MSM types of the global constellations (GPS, GLONASS, Galileo, BeiDou)
 *    must arrive at their period: at least half and at most one and a half
 *    times (plus one) the count the period gives over the window;
 *  - MSM types of regional systems (QZSS, NavIC) are checked the same way
 *    only when they arrive at all: outside their service area the receiver
 *    tracks no satellite of the system and sends nothing for it, which is
 *    not drift (re-applying could never fix it);
 *  - station messages (1005, 1006, 1033, and 1230 only when the receiver
 *    accepted it at the last apply) must have been seen within twice their
 *    period, at least 30 s, before the end of the window; inside a survey
 *    window 1005, 1006 and 1033 are not required (the receiver may hold
 *    them back while it surveys);
 *  - other messages (ephemerides, ...) are not checked;
 *  - no RTCM frame at all within the window: STN_OBS_NO_RTCM, a stall for
 *    the health ladder rather than drift.
 * Extra messages the profile does not list are not drift.
 *
 * PROVISIONAL, to be confirmed on hardware: the rate tolerances, and whether
 * the receiver holds station messages back while it surveys.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#define STN_OBS_WINDOW_MS      30000   /**< shortest window */
#define STN_OBS_STATION_MIN_MS 30000   /**< a station message is due within max(2 x period, this) */
#define STN_OBS_MAX            UM98X_MAX_RTCM_MSGS

#define STN_NEVER INT64_MAX            /**< "no time": nothing scheduled, never seen */

typedef enum {
    STN_OBS_PENDING = 0,   /**< not armed, or the window is still open */
    STN_OBS_PASS,          /**< every checked message as the profile asks */
    STN_OBS_DRIFT,         /**< RTCM flows, but a checked message is missing or at the wrong rate */
    STN_OBS_NO_RTCM,       /**< no RTCM frame at all within the window */
} stn_obs_verdict_t;

typedef struct {
    bool armed;
    uint8_t count;
    uint16_t msg[STN_OBS_MAX];
    uint16_t period_s[STN_OBS_MAX];
    uint32_t seen[STN_OBS_MAX];        /**< frames of that type since the window started */
    int64_t last_ms[STN_OBS_MAX];      /**< latest frame of that type (STN_NEVER = none) */
    int64_t armed_ms;
    int64_t first_ms;                  /**< first RTCM frame since arming: window start (STN_NEVER = none yet) */
    uint32_t frames;                   /**< RTCM frames of any type since the window started */
} stn_obs_t;

typedef struct {
    bool rtcm1230_ok;   /**< the receiver accepted RTCM1230 at the last apply */
    bool surveying;     /**< inside the survey window */
} stn_obs_opts_t;

/** Forget everything (not armed: verdict PENDING). */
void stn_obs_clear(stn_obs_t *o);

/** Start a new window for these messages (the profile's RTCM list). */
void stn_obs_arm(stn_obs_t *o, const um98x_rtcm_out_t *rtcm, uint8_t count, int64_t now_ms);

/** One RTCM frame of type msg (GNSS RX task: constant time per profile entry, never blocks). */
void stn_obs_feed(stn_obs_t *o, uint16_t msg, int64_t now_ms);

/** Window length for the armed message list (ms). */
int64_t stn_obs_window_ms(const stn_obs_t *o);

/**
 * Verdict as of now_ms (see above). *first_bad (optional) gets the first
 * checked message that failed, 0 if none.
 */
stn_obs_verdict_t stn_obs_eval(const stn_obs_t *o, const stn_obs_opts_t *opts, int64_t now_ms,
                               uint16_t *first_bad);

#ifdef __cplusplus
}
#endif
