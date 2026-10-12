/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Signal-group comparison core (pure logic, no IDF or RTOS includes,
 * injected millisecond clock, host-testable). station.c drives it from the
 * station task; requests from other tasks call it under the station's lock.
 *
 * States: IDLE -> SWITCHING(p) -> SETTLING(p) -> MEASURING(p) -> (after
 * phase 1) DECIDING -> RESTORING(target) -> IDLE with an outcome.
 *  - Order: when one of the two groups is the one running now it is
 *    measured first (phase 0), which saves a switch; otherwise A, then B.
 *  - A switch: the core raises want_switch with the target group; the
 *    caller writes that group into the stored profile and queues an
 *    internal re-apply (the station's own reconfigure: casters paused,
 *    planned downtime, the signal group's restart handling and the RESET
 *    rollback all reused), then reports the apply with sgt_apply_done().
 *    Success = the apply succeeded (its read-back shows the group). A group
 *    the receiver refuses -> RESTORING(before), outcome REJECTED; another
 *    failure, more than SGT_SWITCH_MAX_S in SWITCHING, or more than
 *    SGT_NOT_STREAMING_S without streaming while settling or measuring ->
 *    RESTORING(before), outcome FAILED. A phase whose group already runs
 *    skips the switch.
 *  - SETTLING: SGT_SETTLE_S of station running time, nothing measured.
 *  - MEASURING: every SGT_SAMPLE_S the caller hands in a sample (the live
 *    totals and the observation summary); only samples taken while the
 *    station streams with an epoch younger than SGT_FRESH_MS count. The
 *    phase ends after phase_s of good samples (samples x SGT_SAMPLE_S).
 *  - DECIDING: SGT_DECIDE_S of station running time; the receiver stays on
 *    the group measured last. keep(0|1) switches only if that group is not
 *    the one running (outcome KEPT); the time running out restores the
 *    group from before the test (TIMED_OUT).
 *  - stop() from any running state restores the group from before the test
 *    (CANCELLED).
 *  - RESTORING: one switch to the target (skipped when it runs already);
 *    whatever its result, the test then ends (a failed restore is named in
 *    the reason; the station's own apply backoff takes over).
 *
 * Persistence: sgt_encode()/sgt_decode() (a versioned record, the caller's
 * gnss/sg_test), due (save_due) at every transition, every SGT_SAVE_MEASURE_S
 * while measuring and every SGT_SAVE_DECIDE_S while deciding (the caller
 * also stores it before a planned restart, sgt_store_at_shutdown). Every
 * switch is asked for (want_switch) together with a record due, and the
 * caller stores that record before it writes the switch's group into the
 * profile: a restart in between finds the switch in the record and asks for
 * it again, instead of resuming the phase before it with the receiver on
 * the new group. Time is
 * counted in good samples and station running time, never wall-clock time,
 * so a restart resumes cleanly: SWITCHING or RESTORING asks for the switch
 * again, SETTLING or MEASURING settles again and keeps what was measured,
 * DECIDING keeps its countdown. A record that cannot be read ends the test
 * as INTERRUPTED (sgt_interrupted). The finished test's record is kept, so
 * its results are shown after a restart too.
 *
 * Notes (journal, DIAG_EV_SG_TEST): a = event | master << 8 | slave << 16
 * (1 started: the first group to measure, 2 switched: b = phase, 3 refused
 * by the receiver: the refused group, b = the group restored as
 * master << 8 | slave, 4 ended: the group the test ended on, b = outcome).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "station_sgt.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SGT_SETTLE_S         60u
#define SGT_SAMPLE_S         10u
#define SGT_DECIDE_S         86400u
#define SGT_PHASE_MIN_S      60u
#define SGT_PHASE_MAX_S      7200u
#define SGT_PHASE_DEFAULT_S  1800u
#define SGT_NOT_STREAMING_S  600u     /**< settling or measuring without streaming this long: failed */
#define SGT_SWITCH_MAX_S     600u     /**< a switch (or restore) not done after this long: failed */
#define SGT_FRESH_MS         3000u    /**< a good sample: the latest epoch is younger than this */
#define SGT_SAVE_MEASURE_S   300u
#define SGT_SAVE_DECIDE_S    600u
#define SGT_NOTES            8u
#define SGT_REC_VERSION      1u
#define SGT_REC_MAX          1280u    /**< largest encoded record */

/** Journal events (low byte of a). */
typedef enum {
    SGT_EV_STARTED = 1,
    SGT_EV_SWITCHED = 2,
    SGT_EV_REFUSED = 3,
    SGT_EV_ENDED = 4,
} sgt_event_t;

typedef struct {
    uint8_t event;              /**< sgt_event_t */
    int32_t a;
    int32_t b;
} sgt_note_t;

/** What the station says about a start (filled by the caller). */
typedef struct {
    bool settings_ok;
    bool first_apply_held;
    bool autocfg;
    bool maintenance;
    bool journal_open;
    bool safe_mode;
    bool supported;             /**< the model is the one this firmware configures */
    bool identity_confirmed;
    bool streaming;
    bool config_failed;
    bool position_locked;
    station_sg_t running;       /**< the stored profile's group (the receiver runs it while streaming applied) */
} sgt_guard_t;

/** One sample (the caller's gnss_obs reading). */
typedef struct {
    bool streaming;             /**< the station streams */
    uint32_t epoch_age_ms;      /**< since the latest epoch ended (UINT32_MAX = never) */
    uint8_t sys_sats[STATION_SGT_SYS];
    uint8_t band_sats[STATION_SGT_SYS][STATION_SGT_BANDS];
    uint8_t cnr_dbhz[STATION_SGT_SYS][STATION_SGT_BANDS];   /**< 0 = none */
    uint32_t signal_mask[STATION_SGT_SYS];
    uint64_t epoch_gaps;        /**< station total (gnss_obs; starts over after a reconfiguration) */
} sgt_sample_t;

/** The outcome of an apply the core asked for. */
typedef struct {
    bool ok;
    bool rejected;              /**< the receiver refused the group */
    bool read_known;            /**< the session's latest read-back named a group */
    station_sg_t read;          /**< ... that group */
    char reason[64];            /**< the receiver's reply, or why it failed */
} sgt_apply_t;

/** Accumulated samples of one group. */
typedef struct {
    uint8_t status;             /**< station_sgt_res_status_t */
    uint32_t samples;
    uint32_t epoch_gaps;
    uint32_t sys_sat_sum[STATION_SGT_SYS];
    uint32_t sat_sum[STATION_SGT_SYS][STATION_SGT_BANDS];
    uint32_t cnr_sum[STATION_SGT_SYS][STATION_SGT_BANDS];
    uint16_t cnr_n[STATION_SGT_SYS][STATION_SGT_BANDS];
    uint16_t band_seen[STATION_SGT_SYS][STATION_SGT_BANDS];   /**< samples with a satellite on the band */
    uint32_t sig_union[STATION_SGT_SYS];
} sgt_acc_t;

typedef struct {
    uint8_t state;              /**< station_sgt_state_t */
    uint8_t outcome;            /**< station_sgt_outcome_t of the last test */
    bool have_test;
    uint8_t phase;              /**< 0, 1 */
    uint8_t order[2];           /**< groups index measured in phase 0, 1 */
    station_sg_t groups[2];
    station_sg_t before;
    bool cur_known;
    station_sg_t cur;           /**< the group the receiver runs as far as the test knows */
    station_sg_t target;        /**< SWITCHING, RESTORING */
    station_sg_t applying;      /**< the group of the re-apply queued last (switch_queued) */
    uint16_t phase_s;
    int8_t kept;                /**< groups index kept, -1 none */
    char reason[STATION_SGT_REASON_MAX];
    int64_t ended_unix;
    sgt_acc_t acc[2];

    /* this boot (monotonic ms; INT64_MIN = none) */
    int64_t state_ms;           /**< entered the current state */
    int64_t settle_until_ms;
    int64_t next_sample_ms;
    int64_t quiet_since_ms;     /**< not streaming since (settling, measuring) */
    int64_t decide_until_ms;
    int64_t ended_ms;
    int64_t saved_ms;
    bool have_gaps;
    uint64_t last_gaps;

    /* outputs for the caller */
    bool want_switch;           /**< write `target` into the profile and queue the re-apply (sgt_switch_queued) */
    bool switch_queued;         /**< the re-apply for the current switch is queued or running */
    bool save_due;              /**< persist the record (sgt_saved) */
    uint8_t note_n;
    sgt_note_t notes[SGT_NOTES];
} sgt_t;

void sgt_init(sgt_t *t);

/** Valid start arguments: two different pairs, phase_s in range. @return STATION_SGT_OK, _E_GROUPS or _E_PHASE. */
int sgt_check_args(const station_sg_t groups[2], uint32_t phase_s);

/**
 * Start a test (the checks in the order of the API's errors: arguments,
 * then the station's state). @return station_sgt_err_t.
 */
int sgt_start(sgt_t *t, const sgt_guard_t *g, const station_sg_t groups[2], uint32_t phase_s, int64_t now_ms);

/** Keep group 0 or 1 (DECIDING only). @return STATION_SGT_OK, _E_NOT_DECIDING, _E_GROUPS for another index. */
int sgt_keep(sgt_t *t, unsigned idx, int64_t now_ms, int64_t unix_s);

/** Stop a running test. @return STATION_SGT_OK or _E_IDLE. */
int sgt_stop(sgt_t *t, int64_t now_ms, int64_t unix_s);

/** True while a test runs (any state but IDLE). */
bool sgt_running(const sgt_t *t);

/** The caller wrote `target` into the profile and queued the re-apply. */
void sgt_switch_queued(sgt_t *t);

/** The re-apply the core asked for finished. */
void sgt_apply_done(sgt_t *t, const sgt_apply_t *a, int64_t now_ms, int64_t unix_s);

/** A sample is due now (MEASURING, every SGT_SAMPLE_S). */
bool sgt_sample_due(const sgt_t *t, int64_t now_ms);

/**
 * Advance the clocks. `streaming`: the station streams now. `sample`: the
 * sample asked for by sgt_sample_due() (NULL otherwise).
 */
void sgt_tick(sgt_t *t, bool streaming, const sgt_sample_t *sample, int64_t now_ms, int64_t unix_s);

/** The record was persisted. */
void sgt_saved(sgt_t *t, int64_t now_ms);

/**
 * Before a planned restart: the record is worth storing now. While a test
 * runs (what it measured since the last periodic save), and when its end
 * or any other transition is not stored yet (save_due): keep and stop often
 * end the test at once, and the station task stores that later.
 */
bool sgt_store_at_shutdown(const sgt_t *t);

/** The record. @return its length, 0 when it does not fit. */
size_t sgt_encode(const sgt_t *t, int64_t now_ms, uint8_t *buf, size_t cap);

/**
 * Restore a record after a restart (see the file comment).
 * @return false for a short, damaged or unknown-version record (t unchanged).
 */
bool sgt_decode(sgt_t *t, const uint8_t *buf, size_t len, int64_t now_ms);

/** A record was there but could not be read: the test ended as INTERRUPTED (journal note; the caller erases it). */
void sgt_interrupted(sgt_t *t, int64_t now_ms, int64_t unix_s);

/** The API view. */
void sgt_view(const sgt_t *t, int64_t now_ms, station_sg_test_t *out);

/** The /api/status summary (left_s rounded up to 10 s, decide_left_s to 60 s: the section changes rarely). */
void sgt_brief(const sgt_t *t, int64_t now_ms, station_sgt_brief_t *out);

/** Take the journal notes written since the last call. @return count copied. */
unsigned sgt_take_notes(sgt_t *t, sgt_note_t *out, unsigned cap);

/** The journal's `a` for an event about a group. */
int32_t sgt_note_a(uint8_t event, station_sg_t g);

#ifdef __cplusplus
}
#endif
