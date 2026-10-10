/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The flasher on the target: the pure cores (pkg_check, fl_proto,
 * xmodem_tx, fl_session) run over the GNSS link's RAW lease; the journal
 * lives in settings; the RAM copy in PSRAM; SHA-256 through PSA.
 */
#include "um98x_flash.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "fl_proto.h"
#include "fl_session.h"
#include "flash_journal.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pkg_check.h"
#include "pkg_store.h"
#include "psa/crypto.h"
#include "sdkconfig.h"
#include "settings.h"
#include "um98x_flash_bench.h"
#include "um98x_flash_priv.h"

static const char *TAG = "um98x_flash";

_Static_assert(UM98X_FLASH_JOURNAL_VERSION == FLASH_JOURNAL_VERSION, "journal version mismatch");
_Static_assert(UM98X_FLASH_BAUD == FL_BAUD_LOADER, "loader baud mismatch");

#define TRUSTED_UNIX 1700000000
#define WRITE_TIMEOUT_MS 2000u

#ifndef CONFIG_UM98X_FLASH_ENABLE
#define CONFIG_UM98X_FLASH_ENABLE 0
#endif
#ifndef CONFIG_UM98X_FLASH_FALLBACK_115200
#define CONFIG_UM98X_FLASH_FALLBACK_115200 0
#endif
#ifndef CONFIG_UM98X_FLASH_CROSS_LAYOUT
#define CONFIG_UM98X_FLASH_CROSS_LAYOUT 0
#endif
#ifndef CONFIG_UM98X_FLASH_BYTE_CAPTURE
#define CONFIG_UM98X_FLASH_BYTE_CAPTURE 0
#endif

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static um98x_flash_progress_t s_progress;
static volatile bool s_cancel;

/* The prepared RAM copy and its digest. */
static uint8_t *s_buf;
static uint32_t s_len;
static uint8_t s_sha[32];

/* Last receiver text (printable, other bytes as '.'). */
static char s_text[UM98X_FLASH_TEXT_MAX];
static size_t s_text_head;
static size_t s_text_len;

/* Session work state: large, one at a time (the station task). */
static EXT_RAM_BSS_ATTR fl_sess_t s_sess;
static fl_io_t s_io;

typedef struct {
    gnss_link_lease_t lease;
    um98x_flash_progress_cb_t cb;
    void *cb_ctx;
} run_ctx_t;
static run_ctx_t s_run;

/* ---- small helpers -------------------------------------------------------------- */

static void feed(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
#endif
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static int64_t unix_now(void)
{
    time_t t = time(NULL);
    return t >= TRUSTED_UNIX ? (int64_t)t : 0;
}

static void text_add(const uint8_t *d, size_t n)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < n; i++) {
        uint8_t b = d[i];
        char c = ((b >= 0x20 && b <= 0x7E) || b == '\r' || b == '\n' || b == '\t') ? (char)b : '.';
        size_t at = (s_text_head + s_text_len) % sizeof(s_text);
        s_text[at] = c;
        if (s_text_len < sizeof(s_text)) {
            s_text_len++;
        } else {
            s_text_head = (s_text_head + 1u) % sizeof(s_text);
        }
    }
    portEXIT_CRITICAL(&s_lock);
}

size_t um98x_flash_get_text(char *buf, size_t len)
{
    if (buf == NULL || len == 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    size_t n = s_text_len < len - 1u ? s_text_len : len - 1u;
    size_t skip = s_text_len - n;
    for (size_t i = 0; i < n; i++) {
        buf[i] = s_text[(s_text_head + skip + i) % sizeof(s_text)];
    }
    portEXIT_CRITICAL(&s_lock);
    buf[n] = '\0';
    return n;
}

void um98x_flash_text_clear(void)
{
    portENTER_CRITICAL(&s_lock);
    s_text_head = 0;
    s_text_len = 0;
    portEXIT_CRITICAL(&s_lock);
}

/* ---- byte capture (bench builds) -------------------------------------------------- */

#if CONFIG_UM98X_FLASH_BYTE_CAPTURE
#define CAP_SIZE (256u * 1024u)
static EXT_RAM_BSS_ATTR char s_cap[CAP_SIZE];
static uint32_t s_cap_total;   /* bytes ever written (cursor space) */
static uint8_t s_reset_at;     /* bench: restart the ESP32 right after writing this journal state */
static uint32_t s_frames_logged;

static void cap_put(const char *s, size_t n)
{
    portENTER_CRITICAL(&s_lock);
    for (size_t i = 0; i < n; i++) {
        s_cap[s_cap_total % CAP_SIZE] = s[i];
        s_cap_total++;
    }
    portEXIT_CRITICAL(&s_lock);
}



void um98x_flash_cap_printf(bool console, const char *fmt, ...)
{
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (n < 0) {
        return;
    }
    size_t len = (size_t)n < sizeof(line) - 1u ? (size_t)n : sizeof(line) - 1u;
    cap_put(line, len);
    cap_put("\n", 1);
    if (console) {
        printf("%s\n", line);
    }
}

/* One line per 16 bytes: time, direction, hex, printable. Data frames are
 * summarised (their bytes are the package's, checked by its SHA-256): one line
 * each in the capture, every 64th on the console. */
static void capture_bytes(bool tx, const uint8_t *d, size_t n)
{
    uint32_t t = now_ms();
    if (tx && n >= 1028u && d[0] == 0x02) {
        s_frames_logged++;
        um98x_flash_cap_printf((s_frames_logged % 64u) == 1u, "%10u TX frame block %u (%u bytes)%s", (unsigned)t, (unsigned)d[1],
                   (unsigned)n, (s_frames_logged % 64u) == 1u ? " [console: every 64th frame]" : "");
        return;
    }
    for (size_t off = 0; off < n; off += 16u) {
        char hex[16 * 3 + 1];
        char asc[17];
        size_t k = n - off < 16u ? n - off : 16u;
        for (size_t i = 0; i < k; i++) {
            uint8_t b = d[off + i];
            snprintf(hex + i * 3u, 4, "%02x ", (unsigned)b);
            asc[i] = (b >= 0x20 && b <= 0x7E) ? (char)b : '.';
        }
        hex[k * 3u] = '\0';
        asc[k] = '\0';
        um98x_flash_cap_printf(true, "%10u %s %-48s|%s|", (unsigned)t, tx ? "TX" : "RX", hex, asc);
    }
}

bool um98x_flash_capture_available(void)
{
    return true;
}

void um98x_flash_capture_mark(const char *title)
{
    s_frames_logged = 0;
    um98x_flash_cap_printf(true, "==== %s (%u ms since boot) ====", title != NULL ? title : "", (unsigned)now_ms());
}

void um98x_flash_capture_line(const char *text)
{
    um98x_flash_cap_printf(true, "%10u %s", (unsigned)now_ms(), text != NULL ? text : "");
}

size_t um98x_flash_capture_read(uint32_t *cursor, char *buf, size_t len)
{
    if (cursor == NULL || buf == NULL || len == 0) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    uint32_t total = s_cap_total;
    uint32_t oldest = total > CAP_SIZE ? total - CAP_SIZE : 0;
    uint32_t c = *cursor < oldest ? oldest : *cursor;
    size_t n = 0;
    while (c < total && n < len) {
        buf[n++] = s_cap[c % CAP_SIZE];
        c++;
    }
    portEXIT_CRITICAL(&s_lock);
    *cursor = c;
    return n;
}

void um98x_flash_bench_reset_at(uint8_t state)
{
    s_reset_at = state;
}
#else
static void capture_bytes(bool tx, const uint8_t *d, size_t n)
{
    (void)tx;
    (void)d;
    (void)n;
}

void um98x_flash_cap_printf(bool console, const char *fmt, ...)
{
    (void)console;
    (void)fmt;
}

bool um98x_flash_capture_available(void)
{
    return false;
}

void um98x_flash_capture_mark(const char *title)
{
    (void)title;
}

void um98x_flash_capture_line(const char *text)
{
    (void)text;
}

size_t um98x_flash_capture_read(uint32_t *cursor, char *buf, size_t len)
{
    (void)cursor;
    (void)buf;
    (void)len;
    return 0;
}

void um98x_flash_bench_reset_at(uint8_t state)
{
    (void)state;
}
#endif

/* ---- io over the RAW lease -------------------------------------------------------- */

static int io_write(void *ctx, const uint8_t *data, size_t len)
{
    run_ctx_t *r = ctx;
    capture_bytes(true, data, len);
    feed();
    esp_err_t err = gnss_link_write(r->lease, data, len, WRITE_TIMEOUT_MS);
    feed();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "write of %u bytes: %s", (unsigned)len, esp_err_to_name(err));
        return -1;
    }
    return 0;
}

static size_t io_read(void *ctx, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    run_ctx_t *r = ctx;
    feed();
    size_t got = 0;
    esp_err_t err = gnss_link_raw_read(r->lease, buf, cap, &got, timeout_ms > FL_SLICE_MS ? FL_SLICE_MS : timeout_ms);
    feed();
    if (err != ESP_OK || got == 0) {
        return 0;
    }
    text_add(buf, got);
    capture_bytes(false, buf, got);
    return got;
}

static void io_flush(void *ctx)
{
    run_ctx_t *r = ctx;
    (void)gnss_link_raw_flush(r->lease);
}

static int io_set_baud(void *ctx, uint32_t baud)
{
    run_ctx_t *r = ctx;
    feed();
    esp_err_t err = gnss_link_set_baud(r->lease, baud);
    feed();
#if CONFIG_UM98X_FLASH_BYTE_CAPTURE
    um98x_flash_cap_printf(true, "%10u host baud %u: %s", (unsigned)now_ms(), (unsigned)baud, esp_err_to_name(err));
#endif
    return err == ESP_OK ? 0 : -1;
}

static uint32_t io_now(void *ctx)
{
    (void)ctx;
    return now_ms();
}

static void io_sleep(void *ctx, uint32_t ms)
{
    (void)ctx;
    TickType_t t = pdMS_TO_TICKS(ms);
    vTaskDelay(t > 0 ? t : 1);
    feed();
}

static uint32_t io_line_errors(void *ctx)
{
    (void)ctx;
    gnss_link_stats_t st;
    return gnss_link_get_stats(&st) == ESP_OK ? st.line_errors : 0;
}

static void io_note(void *ctx, const char *text)
{
    (void)ctx;
    ESP_LOGI(TAG, "%s", text);
#if CONFIG_UM98X_FLASH_BYTE_CAPTURE
    um98x_flash_cap_printf(false, "%10u -- %s", (unsigned)now_ms(), text);
#endif
}

const fl_io_t *um98x_flash_io_setup(gnss_link_lease_t lease)
{
    memset(&s_run, 0, sizeof(s_run));
    s_run.lease = lease;
    memset(&s_io, 0, sizeof(s_io));
    s_io.ctx = &s_run;
    s_io.write = io_write;
    s_io.read = io_read;
    s_io.flush = io_flush;
    s_io.set_baud = io_set_baud;
    s_io.now_ms = io_now;
    s_io.sleep_ms = io_sleep;
    s_io.line_errors = io_line_errors;
    s_io.note = io_note;
    return &s_io;
}

static void io_setup(gnss_link_lease_t lease)
{
    (void)um98x_flash_io_setup(lease);
}

/* ---- init and the package --------------------------------------------------------- */

esp_err_t um98x_flash_init(void)
{
    portENTER_CRITICAL(&s_lock);
    memset(&s_progress, 0, sizeof(s_progress));
    s_progress.phase = UM98X_FLASH_IDLE;
    portEXIT_CRITICAL(&s_lock);
    if (psa_crypto_init() != PSA_SUCCESS) {
        ESP_LOGW(TAG, "PSA crypto not initialised");
    }
    ESP_LOGI(TAG, "receiver flashing %s in this build%s", CONFIG_UM98X_FLASH_ENABLE ? "built in" : "not enabled",
             CONFIG_UM98X_FLASH_BYTE_CAPTURE ? " (bench byte capture on)" : "");
    return ESP_OK;
}

bool um98x_flash_enabled(void)
{
    return CONFIG_UM98X_FLASH_ENABLE != 0;
}

static bool sha_of(const uint8_t *data, uint32_t len, uint8_t out[32])
{
    psa_hash_operation_t op = psa_hash_operation_init();
    if (psa_hash_setup(&op, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return false;
    }
    const uint32_t step = 64u * 1024u;
    for (uint32_t off = 0; off < len; off += step) {
        uint32_t n = len - off < step ? len - off : step;
        if (psa_hash_update(&op, data + off, n) != PSA_SUCCESS) {
            psa_hash_abort(&op);
            return false;
        }
        feed();
    }
    size_t dl = 0;
    bool ok = psa_hash_finish(&op, out, 32, &dl) == PSA_SUCCESS && dl == 32;
    psa_hash_abort(&op);
    return ok;
}

void um98x_flash_discard(void)
{
    if (s_buf != NULL) {
        heap_caps_free(s_buf);
        s_buf = NULL;
    }
    s_len = 0;
    memset(s_sha, 0, sizeof(s_sha));
}

static void facts_from(const pkg_facts_t *f, um98x_flash_pkg_facts_t *out)
{
    out->size = f->size;
    memcpy(out->model, f->model, sizeof(out->model));
    out->layout = f->layout;
    out->layout_build = f->layout_build;
    memcpy(out->stamps, f->stamps, sizeof(out->stamps));
}

static void tick(void *ctx)
{
    (void)ctx;
    feed();
}

esp_err_t um98x_flash_prepare(const um98x_flash_expect_t *expect, um98x_flash_pkg_facts_t *out)
{
    if (expect == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->layout = -1;
    um98x_flash_discard();
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    out->size = info.size;
    s_buf = heap_caps_malloc(info.size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *work = heap_caps_malloc(PKG_LZO_BLOCK_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_buf == NULL || work == NULL) {
        heap_caps_free(work);
        um98x_flash_discard();
        snprintf(out->why, sizeof(out->why), "no PSRAM for the package copy (%u bytes)", (unsigned)info.size);
        ESP_LOGE(TAG, "%s: refused (never sent from flash)", out->why);
        return ESP_ERR_NO_MEM;
    }
    s_len = info.size;
    esp_err_t err = ESP_OK;
    for (uint32_t off = 0; off < info.size && err == ESP_OK; off += 4096u) {
        uint32_t n = info.size - off < 4096u ? info.size - off : 4096u;
        err = pkg_store_read(off, s_buf + off, n);
        if ((off & 0xFFFFu) == 0) {
            feed();
        }
    }
    if (err != ESP_OK) {
        heap_caps_free(work);
        um98x_flash_discard();
        snprintf(out->why, sizeof(out->why), "package read failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!sha_of(s_buf, s_len, s_sha)) {
        heap_caps_free(work);
        um98x_flash_discard();
        snprintf(out->why, sizeof(out->why), "SHA-256 not computed");
        return ESP_FAIL;
    }
    memcpy(out->sha256, s_sha, sizeof(out->sha256));
    pkg_check_in_t in = {
        .live_model = expect->receiver.model,
        .receiver_build = expect->receiver.build,
        .package_build = expect->expected_build,
        .expected_size = expect->size,
        .expected_sha256 = expect->sha256,
        .actual_sha256 = s_sha,
        .cross_layout_ack = expect->cross_layout_confirmed,
        .cross_layout_allowed = CONFIG_UM98X_FLASH_CROSS_LAYOUT != 0,
        .recovery = expect->recovery,
        .work = work,
        .work_cap = PKG_LZO_BLOCK_MAX,
        .tick = tick,
    };
    pkg_check_out_t res;
    int64_t t0 = esp_timer_get_time();
    uint8_t rule = pkg_check(s_buf, s_len, &in, &res);
    heap_caps_free(work);
    facts_from(&res.facts, out);
    out->cross_layout = res.cross_layout;
    out->failed_rule = rule;
    snprintf(out->why, sizeof(out->why), "%s", rule != 0 ? res.why : "");
    if (rule != 0) {
        ESP_LOGW(TAG, "package refused by rule %u: %s", (unsigned)rule, res.why);
        um98x_flash_discard();
        switch (rule) {
        case 11:
            return ESP_ERR_INVALID_CRC;
        case 8:
        case 12:
            return ESP_ERR_INVALID_VERSION;
        default:
            return ESP_ERR_INVALID_RESPONSE;
        }
    }
    ESP_LOGI(TAG, "package checked in RAM: %u bytes, %u compressed blocks, every rule passed (%u ms)",
             (unsigned)s_len, (unsigned)res.lzo_blocks, (unsigned)((esp_timer_get_time() - t0) / 1000));
    return ESP_OK;
}

static bool flash_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    (void)ctx;
    return pkg_store_read(off, buf, len) == ESP_OK;
}

esp_err_t um98x_flash_inspect_staged(um98x_flash_pkg_facts_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->layout = -1;
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    pkg_facts_t f;
    pkg_inspect(flash_read, NULL, info.size, &f);
    facts_from(&f, out);
    out->size = info.size;
    memcpy(out->sha256, info.sha256, sizeof(out->sha256));
    return ESP_OK;
}

/* ---- session hooks ------------------------------------------------------------------ */

static bool hook_journal_put(void *ctx, const um98x_flash_journal_t *j)
{
    (void)ctx;
    esp_err_t err = um98x_flash_journal_put(j);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "journal (%s) not written: %s", flash_journal_state_name(j->state), esp_err_to_name(err));
        return false;
    }
#if CONFIG_UM98X_FLASH_BYTE_CAPTURE
    um98x_flash_cap_printf(true, "%10u journal: %s (phase %s, %u transfer(s))", (unsigned)now_ms(),
               flash_journal_state_name(j->state), flash_journal_state_name(j->phase), (unsigned)j->transfers);
    if (s_reset_at != 0 && j->state == s_reset_at) {
        s_reset_at = 0;
        um98x_flash_cap_printf(true, "bench: ESP32 restart right after writing \"%s\"", flash_journal_state_name(j->state));
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
#endif
    return true;
}

static bool hook_copy_ok(void *ctx)
{
    (void)ctx;
    if (s_buf == NULL) {
        return false;
    }
    uint8_t now[32];
    return sha_of(s_buf, s_len, now) && memcmp(now, s_sha, sizeof(now)) == 0 &&
           memcmp(now, s_sess.j.sha256, sizeof(now)) == 0;
}

static bool hook_recovery_prepare(void *ctx)
{
    (void)ctx;
    um98x_flash_expect_t e;
    memset(&e, 0, sizeof(e));
    memcpy(e.sha256, s_sess.j.sha256, sizeof(e.sha256));
    e.size = s_sess.j.size;
    snprintf(e.expected_fw, sizeof(e.expected_fw), "%s", s_sess.j.expected_fw);
    e.expected_build = s_sess.j.expected_build;
    snprintf(e.receiver.model, sizeof(e.receiver.model), "%s", s_sess.j.pre_model);   /* the journalled model */
    e.receiver.build = s_sess.j.pre_build;
    e.recovery = true;
    um98x_flash_pkg_facts_t f;
    if (um98x_flash_prepare(&e, &f) != ESP_OK) {
        ESP_LOGE(TAG, "the pinned package fails the recovery gate (rule %u: %s): no transfer", (unsigned)f.failed_rule,
                 f.why);
        return false;
    }
    s_sess.pkg = s_buf;
    s_sess.pkg_len = s_len;
    return true;
}

static bool hook_cancelled(void *ctx)
{
    (void)ctx;
    return s_cancel;
}

static void hook_progress(void *ctx, const um98x_flash_progress_t *p)
{
    run_ctx_t *r = ctx;
    portENTER_CRITICAL(&s_lock);
    um98x_flash_progress_t keep = *p;
    keep.last_err = s_progress.last_err;
    s_progress = keep;
    portEXIT_CRITICAL(&s_lock);
    if (r->cb != NULL) {
        r->cb(&keep, r->cb_ctx);
    }
}

static int64_t hook_unix(void *ctx)
{
    (void)ctx;
    return unix_now();
}

static void sess_setup(gnss_link_lease_t lease, const um98x_flash_request_t *req, bool with_copy)
{
    io_setup(lease);
    s_run.cb = req->progress_cb;
    s_run.cb_ctx = req->progress_ctx;
    s_cancel = false;
    um98x_flash_text_clear();
    fl_sess_hooks_t h = {
        .ctx = &s_run,
        .journal_put = hook_journal_put,
        .copy_ok = hook_copy_ok,
        .recovery_prepare = hook_recovery_prepare,
        .cancelled = hook_cancelled,
        .progress = hook_progress,
        .unix_now = hook_unix,
        .stop_after_blocks = req->stop_after_blocks,
    };
    s_sess.j = req->journal;
    fl_sess_init(&s_sess, &s_io, with_copy ? s_buf : NULL, with_copy ? s_len : 0, UM98X_FLASH_BAUD,
                 CONFIG_UM98X_FLASH_FALLBACK_115200 != 0, &h);
    portENTER_CRITICAL(&s_lock);
    s_progress.last_err = 0;
    portEXIT_CRITICAL(&s_lock);
}

static void result_from(fl_sess_result_t r, um98x_flash_result_t *out)
{
    memset(out, 0, sizeof(*out));
    const fl_sess_out_t *o = &s_sess.out;
    switch (r) {
    case FL_SESS_DONE:
        out->end = UM98X_FLASH_END_DONE;
        break;
    case FL_SESS_ABORTED:
        out->end = UM98X_FLASH_END_ABORTED;
        break;
    case FL_SESS_NOT_STARTED:
        out->end = UM98X_FLASH_END_NOT_STARTED;
        break;
    default:
        out->end = UM98X_FLASH_END_NEEDS_USER;
        break;
    }
    out->failure = o->failure;
    snprintf(out->reason, sizeof(out->reason), "%s", o->reason);
    if (r == FL_SESS_DONE || r == FL_SESS_ABORTED) {
        out->receiver.valid = true;
        snprintf(out->receiver.model, sizeof(out->receiver.model), "%s", o->ver.model);
        snprintf(out->receiver.firmware, sizeof(out->receiver.firmware), "%s", o->ver.firmware);
        out->receiver.build = o->ver.build;
        snprintf(out->receiver.serial, sizeof(out->receiver.serial), "%s", o->ver.serial);
        snprintf(out->receiver.pn, sizeof(out->receiver.pn), "%s", o->ver.pn);
        snprintf(out->receiver.efuse_id, sizeof(out->receiver.efuse_id), "%s", o->ver.efuse_id);
        snprintf(out->receiver.compile_date, sizeof(out->receiver.compile_date), "%s", o->ver.compile_date);
    }
    out->app_baud = o->app_baud_found;
    out->journal = s_sess.j;
    out->ponr = o->ponr || s_sess.j.transfers > 0;
    if (o->outcome_valid) {
        static const char k[] = { 'S', 'L', 'E', 'U', 'N', 'A' };
        out->outcome = (unsigned)o->outcome < sizeof(k) ? k[o->outcome] : '?';
        if (o->outcome == FL_OUT_N_SILENT) {
            out->outcome = 'N';
        } else if (o->outcome == FL_OUT_N_APP) {
            out->outcome = 'A';
        }
    }
    out->handshake_ms = o->handshake_ms;
    out->xm_mode = o->xm_mode == XM_MODE_CRC ? 'C' : o->xm_mode == XM_MODE_SUM ? 'S' : 0;
    out->transfer_ms = o->transfer_ms;
    out->success_ms = o->success_ms;
    out->prompt_ms = o->prompt_ms;
    out->entry_prompt_ms = o->entry.prompt_ms;
    out->stop_key_bytes = o->entry.key_bytes;
    portENTER_CRITICAL(&s_lock);
    s_progress.failure_class = o->failure;
    s_progress.phase = r == FL_SESS_NEEDS_USER ? UM98X_FLASH_NEEDS_USER : s_progress.phase;
    s_progress.journal_state = s_sess.j.state;
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGI(TAG, "session ended: %s (%s)",
             r == FL_SESS_DONE ? "application confirmed" : r == FL_SESS_ABORTED ? "ended before the point of no return"
                                                         : r == FL_SESS_NOT_STARTED ? "not started" : "waits for the user",
             o->reason);
}

esp_err_t um98x_flash_identify(gnss_link_lease_t raw_lease, uint32_t stored_baud, bool loader_key_allowed,
                               um98x_flash_ident_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!CONFIG_UM98X_FLASH_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    io_setup(raw_lease);
    static fl_lis_t lis;
    fl_lis_init(&lis, &s_io, gnss_link_get_baud());
    fl_ident_t id;
    fl_id_result_t r = fl_identify(&lis, stored_baud, loader_key_allowed, &id);
    out->app_baud = id.app_baud;
    out->loader_baud = id.loader_baud;
    out->loader_key_sent = id.loader_key_sent;
    out->app_back = id.app_back;
    switch (r) {
    case FL_ID_OK:
        out->result = UM98X_FLASH_ID_OK;
        out->info.valid = true;
        snprintf(out->info.model, sizeof(out->info.model), "%s", id.ver.model);
        snprintf(out->info.firmware, sizeof(out->info.firmware), "%s", id.ver.firmware);
        out->info.build = id.ver.build;
        snprintf(out->info.serial, sizeof(out->info.serial), "%s", id.ver.serial);
        snprintf(out->info.pn, sizeof(out->info.pn), "%s", id.ver.pn);
        snprintf(out->info.efuse_id, sizeof(out->info.efuse_id), "%s", id.ver.efuse_id);
        snprintf(out->info.compile_date, sizeof(out->info.compile_date), "%s", id.ver.compile_date);
        break;
    case FL_ID_AT_LOADER:
        out->result = UM98X_FLASH_ID_AT_LOADER;
        break;
    case FL_ID_NO_ANSWER:
        out->result = UM98X_FLASH_ID_NO_ANSWER;
        break;
    default:
        out->result = UM98X_FLASH_ID_ERROR;
        break;
    }
    return ESP_OK;
}

esp_err_t um98x_flash_run(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req, um98x_flash_result_t *out)
{
    if (req == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!CONFIG_UM98X_FLASH_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_buf == NULL || s_len != req->journal.size || memcmp(s_sha, req->journal.sha256, sizeof(s_sha)) != 0) {
        return ESP_ERR_INVALID_STATE;   /* no prepared copy of this package */
    }
    sess_setup(raw_lease, req, true);
    um98x_flash_capture_mark("receiver update session");
    result_from(fl_sess_run(&s_sess), out);
    return ESP_OK;
}

esp_err_t um98x_flash_user_retry(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req,
                                 um98x_flash_result_t *out)
{
    if (req == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!CONFIG_UM98X_FLASH_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    um98x_flash_discard();   /* the copy comes through the recovery gate */
    sess_setup(raw_lease, req, false);
    um98x_flash_capture_mark("receiver update: retry (same package)");
    result_from(fl_sess_user_retry(&s_sess), out);
    return ESP_OK;
}

esp_err_t um98x_flash_user_restart(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req,
                                   um98x_flash_result_t *out)
{
    if (req == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!CONFIG_UM98X_FLASH_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    sess_setup(raw_lease, req, false);
    um98x_flash_capture_mark("receiver update: restart the receiver");
    result_from(fl_sess_user_restart(&s_sess), out);
    return ESP_OK;
}

esp_err_t um98x_flash_recover(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req, int plan,
                              bool race_prompt, um98x_flash_result_t *out)
{
    if (req == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!CONFIG_UM98X_FLASH_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    um98x_flash_discard();
    sess_setup(raw_lease, req, false);
    um98x_flash_capture_mark("receiver update: boot-time recovery");
    result_from(fl_sess_recover(&s_sess, (fl_plan_t)plan, race_prompt), out);
    return ESP_OK;
}

esp_err_t um98x_flash_race(gnss_link_lease_t raw_lease, uint32_t max_ms)
{
    if (!CONFIG_UM98X_FLASH_ENABLE) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    io_setup(raw_lease);
    static fl_lis_t lis;
    fl_lis_init(&lis, &s_io, gnss_link_get_baud());
    if (fl_lis_set_baud(&lis, UM98X_FLASH_BAUD) != 0) {
        return ESP_FAIL;
    }
    fl_entry_stats_t st;
    fl_entry_result_t r = fl_stop_key_stream(&lis, max_ms, false, &st);
    ESP_LOGW(TAG, "boot race: %s after %u ms (%u stop-key bytes)", r == FL_ENTRY_PROMPT ? "loader prompt" : "no prompt",
             (unsigned)(r == FL_ENTRY_PROMPT ? st.prompt_ms : max_ms), (unsigned)st.key_bytes);
    return r == FL_ENTRY_PROMPT ? ESP_OK : ESP_ERR_TIMEOUT;
}

void um98x_flash_cancel(void)
{
    s_cancel = true;
}

void um98x_flash_get_progress(um98x_flash_progress_t *out)
{
    if (out != NULL) {
        portENTER_CRITICAL(&s_lock);
        *out = s_progress;
        portEXIT_CRITICAL(&s_lock);
    }
}

/* ---- journal ------------------------------------------------------------------------ */

esp_err_t um98x_flash_journal_get(um98x_flash_journal_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!settings_is_available()) {
        /* Degraded settings: whether a session is open is unknown. */
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t buf[FLASH_JOURNAL_READ_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB, buf, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err != ESP_OK) {
        /* A read error or a record larger than any this firmware knows: it
         * may hide an interrupted flash. */
        return ESP_ERR_INVALID_SIZE;
    }
    switch (flash_journal_decode(buf, len, out)) {
    case FLASH_JOURNAL_OK:
        return ESP_OK;
    case FLASH_JOURNAL_UNKNOWN_VERSION:
        return ESP_ERR_INVALID_VERSION;
    default:
        return ESP_ERR_INVALID_SIZE;
    }
}

esp_err_t um98x_flash_journal_put(const um98x_flash_journal_t *j)
{
    if (j == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t buf[FLASH_JOURNAL_V1_MAX];
    size_t len = flash_journal_encode(j, buf, sizeof(buf));
    if (len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return settings_set_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB, buf, len);
}

esp_err_t um98x_flash_journal_close(void)
{
    esp_err_t err = settings_erase_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FLASH_JOB);
    return (err == ESP_ERR_NVS_NOT_FOUND) ? ESP_OK : err;
}

bool um98x_flash_journal_needs_race(const um98x_flash_journal_t *j, bool receiver_was_reset)
{
    return flash_journal_needs_race(j, receiver_was_reset);
}

int um98x_flash_journal_plan(esp_err_t read_err, const um98x_flash_journal_t *j, bool receiver_was_reset,
                             const char **text)
{
    fj_read_t r;
    switch (read_err) {
    case ESP_OK:
        r = FJ_OPEN;
        break;
    case ESP_ERR_NOT_FOUND:
        r = FJ_NONE;
        break;
    case ESP_ERR_INVALID_STATE:
        r = FJ_UNKNOWN;
        break;
    case ESP_ERR_INVALID_VERSION:
        r = FJ_NEWER;
        break;
    default:
        r = FJ_UNREADABLE;
        break;
    }
    fl_plan_t p = flash_journal_plan(r, j, receiver_was_reset);
    if (text != NULL) {
        *text = flash_journal_plan_text(p);
    }
    return (int)p;
}

const char *um98x_flash_journal_state_name(uint8_t state)
{
    return flash_journal_state_name(state);
}
