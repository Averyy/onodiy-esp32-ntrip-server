/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Bench primitives (um98x_flash_bench.h): the protocol steps of fl_proto.c
 * over the RAW lease, with every byte in the capture. Bench builds only;
 * other builds answer ESP_ERR_NOT_SUPPORTED.
 */
#include "um98x_flash_bench.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_timer.h"
#include "fl_proto.h"
#include "fl_rx.h"
#include "sdkconfig.h"
#include "um98x_flash.h"
#include "um98x_flash_priv.h"

#if CONFIG_UM98X_FLASH_BYTE_CAPTURE

static EXT_RAM_BSS_ATTR fl_lis_t s_lis;
static EXT_RAM_BSS_ATTR fl_rx_t s_rx;
static const fl_io_t *s_io;

static void result_line(um98x_bench_res_t *out, bool pass, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static void result_line(um98x_bench_res_t *out, bool pass, const char *fmt, ...)
{
    char body[180];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    out->pass = pass;
    snprintf(out->line, sizeof(out->line), "%s %s", pass ? "PASS" : "FAIL", body);
    um98x_flash_cap_printf(true, "%s", out->line);
}

/* Fields of the version log's header (before the ';'), from the window. */
static uint8_t header_fields(const fl_lis_t *lis)
{
    const uint8_t *p = fl_memmem(lis->win.buf, lis->win.len, "#VERSIONA,");
    if (p == NULL) {
        return 0;
    }
    uint8_t n = 1;
    for (const uint8_t *q = p; q < lis->win.buf + lis->win.len && *q != ';' && *q != '\n'; q++) {
        n += (*q == ',');
    }
    return n;
}

static bool version(um98x_bench_res_t *out, uint32_t *build, gnss_versiona_t *v)
{
    for (int i = 0; i < 3; i++) {
        if (fl_query_version(&s_lis, v)) {
            *build = v->build;
            out->header_fields = header_fields(&s_lis);
            um98x_flash_cap_printf(true, "version: model %s, firmware %s, build %u, header fields %u", v->model,
                                   v->firmware, (unsigned)v->build, (unsigned)out->header_fields);
            return true;
        }
    }
    return false;
}

/* Up to a confirmed loader prompt. False with the result line set otherwise. */
static bool enter(const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    gnss_versiona_t v;
    if (fl_lis_set_baud(&s_lis, req->app_baud) != 0 || !version(out, &out->build_before, &v)) {
        result_line(out, false, "no version log at %u baud before the entry: nothing else sent", (unsigned)req->app_baud);
        return false;
    }
    if (fl_quiesce(&s_lis) != 0) {
        result_line(out, false, "link error while quiescing");
        return false;
    }
    if (req->app_baud != req->baud) {
        fl_baud_result_t br = fl_raise_baud(&s_lis, req->app_baud, req->baud);
        if (br != FL_BAUD_OK) {
            result_line(out, false, "COM1 at %u baud not verified (%s)", (unsigned)req->baud,
                        br == FL_BAUD_RESTORED ? "restored" : "NOT restored: power-cycle the receiver");
            return false;
        }
    }
    if (fl_pre_reset_quiet(&s_lis) != 0) {
        result_line(out, false, "link error before the reset");
        return false;
    }
    fl_entry_stats_t st;
    fl_entry_result_t e = fl_reset_and_stop(&s_lis, req->baud, &st);
    out->key_bytes = st.key_bytes;
    out->prompt_ms = st.prompt_ms;
    if (e != FL_ENTRY_PROMPT) {
        bool app = fl_app_check(&s_lis, req->app_baud, FL_APP_WAIT_MS, &v);
        result_line(out, false, "no loader prompt at %u baud (%s, %u stop-key bytes); application %s", (unsigned)req->baud,
                    e == FL_ENTRY_APP    ? "the application restarted"
                    : e == FL_ENTRY_LATE ? "data after 8 s without it"
                                         : "nothing within 30 s",
                    (unsigned)st.key_bytes, app ? "back" : "NOT back");
        return false;
    }
    out->prompt = true;
    um98x_flash_cap_printf(true,
                           "loader prompt %u ms after the reset at %u baud (%u stop-key bytes); banner baud %s, "
                           "timeout %s, default action %s, last menu line %s",
                           (unsigned)st.prompt_ms, (unsigned)req->baud, (unsigned)st.key_bytes,
                           st.baud_text ? "seen" : "-", st.timeout_text ? "seen" : "-", st.action_text ? "seen" : "-",
                           st.last_menu_line ? "seen" : "-");
    fl_lis_listen(&s_lis, FL_MENU_QUIET_MS);
    uint64_t m = fl_lis_mark(&s_lis);
    (void)fl_write_str(s_io, "\r\n");
    if (fl_wait_prompt(&s_lis, m, FL_PROMPT_MS, 0) != FL_PROMPT_SEEN) {
        um98x_flash_cap_printf(true, "no prompt after \"\\r\\n\"");
    }
    return true;
}

/* "6", the application, its version. */
static bool leave(const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    fl_exit_t ex;
    if (fl_leave_loader_early(&s_lis, req->baud, req->app_baud, &ex) != FL_EXIT_APP) {
        return false;
    }
    out->build_after = ex.ver.build;
    return true;
}

static void op_listen(const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    um98x_flash_capture_mark(req->record_only ? "listen only (the receiver's other port in use)" : "listen only");
    if (fl_lis_set_baud(&s_lis, req->baud) != 0) {
        result_line(out, false, "baud %u not set", (unsigned)req->baud);
        return;
    }
    fl_lis_listen(&s_lis, req->seconds * 1000u);
    out->bytes = s_lis.bytes;
    out->rtcm = s_lis.rtcm;
    out->sentences = s_lis.sentences;
    out->devicename = fl_lis_seen(&s_lis, FL_S_DEVICENAME);
    out->prompt = fl_lis_seen(&s_lis, FL_S_PROMPT);
    bool ok = req->record_only || out->rtcm > 0 || out->sentences > 0;
    result_line(out, ok,
                "listen %u s at %u baud: %u bytes, %u RTCM frames, %u sentences, devicename %s, loader prompt %s",
                (unsigned)req->seconds, (unsigned)req->baud, (unsigned)out->bytes, (unsigned)out->rtcm,
                (unsigned)out->sentences, out->devicename ? "seen" : "-", out->prompt ? "SEEN" : "-");
}

static void op_version(const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    um98x_flash_capture_mark("version query");
    gnss_versiona_t v;
    memset(&v, 0, sizeof(v));
    bool ok = fl_lis_set_baud(&s_lis, req->baud) == 0 && version(out, &out->build_before, &v);
    result_line(out, ok, "version at %u baud: model %s, firmware %s, build %u, header fields %u", (unsigned)req->baud,
                ok ? v.model : "-", ok ? v.firmware : "-", (unsigned)out->build_before, (unsigned)out->header_fields);
}

static void op_entry(const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    um98x_flash_capture_mark("loader entry probe");
    if (!enter(req, out)) {
        return;
    }
    bool back = leave(req, out);
    result_line(out, back, "loader prompt at %u baud after %u ms (%u stop-key bytes); application %s, build %u -> %u",
                (unsigned)req->baud, (unsigned)out->prompt_ms, (unsigned)out->key_bytes, back ? "back" : "NOT back",
                (unsigned)out->build_before, (unsigned)out->build_after);
}

static void op_menu2(const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    um98x_flash_capture_mark("menu item 2 probe (no data frame is ever sent)");
    if (!enter(req, out)) {
        return;
    }
    fl_rx_init(&s_rx, s_io);
    s_rx.tap = fl_lis_tap;
    s_rx.tap_ctx = &s_lis;
    fl_menu_job_t job = { .confirm_prompt = false };
    fl_menu_stats_t ms;
    fl_menu_result_t mr = fl_menu_download(&s_lis, &s_rx, &job, &ms);
    out->ready_line = (mr == FL_MENU_READY);
    out->submenu = (mr == FL_MENU_SUBMENU);
    out->first_start_ms = UINT32_MAX;
    if (mr == FL_MENU_READY) {
        fl_startbytes_t sb;
        uint64_t m = fl_lis_mark(&s_lis);
        fl_watch_start_bytes(&s_lis, &s_rx, req->seconds * 1000u, &sb);
        out->c_count = sb.c_count;
        out->nak_count = sb.nak_count;
        out->first_start_ms = sb.first_ms;
        out->start_interval_ms = sb.interval_ms;
        static const uint8_t can2[2] = { FL_CAN, FL_CAN };
        (void)fl_write(s_io, can2, sizeof(can2));
        (void)fl_wait_prompt(&s_lis, m, FL_PROMPT_MS, FL_NUDGE_MS);
    }
    bool back = leave(req, out);
    bool same = back && out->build_after == out->build_before;
    result_line(out, same,
                "menu 2: %s%s; start bytes C %u NAK %u, first after %d ms, interval %u ms; application %s, build %u -> %u",
                mr == FL_MENU_READY ? "ready line" : mr == FL_MENU_SUBMENU ? "firmware submenu (left with q)" : ms.text,
                ms.begin_seen ? " (begin text seen)" : "", (unsigned)out->c_count, (unsigned)out->nak_count,
                out->first_start_ms == UINT32_MAX ? -1 : (int)out->first_start_ms, (unsigned)out->start_interval_ms,
                back ? "back" : "NOT back", (unsigned)out->build_before, (unsigned)out->build_after);
}

static void op_idle(const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    um98x_flash_capture_mark("loader idle observation");
    if (!enter(req, out)) {
        return;
    }
    uint64_t m = fl_lis_mark(&s_lis);
    fl_lis_evidence_reset(&s_lis);
    fl_lis_listen(&s_lis, req->seconds * 1000u);
    out->bytes = s_lis.bytes;
    out->left_loader = fl_lis_app_evidence(&s_lis);
    bool tmo = fl_lis_seen_after(&s_lis, FL_S_TIMEOUT_CFG, m);
    bool act = fl_lis_seen_after(&s_lis, FL_S_DEFAULT_ACTION, m);
    bool back;
    if (out->left_loader) {
        gnss_versiona_t v;
        memset(&v, 0, sizeof(v));
        back = fl_app_check(&s_lis, req->app_baud, FL_APP_WAIT_MS, &v);
        out->build_after = v.build;
    } else {
        back = leave(req, out);
    }
    result_line(out, back, "idle %u s at the prompt: %u bytes, timeout text %s, default action text %s, %s; application %s",
                (unsigned)req->seconds, (unsigned)out->bytes, tmo ? "SEEN" : "-", act ? "SEEN" : "-",
                out->left_loader ? "the loader LEFT by itself" : "the loader stayed", back ? "back" : "NOT back");
}

esp_err_t um98x_flash_bench(gnss_link_lease_t raw_lease, const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    if (req == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    s_io = um98x_flash_io_setup(raw_lease);
    um98x_flash_text_clear();
    fl_lis_init(&s_lis, s_io, gnss_link_get_baud());
    switch (req->op) {
    case UM98X_BENCH_LISTEN:
        op_listen(req, out);
        break;
    case UM98X_BENCH_VERSION:
        op_version(req, out);
        break;
    case UM98X_BENCH_ENTRY:
        op_entry(req, out);
        break;
    case UM98X_BENCH_MENU2:
        op_menu2(req, out);
        break;
    case UM98X_BENCH_IDLE:
        op_idle(req, out);
        break;
    default:
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

esp_err_t um98x_flash_bench_race(gnss_link_lease_t raw_lease, uint32_t max_ms, uint32_t app_baud,
                                 um98x_bench_res_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    s_io = um98x_flash_io_setup(raw_lease);
    um98x_flash_capture_mark("stop key from boot");
    fl_lis_init(&s_lis, s_io, gnss_link_get_baud());
    if (fl_lis_set_baud(&s_lis, UM98X_FLASH_BAUD) != 0) {
        result_line(out, false, "loader baud not set");
        return ESP_OK;
    }
    out->race_first_key_ms = (uint32_t)(esp_timer_get_time() / 1000);   /* since the ESP32 reset */
    fl_entry_stats_t st;
    fl_entry_result_t r = fl_stop_key_stream(&s_lis, max_ms, false, &st);
    out->prompt = (r == FL_ENTRY_PROMPT);
    out->prompt_ms = st.prompt_ms;
    out->key_bytes = st.key_bytes;
    bool back;
    if (out->prompt) {
        um98x_bench_req_t req = { .baud = UM98X_FLASH_BAUD, .app_baud = app_baud };
        back = leave(&req, out);
    } else {
        gnss_versiona_t v;
        memset(&v, 0, sizeof(v));
        back = fl_app_check(&s_lis, app_baud, FL_APPCHECK_MS, &v);
        out->build_after = v.build;
    }
    result_line(out, true,
                "stop key from boot: first byte %u ms after the ESP32 reset; loader prompt %s (%u ms, %u stop-key "
                "bytes); application %s",
                (unsigned)out->race_first_key_ms, out->prompt ? "REACHED" : "not reached", (unsigned)out->prompt_ms,
                (unsigned)out->key_bytes, back ? "back" : "NOT back");
    return ESP_OK;
}

#else

esp_err_t um98x_flash_bench(gnss_link_lease_t raw_lease, const um98x_bench_req_t *req, um98x_bench_res_t *out)
{
    (void)raw_lease;
    (void)req;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t um98x_flash_bench_race(gnss_link_lease_t raw_lease, uint32_t max_ms, uint32_t app_baud,
                                 um98x_bench_res_t *out)
{
    (void)raw_lease;
    (void)max_ms;
    (void)app_baud;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
