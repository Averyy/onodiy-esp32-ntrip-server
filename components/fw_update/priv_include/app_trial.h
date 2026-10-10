/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * App trial after an update, and the rolled-back list (pure logic, no IDF or
 * RTOS includes, injected clock; host-testable).
 *
 * The trial record (ota/trial) is written by the image that installs an
 * update, right after the new image was activated, and then by the new image
 * as it goes through its trial:
 *
 *   PENDING ---confirm---> VERIFYING ---baseline reached---> WATCHING ---24 h---> NONE
 *      |                      |  window over, baseline missed   |  crash resets
 *      |                      +--------------+------------------+
 *      |                                     v
 *      |                                 REVERTED   (also: manual rollback)
 *      +--bootloader rolled it back--> ROLLED_BACK  (seen by the previous image)
 *
 * Stage 1 (PENDING on a PENDING_VERIFY image): the bootloader rolls the image
 * back on any reset before it is confirmed. Confirmation needs init complete
 * and `confirm_s` of uptime.
 * Stage 2 (VERIFYING): within `verify_window_s` of its start in this boot the
 * station must reach the baseline saved at install time: the station had an
 * IP, the caster slots that were streaming stream again, the hub is fed. Each
 * part counts once seen during the window. When the window ends short of it,
 * a receiver-side problem (the hub not fed while the station reports a
 * receiver problem) excuses the hub and the casters, so an unplugged antenna
 * does not revert a good image; anything else reverts it.
 * Crash watch (VERIFYING and WATCHING): `crash_revert_count` crash resets
 * before `watch_s` of stage-2 time revert the image. Stage-2 time is the
 * uptime accumulated over the trial's boots (the previous session's uptime
 * is added at each boot when known) or, once the clock is trusted, the
 * wall-clock time since stage 2 started, whichever is larger.
 *
 * A version that is reverted by its trial or rolled back by the user goes on
 * the rolled-back list (ota/bad_ver). A version the bootloader rolled back
 * during stage 1 does not (a power blip and a crash look the same there):
 * the banner offers it again, and if it rolls back again after that second
 * install it goes on the list.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FW_TRIAL_REC_NONE = 0,       /**< no trial (finished, or never updated) */
    FW_TRIAL_REC_PENDING,        /**< activated; the new image has not been confirmed yet */
    FW_TRIAL_REC_VERIFYING,      /**< stage 2: reaching the baseline */
    FW_TRIAL_REC_WATCHING,       /**< stage 2: crash watch */
    FW_TRIAL_REC_REVERTED,       /**< reverted by its trial or rolled back by the user */
    FW_TRIAL_REC_ROLLED_BACK,    /**< the bootloader rolled it back during stage 1 */
    FW_TRIAL_REC_COUNT,
} fw_trial_rec_stage_t;

/** Baseline flags (what the station had reached when the update was installed). */
#define FW_BASELINE_STA_IP  (1u << 0)
#define FW_BASELINE_HUB_FED (1u << 1)

/** Trial record (persisted as a versioned blob). */
typedef struct {
    char version[32];            /**< the version on trial (or reverted / rolled back) */
    uint8_t stage;               /**< fw_trial_rec_stage_t */
    uint8_t attempt;             /**< 1; 2 when installed again after a stage-1 rollback */
    bool baseline_known;         /**< false: activated without a record, stage 2 has nothing to reach */
    uint8_t baseline;            /**< FW_BASELINE_* */
    uint8_t streaming_slots;     /**< bit n: caster slot n was streaming */
    int64_t installed_unix;      /**< 0 = no trusted clock at install */
    int64_t watch_start_unix;    /**< wall-clock start of stage 2, 0 = not known (yet) */
    uint32_t watch_uptime_s;     /**< stage-2 uptime of earlier boots */
    uint32_t confirm_uptime_s;   /**< uptime at confirmation in the boot that confirmed (0 after that boot) */
    uint16_t boots;              /**< boots of the image on trial */
    uint16_t crashes;            /**< crash resets during stage 2 */
} fw_trial_rec_t;

#define FW_TRIAL_REC_MAX_BLOB 96

/** Encode; @return blob length, 0 if it does not fit. */
size_t fw_trial_rec_encode(const fw_trial_rec_t *r, uint8_t *buf, size_t cap);

/** Decode (version 1 and newer writers' trailing fields); false for anything unreadable. */
bool fw_trial_rec_decode(const uint8_t *buf, size_t len, fw_trial_rec_t *out);

/**
 * Record for a freshly activated image. `prev` is the record before the
 * install (NULL if none): installing again the version that the bootloader
 * rolled back during stage 1 makes this attempt 2.
 */
void fw_trial_rec_new(fw_trial_rec_t *out, const fw_trial_rec_t *prev, const char *version, uint8_t baseline,
                      uint8_t streaming_slots, int64_t installed_unix);

/* ---- rolled-back list ---------------------------------------------------------------- */

#define FW_BAD_VER_MAX 8

typedef struct {
    uint8_t count;
    char v[FW_BAD_VER_MAX][32];  /**< oldest first */
} fw_bad_versions_t;

#define FW_BAD_VER_MAX_BLOB (2 + FW_BAD_VER_MAX * 32)

size_t fw_bad_encode(const fw_bad_versions_t *b, uint8_t *buf, size_t cap);
bool fw_bad_decode(const uint8_t *buf, size_t len, fw_bad_versions_t *out);
bool fw_bad_contains(const fw_bad_versions_t *b, const char *version);
/** Add a version (no duplicates; the oldest goes when full). @return true if the list changed. */
bool fw_bad_add(fw_bad_versions_t *b, const char *version);

/* ---- boot ------------------------------------------------------------------------- */

/** Trial state of this boot. */
typedef enum {
    FW_TRIAL_RUN_NONE = 0,
    FW_TRIAL_RUN_CONFIRMING,     /**< stage 1 */
    FW_TRIAL_RUN_VERIFYING,      /**< stage 2, reaching the baseline */
    FW_TRIAL_RUN_WATCHING,       /**< stage 2, crash watch */
} fw_trial_run_t;

typedef struct {
    const char *running_version;
    bool pending_verify;         /**< the running image is not confirmed yet (bootloader rollback armed) */
    bool crash_reset;            /**< this boot follows a crash reset (panic, watchdog, boot failure) */
    uint32_t prev_uptime_s;      /**< previous session's uptime, UINT32_MAX = unknown */
    const char *last_invalid_version; /**< app version in the slot last marked invalid or aborted, "" if none */
    uint32_t crash_revert_count;
} fw_trial_boot_in_t;

typedef struct {
    fw_trial_run_t run;
    bool revert;                 /**< revert the running image now (crash watch) */
    bool persist;                /**< the record changed: write it */
    bool add_bad;                /**< put rec->version on the rolled-back list */
    bool stage1_rollback;        /**< the update installed by this image was rolled back by the bootloader */
} fw_trial_boot_out_t;

/**
 * Boot bookkeeping. `rec` holds the stored record (stage NONE if there is
 * none or it was unreadable) and is updated in place.
 */
void fw_trial_on_boot(fw_trial_rec_t *rec, const fw_trial_boot_in_t *in, fw_trial_boot_out_t *out);

/* ---- running trial -------------------------------------------------------------------- */

/** What stage 2 has seen during the current verify window, and the station now. */
typedef struct {
    bool sta_online_seen;
    bool hub_fed_seen;
    uint8_t streaming_seen;      /**< slots seen streaming */
    bool receiver_problem;       /**< now: the station is detecting, recovering or has no receiver */
} fw_trial_obs_t;

/** Every part of the baseline was seen during the window. */
bool fw_trial_baseline_reached(const fw_trial_rec_t *rec, const fw_trial_obs_t *obs);

/** Reached, or what is missing is excused by a receiver-side problem (end of the window). */
bool fw_trial_baseline_excused(const fw_trial_rec_t *rec, const fw_trial_obs_t *obs);

typedef enum {
    FW_TRIAL_ACT_NONE = 0,
    FW_TRIAL_ACT_CONFIRM,        /**< mark the image valid, enter VERIFYING */
    FW_TRIAL_ACT_VERIFIED,       /**< baseline reached: enter WATCHING */
    FW_TRIAL_ACT_REVERT,         /**< verify window over without the baseline */
    FW_TRIAL_ACT_DONE,           /**< crash watch over: the update is accepted */
} fw_trial_action_t;

typedef struct {
    fw_trial_run_t run;
    uint32_t uptime_s;
    bool init_complete;          /**< all start steps ran (fw_update_start) */
    uint32_t confirm_s;          /**< CONFIG_FW_UPDATE_CONFIRM_S */
    uint32_t window_start_s;     /**< uptime at which this boot's verify window started */
    uint32_t verify_window_s;    /**< CONFIG_FW_UPDATE_VERIFY_WINDOW_S */
    uint32_t watch_elapsed_s;    /**< fw_trial_watch_elapsed_s() */
    uint32_t watch_s;            /**< CONFIG_FW_UPDATE_CRASH_WATCH_H in seconds */
} fw_trial_tick_in_t;

/** One step of the running trial (call about once a second). */
fw_trial_action_t fw_trial_tick(const fw_trial_tick_in_t *in, const fw_trial_rec_t *rec, const fw_trial_obs_t *obs);

/**
 * Stage-2 time so far: earlier boots plus this boot since `stage2_start_s`
 * (uptime), or the wall-clock time since rec->watch_start_unix when the
 * clock is trusted, whichever is larger.
 */
uint32_t fw_trial_watch_elapsed_s(const fw_trial_rec_t *rec, uint32_t uptime_s, uint32_t stage2_start_s,
                                  bool clock_trusted, int64_t now_unix);

/**
 * Seconds until this boot's trial ends at the latest (banner: when a
 * receiver update becomes possible). CONFIRMING counts the confirmation, the
 * crash watch; VERIFYING and WATCHING count the rest of the crash watch.
 */
uint32_t fw_trial_left_s(fw_trial_run_t run, uint32_t uptime_s, uint32_t confirm_s, uint32_t watch_elapsed_s,
                         uint32_t watch_s);

#ifdef __cplusplus
}
#endif
