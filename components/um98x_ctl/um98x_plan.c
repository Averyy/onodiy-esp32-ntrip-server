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

/* The receiver port the link is wired to (index 0 = COM1). */
static uint8_t s_port = 1u;
static const char *const PORT_NAME[] = { "COM1", "COM2", "COM3" };
static const char *const UNLOG[] = { "UNLOG COM1", "UNLOG COM2", "UNLOG COM3" };

void um98x_link_port_set(uint8_t port)
{
    if (port >= 1u && port <= 3u) {
        s_port = port;
    }
}

uint8_t um98x_link_port(void)
{
    return s_port;
}

const char *um98x_link_port_name(void)
{
    return PORT_NAME[s_port - 1u];
}

const char *um98x_cmd_unlog(void)
{
    return UNLOG[s_port - 1u];
}

uint32_t um98x_applied_hash(const um98x_base_profile_t *p)
{
    uint32_t h = um98x_profile_hash(p);
    if (h == 0 || s_port == 1u) {
        return h;
    }
    h ^= 0x9E3779B9u * s_port;   /* the golden-ratio constant spreads the port over every bit */
    return h != 0 ? h : 1u;
}

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
    /* Runs of spaces read as one: the receiver stores them so (build 25164). */
    return um98x_antenna_str_same(rx->antenna_model, p->antenna_model) &&
           um98x_antenna_str_same(rx->antenna_serial, p->antenna_serial) &&
           rx->antenna_setup_id == p->antenna_setup_id;
}

static void add(um98x_plan_t *out, um98x_step_kind_t kind, bool fatal, uint16_t msg, uint16_t period_s)
{
    if (out->count < UM98X_PLAN_MAX) {
        out->step[out->count++] = (um98x_step_t){ (uint8_t)kind, fatal, msg, period_s };
    }
}

bool um98x_rtcm_optional(uint16_t msg)
{
    return msg == UM98X_RTCM_GLO_BIASES || (msg >= 1131u && msg <= 1137u);
}

static void add_rtcm(const um98x_base_profile_t *p, um98x_plan_t *out)
{
    for (size_t i = 0; i < p->rtcm_count; i++) {
        add(out, UM98X_STEP_RTCM, !um98x_rtcm_optional(p->rtcm[i].msg), p->rtcm[i].msg, p->rtcm[i].period_s);
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
        n = snprintf(buf, cap, "CONFIG %s %u", um98x_link_port_name(), (unsigned)p->com1_baud);
        break;
    case UM98X_STEP_MODE:
        return um98x_mode_command(p, buf, cap);
    case UM98X_STEP_RTCM:
        n = snprintf(buf, cap, "RTCM%u %s %u", (unsigned)s->msg, um98x_link_port_name(), (unsigned)s->period_s);
        break;
    case UM98X_STEP_SAT_VIEW:
        n = snprintf(buf, cap, "GPGSV %s %u", um98x_link_port_name(), SAT_VIEW_PERIOD_S);
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

/* One preview line; a text that does not fit ends the list there (never a cut command). */
static bool preview_add(um98x_preview_step_t *out, size_t cap, size_t *n, const char *text, um98x_preview_kind_t kind,
                        um98x_preview_when_t when, bool saved)
{
    if (*n >= cap || text == NULL || text[0] == '\0' || strlen(text) >= UM98X_PREVIEW_TEXT_MAX) {
        return false;
    }
    um98x_preview_step_t *s = &out[(*n)++];
    memset(s, 0, sizeof(*s));
    memcpy(s->text, text, strlen(text));
    s->kind = (uint8_t)kind;
    s->when = (uint8_t)when;
    s->saved = saved;
    return true;
}

/* A plan step through the builder's own formatter. */
static bool preview_step(const um98x_base_profile_t *p, um98x_preview_step_t *out, size_t cap, size_t *n,
                         um98x_step_kind_t kind, uint16_t msg, uint16_t period_s, um98x_preview_when_t when, bool saved)
{
    const um98x_step_t st = { (uint8_t)kind, true, msg, period_s };
    char text[UM98X_PREVIEW_TEXT_MAX];
    if (um98x_step_command(p, &st, text, sizeof(text)) == 0) {
        return false;
    }
    return preview_add(out, cap, n, text, UM98X_PREVIEW_WRITE, when, saved);
}

size_t um98x_plan_preview(const um98x_base_profile_t *p, uint32_t link_baud, um98x_preview_step_t *out, size_t cap)
{
    size_t n = 0;
    if (out == NULL || !um98x_profile_valid(p)) {
        return 0;
    }
    bool ok = preview_add(out, cap, &n, um98x_cmd_unlog(), UM98X_PREVIEW_WRITE, UM98X_PREVIEW_ALWAYS, false) &&
              preview_add(out, cap, &n, UM98X_CMD_VERSION, UM98X_PREVIEW_QUERY, UM98X_PREVIEW_ALWAYS, false) &&
              preview_add(out, cap, &n, UM98X_CMD_CONFIG, UM98X_PREVIEW_QUERY, UM98X_PREVIEW_ALWAYS, false) &&
              preview_add(out, cap, &n, UM98X_CMD_MODE, UM98X_PREVIEW_QUERY, UM98X_PREVIEW_ALWAYS, false) &&
              preview_step(p, out, cap, &n, UM98X_STEP_SIGNALGROUP, 0, 0, UM98X_PREVIEW_IF_DIFFERS, true) &&
              preview_add(out, cap, &n, UM98X_CMD_GROUPS, UM98X_PREVIEW_QUERY, UM98X_PREVIEW_ALWAYS, false);
    if (ok && p->com1_baud != link_baud) {
        ok = preview_step(p, out, cap, &n, UM98X_STEP_BAUD, 0, 0, UM98X_PREVIEW_IF_DIFFERS, false);
    }
    ok = ok && preview_step(p, out, cap, &n, UM98X_STEP_MODE, 0, 0, UM98X_PREVIEW_IF_DIFFERS, false);
    if (ok && um98x_antenna_str_len(p->antenna_model) > 0) {
        /* The dump again right after an accepted antenna step: the name as
         * the receiver stores it. */
        ok = preview_step(p, out, cap, &n, UM98X_STEP_ANTENNA, 0, 0, UM98X_PREVIEW_IF_DIFFERS, false) &&
             preview_add(out, cap, &n, UM98X_CMD_CONFIG, UM98X_PREVIEW_QUERY, UM98X_PREVIEW_IF_DIFFERS, false);
    }
    ok = ok && preview_step(p, out, cap, &n, UM98X_STEP_B1CB2A, 0, 0, UM98X_PREVIEW_IF_OFF, false);
    for (size_t i = 0; ok && i < p->rtcm_count; i++) {
        ok = preview_step(p, out, cap, &n, UM98X_STEP_RTCM, p->rtcm[i].msg, p->rtcm[i].period_s, UM98X_PREVIEW_ALWAYS,
                          false);
    }
    if (ok && p->sat_view_output) {
        ok = preview_step(p, out, cap, &n, UM98X_STEP_SAT_VIEW, 0, 0, UM98X_PREVIEW_ALWAYS, false);
    }
    if (ok) {
        (void)preview_step(p, out, cap, &n, UM98X_STEP_SAVE, 0, 0, UM98X_PREVIEW_ALWAYS, true);
    }
    return n;
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
        n = snprintf(buf, cap, "GPGSV %s %u", um98x_link_port_name(), (unsigned)period_s);
    } else {
        n = snprintf(buf, cap, "UNLOG %s GPGSV", um98x_link_port_name());
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
