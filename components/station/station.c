/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Station supervisor task. Decisions live in the pure cores
 * (station_core.c: states, jobs, schedules; station_obs.c: the observed
 * set); this file runs the jobs they name against the GNSS link, the command
 * layer, the casters, the ledger and the settings, and publishes the status.
 *
 * Threads:
 *  - GNSS RX task: the link sinks (RTCM -> hub + observation summary + the
 *    observed-set tracker; lines -> command layer + observation summary;
 *    framer resets -> hub sticky copies; loader prompt and restart evidence
 *    -> flags for the station task). Never blocks, never logs.
 *  - station task (core 1): the only lease holder; runs one job at a time,
 *    waits in slices of at most 1 s and feeds the task watchdog.
 *  - callers of the request API (web, health, fw_update): set flags in the
 *    core under s_lock and wake the station task; nothing blocking.
 */
#include "station.h"

#include <string.h>
#include <time.h>

#include "board.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "gnss_rtcm3.h"
#include "ledger.h"
#include "lifecycle.h"
#include "ntrip_server.h"
#include "pkg_store.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "settings.h"
#include "station_core.h"
#include "station_obs.h"
#include "um98x_ctl.h"
#include "um98x_flash.h"

static const char *TAG = "station";

/* Boot-time race into the receiver's loader. PROVISIONAL, to be confirmed on hardware. */
#define RACE_MAX_MS 10000u

#define TASK_PRIO        10      /* below the GNSS RX task, above casters and web */
#define TASK_CORE        1       /* the GNSS path's core; Wi-Fi runs on core 0 */
#define NOTIFY_IDX       0       /* the station task waits on nothing else */
#define LOOP_WAIT_MS     1000    /* longest sleep between two decisions (and watchdog feeds) */
#define LEASE_WAIT_MS    1000
#define FRESH_MS         10000   /* "the receiver streams": valid RTCM or a valid line this recently */
#define BREAK_MS         100     /* UART break before a rescan. PROVISIONAL, to be confirmed on hardware */
#define LISTEN_MS        1500    /* listen-only scan: per baud. PROVISIONAL, to be confirmed on hardware */
#define LISTEN_POLL_MS   100
#define POS_MAX_AGE_MS   5000    /* position check: a 1005 at most this old */
#define TRUSTED_UNIX     1700000000   /* earlier wall-clock times are not a set clock */

_Static_assert(STN_IDLE == (int)STATION_STATE_IDLE && STN_DETECTING == (int)STATION_STATE_DETECTING &&
                   STN_CONFIGURING == (int)STATION_STATE_CONFIGURING &&
                   STN_STREAMING == (int)STATION_STATE_STREAMING && STN_FLASHING == (int)STATION_STATE_FLASHING &&
                   STN_FLASH_RECOVERY == (int)STATION_STATE_FLASH_RECOVERY &&
                   STN_RECOVERING == (int)STATION_STATE_RECOVERING &&
                   STN_NO_RECEIVER == (int)STATION_STATE_NO_RECEIVER,
               "core states mirror station_state_t");
_Static_assert(STN_ALERT_UNSUPPORTED_MODEL == (uint32_t)STATION_ALERT_UNSUPPORTED_MODEL &&
                   STN_ALERT_CONFIG_FAILED == (uint32_t)STATION_ALERT_CONFIG_FAILED &&
                   STN_ALERT_POSITION_MISMATCH == (uint32_t)STATION_ALERT_POSITION_MISMATCH &&
                   STN_ALERT_ANTENNA_MOVED == (uint32_t)STATION_ALERT_ANTENNA_MOVED &&
                   STN_ALERT_RECEIVER_ATTENTION == (uint32_t)STATION_ALERT_RECEIVER_ATTENTION &&
                   STN_ALERT_RECEIVER_IN_LOADER == (uint32_t)STATION_ALERT_RECEIVER_IN_LOADER,
               "core alerts mirror station_alert_t");
_Static_assert(STN_REC_RESYNC == (int)STATION_RECOVER_RESYNC &&
                   STN_REC_LINK_RESTART == (int)STATION_RECOVER_LINK_RESTART &&
                   STN_REC_RESCAN == (int)STATION_RECOVER_RESCAN &&
                   STN_REC_RECONFIGURE == (int)STATION_RECOVER_RECONFIGURE &&
                   STN_REC_RECEIVER_RESET == (int)STATION_RECOVER_RECEIVER_RESET,
               "core recovery actions mirror station_recovery_t");

ESP_EVENT_DEFINE_BASE(STATION_EVENT);

/* ---- shared state --------------------------------------------------------- */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

/* Under s_lock. */
static stn_t s_core;
static stn_obs_t s_obs;               /* observed-set tracker, fed by the RX task */
static uint64_t s_rtcm_excluded;      /* frames of types 1101-1107 kept out of the hub */
static uint32_t s_arp_frames;         /* 1005/1006 frames published */
static station_status_t s_status;     /* published by the station task */
static bool s_status_valid;
static int64_t s_lease_release_us = INT64_MIN;   /* INT64_MIN = no release this boot */

/* Journal as read at boot (early step or init); changed only by the user's
 * clear. Under s_lock once the station runs. */
static esp_err_t s_journal_err = ESP_ERR_NOT_FOUND;
static um98x_flash_journal_t s_journal;
static bool s_journal_read;
/* An open journal whose recovery this build cannot run: it waits for the
 * user like one in "needs user" (RAM only: the record stays as written, so
 * a build that can recover it still finds it unchanged). */
static bool s_journal_parked;

/* RX task -> station task (single writer each). */
static volatile bool s_prompt_seen;
static volatile bool s_restart_pending;
static volatile uint32_t s_last_line_ms;
static volatile bool s_line_seen;
static volatile bool s_journal_cleared;

static TaskHandle_t s_task;
static bool s_inited;
static bool s_started;

/* Reboot inhibit and update slot held for an open flash session in full
 * lockout (taken again at boot). */
static bool s_inhibit_held;
static lifecycle_inhibit_t s_inhibit;
static bool s_update_claimed;

/* RAW lease taken by the early boot step (recovery actions build only). */
static gnss_link_lease_t s_boot_lease;

/* Station task only. */
static um98x_base_profile_t s_profile;
static um98x_info_t s_stored_info;       /* gnss/model and gnss/fw as stored (not confirmed) */
static stn_obs_t s_obs_snap;
static um98x_apply_result_t s_apply;
static station_status_t s_pub;
static gnss_link_lease_t s_lease;        /* the running job's lease */
static gnss_link_lease_t s_hold_lease;   /* listen-only while a flash journal is open */
static bool s_paused;
static bool s_rtcm_ok;
static uint32_t s_prompt_baud;
static uint32_t s_arp_fed;
static station_state_t s_posted_state = STATION_STATE_IDLE;

/* ---- small helpers -------------------------------------------------------- */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static uint32_t now_ms32(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void wake(void)
{
    TaskHandle_t t = s_task;
    if (t != NULL) {
        xTaskNotifyGiveIndexed(t, NOTIFY_IDX);
    }
}

static void feed_watchdog(void)
{
#if CONFIG_ESP_TASK_WDT_EN
    if (esp_task_wdt_status(NULL) == ESP_OK) {
        (void)esp_task_wdt_reset();
    }
#endif
}

static bool journal_open(esp_err_t err)
{
    /* Only "not found" means no journal. Anything else (damaged, a newer
     * schema, degraded settings) may hide an interrupted flash. */
    return err != ESP_ERR_NOT_FOUND;
}

static bool journal_waits_for_user(esp_err_t err, const um98x_flash_journal_t *j)
{
    return err != ESP_OK || j->state == UM98X_JOURNAL_NEEDS_USER || s_journal_parked;
}

/* Settings unavailable this boot: the journal's state is unknown. Nothing is
 * sent to the receiver, but there is nothing to clear either. */
static bool journal_unknown(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE;
}

/* Full lockout: reboot inhibit and the update slot, together. */
static void lockout_take(void)
{
    if (!s_update_claimed) {
        esp_err_t err = lifecycle_update_claim(LIFECYCLE_UPDATE_RECEIVER);
        if (err == ESP_OK) {
            s_update_claimed = true;
        } else {
            ESP_LOGE(TAG, "update slot for the open receiver flash not taken: %s", esp_err_to_name(err));
        }
    }
    if (!s_inhibit_held) {
        esp_err_t err = lifecycle_inhibit_acquire("receiver flash", &s_inhibit);
        if (err == ESP_OK) {
            s_inhibit_held = true;
        } else {
            ESP_LOGE(TAG, "reboot inhibit for the open receiver flash not taken: %s", esp_err_to_name(err));
        }
    }
}

static void lockout_release(void)
{
    if (s_update_claimed) {
        s_update_claimed = false;
        lifecycle_update_release(LIFECYCLE_UPDATE_RECEIVER);
    }
    if (s_inhibit_held) {
        s_inhibit_held = false;
        lifecycle_inhibit_release(s_inhibit);   /* a queued reboot may follow at once */
    }
}

static bool app_update_running(void)
{
    return lifecycle_update_active() == LIFECYCLE_UPDATE_APP;
}

static bool autocfg_enabled(void)
{
    uint8_t v = 1;   /* default: configure the receiver automatically */
    if (settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_AUTOCFG, &v) != ESP_OK) {
        v = 1;
    }
    return v != 0;
}

static bool rtcm1230_supported(void)
{
    uint8_t v = 0;
    return settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RTCM1230_OK, &v) == ESP_OK && v != 0;
}

static bool model_known(void)
{
    char model[sizeof(s_stored_info.model)];
    return settings_get_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MODEL, model, sizeof(model)) == ESP_OK &&
           model[0] != '\0';
}

/* gnss/baud, 0 if none (the candidate list then starts at 115200). */
static uint32_t stored_baud(void)
{
    uint32_t baud = 0;
    if (settings_get_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, &baud) != ESP_OK) {
        baud = 0;
    }
    return baud;
}

/* Where the link listens between detection passes: what it opens at. */
static uint32_t home_baud(void)
{
    uint32_t baud = stored_baud();
    return um98x_baud_supported(baud) ? baud : CONFIG_GNSS_LINK_DEFAULT_BAUD;
}

static void persist_baud(uint32_t baud)
{
    if (baud == 0 || stored_baud() == baud) {
        return;
    }
    esp_err_t err = settings_set_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, baud);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "gnss/baud not saved: %s", esp_err_to_name(err));
    }
}

static int64_t unix_now(void)
{
    time_t t = time(NULL);
    return t >= TRUSTED_UNIX ? (int64_t)t : 0;   /* 0: locked before the clock was set */
}

static void read_journal(void)
{
    um98x_flash_journal_t j;
    memset(&j, 0, sizeof(j));
    esp_err_t err = um98x_flash_journal_get(&j);
    portENTER_CRITICAL(&s_lock);
    s_journal_err = err;
    s_journal = j;
    s_journal_read = true;
    portEXIT_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "receiver flash journal open (state %u, phase %u, %u transfer(s)): recovery runs first",
                 (unsigned)j.state, (unsigned)j.phase, (unsigned)j.transfers);
    } else if (journal_unknown(err)) {
        ESP_LOGW(TAG, "settings unavailable: receiver flash journal state unknown, nothing is sent to the receiver");
    } else if (journal_open(err)) {
        ESP_LOGW(TAG, "receiver flash journal unusable (%s): waiting for the user, nothing is sent to the receiver",
                 esp_err_to_name(err));
    }
}

static void load_profile(void)
{
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t len = sizeof(buf);
    if (settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, buf, &len) == ESP_OK &&
        um98x_profile_decode(buf, len, &s_profile)) {
        return;
    }
    /* Default: the station ID comes from the base MAC, stable per device. */
    uint8_t mac[6] = { 0 };
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
        ESP_LOGW(TAG, "base MAC unreadable: station ID 0");
    }
    um98x_profile_default(&s_profile, um98x_station_id_from_mac(mac));
}

/* The profile (and the locked position, or none) in one batch. */
static bool store_profile(const um98x_base_profile_t *p, const um98x_base_position_t *pos)
{
    uint8_t blob[UM98X_PROFILE_BLOB_MAX];
    uint8_t pblob[UM98X_POSITION_BLOB_MAX];
    size_t len = um98x_profile_encode(p, blob, sizeof(blob));
    size_t plen = pos != NULL ? um98x_position_encode(pos, pblob, sizeof(pblob)) : 0;
    if (len == 0 || (pos != NULL && plen == 0)) {
        return false;
    }
    settings_batch_t b;
    esp_err_t err = settings_batch_begin(SETTINGS_NS_GNSS, &b);
    if (err == ESP_OK) {
        err = settings_batch_set_blob(b, SETTINGS_KEY_GNSS_PROFILE, blob, len);
        if (err == ESP_OK && pos != NULL) {
            err = settings_batch_set_blob(b, SETTINGS_KEY_GNSS_BASE_POS, pblob, plen);
        }
        if (err == ESP_OK) {
            err = settings_batch_commit(b);
        } else {
            settings_batch_abort(b);
        }
    }
    if (err == ESP_OK && pos == NULL) {
        esp_err_t e = settings_erase_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BASE_POS);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND) {
            err = e;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "profile not saved: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void load_stored_identity(void)
{
    memset(&s_stored_info, 0, sizeof(s_stored_info));
    if (settings_get_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MODEL, s_stored_info.model,
                         sizeof(s_stored_info.model)) != ESP_OK) {
        s_stored_info.model[0] = '\0';
    }
    if (settings_get_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FW, s_stored_info.firmware,
                         sizeof(s_stored_info.firmware)) != ESP_OK) {
        s_stored_info.firmware[0] = '\0';
    }
}

/* Static part of the applied check: live firmware == gnss/applied_fw,
 * gnss/applied_hash == the profile's hash, and the 1230 result stored
 * (missing = never applied). */
static bool applied_static_ok(const um98x_info_t *info)
{
    uint32_t hash = 0;
    char fw[sizeof(info->firmware)];
    uint8_t v = 0;
    if (settings_get_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_APPLIED, &hash) != ESP_OK ||
        hash != um98x_profile_hash(&s_profile)) {
        return false;
    }
    if (settings_get_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_APPLIED_FW, fw, sizeof(fw)) != ESP_OK ||
        strcmp(fw, info->firmware) != 0) {
        return false;
    }
    return settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RTCM1230_OK, &v) == ESP_OK;
}

/* ---- GNSS link sinks (GNSS RX task: never block, never log) -------------- */

/* RTCM types the hub never carries: 1101-1107 are SBAS MSM numbers the
 * receiver may reuse for its own messages. */
static bool rtcm_type_excluded(uint16_t msg_type)
{
    return msg_type >= 1101 && msg_type <= 1107;
}

static void on_rtcm_frame(const uint8_t *frame, size_t len, uint16_t msg_type, void *ctx)
{
    (void)ctx;
    /* If the hub is not ready the frame is lost; the hub counts the failed
     * publish and the link counters still show it as received, so health
     * sees the mismatch. */
    bool excluded = rtcm_type_excluded(msg_type);
    if (!excluded) {
        (void)rtcm_hub_publish(frame, len, msg_type);
    }
    gnss_obs_feed_rtcm(frame, len, msg_type);
    /* A busy constellation comes as several MSMs of one type per epoch:
     * the observed-set tracker counts epochs. */
    gnss_rtcm3_msm_t msm;
    bool has_epoch = !excluded && gnss_rtcm3_msm_type(msg_type, NULL, NULL) && gnss_rtcm3_msm_decode(frame, len, &msm);
    int64_t t = now_ms();
    portENTER_CRITICAL(&s_lock);
    if (excluded) {
        s_rtcm_excluded++;
    }
    if (msg_type == 1005 || msg_type == 1006) {
        s_arp_frames++;
    }
    stn_obs_feed(&s_obs, msg_type, has_epoch, has_epoch ? msm.epoch_time : 0, t);
    portEXIT_CRITICAL(&s_lock);
}

static void on_text_line(const char *line, size_t len, void *ctx)
{
    um98x_ctl_on_line(line, len, ctx);
    gnss_obs_feed_line(line, len);
    s_last_line_ms = now_ms32();
    s_line_seen = true;
}

static void on_framer_reset(void *ctx)
{
    (void)ctx;
    /* Data before the reset is discontinuous: no stale 1005 is replayed and
     * the next frame starts a new epoch. */
    rtcm_hub_clear_sticky();
}

static void on_loader_prompt(void *ctx)
{
    (void)ctx;
    /* Detection stops probing; the loader's restart key goes out at most
     * once per boot (station task). */
    s_prompt_seen = true;
    um98x_ctl_notify_prompt();
}

static void on_receiver_restart(void *ctx)
{
    (void)ctx;
    s_restart_pending = true;
    wake();
}

/* ---- leases --------------------------------------------------------------- */

static void mark_released(void)
{
    int64_t t = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    s_lease_release_us = t;
    portEXIT_CRITICAL(&s_lock);
}

static esp_err_t lease_take(const char *owner)
{
    esp_err_t err = gnss_link_acquire(GNSS_LINK_MODE_COMMAND, owner, LEASE_WAIT_MS, &s_lease);
    if (err != ESP_OK) {
        s_lease = NULL;
        ESP_LOGW(TAG, "no link lease for %s: %s", owner, esp_err_to_name(err));
    }
    return err;
}

/* Release a lease of ours. A release never times out; an error means the
 * handle was stale (nothing of ours is held), so the handle is dropped
 * either way. */
static void release_lease(gnss_link_lease_t *lease, const char *what)
{
    if (*lease == NULL) {
        return;
    }
    esp_err_t err = gnss_link_release(*lease);   /* framer reset: hub sticky copies cleared */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s lease release: %s", what, esp_err_to_name(err));
    }
    *lease = NULL;
    mark_released();
}

static void lease_drop(void)
{
    release_lease(&s_lease, "job");
}

static bool receiver_fresh(void)
{
    gnss_link_stats_t st;
    if (gnss_link_get_stats(&st) == ESP_OK && st.last_rtcm_age_ms <= FRESH_MS) {
        return true;
    }
    return s_line_seen && (uint32_t)(now_ms32() - s_last_line_ms) <= FRESH_MS;
}

/* ---- jobs ------------------------------------------------------------------ */

static void log_identity(const um98x_info_t *info, bool supported)
{
    ESP_LOGI(TAG, "receiver %s %s (build %u)%s", info->model, info->firmware, (unsigned)info->build,
             supported ? "" : ": not a model this firmware configures, streaming only");
}

static void job_identify(const stn_job_t *job, stn_result_t *r)
{
    load_profile();   /* the applied check compares with the stored profile */
    gnss_link_lease_t lease = NULL;
    if (job->leased) {
        if (lease_take("identify") != ESP_OK) {
            r->err = STN_E_STATE;
            return;
        }
        lease = s_lease;
        (void)um98x_ctl_session_begin(lease);
    }
    um98x_info_t info;
    esp_err_t err = um98x_ctl_query_version(lease, job->after_restart, &info);
    lease_drop();
    switch (err) {
    case ESP_OK:
        r->err = STN_OK;
        r->supported = um98x_ctl_model_supported(&info);
        r->static_ok = r->supported && applied_static_ok(&info);
        log_identity(&info, r->supported);
        load_stored_identity();
        break;
    case ESP_ERR_INVALID_RESPONSE:
        r->err = STN_E_NOT_READY;
        break;
    case ESP_ERR_TIMEOUT:
        r->err = STN_E_TIMEOUT;
        break;
    default:
        r->err = STN_E_STATE;
        break;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "receiver identity not confirmed (%s): stored identity kept, asked again later",
                 esp_err_to_name(err));
    }
}

static void job_mode(stn_result_t *r)
{
    char body[160];
    esp_err_t err = um98x_ctl_query_mode(NULL, body, sizeof(body));
    if (err != ESP_OK) {
        r->err = STN_E_TIMEOUT;
        return;
    }
    r->err = STN_OK;
    r->mode_survey = um98x_mode_is_survey(body);
    r->mode_fixed = um98x_mode_is_fixed_base(body);
}

/* Listen-only pass (the receiver sits at its loader): host baud changes,
 * nothing is transmitted. */
static void listen_scan(bool full, stn_result_t *r)
{
    uint32_t cand[8];
    unsigned n = stn_detect_candidates(stored_baud(), full, cand, 8);
    r->err = STN_E_NOT_FOUND;
    for (unsigned i = 0; i < n; i++) {
        if (gnss_link_set_baud(s_lease, cand[i]) != ESP_OK) {
            r->err = STN_E_STATE;
            return;
        }
        gnss_link_stats_t a;
        gnss_link_stats_t b;
        if (gnss_link_get_stats(&a) != ESP_OK) {
            r->err = STN_E_STATE;
            return;
        }
        int64_t end = now_ms() + LISTEN_MS;
        while (now_ms() < end) {
            vTaskDelay(pdMS_TO_TICKS(LISTEN_POLL_MS));
            feed_watchdog();
            if (gnss_link_get_stats(&b) != ESP_OK) {
                continue;
            }
            if (b.rx_bytes != a.rx_bytes) {
                r->saw_bytes = true;
            }
            if (b.framing.rtcm_frames - a.framing.rtcm_frames >= 1 || b.framing.text_lines - a.framing.text_lines >= 2) {
                r->err = STN_OK;
                persist_baud(cand[i]);
                ESP_LOGI(TAG, "receiver streams again at %u baud", (unsigned)cand[i]);
                return;
            }
        }
    }
}

static void job_detect(const stn_job_t *job, stn_result_t *r)
{
    if (lease_take(job->break_first ? "rescan" : "detect") != ESP_OK) {
        r->err = STN_E_STATE;
        return;
    }
    if (job->break_first) {
        (void)gnss_link_send_break(s_lease, BREAK_MS);
        feed_watchdog();
    }
    if (job->listen_only) {
        listen_scan(job->full, r);
    } else {
        s_prompt_seen = false;
        (void)gnss_link_set_prompt_detect(s_lease, true);
        um98x_detect_result_t d;
        esp_err_t err = um98x_ctl_detect(s_lease, job->full, &d);
        r->saw_bytes = d.saw_bytes;
        if (err == ESP_OK) {
            r->err = STN_OK;
            r->identity = d.identity;
            if (d.identity) {
                load_profile();
                r->supported = um98x_ctl_model_supported(&d.info);
                r->static_ok = r->supported && applied_static_ok(&d.info);
                log_identity(&d.info, r->supported);
                load_stored_identity();
            }
        } else if (err == ESP_ERR_INVALID_RESPONSE || d.prompt || s_prompt_seen) {
            r->err = STN_E_NOT_FOUND;
            r->prompt = true;
            s_prompt_baud = gnss_link_get_baud();
            (void)gnss_link_set_prompt_detect(s_lease, false);
            if (job->loader_key) {
                ESP_LOGW(TAG, "receiver at its loader prompt (%u baud): sending its restart key (once per boot)",
                         (unsigned)s_prompt_baud);
                esp_err_t e = um98x_ctl_leave_loader(s_lease);
                r->key_sent = (e != ESP_ERR_INVALID_STATE);
                r->app_back = (e == ESP_OK);
            }
            if (!r->app_back) {
                ESP_LOGE(TAG, "receiver stays at its loader: listening only until it streams or the user restarts it");
            }
        } else {
            r->err = (err == ESP_ERR_NOT_FOUND) ? STN_E_NOT_FOUND : STN_E_STATE;
        }
        (void)gnss_link_set_prompt_detect(s_lease, false);
    }
    if (r->err != STN_OK) {
        /* Between passes the link listens where it opens at boot. */
        (void)gnss_link_set_baud(s_lease, home_baud());
    }
    lease_drop();
}

static void job_leave_loader(stn_result_t *r)
{
    if (lease_take("restart receiver") != ESP_OK) {
        r->err = STN_E_STATE;
        return;
    }
    if (s_prompt_baud != 0) {
        (void)gnss_link_set_baud(s_lease, s_prompt_baud);
    }
    ESP_LOGW(TAG, "restarting the receiver from its loader at the user's request");
    esp_err_t err = um98x_ctl_leave_loader(s_lease);
    r->err = (err == ESP_OK) ? STN_OK : STN_E_TIMEOUT;
    if (err != ESP_OK) {
        (void)gnss_link_set_baud(s_lease, home_baud());
    }
    lease_drop();
}

static bool answered_since(int64_t t0_us)
{
    um98x_ctl_stats_t st;
    if (um98x_ctl_get_stats(&st) != ESP_OK || st.last_answer_age_ms == UINT32_MAX) {
        return false;
    }
    return esp_timer_get_time() - (int64_t)st.last_answer_age_ms * 1000 >= t0_us;
}

static const char *why_name(uint8_t why)
{
    switch (why) {
    case STN_WHY_CHECK:
        return "applied check";
    case STN_WHY_DRIFT:
        return "receiver output drifted";
    case STN_WHY_RETRY:
        return "retry after a failed apply";
    case STN_WHY_USER:
        return "user";
    case STN_WHY_LADDER:
        return "recovery";
    case STN_WHY_LOCK:
        return "position lock";
    case STN_WHY_RESURVEY:
        return "re-survey";
    default:
        return "?";
    }
}

static void job_apply(const stn_job_t *job, stn_result_t *r)
{
    load_profile();
    if (job->change == STN_CHANGE_LOCK) {
        um98x_base_profile_t next;
        um98x_base_position_t pos;
        esp_err_t err = um98x_ctl_lock_position(&s_profile, unix_now(), &next, &pos);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "position not locked: %s", err == ESP_ERR_NOT_FOUND ? "no 1005 yet" : "implausible 1005");
            r->err = STN_E_NOT_FOUND;
            r->nothing_sent = true;
            return;
        }
        if (!store_profile(&next, &pos)) {
            r->err = STN_E_STATE;
            r->nothing_sent = true;
            return;
        }
        s_profile = next;
        ESP_LOGI(TAG, "position locked (survey result): applying it as a fixed base");
    } else if (job->change == STN_CHANGE_RESURVEY) {
        um98x_base_profile_t next;
        um98x_resurvey_profile(&s_profile, &next);
        if (!store_profile(&next, NULL)) {
            r->err = STN_E_STATE;
            r->nothing_sent = true;
            return;
        }
        s_profile = next;
        ESP_LOGI(TAG, "locked position forgotten: surveying again (%u s)", (unsigned)s_profile.survey_in_s);
    }

    if (lease_take("configure") != ESP_OK) {
        r->err = STN_E_STATE;
        r->nothing_sent = true;
        return;
    }
    ESP_LOGI(TAG, "applying the station profile (%s)", why_name(job->why));
    int64_t t0 = esp_timer_get_time();
    um98x_apply_opts_t opts = { .force_mode = job->force_mode };
    esp_err_t err = um98x_ctl_apply_profile(s_lease, &s_profile, &opts, &s_apply);
    lease_drop();

    r->mode_sent = s_apply.mode_sent;
    r->mode_ack_ms = s_apply.mode_ack_ms;
    r->restarted = s_apply.signal_group_sent || (s_apply.rolled_back && s_apply.rollback_answered);
    r->receiver_lost = s_apply.receiver_lost || err == ESP_ERR_NOT_FOUND;
    r->answered = s_apply.rolled_back ? s_apply.rollback_answered : answered_since(t0);
    switch (err) {
    case ESP_OK:
        r->err = STN_OK;
        gnss_obs_reset();   /* signals, bands and the 1005 point start over */
        break;
    case ESP_ERR_NOT_SUPPORTED:
        r->err = STN_E_UNSUPPORTED;
        r->nothing_sent = true;
        break;
    case ESP_ERR_TIMEOUT:
        r->err = STN_E_TIMEOUT;   /* identity unknown, no version log: nothing configuring was sent */
        r->nothing_sent = true;
        break;
    case ESP_ERR_INVALID_ARG:
        r->err = STN_E_FAIL;
        r->nothing_sent = true;
        break;
    case ESP_ERR_NOT_FOUND:
        r->err = STN_E_NOT_FOUND;
        break;
    case ESP_ERR_INVALID_STATE:
        r->err = STN_E_STATE;
        break;
    default:
        r->err = STN_E_FAIL;
        break;
    }
    if (err != ESP_OK && !r->nothing_sent) {
        ESP_LOGE(TAG, "profile apply failed at \"%s\" (%s)%s: retried later", s_apply.failed_cmd, s_apply.reason,
                 r->receiver_lost ? ", receiver lost" : "");
    }
}

static void job_restore(stn_result_t *r)
{
    if (lease_take("restore output") != ESP_OK) {
        r->err = STN_E_STATE;
        return;
    }
    ESP_LOGW(TAG, "no RTCM after the failed apply: restoring the RTCM outputs only");
    esp_err_t err = um98x_ctl_restore_output(s_lease, &s_profile, &s_apply);
    lease_drop();
    r->err = (err == ESP_OK) ? STN_OK : STN_E_FAIL;
}

static void job_receiver_reset(stn_result_t *r)
{
    const board_desc_t *b = board_get();
    if (b != NULL && b->gnss_reset_gpio >= 0) {
        ESP_LOGW(TAG, "reset line on GPIO%d not used yet (polarity unconfirmed): using the RESET command",
                 b->gnss_reset_gpio);
    }
    if (lease_take("receiver reset") != ESP_OK) {
        r->err = STN_E_STATE;
        return;
    }
    /* One write, never resent (a resend would reset the receiver again),
     * through the command layer so the restart counts as ours: its banner
     * is not reported as an unsolicited restart. The restart is then handled
     * like any other (identity in restart mode, the applied check 30 s
     * later). */
    esp_err_t err = um98x_ctl_reset_receiver(s_lease);
    lease_drop();
    r->err = (err == ESP_ERR_INVALID_STATE) ? STN_E_STATE : STN_OK;
    ESP_LOGW(TAG, "receiver reset command: %s", esp_err_to_name(err));
}

static void job_check_position(stn_result_t *r)
{
    esp_err_t err = um98x_ctl_check_position(&s_profile, POS_MAX_AGE_MS);
    switch (err) {
    case ESP_OK:
        r->err = STN_OK;
        break;
    case ESP_ERR_INVALID_RESPONSE:
        r->err = STN_E_MISMATCH;
        ESP_LOGE(TAG, "the receiver's 1005 position is more than 5 mm from the locked one");
        break;
    default:
        r->err = STN_E_NOT_FOUND;
        break;
    }
}

static void run_job(const stn_job_t *job, stn_result_t *r)
{
    memset(r, 0, sizeof(*r));
    switch (job->kind) {
    case STN_JOB_IDENTIFY:
        job_identify(job, r);
        break;
    case STN_JOB_MODE:
        job_mode(r);
        break;
    case STN_JOB_DETECT:
        job_detect(job, r);
        break;
    case STN_JOB_LEAVE_LOADER:
        job_leave_loader(r);
        break;
    case STN_JOB_APPLY:
        job_apply(job, r);
        break;
    case STN_JOB_RESTORE:
        job_restore(r);
        break;
    case STN_JOB_RESYNC:
        r->err = gnss_link_resync(NULL) == ESP_OK ? STN_OK : STN_E_STATE;
        break;
    case STN_JOB_LINK_RESTART:
        r->err = gnss_link_restart(NULL) == ESP_OK ? STN_OK : STN_E_STATE;
        break;
    case STN_JOB_RECEIVER_RESET:
        job_receiver_reset(r);
        break;
    case STN_JOB_CHECK_POSITION:
        job_check_position(r);
        break;
    default:
        r->err = STN_E_STATE;
        break;
    }
}

/* ---- the loop's bookkeeping ------------------------------------------------ */

static void fill_in(stn_in_t *in)
{
    memset(in, 0, sizeof(*in));
    in->now_ms = now_ms();
    gnss_link_stats_t st;
    if (gnss_link_get_stats(&st) == ESP_OK) {
        in->rtcm_frames = st.framing.rtcm_frames;
        in->text_lines = st.framing.text_lines;
    }
    in->autocfg = autocfg_enabled();
    in->model_known = model_known();
    in->profile_survey = (s_profile.base_mode == UM98X_BASE_SURVEY_IN);
    in->survey_s = s_profile.survey_in_s;
    in->rtcm1230_ok = rtcm1230_supported();
    portENTER_CRITICAL(&s_lock);
    s_obs_snap = s_obs;
    portEXIT_CRITICAL(&s_lock);
    in->obs = &s_obs_snap;
}

static const char *state_name(station_state_t st)
{
    static const char *const k[] = { "idle", "detecting", "configuring", "streaming",
                                     "flashing", "flash recovery", "recovering", "no receiver" };
    return (unsigned)st < sizeof(k) / sizeof(k[0]) ? k[st] : "?";
}

static void publish_status(const stn_in_t *in)
{
    stn_t core;
    portENTER_CRITICAL(&s_lock);
    core = s_core;
    portEXIT_CRITICAL(&s_lock);

    station_status_t *o = &s_pub;
    memset(o, 0, sizeof(*o));
    o->state = (station_state_t)core.state;
    if (um98x_ctl_get_info(&o->receiver) != ESP_OK) {
        o->receiver = s_stored_info;   /* last persisted; valid stays false */
    }
    o->receiver_responding = receiver_fresh();
    o->identity_confirmed = core.identity_ok;
    o->alerts = core.alerts;
    o->profile_applied = core.profile_applied;
    o->rtcm1230_supported = in->rtcm1230_ok;
    o->config_failed = (core.alerts & STN_ALERT_CONFIG_FAILED) != 0;
    o->config_retry_in_s = stn_config_retry_in_s(&core, in->now_ms);
    o->reconfigure_refused = stn_reconfigure_refused(&core, in->autocfg);
    o->position_locked = (s_profile.base_mode == UM98X_BASE_FIXED);
    bool have_pos = false;
    if (o->position_locked) {
        uint8_t blob[UM98X_POSITION_BLOB_MAX];
        size_t len = sizeof(blob);
        have_pos = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BASE_POS, blob, &len) == ESP_OK &&
                   um98x_position_decode(blob, len, &o->position);
    }
    if (!have_pos && um98x_ctl_get_base_position(&o->position) != ESP_OK) {
        memset(&o->position, 0, sizeof(o->position));
    }
    o->surveying = stn_surveying(&core, in->now_ms);
    o->survey_window_left_s = stn_survey_left_s(&core, in->now_ms);
    o->lease_held = s_lease != NULL || s_hold_lease != NULL || s_boot_lease != NULL;
    o->detect_failures = core.detect_failures;
    o->detect_saw_bytes = core.detect_saw_bytes;
    um98x_flash_get_progress(&o->flash);

    portENTER_CRITICAL(&s_lock);
    s_status = *o;
    s_status_valid = true;
    portEXIT_CRITICAL(&s_lock);
}

static void after_step(const stn_in_t *in)
{
    bool paused;
    station_state_t st;
    portENTER_CRITICAL(&s_lock);
    if (s_core.obs_rearm) {
        stn_obs_arm(&s_obs, s_profile.rtcm, s_profile.rtcm_count, in->now_ms);
        s_core.obs_rearm = false;
    }
    paused = s_core.paused;
    st = (station_state_t)s_core.state;
    portEXIT_CRITICAL(&s_lock);

    if (paused != s_paused) {
        s_paused = paused;
        esp_err_t err = ntrip_server_set_paused(paused);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "casters not %s: %s", paused ? "paused" : "resumed", esp_err_to_name(err));
        }
    }
    publish_status(in);
    if (st != s_posted_state) {
        ESP_LOGI(TAG, "%s -> %s", state_name(s_posted_state), state_name(st));
        s_posted_state = st;
        esp_err_t err = esp_event_post(STATION_EVENT, STATION_EVENT_STATE_CHANGED, &st, sizeof(st), 0);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "state event not posted: %s", esp_err_to_name(err));
        }
    }
}

/* Flags from other tasks, the ledger and the survey fallback's 1005 feed. */
static void service(const stn_in_t *in)
{
    if (s_restart_pending) {
        s_restart_pending = false;
        ESP_LOGW(TAG, "the receiver restarted (restart banner or an unasked version log)");
        esp_err_t err = esp_event_post(STATION_EVENT, STATION_EVENT_RECEIVER_RESTARTED, NULL, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "restart event not posted: %s", esp_err_to_name(err));
        }
        portENTER_CRITICAL(&s_lock);
        stn_restart_seen(&s_core);
        portEXIT_CRITICAL(&s_lock);
    }

    if (s_journal_cleared) {
        s_journal_cleared = false;
        release_lease(&s_hold_lease, "listen-only");
        portENTER_CRITICAL(&s_lock);
        stn_journal_cleared(&s_core, in);
        portEXIT_CRITICAL(&s_lock);
        ESP_LOGI(TAG, "receiver flash journal cleared: normal start");
    }

    uint32_t arp_frames;
    portENTER_CRITICAL(&s_lock);
    arp_frames = s_arp_frames;
    portEXIT_CRITICAL(&s_lock);
    if (arp_frames != s_arp_fed) {
        s_arp_fed = arp_frames;
        gnss_obs_arp_t arp;
        if (gnss_obs_get_arp(&arp) == ESP_OK && arp.valid) {
            portENTER_CRITICAL(&s_lock);
            stn_on_arp(&s_core, arp.ecef_01mm);
            portEXIT_CRITICAL(&s_lock);
        }
    }

    gnss_link_stats_t st;
    bool ok = gnss_link_get_stats(&st) == ESP_OK && st.mode == GNSS_LINK_MODE_STREAM &&
              st.last_rtcm_age_ms <= FRESH_MS;
    if (ok != s_rtcm_ok) {
        s_rtcm_ok = ok;
        ledger_set_rtcm_ok(ok);
    }
}

/* An open journal: the UART is the flash session's. Listen only (a COMMAND
 * lease that is never written to keeps the hub unfed and refuses every
 * lease-free write), casters paused. This build cannot run the recovery
 * table itself, so a session that is not already waiting for the user
 * waits for the user here (reduced lockout: the inhibit and the update slot
 * are released, so app updates and reboots work again). */
static void flash_recovery_begin(void)
{
    release_lease(&s_boot_lease, "boot");
    if (gnss_link_acquire(GNSS_LINK_MODE_COMMAND, "flash journal", LEASE_WAIT_MS, &s_hold_lease) != ESP_OK) {
        s_hold_lease = NULL;
        ESP_LOGE(TAG, "no listen-only lease for the open flash journal");
    }
    esp_err_t jerr;
    um98x_flash_journal_t j;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    j = s_journal;
    portEXIT_CRITICAL(&s_lock);
    if (jerr == ESP_OK && j.state != UM98X_JOURNAL_NEEDS_USER) {
        lifecycle_boot_info_t boot;
        lifecycle_get_boot_info(&boot);
        ESP_LOGW(TAG,
                 "report-only: the interrupted receiver update (state %u, phase %u, %s reset) would be recovered "
                 "here; nothing is sent to the receiver and the session waits for the user",
                 (unsigned)j.state, (unsigned)j.phase,
                 (boot.reset_class == LIFECYCLE_RESET_POWER_ON || boot.reset_class == LIFECYCLE_RESET_BROWNOUT)
                     ? "power-on"
                     : "ESP32-only");
        portENTER_CRITICAL(&s_lock);
        s_journal_parked = true;
        portEXIT_CRITICAL(&s_lock);
        lockout_release();
    }
}

static void station_task(void *arg)
{
    (void)arg;
    if (esp_task_wdt_add(NULL) != ESP_OK) {
        ESP_LOGW(TAG, "not on the task watchdog");
    }
    esp_err_t err = gnss_link_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GNSS link not started: %s", esp_err_to_name(err));
    }

    esp_err_t jerr;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    portEXIT_CRITICAL(&s_lock);
    if (journal_open(jerr)) {
        flash_recovery_begin();
    } else {
        release_lease(&s_boot_lease, "boot");
    }

    stn_in_t in;
    fill_in(&in);
    portENTER_CRITICAL(&s_lock);
    stn_start(&s_core, &in);
    portEXIT_CRITICAL(&s_lock);
    after_step(&in);

    for (;;) {
        feed_watchdog();
        fill_in(&in);
        service(&in);
        portENTER_CRITICAL(&s_lock);
        stn_job_t job = stn_next(&s_core, &in);
        portEXIT_CRITICAL(&s_lock);
        after_step(&in);
        if (job.kind == STN_JOB_NONE) {
            (void)ulTaskNotifyTakeIndexed(NOTIFY_IDX, pdTRUE, pdMS_TO_TICKS(LOOP_WAIT_MS));
            continue;
        }

        stn_result_t r;
        run_job(&job, &r);
        feed_watchdog();
        fill_in(&in);
        portENTER_CRITICAL(&s_lock);
        stn_done(&s_core, &r, &in);
        portEXIT_CRITICAL(&s_lock);
        after_step(&in);
    }
}

/* ---- public API ------------------------------------------------------------ */

esp_err_t station_early_boot(void)
{
    read_journal();
    esp_err_t jerr = s_journal_err;
    if (!journal_open(jerr)) {
        return ESP_OK;
    }

    /* Full lockout for a session that is not waiting for the user: no
     * reboot may cut the recovery run short and no app update may start. */
    if (!journal_waits_for_user(jerr, &s_journal)) {
        lockout_take();
    }

    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    bool receiver_was_reset =
        boot.reset_class == LIFECYCLE_RESET_POWER_ON || boot.reset_class == LIFECYCLE_RESET_BROWNOUT;
    if (jerr != ESP_OK || !um98x_flash_journal_needs_race(&s_journal, receiver_was_reset)) {
        return ESP_OK;   /* the rest of the recovery table runs in the station task */
    }

#if CONFIG_UM98X_FLASH_RECOVERY_ACTIONS
    esp_err_t err = board_init();
    if (err == ESP_OK) {
        err = gnss_link_init();
    }
    if (err == ESP_OK) {
        err = gnss_link_acquire(GNSS_LINK_MODE_RAW, "flash recovery", 0, &s_boot_lease);
    }
    if (err == ESP_OK) {
        err = um98x_flash_race(s_boot_lease, RACE_MAX_MS);
        ESP_LOGW(TAG, "boot race into the receiver's loader: %s", esp_err_to_name(err));
    } else {
        ESP_LOGE(TAG, "boot race into the receiver's loader not possible: %s", esp_err_to_name(err));
    }
    return err;
#else
    ESP_LOGW(TAG, "report-only: would race into the receiver's loader at %u baud for %u ms (journal state %u)",
             (unsigned)UM98X_FLASH_BAUD, (unsigned)RACE_MAX_MS, (unsigned)s_journal.state);
    (void)s_boot_lease;
    return ESP_OK;
#endif
}

esp_err_t station_safe_mode_boot(void)
{
    if (!s_journal_read) {
        read_journal();
    }
    if (!journal_open(s_journal_err)) {
        /* Nothing may stay staged for the whole safe-mode episode. */
        esp_err_t rel = pkg_store_release();
        if (rel != ESP_OK) {
            ESP_LOGW(TAG, "staged receiver package not released: %s", esp_err_to_name(rel));
        }
    }
    return ESP_OK;
}

esp_err_t station_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    const gnss_link_sinks_t sinks = {
        .on_rtcm = on_rtcm_frame,
        .on_line = on_text_line,
        .on_reset = on_framer_reset,
        .on_prompt = on_loader_prompt,
        .ctx = NULL,
    };
    esp_err_t err = gnss_link_set_sinks(&sinks);
    if (err != ESP_OK) {
        return err;
    }
    um98x_ctl_set_restart_cb(on_receiver_restart, NULL);
    load_profile();
    load_stored_identity();

    if (!s_journal_read) {
        read_journal();
    }
    if (!journal_open(s_journal_err)) {
        /* Nothing may stay staged without a session the user started. */
        esp_err_t rel = pkg_store_release();
        if (rel != ESP_OK) {
            ESP_LOGW(TAG, "staged receiver package not released: %s", esp_err_to_name(rel));
        }
    }
    portENTER_CRITICAL(&s_lock);
    stn_init(&s_core, journal_open(s_journal_err));
    stn_obs_clear(&s_obs);
    portEXIT_CRITICAL(&s_lock);
    s_inited = true;
    ESP_LOGI(TAG, "sinks wired, profile hash %08x, station ID %u, %s", (unsigned)um98x_profile_hash(&s_profile),
             (unsigned)s_profile.station_id, s_profile.base_mode == UM98X_BASE_FIXED ? "fixed base" : "survey-in");
    return ESP_OK;
}

esp_err_t station_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    TaskHandle_t t = NULL;
    if (xTaskCreatePinnedToCore(station_task, "station", CONFIG_STATION_TASK_STACK, NULL, TASK_PRIO, &t,
                                TASK_CORE) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_task = t;
    s_started = true;
    return ESP_OK;
}

esp_err_t station_get_status(station_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool have;
    esp_err_t jerr;
    um98x_flash_journal_t journal;
    bool read;
    bool parked;
    int64_t released;
    uint64_t excluded;
    portENTER_CRITICAL(&s_lock);
    have = s_status_valid;
    if (have) {
        *out = s_status;
    }
    jerr = s_journal_err;
    journal = s_journal;
    read = s_journal_read;
    parked = s_journal_parked;
    released = s_lease_release_us;
    excluded = s_rtcm_excluded;
    portEXIT_CRITICAL(&s_lock);

    if (!have) {
        /* Not started (safe mode, or before station_start): state IDLE. */
        memset(out, 0, sizeof(*out));
        out->state = STATION_STATE_IDLE;
        if (s_inited) {
            out->position_locked = (s_profile.base_mode == UM98X_BASE_FIXED);
            out->rtcm1230_supported = rtcm1230_supported();
            out->reconfigure_refused = !autocfg_enabled();
            out->receiver = s_stored_info;
        }
    }
    out->rtcm_excluded = excluded;
    /* Live, not from the last published status: a job takes and drops its
     * lease while it runs, between two publications. Every lease on the link
     * is the station's (its jobs, the boot lease, the listen-only lease). */
    gnss_link_mode_t mode = gnss_link_get_mode();
    out->lease_held = (mode == GNSS_LINK_MODE_COMMAND || mode == GNSS_LINK_MODE_RAW);
    if (released == INT64_MIN) {
        out->since_lease_release_ms = UINT32_MAX;
    } else {
        int64_t ms = (esp_timer_get_time() - released) / 1000;
        out->since_lease_release_ms = ms < 0 ? 0 : (ms >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)ms);
    }

    /* Safe mode: the station never ran, read the journal from settings
     * (no UART) so the UI can show it and offer "clear the journal". */
    if (!read) {
        jerr = um98x_flash_journal_get(&journal);
    }
    out->flash_journal_open = journal_open(jerr);
    out->flash_journal_readable = (jerr == ESP_OK);
    out->flash_journal_settings_unavailable = journal_unknown(jerr);
    if (jerr == ESP_OK) {
        out->flash_journal = journal;
    } else {
        memset(&out->flash_journal, 0, sizeof(out->flash_journal));
    }
    if (out->flash_journal_open && !journal_unknown(jerr) &&
        (parked || jerr != ESP_OK || journal.state == UM98X_JOURNAL_NEEDS_USER)) {
        out->flash.phase = UM98X_FLASH_NEEDS_USER;
    }
    return ESP_OK;
}

/* Common gate for requests that need the running station. */
static esp_err_t running_gate(void)
{
    if (!s_inited || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t jerr;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    portEXIT_CRITICAL(&s_lock);
    return journal_open(jerr) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static bool alert_raised(uint32_t alert)
{
    portENTER_CRITICAL(&s_lock);
    bool raised = (s_core.alerts & alert) != 0;
    portEXIT_CRITICAL(&s_lock);
    return raised;
}

esp_err_t station_request_reconfigure(void)
{
    esp_err_t err = running_gate();
    if (err != ESP_OK) {
        return err;
    }
    if (alert_raised(STN_ALERT_UNSUPPORTED_MODEL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_lock);
    stn_request_apply(&s_core);
    portEXIT_CRITICAL(&s_lock);
    wake();
    return ESP_OK;
}

esp_err_t station_request_flash(const station_flash_request_t *req)
{
    if (req == NULL || req->size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    station_state_t state;
    bool supported;
    esp_err_t jerr;
    portENTER_CRITICAL(&s_lock);
    state = (station_state_t)s_core.state;
    supported = s_core.identity_ok && s_core.supported;
    jerr = s_journal_err;
    portEXIT_CRITICAL(&s_lock);
    if (!s_inited || !s_started || journal_open(jerr) || lifecycle_app_trial_active() || app_update_running() ||
        lifecycle_in_safe_mode() || state != STATION_STATE_STREAMING || !receiver_fresh() || !supported) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TODO: prepare (rules on the RAM copy; release the package on failure),
     * claim the update slot (lockout_take: refused if an app update got
     * there first), pin, journal, RAW lease, run. Waits for the flasher. */
    return ESP_ERR_NOT_SUPPORTED;
}

uint32_t station_expected_flash_downtime_s(uint32_t package_size)
{
    /* Transfer at the loader baud (10 bits per byte) + 10 % protocol
     * overhead, plus the receiver's write, restart and reacquisition. No
     * survey term: MSM output continues during a survey (PROVISIONAL, to be
     * confirmed on hardware). Refined from measured runs. */
    uint64_t transfer_s = ((uint64_t)package_size * 10u * 11u) / ((uint64_t)UM98X_FLASH_BAUD * 10u);
    return (uint32_t)transfer_s + CONFIG_STATION_FLASH_RESTART_ALLOWANCE_S;
}

esp_err_t station_flash_user_action(station_flash_action_t action)
{
    if (!s_journal_read && !s_inited) {
        /* Safe mode: the station never read the journal. */
        read_journal();
    }
    esp_err_t jerr;
    um98x_flash_journal_t journal;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    journal = s_journal;
    portEXIT_CRITICAL(&s_lock);

    if (!journal_open(jerr)) {
        if (action == STATION_FLASH_ACTION_RESTART_RECEIVER && alert_raised(STN_ALERT_RECEIVER_IN_LOADER)) {
            /* Receiver found at its loader without a session: one more
             * restart key at the user's request, then detection. */
            if (!s_inited || !s_started) {
                return ESP_ERR_INVALID_STATE;
            }
            portENTER_CRITICAL(&s_lock);
            int rc = stn_request_restart_receiver(&s_core);
            portEXIT_CRITICAL(&s_lock);
            if (rc != STN_OK) {
                return ESP_ERR_INVALID_STATE;
            }
            wake();
            return ESP_OK;
        }
        return ESP_ERR_NOT_FOUND;
    }
    bool waiting = journal_waits_for_user(jerr, &journal);

    switch (action) {
    case STATION_FLASH_ACTION_CLEAR_JOURNAL: {
        /* Not while a session or its recovery runs; always in safe mode,
         * where nothing runs. Not with settings unavailable: there is
         * nothing to clear and nothing could be written. */
        if ((s_inited && !waiting) || journal_unknown(jerr)) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = um98x_flash_journal_close();
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "receiver flash journal cleared by the user");
        portENTER_CRITICAL(&s_lock);
        s_journal_err = ESP_ERR_NOT_FOUND;
        memset(&s_journal, 0, sizeof(s_journal));
        s_journal_parked = false;
        portEXIT_CRITICAL(&s_lock);
        esp_err_t rel = pkg_store_release();
        if (rel != ESP_OK) {
            ESP_LOGW(TAG, "staged receiver package not released: %s", esp_err_to_name(rel));
        }
        lockout_release();
        if (s_inited) {
            s_journal_cleared = true;   /* the station task leaves FLASH_RECOVERY (now or once it runs) */
            wake();
        }
        return ESP_OK;
    }
    case STATION_FLASH_ACTION_RETRY:
        if (!s_inited || !waiting || jerr != ESP_OK || lifecycle_app_trial_active() || app_update_running()) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_SUPPORTED;   /* waits for the flasher */
    case STATION_FLASH_ACTION_RESTART_RECEIVER:
        if (!s_inited || !waiting) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_SUPPORTED;   /* waits for the flasher's exit checks */
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

size_t station_get_flash_log(char *buf, size_t len)
{
    if (buf != NULL && len > 0) {
        buf[0] = '\0';
    }
    return 0;   /* TODO: ring of the last 4 KB of receiver text from the flash session */
}

esp_err_t station_request_recovery(station_recovery_t action)
{
    if ((unsigned)action > (unsigned)STATION_RECOVER_RECEIVER_RESET) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = running_gate();
    if (err != ESP_OK) {
        return err;
    }
    bool autocfg = autocfg_enabled();
    portENTER_CRITICAL(&s_lock);
    int rc = stn_request_recovery(&s_core, (uint8_t)action, autocfg);
    portEXIT_CRITICAL(&s_lock);
    if (rc != STN_OK) {
        /* Sends nothing: refused (the user configures the receiver, or the
         * apply backoff owns the re-applies) or not a state the ladder acts in. */
        return ESP_ERR_INVALID_STATE;
    }
    wake();
    return ESP_OK;
}

esp_err_t station_lock_position(void)
{
    esp_err_t err = running_gate();
    if (err != ESP_OK) {
        return err;
    }
    if (alert_raised(STN_ALERT_UNSUPPORTED_MODEL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    um98x_base_position_t pos;
    if (um98x_ctl_get_base_position(&pos) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;   /* no 1005 from the receiver yet */
    }
    portENTER_CRITICAL(&s_lock);
    stn_request_lock(&s_core);
    portEXIT_CRITICAL(&s_lock);
    wake();
    return ESP_OK;
}

esp_err_t station_request_resurvey(void)
{
    esp_err_t err = running_gate();
    if (err != ESP_OK) {
        return err;
    }
    if (alert_raised(STN_ALERT_UNSUPPORTED_MODEL)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    portENTER_CRITICAL(&s_lock);
    stn_request_resurvey(&s_core);
    portEXIT_CRITICAL(&s_lock);
    wake();
    return ESP_OK;
}
