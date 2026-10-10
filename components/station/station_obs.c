/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "station_obs.h"

#include <string.h>

#include "gnss_rtcm3.h"

typedef enum {
    CLASS_UNCHECKED = 0,
    CLASS_MSM_GLOBAL,     /**< always required at its rate */
    CLASS_MSM_REGIONAL,   /**< not required when absent; never faster than its rate */
    CLASS_STATION,        /**< required within max(2 x period, 30 s), except while surveying */
    CLASS_BIASES,         /**< 1230: like a station message, only when the receiver accepted it */
} msg_class_t;

static msg_class_t classify(uint16_t msg)
{
    gnss_sys_t sys;
    if (gnss_rtcm3_msm_type(msg, &sys, NULL)) {
        switch (sys) {
        case GNSS_SYS_GPS:
        case GNSS_SYS_GLONASS:
        case GNSS_SYS_GALILEO:
        case GNSS_SYS_BEIDOU:
            return CLASS_MSM_GLOBAL;
        default:
            return CLASS_MSM_REGIONAL;
        }
    }
    switch (msg) {
    case 1005:
    case 1006:
    case 1033:
        return CLASS_STATION;
    case 1230:
        return CLASS_BIASES;
    default:
        return CLASS_UNCHECKED;
    }
}

void stn_obs_clear(stn_obs_t *o)
{
    memset(o, 0, sizeof(*o));
    o->armed_ms = STN_NEVER;
    o->first_ms = STN_NEVER;
    for (unsigned i = 0; i < STN_OBS_MAX; i++) {
        o->last_ms[i] = STN_NEVER;
    }
}

void stn_obs_arm(stn_obs_t *o, const um98x_rtcm_out_t *rtcm, uint8_t count, int64_t now_ms)
{
    stn_obs_clear(o);
    if (count > STN_OBS_MAX) {
        count = STN_OBS_MAX;
    }
    for (uint8_t i = 0; i < count; i++) {
        o->msg[i] = rtcm[i].msg;
        o->period_s[i] = rtcm[i].period_s;
    }
    o->count = count;
    o->armed = true;
    o->armed_ms = now_ms;
}

void stn_obs_feed(stn_obs_t *o, uint16_t msg, int64_t now_ms)
{
    if (!o->armed) {
        return;
    }
    if (o->first_ms == STN_NEVER) {
        o->first_ms = now_ms;
    }
    if (o->frames < UINT32_MAX) {
        o->frames++;
    }
    for (uint8_t i = 0; i < o->count; i++) {
        if (o->msg[i] == msg) {
            if (o->seen[i] < UINT32_MAX) {
                o->seen[i]++;
            }
            o->last_ms[i] = now_ms;
            return;
        }
    }
}

int64_t stn_obs_window_ms(const stn_obs_t *o)
{
    int64_t w = STN_OBS_WINDOW_MS;
    for (uint8_t i = 0; i < o->count; i++) {
        if (classify(o->msg[i]) == CLASS_UNCHECKED) {
            continue;
        }
        int64_t twice = (int64_t)o->period_s[i] * 2000;
        if (twice > w) {
            w = twice;
        }
    }
    return w;
}

/* At least half the count the period gives over the window, at most one and
 * a half times (plus one) the count over the span actually observed (an
 * evaluation that comes late has seen more frames). */
static bool msm_rate_ok(uint32_t seen, uint16_t period_s, int64_t window_ms, int64_t span_ms, bool need_min)
{
    int64_t period_ms = (int64_t)(period_s > 0 ? period_s : 1) * 1000;
    int64_t min = (window_ms / period_ms) / 2;
    if (min < 1) {
        min = 1;
    }
    int64_t expected = span_ms / period_ms;
    int64_t max = expected + expected / 2 + 1;
    if (need_min && (int64_t)seen < min) {
        return false;
    }
    return (int64_t)seen <= max;
}

static bool station_msg_ok(int64_t last_ms, uint16_t period_s, int64_t now_ms)
{
    if (last_ms == STN_NEVER) {
        return false;
    }
    int64_t due = (int64_t)period_s * 2000;
    if (due < STN_OBS_STATION_MIN_MS) {
        due = STN_OBS_STATION_MIN_MS;
    }
    return now_ms - last_ms <= due;
}

stn_obs_verdict_t stn_obs_eval(const stn_obs_t *o, const stn_obs_opts_t *opts, int64_t now_ms,
                               uint16_t *first_bad)
{
    if (first_bad != NULL) {
        *first_bad = 0;
    }
    if (!o->armed) {
        return STN_OBS_PENDING;
    }
    int64_t w = stn_obs_window_ms(o);
    if (o->first_ms == STN_NEVER) {
        return (now_ms - o->armed_ms >= w) ? STN_OBS_NO_RTCM : STN_OBS_PENDING;
    }
    if (now_ms - o->first_ms < w) {
        return STN_OBS_PENDING;
    }
    int64_t span = now_ms - o->first_ms;
    for (uint8_t i = 0; i < o->count; i++) {
        bool ok = true;
        switch (classify(o->msg[i])) {
        case CLASS_MSM_GLOBAL:
            ok = msm_rate_ok(o->seen[i], o->period_s[i], w, span, true);
            break;
        case CLASS_MSM_REGIONAL:
            ok = o->seen[i] == 0 || msm_rate_ok(o->seen[i], o->period_s[i], w, span, false);
            break;
        case CLASS_STATION:
            ok = opts->surveying || station_msg_ok(o->last_ms[i], o->period_s[i], now_ms);
            break;
        case CLASS_BIASES:
            ok = !opts->rtcm1230_ok || station_msg_ok(o->last_ms[i], o->period_s[i], now_ms);
            break;
        default:
            break;
        }
        if (!ok) {
            if (first_bad != NULL) {
                *first_bad = o->msg[i];
            }
            return STN_OBS_DRIFT;
        }
    }
    return STN_OBS_PASS;
}
