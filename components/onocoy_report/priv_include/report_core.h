/* SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT */
#pragma once
#include "onocoy_report.h"
#include "ntrip_server.h"

/** Pure eligibility rule: a test slot must never disclose its credential to the public API. */
bool report_eligible(const ntrip_caster_cfg_t *cfg);

/** Complete array, bounded dates [start,end), at most 30 distinct dates, newest first.
 * Missing/null metrics stay unknown; malformed values or duplicate dates fail.
 * Does not publish partial output on failure. */
bool report_parse(const char *json, size_t len, const char *start, const char *end,
                  onocoy_report_day_t *out, size_t *count);

/** Daily on success or a rejected request (4xx); bounded retry for transport/server failures. */
uint32_t report_retry_s(bool success, int http_status, unsigned failures);

/** Successful/empty reports wait for the daily check; only failures allow a manual retry. */
bool report_manual_allowed(onocoy_report_state_t state);
