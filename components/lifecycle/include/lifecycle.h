/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * System lifecycle: boot accounting, safe mode, and the one way to reboot.
 *
 * Boot accounting (lifecycle_early_boot, first line of app_main):
 *   classifies the reset (power-on / planned / crash / brownout / other),
 *   counts crashes since the last stable boot in RTC memory, and decides
 *   safe mode at CONFIG_LIFECYCLE_BOOTLOOP_LIMIT. Brownouts never count; they
 *   raise a power-supply alert instead. Safe mode is time-boxed: after 1 h,
 *   4 h, then 24 h the station retries a normal boot (one more early crash
 *   puts it straight back). Safe mode turns off the receiver path and the
 *   casters; Wi-Fi, the web UI, the update check and user-initiated app
 *   installs stay on so a fix can arrive.
 *   The RTC state is linked after IDF's own RTC data, so a boot into a
 *   different app image may not find it and starts over as after power-on:
 *   never use it to pass information between images. One RTC block, checked
 *   by magic and checksum, holds the crash accounting, the session-uptime
 *   heartbeat (written every 60 s, RTC only; read back by the next boot
 *   after software, panic and watchdog resets only) and the GNSS-ladder
 *   reboot budget (see lifecycle_ladder_reboot_allowed). An invalid block
 *   counts as a fresh start, like a power cycle.
 *
 * Service task (started by lifecycle_init): once per second it marks the
 *   boot stable after CONFIG_LIFECYCLE_STABLE_UPTIME_S, writes the session
 *   heartbeat every 60 s and, in safe mode, requests the normal-boot retry
 *   when the episode's time is up. It also runs
 *   the reboots that lifecycle_request_reboot() and lifecycle_inhibit_release()
 *   commit, so neither caller runs the shutdown hooks on its own stack. Safe
 *   mode therefore ends on time without depending on any higher-layer task.
 *
 * Reboot service (lifecycle_reboot / lifecycle_request_reboot):
 *   every planned reboot goes through here. It records a reason code that
 *   the next boot reads back, posts LIFECYCLE_EVENT_PRE_REBOOT, runs the
 *   registered shutdown hooks within CONFIG_LIFECYCLE_SHUTDOWN_BUDGET_MS (so
 *   casters close their sessions cleanly), then restarts. A hook that
 *   overruns the budget does not stop the restart: a one-shot timer restarts
 *   anyway shortly after the budget. Only one reboot runs: once one is
 *   committed, later requests join it.
 *
 * Reboot inhibit (lifecycle_inhibit_acquire/release):
 *   "do not reboot now" held while something must not be cut short
 *   (a receiver flash session until it ends, waits for the user or its
 *   post-flash re-apply has been checked; app image writes; the app update
 *   trial window).
 *   Non-forced reboots are refused while any inhibit is held; a reboot
 *   requested with lifecycle_request_reboot() is queued and runs when the
 *   last one is released. Checking the inhibits and committing a reboot is
 *   one atomic step, and no inhibit can be taken once a reboot is committed.
 *
 * App trial flag (lifecycle_set_app_trial): fw_update publishes whether a
 *   new app image is in its trial (confirm, verify or crash watch), so the
 *   station, which may not call fw_update, can refuse to start a receiver
 *   flash meanwhile.
 *
 * Update slot (lifecycle_update_claim): one firmware update at a time across
 *   both chips. fw_update claims it for an app download, manual upload or
 *   install; the station claims it for a receiver flash session in full
 *   lockout. Checking and claiming is one atomic step, so an app install and
 *   a receiver flash can never start together, whichever path (banner or
 *   manual upload) starts them.
 *
 * Layer: L1. Anything above may call it; it calls only settings and diag.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_system.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(LIFECYCLE_EVENT);

/** LIFECYCLE_EVENT ids. */
typedef enum {
    LIFECYCLE_EVENT_PRE_REBOOT,   /**< data: lifecycle_reboot_reason_t; restart follows shortly */
} lifecycle_event_t;

/**
 * Why we rebooted ourselves (stored across the reset, shown in boot history).
 * New values are appended: the numbers are stored (life/boots, journal).
 */
typedef enum {
    LIFECYCLE_REBOOT_NONE = 0,
    LIFECYCLE_REBOOT_USER,             /**< web UI / API */
    LIFECYCLE_REBOOT_FACTORY_RESET,
    LIFECYCLE_REBOOT_ADMIN_RESET,      /**< BOOT button: admin password cleared */
    LIFECYCLE_REBOOT_APP_UPDATE,       /**< new app image activated */
    LIFECYCLE_REBOOT_APP_REVERT,       /**< the app trial failed (verify window or crash watch) */
    LIFECYCLE_REBOOT_GNSS_LADDER,
    LIFECYCLE_REBOOT_NETWORK_LADDER,
    LIFECYCLE_REBOOT_LOW_HEAP,
    LIFECYCLE_REBOOT_SAFE_MODE_RETRY,
    LIFECYCLE_REBOOT_SAFE_MODE_CLEARED,
    LIFECYCLE_REBOOT_NVS_SLOT_SWITCH,  /**< NVS written by a newer app: booting that app */
    LIFECYCLE_REBOOT_BOOT_FAILURE,     /**< mandatory service failed at boot; counts as a crash */
    LIFECYCLE_REBOOT_APP_ROLLBACK,     /**< the user rolled the app back (web UI / API) */
} lifecycle_reboot_reason_t;

/**
 * True for a reboot that is a direct consequence of a user action: its
 * downtime is planned, like receiver work (the ledger shows it as planned).
 *   USER, FACTORY_RESET (web UI or the BOOT button held), APP_UPDATE (every
 *   app install is started by the user: the banner's Update or a manual
 *   upload), APP_ROLLBACK (the user's Roll back), SAFE_MODE_CLEARED (the
 *   user left safe mode), ADMIN_RESET (the user held the BOOT button; the
 *   admin reset itself does not restart today): planned.
 *   APP_REVERT (a failed trial: the station undoes an update that did not
 *   work, a recovery like the ladders), NVS_SLOT_SWITCH (automatic, at
 *   boot, before anything runs), SAFE_MODE_RETRY (automatic), the GNSS and
 *   network ladders, LOW_HEAP, BOOT_FAILURE and NONE: not planned.
 * A crash, brownout or power cut has no reason code and is never planned.
 */
bool lifecycle_reboot_user_planned(lifecycle_reboot_reason_t reason);

/** Our classification of the last reset. */
typedef enum {
    LIFECYCLE_RESET_POWER_ON = 0,
    LIFECYCLE_RESET_PLANNED,
    LIFECYCLE_RESET_CRASH,
    LIFECYCLE_RESET_BROWNOUT,
    LIFECYCLE_RESET_OTHER,
} lifecycle_reset_class_t;

/** This boot, as accounted by lifecycle_early_boot(). */
typedef struct {
    esp_reset_reason_t reset_reason;           /**< raw IDF reason */
    lifecycle_reset_class_t reset_class;
    lifecycle_reboot_reason_t prev_reboot_reason; /**< what the previous boot planned, NONE if not planned */
    uint32_t boots_since_power_on;
    uint32_t crashes;                          /**< since the last stable boot */
    uint32_t brownouts_since_power_on;         /**< > 0 raises the power-supply alert */
    bool safe_mode;
    uint32_t safe_mode_retry_s;                /**< when safe mode retries a normal boot (uptime), 0 if not in safe mode */
    bool stable;                               /**< this boot reached CONFIG_LIFECYCLE_STABLE_UPTIME_S */
    uint32_t prev_session_uptime_s;            /**< uptime of the previous boot (s); UINT32_MAX = unknown (power-on, brownout, other resets) */
} lifecycle_boot_info_t;

/** One-shot actions tracked per power cycle (lifecycle_claim_once). */
#define LIFECYCLE_ONCE_NVS_SLOT_SWITCH (1u << 0)

/**
 * First call in app_main, before NVS or anything else that could crash:
 * classify the reset, update the RTC counters, decide safe mode.
 */
void lifecycle_early_boot(void);

/**
 * After settings_init(), in every boot mode: start the lifecycle service task,
 * then append this boot to the persisted boot history (a history written by a
 * newer app is left untouched). ESP_ERR_INVALID_STATE without settings; the
 * service task runs anyway.
 */
esp_err_t lifecycle_init(void);

/** True if this boot runs in safe mode. */
bool lifecycle_in_safe_mode(void);

/** Boot accounting snapshot. */
void lifecycle_get_boot_info(lifecycle_boot_info_t *out);

/** One boot of the persisted history (settings life/boots). */
typedef struct {
    lifecycle_reset_class_t reset_class;
    esp_reset_reason_t reset_reason;            /**< raw IDF reason */
    lifecycle_reboot_reason_t planned_reason;   /**< what the boot before it planned, NONE if not planned */
    uint32_t boot_no;                           /**< boots since power-on */
    uint32_t prev_uptime_s;                     /**< uptime of the session before it (s); UINT32_MAX = unknown */
    char version[32];                           /**< app version that booted */
} lifecycle_boot_record_t;

/** Boots kept in the persisted history. */
#define LIFECYCLE_BOOT_HISTORY_LEN 16

/**
 * Read the persisted boot history, newest first (for the web UI).
 * @param count in = capacity of out, out = entries filled (0 when none is stored)
 * @return ESP_ERR_INVALID_STATE without settings this boot,
 *         ESP_ERR_INVALID_VERSION for a history written by a newer app (left
 *         untouched, nothing filled), ESP_ERR_INVALID_ARG for NULL.
 */
esp_err_t lifecycle_get_boot_history(lifecycle_boot_record_t *out, size_t *count);

/**
 * Leave safe mode now (user action): reboot with the crash counter and the
 * safe-mode episodes cleared. The counters change only if the reboot is
 * committed: while an inhibit is held the call is refused and nothing changes.
 * Never returns on success.
 * @return ESP_ERR_INVALID_STATE while a reboot is inhibited.
 */
esp_err_t lifecycle_clear_safe_mode(void);

/** Opaque inhibit token. */
typedef int lifecycle_inhibit_t;

/**
 * Block non-forced reboots until released.
 * @param what short label shown in status ("receiver flash"); not copied, use a literal.
 * @return ESP_ERR_NO_MEM if CONFIG_LIFECYCLE_MAX_INHIBITS are held;
 *         ESP_ERR_INVALID_STATE if a reboot is already committed (do not start
 *         the operation the inhibit was meant to protect).
 */
esp_err_t lifecycle_inhibit_acquire(const char *what, lifecycle_inhibit_t *out);

/**
 * Release an inhibit. Always returns: when the last one goes, a reboot queued
 * by lifecycle_request_reboot() is committed and runs on the lifecycle service
 * task (in the caller only if that task could not be started) right away.
 * So release only once the protected work is complete, including its final
 * step (e.g. after esp_ota_set_boot_partition(), not before it).
 */
void lifecycle_inhibit_release(lifecycle_inhibit_t token);

/** True while any inhibit is held; *what (optional) gets one holder's label. */
bool lifecycle_reboot_inhibited(const char **what);

/**
 * Reboot now, in the calling task. Never returns on success (also not when
 * another reboot is already under way: the caller then waits for it).
 * @param force reboot even while inhibited (fatal paths only).
 * @return ESP_ERR_INVALID_STATE if inhibited and not forced.
 */
esp_err_t lifecycle_reboot(lifecycle_reboot_reason_t reason, bool force);

/**
 * Reboot as soon as no inhibit is held. Returns once the reboot is committed
 * (it then runs on the lifecycle service task) or queued; never refused.
 * @return ESP_OK, or ESP_ERR_INVALID_ARG for LIFECYCLE_REBOOT_NONE.
 */
esp_err_t lifecycle_request_reboot(lifecycle_reboot_reason_t reason);

/**
 * Mandatory service failed at boot: log, wait CONFIG_LIFECYCLE_FATAL_RETRY_S
 * (no tight loop; console stays readable), reboot. Counts as a crash, so a
 * persistent failure ends in safe mode, and an app still in its first-boot
 * check is rolled back by the bootloader. Never returns.
 */
void lifecycle_fatal_boot_error(const char *what, esp_err_t err) __attribute__((noreturn));

/**
 * Shutdown hook: finish within deadline_ms (e.g. close caster sessions).
 * `reason`: the reboot under way (LIFECYCLE_REBOOT_NONE for an esp_restart()
 * that did not come through lifecycle).
 */
typedef void (*lifecycle_shutdown_hook_t)(lifecycle_reboot_reason_t reason, uint32_t deadline_ms, void *ctx);

/**
 * Register a hook run before a planned restart; hooks run in the order they
 * were registered. Reliable only for reboots through
 * lifecycle_reboot()/lifecycle_request_reboot(), which run the hooks while
 * the network is still up. For any other esp_restart() the hooks still run,
 * but after IDF has stopped Wi-Fi (its shutdown handler is registered later
 * and runs first), so they cannot reach a remote peer: every restart of ours
 * goes through lifecycle.
 */
esp_err_t lifecycle_register_shutdown_hook(lifecycle_shutdown_hook_t hook, void *ctx);

/** True the first time `flag` is claimed since power-on (LIFECYCLE_ONCE_*). */
bool lifecycle_claim_once(uint32_t flag);

/**
 * GNSS-ladder reboot cap: allowed only if no LIFECYCLE_REBOOT_GNSS_LADDER
 * reboot happened in the last 60 min and fewer than three in the last 24 h,
 * both counted across software resets (RTC block; a power cycle or an
 * invalid block after an image switch starts a fresh budget). Health asks
 * before requesting that reboot; lifecycle records each one when it runs.
 * @param retry_in_s optional: seconds until a ladder reboot would be allowed (0 if now).
 */
bool lifecycle_ladder_reboot_allowed(uint32_t *retry_in_s);

/** fw_update: a new app image is in its trial (stage 1, stage 2 or the crash watch). */
void lifecycle_set_app_trial(bool active);

/** True while an app trial runs: receiver flashes (including retries) are refused meanwhile. */
bool lifecycle_app_trial_active(void);

/** Which firmware update holds the update slot. */
typedef enum {
    LIFECYCLE_UPDATE_NONE = 0,
    LIFECYCLE_UPDATE_APP,        /**< fw_update: an app image is being downloaded, uploaded or installed */
    LIFECYCLE_UPDATE_RECEIVER,   /**< station: a receiver flash session in full lockout */
} lifecycle_update_t;

/**
 * Claim the update slot. fw_update claims APP before the first image byte is
 * written (download or manual upload) and releases it when the install fails
 * or is abandoned (a successful one reboots). The station claims RECEIVER
 * before the first journal write of a session, and again at boot for an open
 * journal that is not waiting for the user, and releases it together with
 * its reboot inhibit.
 * @return ESP_OK (also when `what` already holds it), ESP_ERR_INVALID_STATE
 *         while the other update holds it, ESP_ERR_INVALID_ARG for NONE.
 */
esp_err_t lifecycle_update_claim(lifecycle_update_t what);

/** Release the update slot if `what` holds it (otherwise nothing changes). */
void lifecycle_update_release(lifecycle_update_t what);

/** The update holding the slot now (NONE if free). */
lifecycle_update_t lifecycle_update_active(void);

#ifdef __cplusplus
}
#endif
