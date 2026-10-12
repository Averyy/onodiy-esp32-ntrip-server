/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signal-group comparison: the view of a running or finished test (types
 * only, no IDF includes), shared by the station's public API and its
 * host-tested core (sg_test_core.h).
 *
 * The user picks two signal groups and a measuring time per group; the
 * station measures one, then the other (the one running now first, which
 * saves a switch), then waits up to 24 h of station running time for the
 * user to keep one. Doing nothing brings back the group in use before the
 * test. Band and constellation indexes are gnss_band_t and gnss_sys_t
 * (gnss_rtcm3.h).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define STATION_SGT_SYS   7u    /**< constellations (gnss_sys_t) */
#define STATION_SGT_BANDS 5u    /**< band buckets (gnss_band_t) */

/** Test state. */
typedef enum {
    STATION_SGT_IDLE = 0,
    STATION_SGT_SWITCHING,      /**< the receiver is being switched to the phase's group (casters paused) */
    STATION_SGT_SETTLING,       /**< the group runs; the first minute is not measured */
    STATION_SGT_MEASURING,      /**< samples every 10 s while the station streams */
    STATION_SGT_DECIDING,       /**< both measured: waits for the user's choice (24 h of running time) */
    STATION_SGT_RESTORING,      /**< switching to the group the test ends on */
} station_sgt_state_t;

/** How a test ended. */
typedef enum {
    STATION_SGT_OUT_NONE = 0,
    STATION_SGT_OUT_KEPT,        /**< the user kept one of the two */
    STATION_SGT_OUT_TIMED_OUT,   /**< no choice within 24 h: the group from before the test is back */
    STATION_SGT_OUT_CANCELLED,   /**< the user stopped it */
    STATION_SGT_OUT_REJECTED,    /**< the receiver refused one of the groups */
    STATION_SGT_OUT_FAILED,      /**< a switch failed, or the receiver did not stream for 10 min */
    STATION_SGT_OUT_INTERRUPTED, /**< the stored record could not be read after a restart */
} station_sgt_outcome_t;

/** One group's result. */
typedef enum {
    STATION_SGT_RES_PENDING = 0,
    STATION_SGT_RES_MEASURING,
    STATION_SGT_RES_DONE,
    STATION_SGT_RES_REJECTED,
} station_sgt_res_status_t;

typedef struct {
    uint8_t master;
    uint8_t slave;
} station_sg_t;

typedef struct {
    uint8_t band;               /**< gnss_band_t */
    uint16_t sats_x10;          /**< mean satellites with a signal on the band, x10 */
    uint16_t cnr_x10;           /**< mean C/N0 (dB-Hz) x10, 0 = none measured */
    uint32_t signal_mask;       /**< MSM signal IDs seen on the band (bit id-1) */
} station_sgt_band_t;

typedef struct {
    uint8_t sys;                /**< gnss_sys_t */
    uint16_t sats_x10;          /**< mean satellites, x10 */
    uint8_t nband;              /**< bands listed: present in at least half of the samples */
    station_sgt_band_t band[STATION_SGT_BANDS];
} station_sgt_sys_t;

typedef struct {
    station_sg_t group;
    uint8_t status;             /**< station_sgt_res_status_t */
    uint32_t measured_s;        /**< good samples x 10 s */
    uint32_t samples;
    uint32_t epoch_gaps;        /**< epochs missing while measuring */
    uint8_t best_sys;           /**< gnss_sys_t of the rewarded constellation with the most bands; 0xFF = none */
    uint8_t best_bands;
    uint8_t nsys;
    station_sgt_sys_t sys[STATION_SGT_SYS];
} station_sgt_result_t;

/** GET /api/gnss/signal-test. */
typedef struct {
    uint8_t state;              /**< station_sgt_state_t */
    bool have_test;             /**< a test ran (or runs) since the record was last cleared: groups, results */
    station_sg_t groups[2];     /**< as the user chose them (A, B) */
    station_sg_t before;        /**< the group running when the test started */
    int8_t phase;               /**< 0 first measured, 1 second; -1 none (idle, deciding, restoring) */
    uint16_t phase_s;           /**< measuring time per group */
    uint16_t settle_s;
    uint32_t left_s;            /**< settling or measuring: time left in the phase; UINT32_MAX = none */
    uint32_t decide_left_s;     /**< deciding: running time left to choose; UINT32_MAX = none */
    bool have_running;
    station_sg_t running;       /**< the group the phase switches to or measures */
    station_sgt_result_t results[2];   /**< index as groups */
    uint8_t outcome;            /**< station_sgt_outcome_t of the last test (NONE while running) */
    bool have_kept;
    station_sg_t kept;          /**< KEPT: the group kept */
    char reason[96];            /**< what happened, in words ("" = nothing to say) */
    int64_t ended_unix;         /**< 0 = no clock then, or not ended */
    uint32_t ended_age_s;       /**< since it ended this boot, UINT32_MAX = not this boot */
} station_sg_test_t;

/** /api/status receiver.signal_test. */
typedef struct {
    uint8_t state;              /**< station_sgt_state_t */
    int8_t phase;               /**< -1 none */
    bool have_running;
    station_sg_t running;
    uint32_t left_s;            /**< UINT32_MAX = none */
    uint32_t decide_left_s;     /**< UINT32_MAX = none */
    uint8_t outcome;            /**< station_sgt_outcome_t of the last test this boot */
    uint32_t ended_age_s;       /**< UINT32_MAX = none this boot */
} station_sgt_brief_t;

/** Why a start (or keep, stop) was refused. */
typedef enum {
    STATION_SGT_OK = 0,
    STATION_SGT_E_GROUPS,           /**< two different pairs of 0-255 are needed */
    STATION_SGT_E_PHASE,            /**< phase_s outside 60-7200 */
    STATION_SGT_E_FIRST_APPLY_HELD, /**< the receiver's first configuration is held */
    STATION_SGT_E_AUTOCFG_OFF,      /**< automatic configuration is off */
    STATION_SGT_E_MAINTENANCE,      /**< receiver maintenance is on */
    STATION_SGT_E_JOURNAL_OPEN,     /**< a receiver update journal is open */
    STATION_SGT_E_SAFE_MODE,
    STATION_SGT_E_UNSUPPORTED,      /**< not the receiver model this firmware configures */
    STATION_SGT_E_IDENTITY,         /**< the receiver's identity is not confirmed this boot */
    STATION_SGT_E_NOT_STREAMING,
    STATION_SGT_E_CONFIG_FAILED,    /**< the last profile apply failed */
    STATION_SGT_E_NOT_LOCKED,       /**< the position is not locked: every restart would survey again */
    STATION_SGT_E_RUNNING,          /**< a test runs already */
    STATION_SGT_E_SETTINGS,         /**< settings unavailable: the test could not be stored */
    STATION_SGT_E_NOT_DECIDING,     /**< keep: only while deciding */
    STATION_SGT_E_IDLE,             /**< stop: nothing runs */
    STATION_SGT_E_NOT_RUNNING,      /**< the station does not run */
} station_sgt_err_t;

#ifdef __cplusplus
}
#endif
