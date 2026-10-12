/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Worker task: request bodies and downloads that take long (app image and
 * receiver package uploads, the core dump, the whole log as a file) run
 * here as async requests, so the server keeps answering and pushing status
 * meanwhile. Its stack is in internal RAM because uploads write flash.
 *
 * Uploads stream in 4 KB pieces straight to flash; nothing is buffered
 * whole. A refused upload is still read to its end before the error goes
 * out, so the browser sees the answer instead of a reset connection. A
 * client that disconnects aborts the upload (the slot or the package area
 * stays empty).
 *
 * Also here: the uploaded receiver package waiting for the user's
 * confirmation (released after 15 min without one) and the safe-mode exit,
 * which reboots and therefore must not run on the server task.
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "diag.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "fw_update.h"
#include "gnss_link.h"
#include "lifecycle.h"
#include "network.h"
#include "pkg_store.h"
#include "sdkconfig.h"
#include "station.h"
#include "um98x_flash.h"
#include "web_internal.h"

static const char *TAG = "web";

#define WORKER_STACK      6144
#define WORKER_PRIO       4
#define JOBS_MAX          4
#define RECV_RETRIES      3
#define STAGED_TTL_S      900u
#define SAFE_MODE_DELAY_MS 1000

typedef enum {
    JOB_UPLOAD_APP = 0,
    JOB_UPLOAD_GNSS,
    JOB_REFUSE,              /* read the body to its end, then answer with the error */
    JOB_COREDUMP,
    JOB_LOG_TEXT,
    JOB_FLASH_CAPTURE,
    JOB_FRAME_TIMES,
    JOB_RELEASE_EXPIRED,
    JOB_SAFE_MODE_CLEAR,
} job_kind_t;

typedef struct {
    job_kind_t kind;
    httpd_req_t *req;        /* async copy (NULL for the jobs without a request) */
    uint32_t size;
    bool flag;               /* app: any project; log: previous boot */
    bool ack;                /* app: the user knows the version was rolled back here */
    bool reinstall;          /* app: the user asked to install the image already running again (X-Reinstall: 1) */
    web_err_t err;
    uint32_t retry_in_s;
} job_t;

static QueueHandle_t s_jobs;
static TaskHandle_t s_task;
static atomic_bool s_busy;               /* an upload is being received or refused */
static atomic_int s_pending_resp;
static esp_timer_handle_t s_expiry;

static uint8_t s_io[WEB_IO_CHUNK];       /* internal RAM: flash writes read from it */
EXT_RAM_BSS_ATTR static char s_resp[1536];
EXT_RAM_BSS_ATTR static station_status_t s_st;
static char s_disposition[96];

/* ---- upload progress (worker writes, server task reads) ---------------------------------------- */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static web_upload_progress_t s_prog;

static void progress(const char *job, const char *phase, uint32_t bytes, uint32_t total)
{
    portENTER_CRITICAL(&s_lock);
    s_prog.job = job;
    s_prog.phase = phase;
    s_prog.bytes = bytes;
    s_prog.total = total;
    s_prog.done = false;
    s_prog.result = NULL;
    s_prog.err = ESP_OK;
    s_prog.seq++;
    portEXIT_CRITICAL(&s_lock);
}

static void progress_done(const char *phase, const char *result, esp_err_t err)
{
    portENTER_CRITICAL(&s_lock);
    if (phase != NULL) {
        s_prog.phase = phase;
    }
    s_prog.done = true;
    s_prog.result = result;
    s_prog.err = err;
    s_prog.seq++;
    portEXIT_CRITICAL(&s_lock);
}

void web_upload_progress(web_upload_progress_t *out)
{
    portENTER_CRITICAL(&s_lock);
    *out = s_prog;
    portEXIT_CRITICAL(&s_lock);
}

bool web_upload_busy(void)
{
    return atomic_load(&s_busy);
}

int web_worker_pending_responses(void)
{
    return atomic_load(&s_pending_resp);
}

/* ---- the uploaded receiver package ------------------------------------------------------------- */

static struct {
    bool valid;
    um98x_flash_pkg_facts_t facts;
    uint32_t expires_s;
} s_staged;

static void staged_set(const um98x_flash_pkg_facts_t *facts)
{
    portENTER_CRITICAL(&s_lock);
    s_staged.valid = true;
    s_staged.facts = *facts;
    s_staged.expires_s = web_now_s() + STAGED_TTL_S;
    portEXIT_CRITICAL(&s_lock);
    if (s_expiry != NULL) {
        (void)esp_timer_stop(s_expiry);
        (void)esp_timer_start_once(s_expiry, (uint64_t)STAGED_TTL_S * 1000000u);
    }
}

void web_staged_forget(void)
{
    portENTER_CRITICAL(&s_lock);
    s_staged.valid = false;
    portEXIT_CRITICAL(&s_lock);
    if (s_expiry != NULL) {
        (void)esp_timer_stop(s_expiry);
    }
}

bool web_staged_get(um98x_flash_pkg_facts_t *facts, uint32_t *expires_in_s)
{
    portENTER_CRITICAL(&s_lock);
    bool valid = s_staged.valid;
    um98x_flash_pkg_facts_t f = s_staged.facts;
    uint32_t expires = s_staged.expires_s;
    portEXIT_CRITICAL(&s_lock);
    if (!valid) {
        return false;
    }
    /* Still the package in the slot? A download may have replaced it since. */
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid || info.size != f.size ||
        memcmp(info.sha256, f.sha256, sizeof(f.sha256)) != 0) {
        web_staged_forget();
        return false;
    }
    if (facts != NULL) {
        *facts = f;
    }
    if (expires_in_s != NULL) {
        uint32_t now = web_now_s();
        int32_t left = (int32_t)(expires - now);
        *expires_in_s = left > 0 ? (uint32_t)left : 0;
    }
    return true;
}

static void on_expiry(void *arg)
{
    (void)arg;
    job_t job = { .kind = JOB_RELEASE_EXPIRED };
    if (s_jobs != NULL) {
        (void)xQueueSend(s_jobs, &job, 0);
    }
}

static void release_expired(void)
{
    um98x_flash_pkg_facts_t f;
    if (!web_staged_get(&f, NULL)) {
        return;
    }
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) == ESP_OK && info.pinned) {
        /* An update session depends on it now: never released here. */
        web_staged_forget();
        return;
    }
    esp_err_t err = pkg_store_release();
    ESP_LOGW(TAG, "uploaded receiver package not confirmed within %u min: released (%s)",
             (unsigned)(STAGED_TTL_S / 60), esp_err_to_name(err));
    web_staged_forget();
}

/* ---- request body helpers ------------------------------------------------------------------------ */

/* Read up to `want` bytes. @return bytes read, or -1 when the client is gone. */
static int recv_some(httpd_req_t *req, uint8_t *buf, size_t want)
{
    for (int tries = 0; tries < RECV_RETRIES; tries++) {
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n > 0) {
            return n;
        }
        if (n != HTTPD_SOCK_ERR_TIMEOUT) {
            return -1;
        }
    }
    return -1;
}

static bool recv_exact(httpd_req_t *req, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        int n = recv_some(req, buf + got, len - got);
        if (n < 0) {
            return false;
        }
        got += (size_t)n;
    }
    return true;
}

/* Read and drop what is left of the body. */
static bool drain(httpd_req_t *req, uint32_t left)
{
    while (left > 0) {
        int n = recv_some(req, s_io, left < sizeof(s_io) ? left : sizeof(s_io));
        if (n < 0) {
            return false;
        }
        left -= (uint32_t)n;
    }
    return true;
}

/* Refuse after reading the rest: the body is read, so the connection can
 * carry the next request (unless the client asked for it to end). A client
 * that goes away meanwhile gets nothing. */
static void refuse(httpd_req_t *req, uint32_t left, web_err_t err, const char *message, uint32_t retry_in_s)
{
    if (drain(req, left)) {
        web_send_error(req, err, message, NULL, retry_in_s, NULL, NULL);
    }
}

/* Worker: the answer went out cut short (a download that failed half way):
 * the connection must end, or the client would wait for the rest. */
static bool s_cut;

static void finish_request(httpd_req_t *req)
{
    if (req != NULL) {
        int fd = httpd_req_to_sockfd(req);
        if (s_cut || web_conn_req_close(req)) {
            /* The client asked for the connection to end after this answer
             * ("Connection: close", HTTP/1.0), or the answer is incomplete:
             * our side ends now, what the client still sends is drained for
             * a moment, and the socket is shut, so the server deletes the
             * session once it has the connection back. The session is the
             * worker's until the completion below, so its descriptor cannot
             * have gone to another connection meanwhile. */
            web_linger_close(req);
            web_conn_shut(fd);
        }
        s_cut = false;
        web_conn_worker(fd, false);   /* its next request is bounded again */
        (void)httpd_req_async_handler_complete(req);
    }
}

/* ---- app image upload ----------------------------------------------------------------------------- */

static web_err_t app_refusal(uint32_t *retry_in_s)
{
    fw_update_status_t fw;
    *retry_in_s = UINT32_MAX;
    if (lifecycle_update_active() == LIFECYCLE_UPDATE_RECEIVER) {
        return WEB_ERR_JOURNAL_OPEN;
    }
    if (fw_update_get_status(&fw) == ESP_OK && (fw.trial == FW_TRIAL_CONFIRMING || fw.trial == FW_TRIAL_VERIFYING)) {
        *retry_in_s = fw.trial_left_s;
        return WEB_ERR_APP_TRIAL;
    }
    return WEB_ERR_BUSY;
}

static void upload_app(const job_t *job)
{
    httpd_req_t *req = job->req;
    uint32_t size = job->size;
    progress("app_upload", "receiving", 0, size);
    size_t head = size < sizeof(s_io) ? size : sizeof(s_io);
    if (!recv_exact(req, s_io, head)) {
        progress_done(NULL, "failed", ESP_ERR_TIMEOUT);
        return;
    }
    uint32_t left = size - (uint32_t)head;
    web_image_info_t info;
    web_image_check_t chk = web_image_check(s_io, head, (uint16_t)CONFIG_IDF_FIRMWARE_CHIP_ID,
                                            job->flag ? NULL : WEB_PROJECT_NAME, &info);
    bool rolled_back = chk == WEB_IMAGE_OK && fw_update_is_rolled_back(info.version);
    bool same = chk == WEB_IMAGE_OK && info.has_elf_sha && fw_update_same_as_running(info.elf_sha256);
    web_err_t verdict = web_app_upload_verdict(chk, rolled_back, job->ack, same, job->reinstall);
    /* The two refusals that wait for the user's answer are noted as what
     * they are, not as a state change: the page asks and may send it again. */
    if (verdict == WEB_ERR_SAME_IMAGE || verdict == WEB_ERR_ROLLED_BACK_ACK_REQUIRED) {
        /* The code running now: installing it again restarts the station for
         * nothing, unless the user asked for exactly that. A rolled-back
         * version: the same warning as from the banner. Before anything is
         * written. */
        const char *result = web_app_refusal_result(verdict);
        progress_done(NULL, result, ESP_ERR_INVALID_STATE);
        refuse(req, left, verdict, NULL, UINT32_MAX);
        web_note_result("app_upload", ESP_ERR_INVALID_STATE, result);
        ESP_LOGI(TAG, "app upload not installed (%s): %s", result, info.version);
        return;
    }
    if (verdict != WEB_ERR_COUNT) {
        const char *result = web_app_refusal_result(verdict);
        progress_done(NULL, result, ESP_ERR_INVALID_VERSION);
        refuse(req, left, verdict, NULL, UINT32_MAX);
        web_note_result("app_upload", ESP_ERR_INVALID_VERSION, result);
        return;
    }
    fw_update_upload_t up = NULL;
    esp_err_t err = fw_update_upload_app_begin(size, &up);
    if (err != ESP_OK) {
        uint32_t retry = UINT32_MAX;
        web_err_t e = err == ESP_ERR_INVALID_SIZE ? WEB_ERR_TOO_LARGE
                      : err == ESP_ERR_INVALID_STATE ? app_refusal(&retry)
                                                      : WEB_ERR_INTERNAL;
        progress_done(NULL, "refused", err);
        refuse(req, left, e, e == WEB_ERR_INTERNAL ? esp_err_to_name(err) : NULL, retry);
        return;
    }
    err = fw_update_upload_app_write(up, s_io, head);
    uint32_t got = (uint32_t)head;
    while (err == ESP_OK && left > 0) {
        int n = recv_some(req, s_io, left < sizeof(s_io) ? left : sizeof(s_io));
        if (n < 0) {
            /* Client gone: the slot stays as it was. */
            fw_update_upload_app_abort(up);
            progress_done(NULL, "failed", ESP_ERR_TIMEOUT);
            ESP_LOGW(TAG, "app upload aborted after %u of %u bytes", (unsigned)got, (unsigned)size);
            return;
        }
        err = fw_update_upload_app_write(up, s_io, (size_t)n);
        left -= (uint32_t)n;
        got += (uint32_t)n;
        portENTER_CRITICAL(&s_lock);
        s_prog.bytes = got;
        s_prog.seq++;
        portEXIT_CRITICAL(&s_lock);
    }
    if (err != ESP_OK) {
        fw_update_upload_app_abort(up);
        bool image = err == ESP_ERR_OTA_VALIDATE_FAILED || err == ESP_ERR_INVALID_SIZE;
        progress_done(NULL, image ? "invalid_image" : "failed", err);
        refuse(req, left, image ? WEB_ERR_INVALID_IMAGE : WEB_ERR_INTERNAL, image ? NULL : esp_err_to_name(err),
               UINT32_MAX);
        web_note_result("app_upload", err, image ? "invalid_image" : "failed");
        return;
    }
    progress("app_upload", "verifying", got, size);
    uint8_t sha[32];
    atomic_fetch_add(&s_pending_resp, 1);   /* the reboot waits for this response */
    err = fw_update_upload_app_finish(up, sha);
    if (err != ESP_OK) {
        web_err_t e = err == ESP_ERR_INVALID_SIZE ? WEB_ERR_SIZE_MISMATCH : WEB_ERR_INVALID_IMAGE;
        const char *result = err == ESP_ERR_INVALID_SIZE ? "size_mismatch" : "invalid_image";
        progress_done(NULL, result, err);
        web_send_error(req, e, NULL, NULL, UINT32_MAX, NULL, NULL);
        web_note_result("app_upload", err, result);
    } else {
        web_resp_t r;
        web_resp_begin(&r, req, 200, s_resp, sizeof(s_resp));
        wj_obj_open(&r.w, NULL);
        wj_hex(&r.w, "sha256", sha, sizeof(sha));
        wj_uint(&r.w, "size", size);
        wj_str(&r.w, "version", info.version);
        wj_str(&r.w, "project", info.project);
        wj_bool(&r.w, "reboot", true);
        wj_obj_close(&r.w);
        (void)web_resp_end(&r);
        progress_done("rebooting", "ok", ESP_OK);
        web_note_result("app_upload", ESP_OK, "ok");
        ESP_LOGW(TAG, "app image %s uploaded: rebooting into it", info.version);
    }
    atomic_fetch_sub(&s_pending_resp, 1);
}

/* ---- receiver package upload ------------------------------------------------------------------------ */

static void upload_gnss(const job_t *job)
{
    httpd_req_t *req = job->req;
    uint32_t size = job->size;
    progress("gnss_upload", "receiving", 0, size);
    pkg_store_writer_t w = NULL;
    esp_err_t err = pkg_store_begin(size, NULL, &w);
    if (err != ESP_OK) {
        pkg_store_info_t info;
        bool pinned = pkg_store_get_info(&info) == ESP_OK && info.pinned;
        web_err_t e = err == ESP_ERR_INVALID_SIZE  ? WEB_ERR_TOO_LARGE
                      : err == ESP_ERR_INVALID_STATE ? (pinned ? WEB_ERR_PACKAGE_PINNED : WEB_ERR_BUSY)
                                                     : WEB_ERR_INTERNAL;
        progress_done(NULL, "refused", err);
        refuse(req, size, e, e == WEB_ERR_INTERNAL ? esp_err_to_name(err) : NULL, UINT32_MAX);
        return;
    }
    /* Whatever was waiting for confirmation is gone now. */
    web_staged_forget();
    uint32_t left = size;
    uint32_t got = 0;
    while (left > 0) {
        int n = recv_some(req, s_io, left < sizeof(s_io) ? left : sizeof(s_io));
        if (n < 0) {
            pkg_store_abort(w);
            progress_done(NULL, "failed", ESP_ERR_TIMEOUT);
            ESP_LOGW(TAG, "receiver package upload aborted after %u of %u bytes", (unsigned)got, (unsigned)size);
            return;
        }
        err = pkg_store_write(w, s_io, (size_t)n);
        left -= (uint32_t)n;
        got += (uint32_t)n;
        if (err != ESP_OK) {
            pkg_store_abort(w);
            progress_done(NULL, "failed", err);
            refuse(req, left, WEB_ERR_INTERNAL, esp_err_to_name(err), UINT32_MAX);
            web_note_result("gnss_upload", err, "failed");
            return;
        }
        portENTER_CRITICAL(&s_lock);
        s_prog.bytes = got;
        s_prog.seq++;
        portEXIT_CRITICAL(&s_lock);
    }
    progress("gnss_upload", "inspecting", got, size);
    pkg_store_info_t info;
    err = pkg_store_finish(w, "upload", &info);
    if (err != ESP_OK) {
        web_err_t e = err == ESP_ERR_INVALID_SIZE ? WEB_ERR_SIZE_MISMATCH : WEB_ERR_INTERNAL;
        progress_done(NULL, err == ESP_ERR_INVALID_SIZE ? "size_mismatch" : "failed", err);
        web_send_error(req, e, e == WEB_ERR_INTERNAL ? esp_err_to_name(err) : NULL, NULL, UINT32_MAX, NULL, NULL);
        web_note_result("gnss_upload", err, err == ESP_ERR_INVALID_SIZE ? "size_mismatch" : "failed");
        return;
    }
    /* Facts without the receiver; parts the inspector cannot read yet stay unknown. */
    web_staged_snap_t snap;
    memset(&snap, 0, sizeof(snap));
    esp_err_t ierr = um98x_flash_inspect_staged(&snap.facts);
    if (ierr != ESP_OK && ierr != ESP_ERR_NOT_SUPPORTED) {
        memset(&snap.facts, 0, sizeof(snap.facts));
        snap.facts.layout = -1;
    }
    snap.facts.size = info.size;
    memcpy(snap.facts.sha256, info.sha256, sizeof(info.sha256));
    staged_set(&snap.facts);
    if (station_get_status(&s_st) != ESP_OK) {
        memset(&s_st, 0, sizeof(s_st));
    }
    snap.expires_in_s = STAGED_TTL_S;
    snap.st = &s_st;
    snap.expected_downtime_s = station_expected_flash_downtime_s(info.size);
    snap.budget_left_s = web_budget_now();
    snap.flash_available = WEB_FLASH_AVAILABLE;

    web_resp_t r;
    web_resp_begin(&r, req, 200, s_resp, sizeof(s_resp));
    web_build_staged(&r.w, &snap);
    (void)web_resp_end(&r);
    progress_done(NULL, "ok", ESP_OK);
    web_note_result("gnss_upload", ESP_OK, "ok");
    ESP_LOGI(TAG, "receiver package uploaded (%u bytes): waiting for confirmation", (unsigned)info.size);
}

/* ---- downloads ---------------------------------------------------------------------------------------- */

static void download_headers(httpd_req_t *req, const char *type, const char *prefix, const char *ext)
{
    char ver[32];
    web_safe_filename_part(esp_app_get_description()->version, ver, sizeof(ver));
    snprintf(s_disposition, sizeof(s_disposition), "attachment; filename=\"%s-%s.%s\"", prefix, ver, ext);
    httpd_resp_set_type(req, type);
    httpd_resp_set_hdr(req, "Content-Disposition", s_disposition);
    web_set_common_headers(req, true);
}

static void send_coredump(httpd_req_t *req)
{
    diag_coredump_info_t info;
    if (diag_coredump_info(&info) != ESP_OK || !info.present || info.size == 0) {
        web_send_err(req, WEB_ERR_NO_COREDUMP);
        return;
    }
    download_headers(req, "application/octet-stream", "coredump", "elf");
    for (size_t off = 0; off < info.size;) {
        size_t n = info.size - off < sizeof(s_io) ? info.size - off : sizeof(s_io);
        esp_err_t err = diag_coredump_read(off, s_io, n);
        if (err != ESP_OK || httpd_resp_send_chunk(req, (const char *)s_io, (ssize_t)n) != ESP_OK) {
            ESP_LOGW(TAG, "core dump download stopped at %u bytes: %s", (unsigned)off, esp_err_to_name(err));
            s_cut = true;   /* the connection ends: a half file is never mistaken for a whole one */
            return;
        }
        off += n;
    }
    (void)httpd_resp_send_chunk(req, NULL, 0);
}

static void send_log_text(httpd_req_t *req, bool prev)
{
    download_headers(req, "text/plain; charset=utf-8", "log", "txt");
    uint32_t cursor = 0;
    for (;;) {
        size_t n = prev ? diag_log_read_prev_boot(&cursor, (char *)s_io, sizeof(s_io))
                        : diag_log_read(&cursor, (char *)s_io, sizeof(s_io));
        if (n == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)s_io, (ssize_t)n) != ESP_OK) {
            return;
        }
    }
    (void)httpd_resp_send_chunk(req, NULL, 0);
}

/* Bench builds: the receiver byte capture, oldest kept line first. */
static void send_flash_capture(httpd_req_t *req)
{
    download_headers(req, "text/plain; charset=utf-8", "flash-capture", "txt");
    uint32_t cursor = 0;
    for (;;) {
        size_t n = um98x_flash_capture_read(&cursor, (char *)s_io, sizeof(s_io));
        if (n == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)s_io, (ssize_t)n) != ESP_OK) {
            return;
        }
    }
    (void)httpd_resp_send_chunk(req, NULL, 0);
}

/* Bench builds: the receiver link's frame and drain-pass times as CSV. */
static void send_frame_times(httpd_req_t *req)
{
#if CONFIG_GNSS_LINK_BENCH_HOOKS
    static network_status_t ns;
    bool synced = network_get_status(&ns) == ESP_OK && ns.time_synced;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int64_t timer_us = esp_timer_get_time();
    gnss_link_timing_info_t info;
    (void)gnss_link_bench_timing_read(0, NULL, 0, &info);
    char unix_ms[24] = "";
    if (synced) {
        snprintf(unix_ms, sizeof(unix_ms), "%" PRId64, (int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000);
    }
    download_headers(req, "text/plain; charset=utf-8", "frame-times", "csv");
    int n = snprintf((char *)s_io, sizeof(s_io),
                     "# esp_timer_us=%" PRId64 ",unix_ms=%s,clock=%s,records=%" PRIu32 ",full=%u,running=%u\n"
                     "# F,read_us,published_us,msg,len,sys,epoch,mmb,held\n"
                     "# P,begin_us,end_us,bytes,capped,next_wait_ms,gap_us\n"
                     "# S,begin_us,end_us,where\n",
                     timer_us, unix_ms, synced ? "sntp" : "none", info.count, info.full ? 1u : 0u,
                     info.running ? 1u : 0u);
    if (n < 0 || httpd_resp_send_chunk(req, (const char *)s_io, n) != ESP_OK) {
        return;
    }
    size_t cursor = 0;
    for (;;) {
        size_t len = gnss_link_bench_timing_csv(&cursor, (char *)s_io, sizeof(s_io));
        if (len == 0) {
            break;
        }
        if (httpd_resp_send_chunk(req, (const char *)s_io, (ssize_t)len) != ESP_OK) {
            return;
        }
    }
    (void)httpd_resp_send_chunk(req, NULL, 0);
#else
    web_send_err(req, WEB_ERR_NOT_FOUND);
#endif
}

/* ---- task ---------------------------------------------------------------------------------------------- */

static void worker(void *arg)
{
    (void)arg;
    for (;;) {
        job_t job;
        if (xQueueReceive(s_jobs, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        switch (job.kind) {
        case JOB_UPLOAD_APP:
            upload_app(&job);
            atomic_store(&s_busy, false);
            break;
        case JOB_UPLOAD_GNSS:
            upload_gnss(&job);
            atomic_store(&s_busy, false);
            break;
        case JOB_REFUSE:
            refuse(job.req, job.size, job.err, NULL, job.retry_in_s);
            atomic_store(&s_busy, false);
            break;
        case JOB_COREDUMP:
            send_coredump(job.req);
            break;
        case JOB_LOG_TEXT:
            send_log_text(job.req, job.flag);
            break;
        case JOB_FLASH_CAPTURE:
            send_flash_capture(job.req);
            break;
        case JOB_FRAME_TIMES:
            send_frame_times(job.req);
            break;
        case JOB_RELEASE_EXPIRED:
            release_expired();
            break;
        case JOB_SAFE_MODE_CLEAR: {
            vTaskDelay(pdMS_TO_TICKS(SAFE_MODE_DELAY_MS));
            esp_err_t err = lifecycle_clear_safe_mode();   /* does not return on success */
            ESP_LOGW(TAG, "leaving safe mode refused: %s", esp_err_to_name(err));
            break;
        }
        }
        finish_request(job.req);
    }
}

/* Hand a request to the worker. @return false (nothing queued) when it is full. */
static bool queue_async(httpd_req_t *req, job_t *job)
{
    if (s_jobs == NULL || uxQueueSpacesAvailable(s_jobs) == 0) {
        return false;
    }
    httpd_req_t *copy = NULL;
    if (httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
        return false;
    }
    job->req = copy;
    /* Before the worker can run: its body reads are not held to the
     * server's request deadline (large uploads at the client's pace). */
    int fd = httpd_req_to_sockfd(req);
    web_conn_worker(fd, true);
    if (xQueueSend(s_jobs, job, 0) != pdTRUE) {
        web_conn_worker(fd, false);
        (void)httpd_req_async_handler_complete(copy);
        return false;
    }
    return true;
}

/* Answer at once and close (the body is not read; what is in flight is drained for a moment, web_linger_close). */
static esp_err_t answer_and_close(httpd_req_t *req, web_err_t err, uint32_t retry_in_s)
{
    return web_refuse_and_close_retry(req, err, retry_in_s);
}

/* Refuse an upload after reading its body (on the worker), or at once when it is busy. */
static esp_err_t refuse_upload(httpd_req_t *req, web_err_t err, uint32_t retry_in_s)
{
    job_t job = { .kind = JOB_REFUSE, .size = (uint32_t)req->content_len, .err = err, .retry_in_s = retry_in_s };
    if (!queue_async(req, &job)) {
        atomic_store(&s_busy, false);
        return answer_and_close(req, err, retry_in_s);
    }
    return ESP_OK;
}

/* Content-Length is required (no chunked uploads). */
static bool has_length(httpd_req_t *req)
{
    return httpd_req_get_hdr_value_len(req, "Content-Length") > 0;
}

esp_err_t web_upload_app(web_req_t *r)
{
    httpd_req_t *req = r->req;
    if (!has_length(req)) {
        return answer_and_close(req, WEB_ERR_LENGTH_REQUIRED, UINT32_MAX);
    }
    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (req->content_len == 0) {
        return answer_and_close(req, WEB_ERR_BAD_REQUEST, UINT32_MAX);
    }
    if (slot == NULL || req->content_len > slot->size) {
        return answer_and_close(req, WEB_ERR_TOO_LARGE, UINT32_MAX);
    }
    if (atomic_exchange(&s_busy, true)) {
        return answer_and_close(req, WEB_ERR_BUSY, UINT32_MAX);
    }
    /* The refusals fw_update would give, checked before the first byte. */
    fw_update_status_t fw;
    bool fw_ok = fw_update_get_status(&fw) == ESP_OK;
    if (lifecycle_update_active() == LIFECYCLE_UPDATE_RECEIVER) {
        return refuse_upload(req, WEB_ERR_JOURNAL_OPEN, UINT32_MAX);
    }
    if (fw_ok && (fw.trial == FW_TRIAL_CONFIRMING || fw.trial == FW_TRIAL_VERIFYING)) {
        return refuse_upload(req, WEB_ERR_APP_TRIAL, fw.trial_left_s);
    }
    if (fw_ok && fw.state != FW_UPDATE_IDLE && fw.state != FW_UPDATE_FAILED) {
        return refuse_upload(req, WEB_ERR_BUSY, UINT32_MAX);
    }
    char v[4];
    bool any_project = web_query_value(r->query, "any_project", v, sizeof(v)) && strcmp(v, "1") == 0;
    bool ack = web_query_value(r->query, "rolled_back_ack", v, sizeof(v)) && strcmp(v, "1") == 0;
    bool reinstall = httpd_req_get_hdr_value_str(req, "X-Reinstall", v, sizeof(v)) == ESP_OK && strcmp(v, "1") == 0;
    job_t job = { .kind = JOB_UPLOAD_APP, .size = (uint32_t)req->content_len, .flag = any_project, .ack = ack,
                  .reinstall = reinstall };
    if (!queue_async(req, &job)) {
        atomic_store(&s_busy, false);
        return answer_and_close(req, WEB_ERR_BUSY, UINT32_MAX);
    }
    return ESP_OK;
}

/* The receiver-update refusals of an upload, before anything is stored. */
static web_err_t gnss_refusal(uint32_t *retry_in_s)
{
    *retry_in_s = UINT32_MAX;
    if (web_rx_hands_off()) {
        return WEB_ERR_RECEIVER_MAINTENANCE;   /* nothing is sent to the receiver */
    }
    fw_update_status_t fw;
    if (fw_update_get_status(&fw) == ESP_OK) {
        switch (fw.gnss_blocked) {
        case FW_GNSS_BLOCK_SAFE_MODE:
            return WEB_ERR_SAFE_MODE;
        case FW_GNSS_BLOCK_APP_TRIAL:
            *retry_in_s = fw.gnss_blocked_left_s;
            return WEB_ERR_APP_TRIAL;
        case FW_GNSS_BLOCK_APP_UPDATE:
            return WEB_ERR_APP_UPDATE;
        case FW_GNSS_BLOCK_JOURNAL_OPEN:
            return WEB_ERR_JOURNAL_OPEN;
        case FW_GNSS_BLOCK_RECEIVER_NOT_STREAMING:
            return WEB_ERR_RECEIVER_NOT_STREAMING;
        case FW_GNSS_BLOCK_UNSUPPORTED_MODEL:
            return WEB_ERR_UNSUPPORTED_MODEL;
        default:
            break;
        }
        if (fw.state == FW_UPDATE_DOWNLOADING_GNSS) {
            return WEB_ERR_BUSY;
        }
    }
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) == ESP_OK && info.pinned) {
        return WEB_ERR_PACKAGE_PINNED;
    }
    return WEB_ERR_COUNT;
}

esp_err_t web_upload_gnss(web_req_t *r)
{
    httpd_req_t *req = r->req;
    if (!has_length(req)) {
        return answer_and_close(req, WEB_ERR_LENGTH_REQUIRED, UINT32_MAX);
    }
    if (req->content_len == 0) {
        return answer_and_close(req, WEB_ERR_BAD_REQUEST, UINT32_MAX);
    }
    if (req->content_len > pkg_store_capacity()) {
        return answer_and_close(req, WEB_ERR_TOO_LARGE, UINT32_MAX);
    }
    if (atomic_exchange(&s_busy, true)) {
        return answer_and_close(req, WEB_ERR_BUSY, UINT32_MAX);
    }
    uint32_t retry = UINT32_MAX;
    web_err_t e = gnss_refusal(&retry);
    if (e != WEB_ERR_COUNT) {
        return refuse_upload(req, e, retry);
    }
    job_t job = { .kind = JOB_UPLOAD_GNSS, .size = (uint32_t)req->content_len };
    if (!queue_async(req, &job)) {
        atomic_store(&s_busy, false);
        return answer_and_close(req, WEB_ERR_BUSY, UINT32_MAX);
    }
    return ESP_OK;
}

esp_err_t web_download_coredump(web_req_t *r)
{
    diag_coredump_info_t info;
    if (diag_coredump_info(&info) != ESP_OK || !info.present || info.size == 0) {
        return web_send_err(r->req, WEB_ERR_NO_COREDUMP);
    }
    job_t job = { .kind = JOB_COREDUMP };
    if (!queue_async(r->req, &job)) {
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    return ESP_OK;
}

esp_err_t web_download_flash_capture(web_req_t *r)
{
    if (!um98x_flash_capture_available()) {
        return web_send_err(r->req, WEB_ERR_NOT_FOUND);
    }
    job_t job = { .kind = JOB_FLASH_CAPTURE };
    if (!queue_async(r->req, &job)) {
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    return ESP_OK;
}

esp_err_t web_download_frame_times(web_req_t *r)
{
#if CONFIG_GNSS_LINK_BENCH_HOOKS
    job_t job = { .kind = JOB_FRAME_TIMES };
    if (!queue_async(r->req, &job)) {
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    return ESP_OK;
#else
    return web_send_err(r->req, WEB_ERR_NOT_FOUND);
#endif
}

esp_err_t web_download_log_text(web_req_t *r, bool prev_boot)
{
    job_t job = { .kind = JOB_LOG_TEXT, .flag = prev_boot };
    if (!queue_async(r->req, &job)) {
        return web_send_err(r->req, WEB_ERR_BUSY);
    }
    return ESP_OK;
}

esp_err_t web_worker_clear_safe_mode(void)
{
    job_t job = { .kind = JOB_SAFE_MODE_CLEAR };
    if (s_jobs == NULL || xQueueSend(s_jobs, &job, 0) != pdTRUE) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t web_worker_init(void)
{
    if (s_jobs != NULL) {
        return ESP_OK;
    }
    s_jobs = xQueueCreate(JOBS_MAX, sizeof(job_t));
    if (s_jobs == NULL) {
        return ESP_ERR_NO_MEM;
    }
    const esp_timer_create_args_t args = { .callback = on_expiry, .name = "web_staged" };
    esp_err_t err = esp_timer_create(&args, &s_expiry);
    if (err != ESP_OK) {
        return err;
    }
    /* Internal-RAM stack (the default for task stacks): uploads write flash. */
    if (xTaskCreate(worker, "web_io", WORKER_STACK, NULL, WORKER_PRIO, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
