/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * What the update banner offers, and when the daily check runs (pure logic,
 * no IDF or RTOS includes, injected clock; host-testable).
 *
 * Banner:
 *  - App: offered when the release is newer than the running version, the
 *    running image is a release (a dev build, "+dev", has no comparable
 *    version and is never offered anything) and the release is not in the
 *    rolled-back list. A rolled-back release is reported as such (installable
 *    with a warning) but never offered. A release the bootloader rolled back
 *    during its first minute is offered again, flagged so the banner can say
 *    so; one more such rollback puts it on the list (app_trial.h).
 *  - Receiver: the manifest entry for exactly the detected model whose build
 *    is higher than the receiver's build. Nothing is offered while the model
 *    or the build is unknown. Downgrades are never offered. A dev build is
 *    offered no receiver build either (nothing at all is offered to it), and
 *    the install path asks the same question, so it cannot start one. A
 *    manual package upload is not affected.
 *
 * Daily check (seconds of uptime; the trusted clock is the unix time once
 * SNTP has set it):
 *  - Due CHECK_INTERVAL after the last successful check: by the persisted
 *    time of that check when the clock is trusted (so reboots add no
 *    checks), else by this boot's last successful check.
 *  - Never checked, or a stored time in the future: due as soon as the
 *    clock is trusted. No trusted clock and no check this boot: due
 *    FW_SCHED_NO_CLOCK_WAIT_S after boot at the latest.
 *  - Never before `first_check_s` (5-15 min after boot, from the MAC, so a
 *    fleet that reboots together does not check together).
 *  - A check that got no answer (offline, DNS, TLS, timeout) is retried
 *    after FW_SCHED_RETRY_S. A check that got an answer that held no usable
 *    manifest (HTTP error, bad signature, unknown format) waits a full
 *    interval: retrying sooner would not change the answer.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "manifest.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FW_SCHED_JITTER_MIN_S   300u    /**< first check of a boot: not before 5 min ... */
#define FW_SCHED_JITTER_SPAN_S  600u    /**< ... and not after 15 min (plus the clock rules) */
#define FW_SCHED_NO_CLOCK_WAIT_S 3600u  /**< without a trusted clock, the first check waits at most this long */
#define FW_SCHED_RETRY_S        3600u   /**< retry delay after a check that got no answer */

/** Offer for the running app. */
typedef struct {
    bool available;            /**< banner: offer `latest` */
    bool rolled_back;          /**< `latest` is on the rolled-back list (not offered; installable with a warning) */
    bool retry_after_stage1;   /**< `latest` was rolled back during its first minute: offered again, worded so */
} fw_app_offer_t;

/**
 * @param running          running app version (esp_app_desc_t.version)
 * @param latest           manifest version, "" or NULL if none known
 * @param is_bad           rolled-back list lookup (may be NULL: empty list)
 * @param bad_ctx          passed to is_bad
 * @param stage1_version   the version the bootloader rolled back in its first minute ("" if none)
 */
void fw_policy_app_offer(const char *running, const char *latest, bool (*is_bad)(const char *v, void *ctx),
                         void *bad_ctx, const char *stage1_version, fw_app_offer_t *out);

/**
 * May the user install `offered` from the manifest (the Update click)?
 * Refused when it is not newer than the running release (downgrades and
 * re-installs are manual uploads). A dev build may install any release.
 */
bool fw_policy_app_install_allowed(const char *running, const char *offered);

/**
 * Manifest entry to offer for the receiver: exact model match, build higher
 * than `current_build`; the highest such build if several. NULL if none,
 * if the model is "" or the build is 0 (unknown), or if `running` (the app
 * version) is not a release (a dev build is offered nothing).
 */
const fw_manifest_gnss_t *fw_policy_gnss_offer(const fw_manifest_t *m, const char *running, const char *model,
                                               uint32_t current_build);

/** Outcome of an update check, for the schedule. */
typedef enum {
    FW_CHECK_NONE = 0,       /**< no check attempted this boot */
    FW_CHECK_OK,             /**< a verified manifest (or "not modified") */
    FW_CHECK_UNUSABLE,       /**< an answer without a usable manifest */
    FW_CHECK_NO_RESPONSE,    /**< no answer */
} fw_check_result_t;

typedef struct {
    uint32_t uptime_s;
    bool clock_trusted;
    int64_t now_unix;            /**< valid when clock_trusted */
    int64_t last_check_unix;     /**< persisted time of the last successful check, 0 = never */
    uint32_t interval_s;         /**< CONFIG_FW_UPDATE_CHECK_INTERVAL_H in seconds */
    uint32_t first_check_s;      /**< fw_sched_jitter_s() */
    fw_check_result_t last_result;  /**< of this boot's last attempt */
    uint32_t last_attempt_s;     /**< uptime of that attempt */
    bool have_success;           /**< a check succeeded this boot */
    uint32_t last_success_s;     /**< uptime of this boot's last successful check */
} fw_sched_t;

/** Seconds until the next automatic check is due; 0 = now. */
uint32_t fw_sched_wait_s(const fw_sched_t *s);

/** Earliest uptime of a boot's first check, 300..900 s, derived from the MAC. */
uint32_t fw_sched_jitter_s(const uint8_t mac[6]);

#ifdef __cplusplus
}
#endif
