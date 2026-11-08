/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "web_core.h"

#include <string.h>

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
