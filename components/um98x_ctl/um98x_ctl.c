/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * PROVISIONAL stub. Detection and the command/reply protocol (Unicore N4
 * command set, public manual) are still to be implemented and confirmed on
 * hardware. Profile defaults and persistence live in um98x_profile.c.
 */
#include "um98x_ctl.h"

#include <string.h>

#include "esp_log.h"

static const char *TAG = "um98x_ctl";

esp_err_t um98x_ctl_init(void)
{
    ESP_LOGI(TAG, "stub");
    return ESP_OK;
}

void um98x_ctl_on_line(const char *line, size_t len, void *ctx)
{
    (void)line;
    (void)len;
    (void)ctx;
}

esp_err_t um98x_ctl_detect(gnss_link_lease_t lease, um98x_info_t *out)
{
    (void)lease;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_ctl_query_version(um98x_info_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_ctl_get_info(um98x_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_FOUND;
}

esp_err_t um98x_ctl_command(gnss_link_lease_t lease, const char *cmd, char *reply, size_t reply_len,
                            uint32_t timeout_ms)
{
    (void)lease;
    (void)cmd;
    (void)timeout_ms;
    if (reply != NULL && reply_len > 0) {
        reply[0] = '\0';
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_ctl_apply_profile(gnss_link_lease_t lease, const um98x_base_profile_t *profile)
{
    (void)lease;
    if (!um98x_profile_valid(profile)) {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_ctl_get_base_position(um98x_base_position_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    return ESP_ERR_NOT_SUPPORTED;
}
