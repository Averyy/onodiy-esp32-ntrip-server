/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Firmware updates from GitHub Releases, for both chips, and everything about
 * which app image runs (install, trial, confirm, revert, rollback).
 *
 * Every update is started by the user. The only automatic part is the
 * update check, once a day: every CONFIG_FW_UPDATE_CHECK_INTERVAL_H (24 h, a
 * build-time constant, not a setting), plus "check now" in the UI. The time
 * of the last successful check is kept in ota/last_check (written once per
 * check), so a reboot does not add a check: the first check of a boot runs
 * 5-15 min after boot (jittered by the MAC) only when the last one is at
 * least 24 h old by the trusted clock, otherwise when it becomes due. Without
 * a trusted clock the first check waits for one, at most 1 h after boot. A
 * check that got no answer (offline, DNS, TLS, timeout, still no trusted
 * clock to check certificate dates with) is retried after 1 h; one that got
 * an answer without a usable manifest (HTTP error, bad or missing signature
 * where one is required, unknown format, a redirect off the repository's
 * releases on GitHub's release hosts) waits for the next daily check. Checks pause
 * while the station is offline. A check fetches and checks the manifest
 * and refreshes the banner state; it never downloads an app image or a
 * receiver package, and never writes an OTA slot or pkg_store. Nothing waits
 * in flash for later. The accepted manifest is kept in RAM for the boot, so
 * after a reboot the banner is empty until the next check (or "check now").
 *
 * Source of truth is a small manifest attached to every release
 * (CONFIG_FW_UPDATE_MANIFEST_ASSET): app version + asset name + size + SHA-256,
 * and receiver package model + version + build + asset name + size + SHA-256.
 * Only the stable channel is read:
 * https://github.com/<repo>/releases/latest/download/<manifest> (a redirect,
 * not the rate-limited REST API); pre-releases are installed by manual
 * upload. Responses are capped at CONFIG_FW_UPDATE_MANIFEST_MAX_KB; the
 * manifest ETag is kept (ota/etag, written only when it changes) and sent
 * as If-None-Match while this boot holds the manifest it belongs to. GitHub's
 * asset redirects point at long signed CDN URLs, so every request uses
 * CONFIG_FW_UPDATE_HTTP_BUF_SIZE for both the receive and the transmit buffer
 * and follows up to CONFIG_FW_UPDATE_MAX_REDIRECTS redirects, each only
 * within the repository's releases on GitHub's release hosts
 * (release_url.h). Assets are downloaded from
 * https://github.com/<repo>/releases/download/<manifest version>/<asset>.
 * The repository is CONFIG_FW_UPDATE_GITHUB_REPO (and must be public: no
 * access token is sent).
 *
 * Banner (web UI, every page, admin session): "App vX / receiver build Y
 * available - Update / Later". It never offers a lower version (downgrades
 * are manual uploads), a version in ota/bad_ver, or anything to a dev build
 * ("+dev": no comparable version). "Later" is browser-side state only.
 *
 * One update at a time, across both chips: an app download, manual upload or
 * install holds lifecycle's update slot (lifecycle_update_claim, APP) from
 * before the first image byte until it fails or the new image reboots; a
 * receiver flash session holds it (RECEIVER) during its full lockout. The
 * claim is atomic, so neither path (banner or manual upload) can start while
 * the other runs.
 *
 * App install (fw_update_install_app, after the user's Update click, or a
 * manual upload, fw_update_upload_app_*): refused while the station holds
 * the update slot (a receiver flash journal open and not waiting for the
 * user), while another update or check runs, and during the running image's
 * own stage 1 or verify window. Claim the slot, take a reboot inhibit, save a
 * baseline of the running station (Wi-Fi had an IP, which caster slots were
 * streaming, hub fed). A release image is streamed into the inactive slot
 * (esp_ota_begin/write); its size and SHA-256 must equal the manifest's on
 * the stream, esp_ota_end validates it, the SHA-256 of what is in flash must
 * equal the manifest's again and its app description must name the
 * manifest's version and this project before it is activated. A manual
 * upload is hashed as it streams in and validated by esp_ota_end (no
 * manifest digest; the SHA-256 is returned for the UI). Then the trial
 * record is written, the inhibit released and
 * lifecycle_request_reboot(APP_UPDATE) reboots into the new image.
 *
 * App trial after an update (two stages; it protects user-initiated updates
 * too, so it stays automatic):
 *  1. Confirm (bootloader rollback armed): the new image runs
 *     PENDING_VERIFY. After CONFIG_FW_UPDATE_CONFIRM_S with init complete and
 *     no reset, it is marked valid. A crash or hang before that, or any reset
 *     at all (the bootloader treats every reset as a failure here), rolls back
 *     to the previous image. The window is kept short for that reason.
 *  2. Verify (app-managed): within CONFIG_FW_UPDATE_VERIFY_WINDOW_S the
 *     station must reach the saved baseline (receiver problems excepted: an
 *     unplugged antenna must not cause rollback loops). Then, for
 *     CONFIG_FW_UPDATE_CRASH_WATCH_H, CONFIG_FW_UPDATE_CRASH_REVERT_COUNT
 *     crash resets revert it. A failed verify or crash watch marks the
 *     running image invalid (esp_ota_mark_app_invalid_rollback) and reboots
 *     into the previous one. Trial state is persisted (ota/trial), so a power
 *     blip during stage 2 just continues the trial. A reboot inhibit is held
 *     for the verify window.
 * While a trial runs (stage 1, stage 2 or the crash watch), the trial flag
 * in lifecycle is set and receiver flashes are refused; the banner says when
 * the receiver update becomes possible.
 *
 * Rollback records: a version reverted by stage 2 or rolled back manually
 * goes to the rolled-back list (ota/bad_ver: the last
 * FW_UPDATE_ROLLED_BACK_MAX versions, oldest dropped first, written only
 * when a version is added, kept through a factory reset) and is never
 * offered by the banner again, however many releases later it is checked
 * (the user can still install it, with a warning:
 * fw_update_get_rolled_back, fw_update_is_rolled_back). A stage-1 rollback
 * (crash or reset in the first minute; a power blip and a real crash look
 * the same to the bootloader) is not retried automatically: the banner says
 * the update was rolled back during its first minute and offers to install
 * it again; if that one rolls back too, the version goes on the list.
 *
 * Authenticity: a release is trusted because of where it comes from. Every
 * transfer is HTTPS with the server certificate checked against the CA
 * bundle and every certificate's dates against the trusted clock, and goes
 * only to GitHub's release hosts, on github.com only to the release paths
 * of the repository set at build time (CONFIG_FW_UPDATE_GITHUB_REPO), for
 * the first URL and every redirect. The manifest names every file with its
 * size and SHA-256, and each downloaded app image and receiver package must
 * match both before it is used. No signing key is needed: without
 * CONFIG_FW_UPDATE_MANIFEST_KEY_1/_2 the plain manifest (or a signed
 * envelope, its signature unchecked) is accepted. A build that configures a
 * key (e.g. a fork that signs its releases; two for rotation) accepts only a
 * manifest signed with it, verified through PSA before any field is used,
 * refuses unsigned ones, and may then also take its repository from
 * ota/repo. Format and rules: manifest.h. No secure boot or app-image
 * signing (a self-built image can always be uploaded by hand).
 *
 * Receiver (fw_update_install_gnss, after the user confirmed the dialog:
 * version, expected downtime against today's remaining Onocoy budget, "do
 * not power off or plug into the receiver's USB port", "the station profile
 * will be re-applied"): download the manifest's package for the detected
 * model into pkg_store (size and SHA-256 from the manifest, checked on the
 * stream and again on the flash copy), then hand it to the station
 * (station_request_flash), which validates the RAM copy and flashes. Refused during an app trial, while an app update runs, in safe
 * mode, with a flash journal open, unless the station is streaming (valid
 * RTCM or NMEA within the last 10 s), for an unsupported model, or while a
 * package is pinned. The package slot is released when the session ends; a
 * failed validation releases it at once, and so does a station that does
 * not take the request.
 *
 * Safe mode: the update check, the banner and user-initiated app installs
 * keep working (a fix must be able to arrive); receiver updates are off
 * because the station does not run there (see station.h).
 *
 * All transfers are HTTPS with the CA bundle; plain HTTP is never followed,
 * and neither is a redirect outside the repository's releases on GitHub's
 * release hosts or one carrying user info (ESP_ERR_NOT_ALLOWED as the job's
 * error).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(FW_UPDATE_EVENT);

/** FW_UPDATE_EVENT ids; data is fw_update_state_t. */
typedef enum {
    FW_UPDATE_EVENT_STATE_CHANGED,
} fw_update_event_t;

typedef enum {
    FW_UPDATE_IDLE = 0,
    FW_UPDATE_CHECKING,
    FW_UPDATE_DOWNLOADING_APP,
    FW_UPDATE_DOWNLOADING_GNSS,
    FW_UPDATE_REBOOT_PENDING,   /**< new app activated (or rolled back), reboot scheduled */
    FW_UPDATE_FAILED,           /**< the last check or install failed: last_err */
    FW_UPDATE_UPLOADING_APP,    /**< a manual app upload is being written */
} fw_update_state_t;

/** Where the running image is in its post-update trial. */
typedef enum {
    FW_TRIAL_NONE = 0,          /**< confirmed and past the crash watch (or never updated) */
    FW_TRIAL_CONFIRMING,        /**< stage 1: bootloader rollback armed */
    FW_TRIAL_VERIFYING,         /**< stage 2: must reach the baseline */
    FW_TRIAL_WATCHING,          /**< stage 2: crash watch */
} fw_update_trial_t;

/** Why a receiver update cannot start now (banner and dialog). */
typedef enum {
    FW_GNSS_BLOCK_NONE = 0,
    FW_GNSS_BLOCK_APP_TRIAL,          /**< an app trial runs; gnss_blocked_left_s says until when */
    FW_GNSS_BLOCK_JOURNAL_OPEN,       /**< a receiver flash session is open */
    FW_GNSS_BLOCK_UNSUPPORTED_MODEL,  /**< the detected receiver is not one we flash */
    FW_GNSS_BLOCK_SAFE_MODE,
    FW_GNSS_BLOCK_APP_UPDATE,         /**< an app download, upload or install runs (one update at a time) */
    FW_GNSS_BLOCK_RECEIVER_NOT_STREAMING, /**< no valid RTCM or NMEA from the receiver in the last 10 s */
} fw_update_gnss_block_t;

/** How the last update check went (status, web UI). */
typedef enum {
    FW_CHECK_RESULT_NONE = 0,         /**< no check attempted this boot */
    FW_CHECK_RESULT_OK,               /**< a usable manifest, or "not modified" */
    FW_CHECK_RESULT_NO_RELEASE,       /**< HTTP 404: the repository has no (public) release with a manifest */
    FW_CHECK_RESULT_HTTP_ERROR,       /**< another HTTP status: last_http_status */
    FW_CHECK_RESULT_NO_RESPONSE,      /**< no answer (offline, DNS, TLS, timeout, no trusted clock yet) */
    FW_CHECK_RESULT_UNTRUSTED_REDIRECT, /**< GitHub redirected somewhere this firmware does not trust */
    FW_CHECK_RESULT_BAD_MANIFEST,     /**< an answer, but no manifest this build accepts */
} fw_update_check_result_t;

/** Versions kept on the rolled-back list (ota/bad_ver). */
#define FW_UPDATE_ROLLED_BACK_MAX 8

/** Buffer for "owner/name" (GitHub: owner up to 39, name up to 100 characters). */
#define FW_UPDATE_REPO_MAX 144

/** Status snapshot for the web UI (also the banner fields of /api/status). */
typedef struct {
    fw_update_state_t state;
    char running_version[32];
    fw_update_trial_t trial;
    uint32_t trial_left_s;         /**< remaining time of the current trial stage */
    char reverted_version[32];     /**< last version rolled back, "" if none (the whole list: fw_update_get_rolled_back) */
    bool reverted_in_stage1;       /**< it rolled back in its first minute (crash or reset): offer it again */
    bool rollback_available;       /**< the other slot holds a valid image */
    char latest_version[32];       /**< "" until a check succeeded */
    bool app_update_available;     /**< banner: newer than running, not in bad_ver, running is not a dev build */
    bool latest_rolled_back;       /**< latest_version is in ota/bad_ver: not offered, installable with a warning */
    char gnss_running[48];         /**< receiver firmware as detected */
    char gnss_latest[48];
    uint32_t gnss_latest_build;
    bool gnss_update_available;    /**< banner: a higher build for the detected model, running is not a dev build */
    fw_update_gnss_block_t gnss_blocked;
    uint32_t gnss_blocked_left_s;  /**< for FW_GNSS_BLOCK_APP_TRIAL: seconds until the trial ends */
    uint32_t gnss_expected_downtime_s; /**< for the confirmation dialog (never a gate) */
    uint8_t progress_pct;          /**< current download */
    int64_t last_check_unix;       /**< last successful check (ota/last_check), 0 = never */
    int64_t last_attempt_unix;     /**< last check attempt this boot, successful or not; 0 = none or no trusted clock then */
    fw_update_check_result_t last_check_result; /**< how that attempt went (FW_CHECK_RESULT_NONE = none this boot) */
    uint16_t last_http_status;     /**< its final HTTP status, 0 = no answer */
    uint32_t next_check_in_s;      /**< until the next automatic check is due (0 = due now, it waits for the network),
                                        UINT32_MAX = checks are off in this build */
    esp_err_t last_err;
} fw_update_status_t;

/**
 * Boot-time image bookkeeping (early in init, right after settings and
 * lifecycle): note a rollback that happened during the previous boot
 * (esp_ota_get_last_invalid_partition), count trial crashes, revert a failing
 * trial image, publish the trial flag to lifecycle.
 */
esp_err_t fw_update_init(void);

/** Start the worker task, the trial timers and the daily check (due from ota/last_check). */
esp_err_t fw_update_start(void);

/**
 * Update checks can run in this build: CONFIG_FW_UPDATE_GITHUB_REPO and
 * CONFIG_FW_UPDATE_MANIFEST_ASSET are valid, and when a manifest key is
 * configured at least one of them parses (a build that requires signatures
 * never falls back to unsigned manifests). False before fw_update_init.
 */
bool fw_update_checks_enabled(void);

/** This build has a manifest key: only signed manifests are accepted. */
bool fw_update_signature_required(void);

/** "owner/name" releases are read from (fixed at fw_update_init; "" before). */
const char *fw_update_release_repo(void);

/**
 * Fetch the manifest and refresh the banner state now (async). Never
 * downloads an image or package.
 * @return ESP_OK (also when a check is already running),
 *         ESP_ERR_NOT_SUPPORTED when fw_update_checks_enabled() is false,
 *         ESP_ERR_INVALID_STATE while an install runs or the new image's
 *         first minute is not over.
 */
esp_err_t fw_update_check(void);

/**
 * The user clicked Update: claim the update slot, download, verify, install,
 * reboot (async).
 * @param offered_version the version the banner showed; refused
 *        (ESP_ERR_INVALID_STATE) if the manifest changed since, or if it is
 *        not newer than the running release (a dev build may install any
 *        release; a rolled-back version is allowed, the UI warns).
 * @return ESP_ERR_INVALID_STATE also while a receiver flash holds the update
 *         slot (its full lockout), while another update or check runs, and
 *         during the running image's stage 1 or verify window.
 */
esp_err_t fw_update_install_app(const char *offered_version);

/** Opaque manual app upload. */
typedef struct fw_update_upload *fw_update_upload_t;

/**
 * Manual app upload (POST /api/upload/app), into the same install flow:
 * claims the update slot and a reboot inhibit before the first byte, saves
 * the trial baseline, opens the inactive slot. Call from a task whose stack
 * is in internal RAM (flash writes).
 * @param size image size from the request (0 = unknown).
 * @return ESP_ERR_INVALID_STATE for the same reasons as
 *         fw_update_install_app (except the version rules: any image may be
 *         uploaded), ESP_ERR_INVALID_SIZE if it cannot fit the slot.
 */
esp_err_t fw_update_upload_app_begin(uint32_t size, fw_update_upload_t *out);

/** Append image bytes. */
esp_err_t fw_update_upload_app_write(fw_update_upload_t u, const void *data, size_t len);

/**
 * Validate (esp_ota_end), activate, record the trial and reboot into the
 * new image (async). The upload is closed in every case.
 * @param sha256_out optional: SHA-256 of the received image, for the UI.
 * @return ESP_ERR_OTA_VALIDATE_FAILED and friends for an image that is not
 *         a valid app for this chip; ESP_ERR_INVALID_SIZE if fewer bytes than
 *         announced arrived.
 */
esp_err_t fw_update_upload_app_finish(fw_update_upload_t u, uint8_t sha256_out[32]);

/** Abandon an upload (client gone, cancelled): releases the slot and the inhibit. */
void fw_update_upload_app_abort(fw_update_upload_t u);

/**
 * True if an app image with this app ELF SHA-256 (esp_app_desc_t
 * app_elf_sha256) is the code running now. The web refuses such an upload
 * unless the user asked to install it again; fw_update_upload_app_finish()
 * gives the identical image a stage-1-only trial (no verify window, no crash
 * watch: receiver updates are not blocked for the watch's day).
 */
bool fw_update_same_as_running(const uint8_t elf_sha256[32]);

/**
 * The user confirmed the receiver update: download the package for the
 * detected model into pkg_store and hand it to the station (async). The
 * package slot is released when the session ends. Refused
 * (ESP_ERR_INVALID_STATE) for every fw_update_gnss_block_t reason.
 * @param offered_build    the build the banner showed; refused if it changed
 *        or if nothing is offered (a dev build is offered nothing).
 * @param cross_layout_ack the user confirmed a package with a different
 *        partition layout (still refused until bench-tested).
 */
esp_err_t fw_update_install_gnss(uint32_t offered_build, bool cross_layout_ack);

/**
 * Manual rollback: mark the running image invalid, record its version in
 * ota/bad_ver, reboot into the other slot (async, via the lifecycle reboot
 * service). ESP_ERR_NOT_FOUND if the other slot holds no valid image;
 * ESP_ERR_INVALID_STATE while a reboot is inhibited (the trial's own
 * inhibit does not count), while an update runs or a receiver flash holds
 * the update slot.
 */
esp_err_t fw_update_rollback(void);

/** Status snapshot. */
esp_err_t fw_update_get_status(fw_update_status_t *out);

/**
 * The versions rolled back on this station, newest first: reverted by their
 * trial, rolled back by the user, or rolled back twice in their first
 * minute (the rolled-back list, ota/bad_ver). The banner offers none of
 * them; the UI shows the list and asks for an acknowledgement before
 * installing one (banner or manual upload).
 * @return entries written (at most cap; 0 before fw_update_init).
 */
size_t fw_update_get_rolled_back(char out[][32], size_t cap);

/** True if `version` (exact string) is on the rolled-back list. */
bool fw_update_is_rolled_back(const char *version);

/**
 * Compare two version strings "vMAJOR.MINOR.PATCH[-pre]" (leading 'v'
 * optional; pre-releases sort before the release).
 *
 * @param out <0, 0 or >0 like strcmp.
 * @return ESP_ERR_INVALID_ARG if either string is not a version (e.g. a dev
 *         build "v0.3.0-4-g47fc47b-dirty+dev"; dev builds are never offered
 *         updates).
 */
esp_err_t fw_update_version_compare(const char *a, const char *b, int *out);

#ifdef __cplusplus
}
#endif
