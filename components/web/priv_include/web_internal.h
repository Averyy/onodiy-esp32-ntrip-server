/* SPDX-License-Identifier: MIT
 * Station adapter. HTTP callbacks/tick run on the HTTP task; events only
 * copy data or set flags. Blocking job bodies run on the shared worker.
 */
#pragma once
#include <stdatomic.h>
#include "web_shell.h"
#include "web_shell_auth.h"
#include "web_build.h"
#include "web_core.h"
#define web_station_name web_shell_name
#define web_station_name_set web_shell_name_set
#define WEB_FLASH_AVAILABLE station_flash_available()
#define WEB_FLASH_REASON station_flash_reason()
#define WEB_ONOCOY_TLS_PORT 2121u
#define WEB_MAP_TILE_ORIGIN "https://tiles.openfreemap.org"
#define WEB_PROJECT_NAME "onodiy-esp32-ntrip-server"
#define WEB_IO_CHUNK 4096u
extern const web_route_t web_api_routes[];
extern const size_t web_api_route_count;
bool web_api_path_known(const char *path);
void web_station_tick(void *ctx, bool second, bool half);
void web_ws_note_flash_progress(const um98x_flash_progress_t *p, bool final);
void web_ws_progress(const web_progress_t *p);
void web_ws_event(const char *json);
size_t web_ws_client_count(void);
/* ---- REST helpers (web_api.c) ------------------------------------------------------------------------ */

/** Gather the status snapshot (into PSRAM storage the caller owns). */
void web_snap_status(web_status_snap_t *s);

/** Record the outcome of an update job for GET /api/update. */
void web_note_result(const char *job, esp_err_t err, const char *result);

/** Note that the server started an update job (attribution of the next end). */
void web_note_job_started(const char *job);

/**
 * Watch fw_update's state: progress of a running job, its result when it
 * ends. Called on FW_UPDATE_EVENT (`event`) and twice a second.
 */
void web_track_fw_state(bool event);

/**
 * Receiver maintenance is on (gnss_link refuses every write and lease): the
 * receiver-update refusals answer receiver_maintenance then.
 */
bool web_rx_hands_off(void);

/** Protected-slot budget for the update dialogs (any task); UINT32_MAX = unknown. */
uint32_t web_budget_now(void);

/** The network reported a finished scan. */
void web_note_scan_done(void);

/* ---- worker (web_worker.c) ---------------------------------------------------------------------------------- */

esp_err_t web_station_jobs_init(void);

/** POST /api/upload/app and /api/upload/gnss, after the request checks. */
esp_err_t web_upload_app(web_req_t *r);
esp_err_t web_upload_gnss(web_req_t *r);

/** GET /api/coredump and GET /api/logs?format=text. */
esp_err_t web_download_coredump(web_req_t *r);
esp_err_t web_download_log_text(web_req_t *r, bool prev_boot);
/** GET /api/diag/flash-capture (bench builds; 404 otherwise). */
esp_err_t web_download_flash_capture(web_req_t *r);
/** GET /api/diag/frame-times (bench builds with the link's bench hooks; 404 otherwise). */
esp_err_t web_download_frame_times(web_req_t *r);

/** An upload is being received or refused (one at a time). */
bool web_upload_busy(void);

/**
 * DELETE /api/journal: diag_journal_clear() on the worker, which answers;
 * 409 busy at once while a clear is queued or runs, or an upload holds the
 * worker.
 */
esp_err_t web_clear_journal(web_req_t *r);

/** Uploaded receiver package waiting for confirmation (matches the staged one). */
bool web_staged_get(um98x_flash_pkg_facts_t *facts, uint32_t *expires_in_s);
/** Forget it (released, confirmed or replaced). */
void web_staged_forget(void);

/** Run lifecycle_clear_safe_mode() on the worker after the response went out. */
esp_err_t web_worker_clear_safe_mode(void);

/** Progress of an upload for the WebSocket (job NULL = none). */
typedef struct {
    const char *job;     /**< "app_upload" / "gnss_upload" */
    const char *phase;
    uint32_t bytes;
    uint32_t total;
    bool done;
    const char *result;
    esp_err_t err;
    uint32_t seq;        /**< changes with every update */
} web_upload_progress_t;

void web_upload_progress(web_upload_progress_t *out);

/** Responses still being written by the worker (the reboot hook waits for them). */
int web_worker_pending_responses(void);
