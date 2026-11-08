/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "web_shell_core.h"
#include "web_shell_errors.h"
#include "ntrip_policy.h"

#ifdef __cplusplus
extern "C" {
#endif

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
