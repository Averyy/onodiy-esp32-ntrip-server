/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Optional, read-only daily reports from api-hub.onocoy.com. Disabled by
 * default in each caster's configuration. Only a real, enabled Onocoy slot
 * with credentials and explicit opt-in is eligible; test slots never send
 * their credentials to the public API. Downloads use a separate worker,
 * strict HTTPS, no redirects, and never affect the NTRIP session.
 * Reports are cached in RAM, cleared on credential changes, and fetched
 * after boot/enabling, then once per day. Manual refresh is rate limited.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define ONOCOY_REPORT_DAYS 30u
#define ONOCOY_REPORT_REFRESH_S 86400u

enum {
    ONOCOY_REPORT_UPTIME = 1u << 0,
    ONOCOY_REPORT_LATENCY_AVG = 1u << 1,
    ONOCOY_REPORT_LATENCY_MAX = 1u << 2,
    ONOCOY_REPORT_REWARDS = 1u << 3,
};

typedef struct {
    char date[11];
    uint8_t known;
    uint32_t uptime_s;
    double latency_avg;
    double latency_max;
    double rewards;
} onocoy_report_day_t;

typedef enum {
    ONOCOY_REPORT_OFF = 0,
    ONOCOY_REPORT_PENDING,
    ONOCOY_REPORT_WAIT_NETWORK,
    ONOCOY_REPORT_WAIT_TIME,
    ONOCOY_REPORT_OK,
    ONOCOY_REPORT_EMPTY,
    ONOCOY_REPORT_AUTH,
    ONOCOY_REPORT_ERROR,
} onocoy_report_state_t;

typedef struct {
    bool enabled;
    bool busy;
    onocoy_report_state_t state;
    int http_status;
    int64_t checked_unix;   /* last successful response (including an empty array) */
    uint32_t refresh_in_s;
    uint32_t manual_in_s;
    size_t count;
    onocoy_report_day_t days[ONOCOY_REPORT_DAYS]; /* newest first, completed dates only */
} onocoy_report_status_t;

esp_err_t onocoy_report_start(void);
void onocoy_report_get_status(onocoy_report_status_t *out);
esp_err_t onocoy_report_refresh(void);
void onocoy_report_config_changed(void);
