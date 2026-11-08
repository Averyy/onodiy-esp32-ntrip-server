/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_core.h"

#include <string.h>

/* ---- errors -------------------------------------------------------------------------- */

typedef struct {
    const char *code;
    int status;
    const char *message;
} err_row_t;

static const err_row_t s_errors[WEB_ERR_COUNT] = {
    [WEB_ERR_BAD_JSON] = { "bad_json", 400, "The request body is not valid JSON" },
    [WEB_ERR_INVALID_FIELD] = { "invalid_field", 400, "A field has an invalid value" },
    [WEB_ERR_CONFIRM_REQUIRED] = { "confirm_required", 400, "This action needs {\"confirm\": true}" },
    [WEB_ERR_BAD_REQUEST] = { "bad_request", 400, "Bad request" },
    [WEB_ERR_INVALID_IMAGE] = { "invalid_image", 400, "Not a valid app image for this chip" },
    [WEB_ERR_SIZE_MISMATCH] = { "size_mismatch", 400, "Fewer bytes arrived than announced" },
    [WEB_ERR_UNAUTHORIZED] = { "unauthorized", 401, "Login first" },
    [WEB_ERR_FORBIDDEN] = { "forbidden", 403, "Request refused" },
    [WEB_ERR_WRONG_PASSWORD] = { "wrong_password", 403, "Wrong password" },
    [WEB_ERR_NOT_ON_SETUP_AP] = { "not_on_setup_ap", 403, "The first password can only be set over the station's own Wi-Fi" },
    [WEB_ERR_NOT_FOUND] = { "not_found", 404, "Not found" },
    [WEB_ERR_NOTHING_STAGED] = { "nothing_staged", 404, "No uploaded receiver package is waiting" },
    [WEB_ERR_NO_JOURNAL] = { "no_journal", 404, "No receiver update session is open" },
    [WEB_ERR_NO_COREDUMP] = { "no_coredump", 404, "No core dump stored" },
    [WEB_ERR_NOTHING_PENDING] = { "nothing_pending", 404, "No Wi-Fi credentials are pending" },
    [WEB_ERR_METHOD_NOT_ALLOWED] = { "method_not_allowed", 405, "Method not allowed" },
    [WEB_ERR_BUSY] = { "busy", 409, "Another operation is running" },
    [WEB_ERR_JOURNAL_OPEN] = { "journal_open", 409, "A receiver update session is open" },
    [WEB_ERR_NOT_WAITING] = { "not_waiting", 409, "The receiver update session is still running" },
    [WEB_ERR_APP_TRIAL] = { "app_trial", 409, "The new app version is still being checked" },
    [WEB_ERR_NO_APP_CHECK] = { "no_app_check", 409, "No first check of a new app version runs" },
    [WEB_ERR_APP_UPDATE] = { "app_update", 409, "An app update is running" },
    [WEB_ERR_SAFE_MODE] = { "safe_mode", 409, "Not available in safe mode" },
    [WEB_ERR_NOT_IN_SAFE_MODE] = { "not_in_safe_mode", 409, "The station is not in safe mode" },
    [WEB_ERR_RECEIVER_NOT_STREAMING] = { "receiver_not_streaming", 409, "The receiver is not streaming" },
    [WEB_ERR_UNSUPPORTED_MODEL] = { "unsupported_model", 409, "This receiver model is not supported" },
    [WEB_ERR_NEEDS_FIRMWARE] = { "needs_firmware", 409, "This receiver firmware does not have that command" },
    [WEB_ERR_PACKAGE_PINNED] = { "package_pinned", 409, "A receiver package is held by an open update session" },
    [WEB_ERR_MANIFEST_CHANGED] = { "manifest_changed", 409, "The offered update changed" },
    [WEB_ERR_NOT_OFFERED] = { "not_offered", 409, "This version is not offered" },
    [WEB_ERR_ROLLED_BACK_ACK_REQUIRED] = { "rolled_back_ack_required", 409, "This version was rolled back here before" },
    [WEB_ERR_REBOOT_INHIBITED] = { "reboot_inhibited", 409, "A reboot is held off right now" },
    [WEB_ERR_SETTINGS_UNAVAILABLE] = { "settings_unavailable", 409, "Settings cannot be stored this boot" },
    [WEB_ERR_NO_POSITION_YET] = { "no_position_yet", 409, "The receiver has not reported a position yet" },
    [WEB_ERR_CASTER_POLICY] = { "caster_policy", 409, "This caster combination is not allowed" },
    [WEB_ERR_SHA256_MISMATCH] = { "sha256_mismatch", 409, "The SHA-256 does not match the uploaded package" },
    [WEB_ERR_DOWNGRADE] = { "downgrade", 409, "The package is older than the receiver's firmware" },
    [WEB_ERR_NO_ROLLBACK_IMAGE] = { "no_rollback_image", 409, "No previous app image to go back to" },
    [WEB_ERR_ADMIN_ALREADY_SET] = { "admin_already_set", 409, "An admin password is already set" },
    [WEB_ERR_ADMIN_NOT_SET] = { "admin_not_set", 409, "No admin password set yet" },
    [WEB_ERR_RECEIVER_MAINTENANCE] = { "receiver_maintenance", 409,
                                       "Receiver maintenance: nothing is sent to the receiver" },
    [WEB_ERR_NOT_IN_MAINTENANCE] = { "not_in_maintenance", 409, "Receiver maintenance is not on" },
    [WEB_ERR_LIVE_VIEW_CLOSED] = { "live_view_closed", 409, "The live view is not open" },
    [WEB_ERR_LENGTH_REQUIRED] = { "length_required", 411, "Content-Length required" },
    [WEB_ERR_TOO_LARGE] = { "too_large", 413, "Request body too large" },
    [WEB_ERR_UNSUPPORTED_MEDIA_TYPE] = { "unsupported_media_type", 415, "Unsupported content type" },
    [WEB_ERR_BAD_HOST] = { "bad_host", 421, "Unknown host name" },
    [WEB_ERR_NOT_THIS_PROJECT] = { "not_this_project", 422, "The image belongs to another project" },
    [WEB_ERR_RATE_LIMITED] = { "rate_limited", 429, "Too many attempts, try again later" },
    [WEB_ERR_INTERNAL] = { "internal", 500, "Internal error" },
    [WEB_ERR_NOT_AVAILABLE_YET] = { "not_available_yet", 501, "Not available in this firmware version yet" },
    [WEB_ERR_CHECKS_DISABLED] = { "checks_disabled", 501, "This build cannot check for updates" },
    [WEB_ERR_TOO_MANY_CLIENTS] = { "too_many_clients", 503, "Too many live connections" },
    [WEB_ERR_NOT_NO_RECEIVER] = { "not_no_receiver", 409, "The station is not without a receiver: nothing to search for" },
    [WEB_ERR_SAME_IMAGE] = { "same_image", 409, "This image is the one already running (send X-Reinstall: 1 to install it again)" },
    [WEB_ERR_SLOT_DISABLED] = { "slot_disabled", 409, "The caster slot is not enabled" },
    [WEB_ERR_SIGNAL_TEST_RUNNING] = { "signal_test_running", 409,
                                      "A signal-group comparison runs: stop it first" },
    [WEB_ERR_SIGNAL_TEST_IDLE] = { "signal_test_idle", 409, "No signal-group comparison runs" },
    [WEB_ERR_SIGNAL_TEST_NOT_DECIDING] = { "signal_test_not_deciding", 409,
                                           "The comparison is not waiting for your choice yet" },
    [WEB_ERR_FIRST_APPLY_HELD] = { "first_apply_held", 409,
                                   "The receiver's first configuration waits for you: apply it first" },
    [WEB_ERR_AUTOCFG_OFF] = { "autocfg_off", 409, "Automatic receiver configuration is off" },
    [WEB_ERR_IDENTITY_UNCONFIRMED] = { "identity_unconfirmed", 409, "The receiver has not identified itself yet" },
    [WEB_ERR_CONFIG_FAILED] = { "config_failed", 409, "The last receiver configuration failed" },
    [WEB_ERR_POSITION_NOT_LOCKED] = { "position_not_locked", 409,
                                      "Lock the position first: every receiver restart would survey again" },
    [WEB_ERR_NO_RECEIVER] = { "no_receiver", 409, "No receiver is connected" },
    [WEB_ERR_PACKAGE_NEEDED] = { "package_needed", 409,
                                 "Upload the same receiver package again: the station kept it only until the restart" },
    [WEB_ERR_QUERY_RUNNING] = { "query_running", 409, "A receiver query runs: wait for its answer" },
    [WEB_ERR_REPORT_NOT_FAILED] = { "report_not_failed", 409,
                                     "Refresh is available after a failed Onocoy report check" },
    [WEB_ERR_NOT_CANCELLABLE] = { "not_cancellable", 409,
                                   "No download runs that can be cancelled (none, or its last byte is in)" },
};

const char *web_err_code(web_err_t e)
{
    return (unsigned)e < WEB_ERR_COUNT ? s_errors[e].code : "internal";
}

const char *web_err_message(web_err_t e)
{
    return (unsigned)e < WEB_ERR_COUNT ? s_errors[e].message : "Internal error";
}

int web_err_status(web_err_t e)
{
    return (unsigned)e < WEB_ERR_COUNT ? s_errors[e].status : 500;
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

uint32_t web_expected_build(const char *fw)
{
    if (fw == NULL || strlen(fw) > 47 || !web_no_ctrl(fw)) {
        return 0;
    }
    const char *b = strstr(fw, "Build");
    if (b == NULL) {
        return 0;
    }
    b += 5;
    uint64_t v = 0;
    int digits = 0;
    while (is_digit(*b) && digits < 10) {
        v = v * 10u + (uint64_t)(*b - '0');
        b++;
        digits++;
    }
    if (digits == 0 || v == 0 || v > UINT32_MAX) {
        return 0;
    }
    return (uint32_t)v;
}

bool web_maint_duration_ok(uint32_t s)
{
    static const uint32_t choices[] = WEB_MAINT_CHOICES_S;
    for (size_t i = 0; i < sizeof(choices) / sizeof(choices[0]); i++) {
        if (choices[i] == s) {
            return true;
        }
    }
    return false;
}

const char *web_policy_reason(ntrip_policy_t p)
{
    switch (p) {
    case NTRIP_POLICY_SECOND_ONOCOY:
        return "second_onocoy";
    case NTRIP_POLICY_SECOND_RTK2GO:
        return "second_rtk2go";
    case NTRIP_POLICY_NEED_DUP_ACK:
        return "dup_stream_ack_required";
    default:
        return NULL;
    }
}

web_err_t web_app_upload_verdict(web_image_check_t chk, bool rolled_back, bool rolled_back_ack, bool same_image,
                                 bool reinstall)
{
    switch (chk) {
    case WEB_IMAGE_OK:
        if (same_image && !reinstall) {
            return WEB_ERR_SAME_IMAGE;
        }
        return (rolled_back && !rolled_back_ack) ? WEB_ERR_ROLLED_BACK_ACK_REQUIRED : WEB_ERR_COUNT;
    case WEB_IMAGE_OTHER_PROJECT:
        return WEB_ERR_NOT_THIS_PROJECT;
    default:
        return WEB_ERR_INVALID_IMAGE;
    }
}

const char *web_app_refusal_result(web_err_t verdict)
{
    switch (verdict) {
    case WEB_ERR_SAME_IMAGE:
        return "same_image";
    case WEB_ERR_ROLLED_BACK_ACK_REQUIRED:
        return "rolled_back_ack_required";
    default:
        return "invalid_image";
    }
}
