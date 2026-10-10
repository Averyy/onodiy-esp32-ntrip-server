/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "um98x_plan.h"

#include <stdio.h>
#include <string.h>

#include "um98x_base.h"
#include "um98x_profile.h"

/* Satellite-in-view output for the sky view: every 10 s, "GP" prefix as the
 * manual writes it. */
#define SAT_VIEW_PERIOD_S 10u

bool um98x_model_supported(const char *model)
{
    return model != NULL && strcmp(model, UM98X_SUPPORTED_MODEL) == 0;
}

bool um98x_signal_group_matches(const um98x_base_profile_t *p, const um98x_readback_t *rb)
{
    return p != NULL && rb != NULL && rb->config_ok && rb->config.signal_group_known &&
           rb->config.signal_group_master == p->signal_group_master &&
           rb->config.signal_group_slave == p->signal_group_slave;
}

bool um98x_antenna_matches(const um98x_base_profile_t *p, const um98x_readback_t *rb)
{
    if (p == NULL) {
        return false;
    }
    size_t n = um98x_antenna_str_len(p->antenna_model);
    if (n == 0) {
        return true;   /* nothing to send */
    }
    if (rb == NULL || !rb->config_ok || !rb->config.rx.antenna_known) {
        return false;
    }
    const um98x_rx_settings_t *rx = &rb->config.rx;
    size_t k = um98x_antenna_str_len(p->antenna_serial);
    return um98x_antenna_str_len(rx->antenna_model) == n && memcmp(rx->antenna_model, p->antenna_model, n) == 0 &&
           um98x_antenna_str_len(rx->antenna_serial) == k && memcmp(rx->antenna_serial, p->antenna_serial, k) == 0 &&
           rx->antenna_setup_id == p->antenna_setup_id;
}

static void add(um98x_plan_t *out, um98x_step_kind_t kind, bool fatal, uint16_t msg, uint16_t period_s)
{
    if (out->count < UM98X_PLAN_MAX) {
        out->step[out->count++] = (um98x_step_t){ (uint8_t)kind, fatal, msg, period_s };
    }
}

static void add_rtcm(const um98x_base_profile_t *p, um98x_plan_t *out)
{
    for (size_t i = 0; i < p->rtcm_count; i++) {
        add(out, UM98X_STEP_RTCM, p->rtcm[i].msg != UM98X_RTCM_OPTIONAL, p->rtcm[i].msg, p->rtcm[i].period_s);
    }
}

bool um98x_plan_build(const um98x_base_profile_t *p, const um98x_readback_t *rb, uint32_t link_baud,
                      bool force_mode, um98x_plan_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!um98x_profile_valid(p) || rb == NULL) {
        return false;
    }
    if (!um98x_signal_group_matches(p, rb)) {
        add(out, UM98X_STEP_SIGNALGROUP, true, 0, 0);
    }
    if (p->com1_baud != link_baud) {
        add(out, UM98X_STEP_BAUD, true, 0, 0);
    }
    if (force_mode || !rb->mode_ok || !um98x_mode_matches(p, rb->mode_body)) {
        add(out, UM98X_STEP_MODE, true, 0, 0);
    }
    if (!um98x_antenna_matches(p, rb)) {
        add(out, UM98X_STEP_ANTENNA, false, 0, 0);
    }
    if (rb->config_ok && rb->config.rx.b1cb2a == UM98X_SETTING_OFF) {
        add(out, UM98X_STEP_B1CB2A, false, 0, 0);
    }
    add_rtcm(p, out);
    if (p->sat_view_output) {
        add(out, UM98X_STEP_SAT_VIEW, false, 0, 0);
    }
    add(out, UM98X_STEP_SAVE, true, 0, 0);
    return true;
}

bool um98x_plan_rtcm_only(const um98x_base_profile_t *p, um98x_plan_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!um98x_profile_valid(p)) {
        return false;
    }
    add_rtcm(p, out);
    return true;
}

size_t um98x_step_command(const um98x_base_profile_t *p, const um98x_step_t *s, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    if (p == NULL || s == NULL) {
        return 0;
    }
    int n;
    switch ((um98x_step_kind_t)s->kind) {
    case UM98X_STEP_SIGNALGROUP:
        n = snprintf(buf, cap, "CONFIG SIGNALGROUP %u %u", (unsigned)p->signal_group_master,
                     (unsigned)p->signal_group_slave);
        break;
    case UM98X_STEP_BAUD:
        n = snprintf(buf, cap, "CONFIG COM1 %u", (unsigned)p->com1_baud);
        break;
    case UM98X_STEP_MODE:
        return um98x_mode_command(p, buf, cap);
    case UM98X_STEP_RTCM:
        n = snprintf(buf, cap, "RTCM%u COM1 %u", (unsigned)s->msg, (unsigned)s->period_s);
        break;
    case UM98X_STEP_SAT_VIEW:
        n = snprintf(buf, cap, "GPGSV COM1 %u", SAT_VIEW_PERIOD_S);
        break;
    case UM98X_STEP_SAVE:
        n = snprintf(buf, cap, "%s", UM98X_CMD_SAVE);
        break;
    case UM98X_STEP_ANTENNA: {
        int nm = (int)um98x_antenna_str_len(p->antenna_model);
        int ns = (int)um98x_antenna_str_len(p->antenna_serial);
        if (nm == 0) {
            return 0;
        }
        n = snprintf(buf, cap, "CONFIG BASEANTENNAMODEL \"%.*s\" \"%.*s\" %u USER", nm, p->antenna_model, ns,
                     p->antenna_serial, (unsigned)p->antenna_setup_id);
        break;
    }
    case UM98X_STEP_B1CB2A:
        n = snprintf(buf, cap, "CONFIG RTCMB1CB2A ENABLE");
        break;
    default:
        return 0;
    }
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

size_t um98x_sat_view_command(bool on, uint16_t period_s, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return 0;
    }
    buf[0] = '\0';
    int n;
    if (on) {
        if (period_s == 0 || period_s > UM98X_SAT_VIEW_PERIOD_MAX) {
            return 0;
        }
        n = snprintf(buf, cap, "GPGSV COM1 %u", (unsigned)period_s);
    } else {
        n = snprintf(buf, cap, "UNLOG COM1 GPGSV");
    }
    if (n < 0 || (size_t)n >= cap) {
        buf[0] = '\0';
        return 0;
    }
    return (size_t)n;
}

size_t um98x_detect_candidates(uint32_t stored_baud, bool full, uint32_t *out, size_t cap)
{
    static const uint32_t short_list[] = { 115200, 460800, 921600, 230400 };
    static const uint32_t rest[] = { 57600, 38400, 19200, 9600 };
    uint32_t all[1 + sizeof(short_list) / sizeof(short_list[0]) + sizeof(rest) / sizeof(rest[0])];
    size_t n_all = 0;
    if (um98x_baud_supported(stored_baud)) {
        all[n_all++] = stored_baud;
    }
    for (size_t i = 0; i < sizeof(short_list) / sizeof(short_list[0]); i++) {
        all[n_all++] = short_list[i];
    }
    if (full) {
        for (size_t i = 0; i < sizeof(rest) / sizeof(rest[0]); i++) {
            all[n_all++] = rest[i];
        }
    }
    size_t n = 0;
    for (size_t i = 0; i < n_all && n < cap; i++) {
        bool dup = false;
        for (size_t j = 0; j < n; j++) {
            if (out[j] == all[i]) {
                dup = true;
                break;
            }
        }
        if (!dup) {
            out[n++] = all[i];
        }
    }
    return n;
}
