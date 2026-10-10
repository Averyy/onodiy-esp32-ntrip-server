/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * JSON bodies of the API (pure logic over snapshots of the component
 * structs, no RTOS or driver calls, host-testable). The server gathers the
 * snapshots (web_api.c, web_ws.c); everything here only formats them, so
 * the field rules of the contract (null sentinels, enum strings, what the
 * public form leaves out, passwords never written) are tested on the host.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "diag.h"
#include "fw_update.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "health.h"
#include "ledger.h"
#include "lifecycle.h"
#include "network.h"
#include "ntrip_server.h"
#include "pkg_store.h"
#include "rtcm_hub.h"
#include "station.h"
#include "um98x_flash.h"
#include "web_core.h"
#include "web_json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** API version (GET /api/session, /api/status, WebSocket hello). */
#define WEB_API_VERSION 1

/** Upper bound of caster slots (CONFIG_NTRIP_MAX_CASTERS range). */
#define WEB_MAX_SLOTS 8u

/* ---- receiver update session view ------------------------------------------------- */

/** `journal` of /api/status and /api/gnss/flash. */
typedef enum {
    WEB_JOURNAL_NONE = 0,
    WEB_JOURNAL_RUNNING,
    WEB_JOURNAL_WAITING,
    WEB_JOURNAL_UNREADABLE,
    WEB_JOURNAL_UNKNOWN,
} web_journal_t;

/** Journal view of a station status (safe mode: nothing runs, so an open one waits). */
web_journal_t web_journal_view(const station_status_t *st, bool safe_mode);
const char *web_journal_name(web_journal_t j);

/** What may be asked of an open session now (the `actions` of /api/gnss/flash). */
typedef struct {
    web_journal_t journal;
    bool safe_mode;
    bool app_trial;
    bool app_update;
    bool receiver_in_loader;
    bool station_running;      /**< station initialised and started (not safe mode, no failed init) */
    bool flash_available;      /**< receiver flashing is built */
    bool maintenance;          /**< receiver maintenance: nothing is sent to the receiver */
} web_flash_ctx_t;

typedef enum {
    WEB_FLASH_RETRY = 0,
    WEB_FLASH_RESTART_RECEIVER,
    WEB_FLASH_CLEAR,
} web_flash_action_t;

/**
 * Why an action is refused now ("no_journal", "not_waiting", "safe_mode",
 * "settings_unavailable", "receiver_maintenance", "app_trial", "app_update",
 * "not_available_yet", "unreadable"), NULL when it is allowed. `err` gets the error code the POST
 * answers with.
 */
const char *web_flash_action_why(const web_flash_ctx_t *c, web_flash_action_t a, web_err_t *err);

/* ---- status ----------------------------------------------------------------------- */

/** Everything /api/status (and the WebSocket status topic) is built from. */
typedef struct {
    uint32_t uptime_s;
    char hostname[33];             /**< hostname in use */
    char version[32];              /**< running app version */
    int64_t time_unix;             /**< 0 = no trusted clock */
    ledger_clock_t clock;
    bool settings_available;
    lifecycle_boot_info_t boot;
    network_status_t net;
    station_status_t st;
    uint32_t rtcm_age_ms;          /**< gnss_link: since the last CRC-valid RTCM frame (UINT32_MAX = never) */
    bool uart_closed;              /**< gnss_link: the receiver UART is closed and being reopened (link opened once, link_open false) */
    gnss_sys_t best_sys;
    uint8_t best_bands;
    bool obs_valid;                /**< best_sys / best_bands come from a summary */
    size_t slots;
    bool cfg_valid;                /**< configs loaded (not in safe mode) */
    ntrip_caster_cfg_t cfg[WEB_MAX_SLOTS];        /**< read without secrets */
    ntrip_caster_status_t cst[WEB_MAX_SLOTS];
    health_status_t health;
    fw_update_status_t fw;
    uint32_t protected_down_today_s;
    uint32_t ledger_budget_left_s;
    bool flash_available;
} web_status_snap_t;

/** Sections of the status object, in the order they are written. */
typedef enum {
    WEB_SEC_DEVICE = 0,
    WEB_SEC_SAFE_MODE,
    WEB_SEC_NETWORK,
    WEB_SEC_RECEIVER,
    WEB_SEC_HEALTH,
    WEB_SEC_ALERTS,
    WEB_SEC_AVAILABILITY,
    WEB_SEC_UPDATE,
    WEB_SEC_COUNT,
} web_section_t;

/** Name of a section ("device"); "casters" is handled per slot. */
const char *web_section_name(web_section_t sec);

/** True if the section is part of the public form. */
bool web_section_public(web_section_t sec);

/** The whole status object (admin or public form). */
void web_build_status(wj_t *w, const web_status_snap_t *s, bool pub);

/** One section as an object member (`"device":{...}`); nothing for a section the form leaves out. */
void web_build_section(wj_t *w, const web_status_snap_t *s, web_section_t sec, bool pub);

/** One caster summary as an array element. */
void web_build_caster_summary(wj_t *w, const web_status_snap_t *s, size_t slot, bool pub);

/**
 * Data-rate-adjusted budget of the protected slot, else the ledger's;
 * UINT32_MAX (null) without an enabled protected slot or a trusted clock.
 */
uint32_t web_budget_left_s(const web_status_snap_t *s);

/* ---- receiver ---------------------------------------------------------------------- */

typedef struct {
    const station_status_t *st;
    gnss_link_stats_t link;
    bool autocfg;
    health_recovery_mode_t recovery;   /**< gnss/recovery as stored (out of range reads as full) */
    um98x_base_profile_t profile;
    gnss_obs_arp_t arp;            /**< valid = false before the first frame */
    gnss_obs_1033_t rtcm1033;      /**< valid = false before the first one that decoded */
} web_gnss_snap_t;

/** GET /api/gnss. */
void web_build_gnss(wj_t *w, const web_gnss_snap_t *g);

/** A profile object (as stored) under `key`. */
void web_build_profile(wj_t *w, const char *key, const um98x_base_profile_t *p);

/** GET /api/gnss/signals. `stats` NULL = message statistics not available. */
void web_build_signals(wj_t *w, const gnss_obs_summary_t *sum, const gnss_obs_msg_stat_t *stats, size_t nstats,
                       const um98x_base_profile_t *profile, bool rtcm1230_supported);

/** GET /api/sky. `reason` NULL when available. */
void web_build_sky(wj_t *w, const gnss_obs_sat_t *sats, size_t count, const char *reason);

/** GET /api/gnss/flash. */
void web_build_flash(wj_t *w, const station_status_t *st, const web_flash_ctx_t *ctx, const char *receiver_text);

/**
 * Error of a refused POST /api/gnss/maintenance (station_maint_why_t): a
 * receiver update session (journal_open), a running receiver session or job
 * (busy, *reason "receiver_session": offer it again when that ends), a
 * duration not offered (invalid_field, *field "duration_s"), the station
 * not running (safe_mode in safe mode, else internal); "end now" without
 * the state (STATION_MAINT_NOT_ON): not_in_maintenance. *field and *reason
 * are NULL when the body carries none.
 */
web_err_t web_maint_refusal(station_maint_why_t why, bool safe_mode, const char **field, const char **reason);

/** Error of a refused POST /api/gnss/live (station_live_why_t): journal_open (receiver update), safe_mode, else internal. */
web_err_t web_live_refusal(station_live_why_t why, bool safe_mode);

/** Live view window limits (the station's Kconfig). */
typedef struct {
    uint32_t default_min;          /**< CONFIG_STATION_LIVE_VIEW_MIN */
    uint32_t max_min;              /**< CONFIG_STATION_LIVE_VIEW_MAX_MIN */
    uint32_t idle_s;               /**< CONFIG_STATION_LIVE_VIEW_IDLE_S */
} web_live_limits_t;

/** The live view window (station_status_t.live) under `key`. */
void web_build_live_window(wj_t *w, const char *key, const station_live_status_t *win, const web_live_limits_t *lim);

typedef struct {
    const station_status_t *st;    /**< window, receiver state, base position */
    web_live_limits_t limits;
    const gnss_obs_live_sat_t *sats;
    size_t count;
    const gnss_obs_live_t *totals; /**< NULL = not available (then no satellites either) */
} web_live_snap_t;

/** GET/POST /api/gnss/live. */
void web_build_live(wj_t *w, const web_live_snap_t *l);

/* ---- casters ------------------------------------------------------------------------- */

/** Caster config under `key` (password never written, only password_set). NULL cfg writes null. */
void web_build_caster_config(wj_t *w, const char *key, const ntrip_caster_cfg_t *cfg);

/** Caster status under `key`; protected-slot fields null unless `protected_slot`. */
void web_build_caster_status(wj_t *w, const char *key, const ntrip_caster_status_t *st, bool protected_slot);

/** {"slot", "config", "status"} as an array element or a top-level object (key NULL either way). */
void web_build_caster_entry(wj_t *w, size_t slot, const ntrip_caster_cfg_t *cfg, const ntrip_caster_status_t *st);

/** The "onocoy_preset" member. */
void web_build_onocoy_preset(wj_t *w, const ntrip_caster_cfg_t *preset, uint16_t tls_port);

/* ---- Wi-Fi, system, updates -------------------------------------------------------------- */

typedef struct {
    const network_status_t *net;
    const network_scan_entry_t *scan;
    size_t scan_count;
    uint32_t scan_age_s;           /**< UINT32_MAX = never scanned */
    bool scan_running;
    bool ap_password_set;
} web_wifi_snap_t;

/** GET /api/wifi. */
void web_build_wifi(wj_t *w, const web_wifi_snap_t *s);

typedef struct {
    const char *hostname;          /**< stored ("" = none, the active one is used) */
    const char *hostname_active;
    bool public_status;
    bool setup_done;
    bool settings_available;
    const char *version;
    const char *project;
    const char *idf;
    const uint8_t *mac;            /**< 6 bytes */
    const char *chip_model;
    unsigned chip_revision;        /**< major * 100 + minor */
    uint32_t flash_mb;
    uint32_t psram_mb;
    int reboot_required;           /**< -1 = leave the field out */
} web_system_snap_t;

/** GET/PUT /api/system. */
void web_build_system(wj_t *w, const web_system_snap_t *s);

/** Outcome of the last update job the server saw end (`last_result`). */
typedef struct {
    bool valid;
    char job[16];                  /**< "check", "app_update", ... */
    char result[24];               /**< "ok", "not_available_yet", ... */
    esp_err_t err;                 /**< ESP_OK: no message */
    uint8_t failed_rule;           /**< package_rejected only (0 = none) */
    bool cross_layout;
} web_last_result_t;

typedef struct {
    const fw_update_status_t *fw;
    const station_status_t *st;
    const web_last_result_t *last;
    bool checks_enabled;
    const char *release_repo;      /**< "owner/name" (NULL or "" = null) */
    bool signed_manifests;         /**< the build accepts signed manifests only */
    uint32_t budget_left_s;        /**< UINT32_MAX = null */
    bool flash_available;
    const char (*rolled_back)[32]; /**< fw_update_get_rolled_back(), newest first */
    size_t rolled_back_count;
} web_update_snap_t;

/** GET /api/update. */
void web_build_update(wj_t *w, const web_update_snap_t *u);

/**
 * Result code of a finished update job from its esp_err_t ("ok",
 * "refused", "download_failed", ...). `job` as in web_last_result_t.
 */
const char *web_result_code(const char *job, esp_err_t err);

/* ---- uploads ---------------------------------------------------------------------------- */

typedef struct {
    um98x_flash_pkg_facts_t facts;
    uint32_t expires_in_s;
    const station_status_t *st;
    uint32_t expected_downtime_s;
    uint32_t budget_left_s;        /**< UINT32_MAX = null */
    bool flash_available;
} web_staged_snap_t;

/** POST/GET /api/upload/gnss. */
void web_build_staged(wj_t *w, const web_staged_snap_t *s);

/* ---- diagnostics ------------------------------------------------------------------------------ */

typedef struct {
    const health_status_t *health;
    const lifecycle_boot_info_t *boot;
    uint32_t uptime_s;
    const lifecycle_boot_record_t *history;
    size_t history_count;
    const gnss_link_stats_t *link;
    const rtcm_hub_stats_t *hub;
    const station_status_t *st;
    const gnss_obs_summary_t *obs;
    const network_status_t *net;
    const diag_coredump_info_t *coredump;
    const diag_heap_info_t *heap;          /**< NULL = the PSRAM low-water and largest block are left out */
    const diag_journal_info_t *journal;    /**< NULL = "journal" is null */
    const diag_log_stats_t *log;           /**< NULL = "log" is null */
    const char *inhibited_by;      /**< NULL = none */
    lifecycle_update_t update_slot;
} web_health_snap_t;

/** GET /api/health. */
void web_build_health(wj_t *w, const web_health_snap_t *h);

/** How long /api/status says a BOOT-button factory reset was refused (s). */
#define WEB_FACTORY_REFUSED_SHOW_S 600u

typedef struct {
    ledger_clock_t clock;
    int32_t second_of_day;         /**< -1 = no trusted clock */
    size_t slots;
    const ntrip_caster_cfg_t *cfg; /**< NULL when not loaded (safe mode) */
    int protected_slot;            /**< -1 = none */
    const ntrip_caster_status_t *protected_status;
    uint32_t down_today_s;
    uint32_t budget_left_s;        /**< UINT32_MAX = null */
    bool today_valid;
    const ledger_day_t *today;
    const ledger_day_t *history;
    size_t history_count;
} web_avail_snap_t;

/** GET /api/availability. */
void web_build_availability(wj_t *w, const web_avail_snap_t *a);

/** PRN shown in the sky view for a gnss_obs satellite number (QZSS, SBAS offsets). */
unsigned web_sky_prn(gnss_sys_t sys, uint8_t prn);

/** GET /api/journal. */
void web_build_journal(wj_t *w, const diag_journal_entry_t *e, size_t count, uint32_t cursor, bool more);

/** Records the connection history lists (GET /api/connections). */
#define WEB_HISTORY_MAX 64u

/** A journal kind of the connection history: caster_down, caster_up, wifi_up, wifi_disconnect. */
bool web_history_kind(uint16_t kind);

/**
 * Collect the connection history from one journal page (oldest first, as
 * diag_journal_read returns it): its connection records with seq <= hi are
 * appended to out newest first, while out (*n of max) has room.
 * @return true while out has room for more.
 */
bool web_history_collect(const diag_journal_entry_t *page, size_t count, uint32_t hi, diag_journal_entry_t *out,
                      size_t *n, size_t max);

/**
 * GET /api/connections: `e` newest first; slot names from `cfg` (NULL in
 * safe mode: null); `complete` = the whole journal was searched.
 */
void web_build_connections(wj_t *w, const diag_journal_entry_t *e, size_t count, const ntrip_caster_cfg_t *cfg,
                           size_t slots, bool complete);

/** GET /api/diag/tasks. */
void web_build_tasks(wj_t *w, const diag_task_info_t *t, size_t count);

/** GET /api/logs (JSON form). */
void web_build_logs(wj_t *w, uint32_t cursor, bool more, bool gap, const char *text, size_t len);

/* ---- errors and WebSocket messages ---------------------------------------------------------- */

/**
 * Error body. `message` NULL = the code's default text; `field`, `by`,
 * `reason` NULL = null / left out (reason only appears when set);
 * retry_in_s UINT32_MAX = null.
 */
void web_build_error(wj_t *w, web_err_t err, const char *message, const char *field, uint32_t retry_in_s,
                     const char *by, const char *reason);

/** Progress of a job (`progress` messages). */
typedef struct {
    const char *job;               /**< "check", "app_update", "app_upload", "gnss_update", "gnss_upload", "gnss_flash" */
    const char *phase;
    int pct;                       /**< -1 = left out */
    int64_t bytes;                 /**< -1 = left out (with total) */
    int64_t total;
    bool done;
    const char *result;            /**< final message only */
    esp_err_t err;                 /**< final message: message = its name when not ESP_OK */
    const um98x_flash_progress_t *flash; /**< gnss_flash: the flash counters */
} web_progress_t;

void web_build_progress(wj_t *w, const web_progress_t *p);

#ifdef __cplusplus
}
#endif
