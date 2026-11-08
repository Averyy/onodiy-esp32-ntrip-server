/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "web_shell_core.h"
#include "ntrip_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- errors ------------------------------------------------------------------------ */

/** Every error code of the API, in the order of its table (HTTP status by code). */
typedef enum {
    WEB_ERR_BAD_JSON = 0,
    WEB_ERR_INVALID_FIELD,
    WEB_ERR_CONFIRM_REQUIRED,
    WEB_ERR_BAD_REQUEST,
    WEB_ERR_INVALID_IMAGE,
    WEB_ERR_SIZE_MISMATCH,
    WEB_ERR_UNAUTHORIZED,
    WEB_ERR_FORBIDDEN,
    WEB_ERR_WRONG_PASSWORD,
    WEB_ERR_NOT_ON_SETUP_AP,
    WEB_ERR_NOT_FOUND,
    WEB_ERR_NOTHING_STAGED,
    WEB_ERR_NO_JOURNAL,
    WEB_ERR_NO_COREDUMP,
    WEB_ERR_NOTHING_PENDING,
    WEB_ERR_METHOD_NOT_ALLOWED,
    WEB_ERR_BUSY,
    WEB_ERR_JOURNAL_OPEN,
    WEB_ERR_NOT_WAITING,
    WEB_ERR_APP_TRIAL,
    WEB_ERR_NO_APP_CHECK,
    WEB_ERR_APP_UPDATE,
    WEB_ERR_SAFE_MODE,
    WEB_ERR_NOT_IN_SAFE_MODE,
    WEB_ERR_RECEIVER_NOT_STREAMING,
    WEB_ERR_UNSUPPORTED_MODEL,
    WEB_ERR_NEEDS_FIRMWARE,
    WEB_ERR_PACKAGE_PINNED,
    WEB_ERR_MANIFEST_CHANGED,
    WEB_ERR_NOT_OFFERED,
    WEB_ERR_ROLLED_BACK_ACK_REQUIRED,
    WEB_ERR_REBOOT_INHIBITED,
    WEB_ERR_SETTINGS_UNAVAILABLE,
    WEB_ERR_NO_POSITION_YET,
    WEB_ERR_CASTER_POLICY,
    WEB_ERR_SHA256_MISMATCH,
    WEB_ERR_DOWNGRADE,
    WEB_ERR_NO_ROLLBACK_IMAGE,
    WEB_ERR_ADMIN_ALREADY_SET,
    WEB_ERR_ADMIN_NOT_SET,
    WEB_ERR_RECEIVER_MAINTENANCE,
    WEB_ERR_NOT_IN_MAINTENANCE,
    WEB_ERR_LIVE_VIEW_CLOSED,
    WEB_ERR_LENGTH_REQUIRED,
    WEB_ERR_TOO_LARGE,
    WEB_ERR_UNSUPPORTED_MEDIA_TYPE,
    WEB_ERR_BAD_HOST,
    WEB_ERR_NOT_THIS_PROJECT,
    WEB_ERR_RATE_LIMITED,
    WEB_ERR_INTERNAL,
    WEB_ERR_NOT_AVAILABLE_YET,
    WEB_ERR_CHECKS_DISABLED,
    WEB_ERR_TOO_MANY_CLIENTS,
    WEB_ERR_NOT_NO_RECEIVER,
    WEB_ERR_SAME_IMAGE,
    WEB_ERR_SLOT_DISABLED,
    WEB_ERR_SIGNAL_TEST_RUNNING,
    WEB_ERR_SIGNAL_TEST_IDLE,
    WEB_ERR_SIGNAL_TEST_NOT_DECIDING,
    WEB_ERR_FIRST_APPLY_HELD,
    WEB_ERR_AUTOCFG_OFF,
    WEB_ERR_IDENTITY_UNCONFIRMED,
    WEB_ERR_CONFIG_FAILED,
    WEB_ERR_POSITION_NOT_LOCKED,
    WEB_ERR_NO_RECEIVER,
    WEB_ERR_PACKAGE_NEEDED,
    WEB_ERR_QUERY_RUNNING,
    WEB_ERR_REPORT_NOT_FAILED,
    WEB_ERR_NOT_CANCELLABLE,
    WEB_ERR_COUNT,
} web_err_t;

/** Machine-readable code ("journal_open"). */
const char *web_err_code(web_err_t e);
/** English fallback text for logs and the UI. */
const char *web_err_message(web_err_t e);
/** HTTP status (409, ...). */
int web_err_status(web_err_t e);

/**
 * Receiver firmware string the user expects: <= 47 characters, no control
 * characters, containing "Build" followed by digits. @return the build
 * number, 0 if the string is not usable.
 */
uint32_t web_expected_build(const char *fw);

/**
 * Receiver maintenance durations the station accepts (s; station.h), the
 * default (a request without a duration) and the most time left after an
 * extension.
 */
#define WEB_MAINT_CHOICES_S  { 900u, 1800u, 3600u, 7200u, 14400u }
#define WEB_MAINT_DEFAULT_S  1800u
#define WEB_MAINT_MAX_LEFT_S 14400u

/** One of WEB_MAINT_CHOICES_S. */
bool web_maint_duration_ok(uint32_t s);

/* ---- caster policy ------------------------------------------------------------------------ */

/**
 * API name of the cross-slot rule ntrip_server_set_config() reported (the
 * `reason` of `409 caster_policy`): "second_onocoy", "second_rtk2go",
 * "dup_stream_ack_required"; NULL for NTRIP_POLICY_OK and unknown values.
 * The rules themselves live in ntrip_server only.
 */
const char *web_policy_reason(ntrip_policy_t p);

/**
 * What an app upload gets after its first bytes were checked: WEB_ERR_COUNT
 * = go on and install; WEB_ERR_INVALID_IMAGE, WEB_ERR_NOT_THIS_PROJECT;
 * WEB_ERR_SAME_IMAGE for the image already running (`same_image`: its ELF
 * SHA-256 equals the running one) unless the user asked to install it again
 * (`reinstall`, header X-Reinstall: 1); or WEB_ERR_ROLLED_BACK_ACK_REQUIRED
 * for a version that was rolled back on this station (`rolled_back`) unless
 * the user acknowledged it (`?rolled_back_ack=1`), the same warning an
 * install from the banner gives.
 */
web_err_t web_app_upload_verdict(web_image_check_t chk, bool rolled_back, bool rolled_back_ack, bool same_image,
                                 bool reinstall);

/**
 * The result an app upload refused by its verdict (not WEB_ERR_COUNT) is
 * noted with (last_result, the final progress message): "same_image" and
 * "rolled_back_ack_required" wait for the user's answer and say so (nothing
 * failed and nothing changed); anything else is "invalid_image".
 */
const char *web_app_refusal_result(web_err_t verdict);

#ifdef __cplusplus
}
#endif
