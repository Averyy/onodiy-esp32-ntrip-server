/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Boot-loop guard (pure logic, no IDF includes, host-testable).
 *
 * The state lives in RTC memory that survives software, panic and watchdog
 * resets but not power loss. It counts crashes since the last stable boot,
 * decides safe mode, time-boxes safe mode with growing retry intervals, and
 * carries the reason code of a planned reboot into the next boot.
 *
 * It also carries, validated by the same magic and checksum:
 *  - the session-uptime heartbeat: the running boot's uptime, written every
 *    60 s and at a planned reboot (RTC only, no flash). The next boot reports
 *    it as the previous session's uptime only after a software, panic or
 *    watchdog reset; after power-on, brownout or other resets it is unknown;
 *  - "station time": uptime accumulated over the boots since power-on (each
 *    boot adds the previous session's last heartbeat, except after a
 *    brownout, whose RTC content is not trusted for that);
 *  - the GNSS-ladder reboot budget: the station times of the last three
 *    GNSS-ladder reboots. Such a reboot is allowed only if none happened in
 *    the last 60 min and fewer than three in the last 24 h. Losing the
 *    state (power cycle, or the first boot of another app image whose RTC
 *    layout differs) starts a fresh budget, like a power cycle;
 *  - the stamp: the image generation of the run that wrote the block
 *    (boot_gen.h). boot_guard_on_boot() runs before the settings can be read
 *    and accounts the boot on whatever valid block it finds; lifecycle_init
 *    then checks the stamp and, for a block that predates another image's
 *    run, accounts the boot again from scratch (boot_guard_restart), as
 *    after power loss. A fresh block is unstamped until boot_guard_stamp().
 *
 * Counting rules:
 *  - power-on (or lost/corrupt state): everything starts over;
 *  - crash (panic, watchdog, lockup, or a reboot flagged BOOT_GUARD_REASON_CRASH):
 *    +1;
 *  - brownout: never counts toward safe mode, tracked separately (a supply
 *    problem that safe mode cannot fix);
 *  - planned reboot, reset pin, unexpected software restart: unchanged;
 *  - stable uptime reached in normal mode: counter and safe-mode episodes
 *    cleared. Reaching stable uptime in safe mode clears nothing.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BOOT_GUARD_MAGIC 0x4C494645u        /* "LIFE" */
/** Set in a reboot reason code to make the next boot count as a crash. */
#define BOOT_GUARD_REASON_CRASH 0x80000000u
/** Previous session uptime not known. */
#define BOOT_GUARD_UNKNOWN 0xFFFFFFFFu

#define BOOT_GUARD_LADDER_KEEP     3u        /**< GNSS-ladder reboots remembered (and allowed per window) */
#define BOOT_GUARD_LADDER_GAP_S    3600u     /**< at least this long between two of them */
#define BOOT_GUARD_LADDER_WINDOW_S 86400u    /**< at most BOOT_GUARD_LADDER_KEEP within this window */

/** What the hardware says about the last reset (mapped from esp_reset_reason()). */
typedef enum {
    BOOT_HW_POWER_ON = 0,
    BOOT_HW_SOFTWARE,      /**< esp_restart() */
    BOOT_HW_CRASH,         /**< panic, interrupt/task/other watchdog, CPU lockup */
    BOOT_HW_BROWNOUT,      /**< brownout or power glitch */
    BOOT_HW_OTHER,         /**< reset pin, USB, JTAG, deep sleep, ... */
} boot_hw_reset_t;

/** Our classification of the last reset. */
typedef enum {
    BOOT_RESET_POWER_ON = 0,
    BOOT_RESET_PLANNED,
    BOOT_RESET_CRASH,
    BOOT_RESET_BROWNOUT,
    BOOT_RESET_OTHER,
} boot_reset_class_t;

typedef struct {
    uint32_t magic;
    uint32_t boots;             /**< boots since the state started (power-on) */
    uint32_t crashes;           /**< crashes since the last stable normal-mode boot */
    uint32_t brownouts;         /**< since power-on */
    uint32_t safe_episodes;     /**< safe-mode entries since the last stable normal-mode boot */
    uint32_t reboot_reason;     /**< set before a planned reboot, consumed by the next boot */
    uint32_t once_flags;        /**< one-shot actions already taken since power-on */
    uint32_t safe_mode;         /**< this boot runs in safe mode */
    uint32_t uptime_base_s;     /**< uptime of the earlier boots since power-on (station time at this boot's start) */
    uint32_t session_s;         /**< heartbeat: this boot's uptime at the last write */
    uint32_t ladder_count;      /**< valid entries in ladder_at (<= BOOT_GUARD_LADDER_KEEP) */
    uint32_t ladder_at[BOOT_GUARD_LADDER_KEEP]; /**< station times of the last GNSS-ladder reboots, oldest first */
    uint32_t gen;               /**< image generation of the run that wrote it (BOOT_GEN_UNSTAMPED = none yet) */
    uint32_t check;             /**< checksum over the fields above */
} boot_guard_state_t;

typedef struct {
    boot_reset_class_t cls;
    uint32_t prev_reboot_reason;   /**< reason code the previous boot left (0 = none) */
    bool state_was_lost;           /**< RTC state was invalid (first boot or memory lost) */
    bool safe_mode;
    bool entered_safe_mode;        /**< this boot is the first of a safe-mode episode */
    uint32_t prev_session_s;       /**< previous boot's uptime (s), BOOT_GUARD_UNKNOWN if not trusted */
} boot_guard_result_t;

/** True if the state carries the magic and a matching checksum. */
bool boot_guard_valid(const boot_guard_state_t *st);

/** Recompute the checksum after changing the state. */
void boot_guard_seal(boot_guard_state_t *st);

/** Account for this boot. `limit` = crashes that trigger safe mode. Seals the state. */
void boot_guard_on_boot(boot_guard_state_t *st, boot_hw_reset_t hw, uint32_t limit, boot_guard_result_t *out);

/**
 * The state boot_guard_on_boot() used turned out to predate another image's
 * run (its stamp was not accepted): account this boot again from a fresh
 * state, as after power loss, keeping the one-shot flags `claimed` since the
 * first accounting (they belong to this power cycle's boots of this image).
 * The result replaces the first one. Seals.
 */
void boot_guard_restart(boot_guard_state_t *st, boot_hw_reset_t hw, uint32_t limit, uint32_t claimed,
                        boot_guard_result_t *out);

/** Stamp the state with the generation of this boot's run. Seals. */
void boot_guard_stamp(boot_guard_state_t *st, uint32_t gen);

/** Stable uptime reached. Clears the counters in normal mode only. Seals. */
void boot_guard_on_stable(boot_guard_state_t *st);

/** Record the reason code for the planned reboot that follows. Seals. */
void boot_guard_set_reboot_reason(boot_guard_state_t *st, uint32_t reason);

/** Seconds a safe-mode episode lasts before a normal boot is retried: 1 h, 4 h, then 24 h. */
uint32_t boot_guard_safe_retry_s(const boot_guard_state_t *st);

/** Prepare the retry from safe mode: counter = limit - 1, so one more crash re-enters safe mode. Seals. */
void boot_guard_prepare_safe_retry(boot_guard_state_t *st, uint32_t limit);

/** User cleared safe mode: counter and episodes to zero. Seals. */
void boot_guard_clear(boot_guard_state_t *st);

/** True the first time `flag` is claimed since power-on. Seals. */
bool boot_guard_claim_once(boot_guard_state_t *st, uint32_t flag);

/** Session heartbeat: record this boot's uptime (every 60 s and before a planned reboot). Seals. */
void boot_guard_heartbeat(boot_guard_state_t *st, uint32_t uptime_s);

/** Station time: uptime accumulated since power-on, including this boot's `uptime_s`. */
uint32_t boot_guard_station_time(const boot_guard_state_t *st, uint32_t uptime_s);

/**
 * May the GNSS ladder reboot now? `retry_in_s` (optional) gets the seconds
 * until it may, 0 when allowed.
 */
bool boot_guard_ladder_reboot_allowed(const boot_guard_state_t *st, uint32_t uptime_s, uint32_t *retry_in_s);

/** Record a GNSS-ladder reboot happening now (also writes the heartbeat). Seals. */
void boot_guard_record_ladder_reboot(boot_guard_state_t *st, uint32_t uptime_s);

#ifdef __cplusplus
}
#endif
