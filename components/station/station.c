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

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "diag.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_rtc_time.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#include "sg_test_core.h"
#include "station_core.h"
#include "station_obs.h"
#include "um98x_bands.h"
#include "um98x_ctl.h"
#include "um98x_flash.h"
#include "um98x_flash_bench.h"

static const char *TAG = "station";

_Static_assert(STATION_BENCH_LISTEN == (int)UM98X_BENCH_LISTEN && STATION_BENCH_VERSION == (int)UM98X_BENCH_VERSION &&
                   STATION_BENCH_ENTRY == (int)UM98X_BENCH_ENTRY && STATION_BENCH_MENU2 == (int)UM98X_BENCH_MENU2 &&
                   STATION_BENCH_IDLE == (int)UM98X_BENCH_IDLE,
               "bench operations mirror um98x_bench_op_t");

#ifndef CONFIG_UM98X_FLASH_BYTE_CAPTURE
#define CONFIG_UM98X_FLASH_BYTE_CAPTURE 0
#endif
#ifndef CONFIG_UM98X_FLASH_ENABLE
#define CONFIG_UM98X_FLASH_ENABLE 0
#endif
#ifndef CONFIG_UM98X_FLASH_RECOVERY_ACTIONS
#define CONFIG_UM98X_FLASH_RECOVERY_ACTIONS 0
#endif
#ifndef CONFIG_STATION_HOLD_FIRST_APPLY
#define CONFIG_STATION_HOLD_FIRST_APPLY 0
#endif

/* Boot-time race into the receiver's loader. PROVISIONAL, to be confirmed on hardware. */
#define RACE_MAX_MS 10000u

#define TASK_PRIO        10      /* below the GNSS RX task, above casters and web */
#define TASK_CORE        1       /* the GNSS path's core; Wi-Fi runs on core 0 */
#define NOTIFY_IDX       0       /* the station task waits on nothing else */
#define LOOP_WAIT_MS     1000    /* longest sleep between two decisions (and watchdog feeds) */
#define LEASE_WAIT_MS    1000
#define BREAK_MS         100     /* UART break before a rescan. PROVISIONAL, to be confirmed on hardware */
#define LISTEN_MS        1500    /* listen-only scan: per baud. PROVISIONAL, to be confirmed on hardware */
#define LISTEN_POLL_MS   100
#define POS_MAX_AGE_MS   5000    /* position check: a 1005 at most this old */
#define EPOCH_WAIT_MS    1000    /* a caster pause waits this long at most for the epoch in flight */
#define EPOCH_POLL_MS    10
#define HOLD_NOTE_MS     300     /* a caster hold that waited this long for its epoch is logged */
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
                   STN_ALERT_RECEIVER_IN_LOADER == (uint32_t)STATION_ALERT_RECEIVER_IN_LOADER &&
                   STN_ALERT_NO_RECEIVER == (uint32_t)STATION_ALERT_NO_RECEIVER &&
                   STN_ALERT_SIGNALS_MISMATCH == (uint32_t)STATION_ALERT_SIGNALS_MISMATCH &&
                   STN_ALERT_FIXED_FAR == (uint32_t)STATION_ALERT_FIXED_FAR,
               "core alerts mirror station_alert_t");
_Static_assert(STN_FIX_OFF == (int)STATION_FIX_OFF && STN_FIX_CHECKING == (int)STATION_FIX_CHECKING &&
                   STN_FIX_NEAR == (int)STATION_FIX_NEAR && STN_FIX_FAR == (int)STATION_FIX_FAR &&
                   STN_FIX_NO_SOLUTION == (int)STATION_FIX_NO_SOLUTION && STN_FIX_REFUSED == (int)STATION_FIX_REFUSED,
               "core fixed-position check states mirror station_fix_state_t");
_Static_assert(STN_REC_RESYNC == (int)STATION_RECOVER_RESYNC &&
                   STN_REC_LINK_RESTART == (int)STATION_RECOVER_LINK_RESTART &&
                   STN_REC_RESCAN == (int)STATION_RECOVER_RESCAN &&
                   STN_REC_RECONFIGURE == (int)STATION_RECOVER_RECONFIGURE &&
                   STN_REC_RECEIVER_RESET == (int)STATION_RECOVER_RECEIVER_RESET,
               "core recovery actions mirror station_recovery_t");

_Static_assert(STN_MAINT_OK == (int)STATION_MAINT_OK && STN_MAINT_FLASH == (int)STATION_MAINT_REFUSED_FLASH &&
                   STN_MAINT_SESSION == (int)STATION_MAINT_REFUSED_SESSION &&
                   STN_MAINT_DURATION == (int)STATION_MAINT_REFUSED_DURATION &&
                   STN_MAINT_NOT_ON == (int)STATION_MAINT_NOT_ON &&
                   STN_MAINT_SKY_OUTPUT == (int)STATION_MAINT_REFUSED_SKY_OUTPUT,
               "core maintenance reasons mirror station_maint_why_t");
_Static_assert(STN_SKY_OFF == (int)STATION_LIVE_SKY_OFF && STN_SKY_PENDING == (int)STATION_LIVE_SKY_PENDING &&
                   STN_SKY_ON == (int)STATION_LIVE_SKY_ON && STN_SKY_ON_PROFILE == (int)STATION_LIVE_SKY_ON_PROFILE &&
                   STN_SKY_BLOCKED == (int)STATION_LIVE_SKY_BLOCKED &&
                   STN_SKY_STOPPING == (int)STATION_LIVE_SKY_STOPPING,
               "core sky states mirror station_live_sky_t");
_Static_assert(STN_SKY_WHY_NONE == (int)STATION_LIVE_WHY_NONE && STN_SKY_WHY_BUSY == (int)STATION_LIVE_WHY_BUSY &&
                   STN_SKY_WHY_NOT_STREAMING == (int)STATION_LIVE_WHY_NOT_STREAMING &&
                   STN_SKY_WHY_MAINTENANCE == (int)STATION_LIVE_WHY_MAINTENANCE &&
                   STN_SKY_WHY_AUTOCFG_OFF == (int)STATION_LIVE_WHY_AUTOCFG_OFF &&
                   STN_SKY_WHY_UNSUPPORTED == (int)STATION_LIVE_WHY_UNSUPPORTED &&
                   STN_SKY_WHY_RECONFIGURED == (int)STATION_LIVE_WHY_RECONFIGURED &&
                   STN_SKY_WHY_REJECTED == (int)STATION_LIVE_WHY_REJECTED &&
                   STN_SKY_WHY_NO_ANSWER == (int)STATION_LIVE_WHY_NO_ANSWER &&
                   STN_SKY_WHY_FLASH == (int)STATION_LIVE_WHY_FLASH,
               "core sky reasons mirror station_live_why_t");
_Static_assert(STN_LIVE_END_NONE == (int)STATION_LIVE_END_NONE && STN_LIVE_END_USER == (int)STATION_LIVE_END_USER &&
                   STN_LIVE_END_EXPIRED == (int)STATION_LIVE_END_EXPIRED &&
                   STN_LIVE_END_IDLE == (int)STATION_LIVE_END_IDLE && STN_LIVE_END_FLASH == (int)STATION_LIVE_END_FLASH,
               "core live view end reasons mirror station_live_end_t");

_Static_assert(STATION_SGT_OK == 0 && STATION_SGT_E_NOT_RUNNING > STATION_SGT_E_IDLE, "comparison errors");
_Static_assert(STN_RXBOOT_KEPT == (int)DIAG_RX_BOOT_KEPT && STN_RXBOOT_RESTARTED == (int)DIAG_RX_BOOT_RESTARTED &&
                   STN_RXBOOT_UNKNOWN == (int)DIAG_RX_BOOT_UNKNOWN && STN_RXBOOT_BY_NONE == (int)DIAG_RX_BOOT_BY_NONE &&
                   STN_RXBOOT_BY_RESTART == (int)DIAG_RX_BOOT_BY_RESTART &&
                   STN_RXBOOT_BY_UPTIME == (int)DIAG_RX_BOOT_BY_UPTIME &&
                   STN_RXBOOT_BY_STREAM == (int)DIAG_RX_BOOT_BY_STREAM,
               "power-on verdicts are journalled as the core names them");
_Static_assert(STN_RXBOOT_BY_NONE == (int)STATION_RX_BOOT_BY_NONE &&
                   STN_RXBOOT_BY_RESTART == (int)STATION_RX_BOOT_BY_RESTART &&
                   STN_RXBOOT_BY_UPTIME == (int)STATION_RX_BOOT_BY_UPTIME &&
                   STN_RXBOOT_BY_STREAM == (int)STATION_RX_BOOT_BY_STREAM,
               "core evidence kinds mirror station_rx_boot_by_t");

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
/* What the live view's status needs besides the core (the station task
 * keeps them current), so a status read computes the window at read time. */
static bool s_in_profile_sky;
static bool s_in_autocfg = true;
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
/* A receiver update job was accepted (a new update, the user's retry or
 * restart, the boot-time recovery) and has not ended: the receiver is worked
 * on, whatever the record says (a retry's record says "needs user" until the
 * session's first write). The journal does not wait for the user meanwhile.
 * Under s_lock. */
static bool s_flash_running;
/* The user's clear of the journal is under way: no retry or restart may be
 * accepted until it is done. Under s_lock. */
static bool s_journal_clearing;

/* RX task -> station task (single writer each). */
static volatile bool s_prompt_seen;
static volatile bool s_restart_pending;
static int64_t s_line_ms = STN_NEVER;   /* the last checksum-valid text line (now_ms); under s_lock */
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

/* Receiver maintenance across an ESP32 reset (station_core.h, "Maintenance
 * across an ESP32 reset"): the RTC record, rewritten every step (exact, no
 * flash; lost by the EN pin and by a power cut), and the stored copy in
 * settings (gnss/maint: at entry, at each extension and every
 * STN_MAINT_SAVE_S, erased when it ends). About 240 small writes for 4 h of
 * maintenance, next to the ledger's one a minute around the clock: no wear
 * concern for the 112 KB NVS partition. Station task only, apart from the
 * flags. */
static RTC_NOINIT_ATTR stn_maint_rtc_t s_maint_rtc;
static int64_t s_maint_saved_ms = STN_NEVER;   /* the stored copy's last write (STN_NEVER: none this boot) */
static uint32_t s_maint_saved_chosen;          /* its chosen duration (an extension saves at once) */
static bool s_maint_stored;                    /* a stored copy may exist: erased when maintenance is off */
static int64_t s_maint_store_retry_ms = STN_NEVER;
static volatile bool s_maint_end_req;    /* the user's "end now", for the station task */
static volatile bool s_maint_power_cut;  /* the power-on verdict: the receiver restarted too */
static volatile bool s_factory_reset;    /* the settings were erased for a factory reset: no record from here on */
/* The first-apply hold for this boot only: due, but it could not be stored
 * (settings unusable, or the write failed). Set at init, with automatic
 * configuration off for this boot (what the batch would have stored). A
 * successful apply ends the hold; only the user turning automatic
 * configuration on ends both. */
static volatile bool s_hold_ram;
static volatile bool s_autocfg_ram_off;

/* The live view's satellite-in-view output may be on (unsaved) across an
 * ESP32-only reset: the receiver keeps it, so the next boot turns it off.
 * Station task only (and station_init before it runs). */
static RTC_NOINIT_ATTR stn_sky_rtc_t s_sky_rtc;

/* A power-on boot: did the receiver keep running (stn_rxboot_judge)? The RX
 * task notes the first restart evidence and the first data; the station
 * task judges until there is a verdict, and keeps it. */
static int64_t s_rxb_restart_us = INT64_MAX;     /* under s_lock (written by the RX task) */
static int64_t s_rxb_data_us = INT64_MAX;        /* under s_lock (written by the RX task) */
static uint8_t s_rxb_state;                      /* station_rx_boot_t, under s_lock */
static uint8_t s_rxb_by;                         /* station_rx_boot_by_t, under s_lock */
static bool s_rxb_judging;                       /* station task (set at init) */
static int64_t s_rxb_uptime_at_ms = STN_NEVER;   /* station task */
static uint32_t s_rxb_uptime_ms;                 /* station task */
static bool s_rxb_clock_closed;                  /* station task: a job of ours may have restarted the receiver */

/* Station task only. */
static um98x_base_profile_t s_profile;
static um98x_info_t s_stored_info;       /* gnss/model and gnss/fw as stored (not confirmed) */
static stn_obs_t s_obs_snap;
static um98x_apply_result_t s_apply;
static station_status_t s_pub;
static gnss_link_lease_t s_lease;        /* the running job's lease */
static gnss_link_lease_t s_hold_lease;   /* listen-only while a flash journal is open */
static bool s_paused;
static bool s_gate_on;                   /* maintenance as the ledger knows it (the link's gate follows the core) */
static bool s_rtcm_ok;
static uint32_t s_prompt_baud;
static uint32_t s_arp_fed;
static station_state_t s_posted_state = STATION_STATE_IDLE;

/* What the last apply session read back and how its antenna and B1C/B2a
 * steps went (station task; published in the status). */
static bool s_rx_valid;
static um98x_rx_settings_t s_rx_settings;
static int64_t s_rx_at_ms;
static int64_t s_rx_unix;
static bool s_ant_sent;
static bool s_ant_ok;
static char s_ant_reply[64];
static bool s_b1c_sent;
static bool s_b1c_ok;
static char s_b1c_reply[64];
static char s_sky_reply[64];             /* the last failed satellite-in-view toggle */
static bool s_rx_sg_valid;               /* the latest read-back named the receiver's signal group */
static station_sg_t s_rx_sg;
/* What the running group configures, from the last session that asked
 * (ANT1GROUPA), and how it compared with the profile's group then. */
static bool s_bands_asked;
static um98x_bands_t s_bands;
static int64_t s_bands_at_ms;
static int64_t s_bands_unix;
static uint8_t s_bands_master;
static bool s_bands_match;
static char s_bands_reply[64];

/* A refused signal group (until the next profile save; under s_lock). */
static bool s_sg_refused;
static station_sg_t s_sg_refused_group;
static bool s_sg_back_valid;
static station_sg_t s_sg_back;
EXT_RAM_BSS_ATTR static char s_sg_refused_reply[64];   /* never touched with the cache off */

/* The signal-group comparison: its core and record, under s_sgt_mx (the
 * station task and the web's requests). Large: in PSRAM. */
EXT_RAM_BSS_ATTR static sgt_t s_sgt;
EXT_RAM_BSS_ATTR static uint8_t s_sgt_rec[SGT_REC_MAX];
EXT_RAM_BSS_ATTR static station_status_t s_sgt_st;   /* the status a start is judged on (s_sgt_mx) */
static SemaphoreHandle_t s_sgt_mx;
static esp_err_t sg_record_flush(int64_t now);

/* Receiver update sessions (station task, except where noted). */
static station_flash_request_t s_flash_req;   /* the user's request (written under s_lock by the requester) */
static uint32_t s_bench_stop_after;           /* bench: stop after this many blocks (under s_lock) */
static station_flash_request_t s_check_req;   /* check-only request (under s_lock) */
static bool s_check_running;                  /* under s_lock */
static station_flash_last_t s_flash_last;     /* under s_lock */
static int64_t s_flash_last_us = INT64_MIN;   /* under s_lock */
static um98x_info_t s_after;                  /* the version log after the session */
static bool s_after_aborted;
static char s_after_reason[96];
static um98x_flash_phase_t s_progress_phase;
static int64_t s_progress_post_us;
static int s_recover_plan;                    /* boot-time table, from station_early_boot */
static bool s_race_prompt;                    /* the early race reached the loader prompt */

#if CONFIG_UM98X_FLASH_BYTE_CAPTURE
/* Bench console (bench builds). */
static station_bench_req_t s_bench_req;
static station_bench_res_t s_bench_res;
static SemaphoreHandle_t s_bench_done;
#endif

/* Journal records waiting for diag's rate limits (station task). */
#define JQ_LEN 8
typedef struct {
    diag_event_t kind;
    int32_t a;
    int32_t b;
} jrec_t;
static jrec_t s_jq[JQ_LEN];
static unsigned s_jq_n;

static void journal_push(diag_event_t kind, int32_t a, int32_t b);
static void hold_take(void);

/* ---- small helpers -------------------------------------------------------- */

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
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

/* Under s_lock once the station runs (reads s_journal_parked and s_flash_running). */
static bool journal_waits_for_user(esp_err_t err, const um98x_flash_journal_t *j)
{
    return !s_flash_running && (err != ESP_OK || j->state == UM98X_JOURNAL_NEEDS_USER || s_journal_parked);
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

/* A factory reset erased the settings (the reboot follows): receiver
 * maintenance keeps no record from here on, so the next boot resumes none. */
static bool s_factory_hooked;

static void on_factory_reset(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    s_factory_reset = true;
    wake();
}

/* The resets after which the RTC timer has run on: the CPUs were reset,
 * not the chip (esp_rtc_get_time_us keeps counting across them). */
static bool rtc_timer_kept(esp_reset_reason_t r)
{
    return r == ESP_RST_SW || r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT;
}

/* Does maintenance that was on before this boot's reset go on, and for how
 * long (stn_maint_boot)? From the RTC record when it survived, else from
 * the stored copy (the EN pin's reset, which reads as power-on, loses the
 * RTC memory while the receiver keeps running and may be mid-work). Never
 * with an open journal or after a factory reset. `found`: a stored copy
 * exists (erased unless maintenance goes on). */
static void maint_boot_decide(stn_maint_boot_t *out, bool *found)
{
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    stn_maint_boot_in_t in;
    memset(&in, 0, sizeof(in));
    in.blocked = journal_open(s_journal_err) || (boot.reset_class == LIFECYCLE_RESET_PLANNED &&
                                                 boot.prev_reboot_reason == LIFECYCLE_REBOOT_FACTORY_RESET);
    in.rtc_ok = stn_maint_rtc_read(&s_maint_rtc, &in.rtc) && lifecycle_rtc_stamp_ok(in.rtc.stamp);
    in.rtc_timer_kept = rtc_timer_kept(boot.reset_reason);
    in.rtc_now_us = esp_rtc_get_time_us();
    uint8_t buf[STN_MAINT_STORE_MAX];
    size_t len = sizeof(buf);
    esp_err_t err = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MAINT, buf, &len);
    *found = settings_is_available() && err != ESP_ERR_NVS_NOT_FOUND;
    in.store_ok = err == ESP_OK && stn_maint_store_decode(&in.store, buf, len) &&
                  lifecycle_rtc_stamp_ok(in.store.stamp);
    in.uptime_ms = now_ms();
    stn_maint_boot(&in, out);
}

static bool app_update_running(void)
{
    return lifecycle_update_active() == LIFECYCLE_UPDATE_APP;
}

static bool autocfg_enabled(void)
{
    if (s_autocfg_ram_off) {
        return false;   /* off for this boot (a hold that could not be stored): not the stored value or the default */
    }
    uint8_t v = 1;   /* default: configure the receiver automatically */
    if (settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_AUTOCFG, &v) != ESP_OK) {
        v = 1;
    }
    return v != 0;
}

/* gnss/rtcm1230_ok: 1 accepted, 0 refused at the last apply, -1 never applied (not stored). */
static int rtcm1230_state(void)
{
    uint8_t v = 0;
    if (settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RTCM1230_OK, &v) != ESP_OK) {
        return -1;
    }
    return v != 0 ? 1 : 0;
}

static bool rtcm1230_supported(void)
{
    return rtcm1230_state() == 1;
}

static bool model_known(void)
{
    char model[sizeof(s_stored_info.model)];
    return settings_get_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MODEL, model, sizeof(model)) == ESP_OK &&
           model[0] != '\0';
}

/* A gnss key is stored (or its presence cannot be read: counted as stored, so nothing is written on a doubt). */
static bool gnss_key_present_u8(const char *key)
{
    uint8_t v = 0;
    return settings_get_u8(SETTINGS_NS_GNSS, key, &v) != ESP_ERR_NVS_NOT_FOUND;
}

/* The first receiver configuration waits for the user: gnss/hold as stored, or held for this boot only. */
static bool first_apply_held_stored(void)
{
    uint8_t v = 0;
    return s_hold_ram ||
           (settings_is_available() && settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_HOLD, &v) == ESP_OK);
}

/*
 * The first-apply hold at init, before anything is sent to the receiver: a
 * build with the option, on a device whose profile was never applied and
 * whose automatic configuration was never set, writes it once (one batch).
 * A hold that is due but cannot be stored (settings unusable this boot, so
 * the device's applied state is unknown; or the write failed) holds for this
 * boot in RAM: it fails closed, never open.
 * @return the hold as it stands afterwards.
 */
static bool first_apply_hold_boot(void)
{
    uint32_t hash = 0;
    bool settings_ok = settings_is_available();
    bool have_hash = settings_get_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_APPLIED, &hash) != ESP_ERR_NVS_NOT_FOUND;
    stn_hold_writes_t w;
    if (stn_hold_at_boot(CONFIG_STATION_HOLD_FIRST_APPLY, settings_ok, have_hash,
                         gnss_key_present_u8(SETTINGS_KEY_GNSS_AUTOCFG), gnss_key_present_u8(SETTINGS_KEY_GNSS_RECOVERY),
                         &w) &&
        w.ram_only) {
        s_hold_ram = true;
        s_autocfg_ram_off = true;
        ESP_LOGW(TAG, "first receiver configuration held for this boot only (settings unusable, nothing stored): "
                      "nothing that changes the receiver is sent until you apply");
    } else if (w.hold) {
        settings_batch_t b;
        esp_err_t err = settings_batch_begin(SETTINGS_NS_GNSS, &b);
        if (err == ESP_OK) {
            err = settings_batch_set_u8(b, SETTINGS_KEY_GNSS_AUTOCFG, 0);
            if (err == ESP_OK && w.recovery_alerts) {
                err = settings_batch_set_u8(b, SETTINGS_KEY_GNSS_RECOVERY, 2);
            }
            if (err == ESP_OK) {
                err = settings_batch_set_u8(b, SETTINGS_KEY_GNSS_HOLD, 1);
            }
            if (err == ESP_OK) {
                err = settings_batch_commit(b);
            } else {
                settings_batch_abort(b);
            }
        }
        if (err == ESP_OK) {
            ESP_LOGW(TAG, "first receiver configuration held: autocfg off, recovery %s; nothing that changes the "
                          "receiver is sent until you apply",
                     w.recovery_alerts ? "alerts only" : "as set");
        } else {
            s_hold_ram = true;
            s_autocfg_ram_off = true;
            ESP_LOGE(TAG, "first receiver configuration hold not stored (%s): held for this boot only; nothing that "
                          "changes the receiver is sent until you apply",
                     esp_err_to_name(err));
        }
    }
    bool held = first_apply_held_stored();
    if (held && !(w.hold)) {
        ESP_LOGW(TAG, "first receiver configuration still held: only queries reach the receiver until you apply");
    }
    return held;
}

/* After a successful apply, or after the user turned automatic configuration
 * on: both end a hold held for this boot only (automatic configuration stays
 * off for the boot unless the user turned it on); a stored hold they erase. */
static void first_apply_hold_update(void)
{
    s_hold_ram = false;
    bool held = first_apply_held_stored();
    portENTER_CRITICAL(&s_lock);
    bool was = s_core.first_apply_held;
    stn_set_first_apply_held(&s_core, held);
    portEXIT_CRITICAL(&s_lock);
    if (was && !held) {
        ESP_LOGI(TAG, "first receiver configuration released: the station configures the receiver as set");
    }
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
    esp_err_t err = settings_set_state_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, baud);
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
    if (!(settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, buf, &len) == ESP_OK &&
          um98x_profile_decode(buf, len, &s_profile))) {
        /* Default: the station ID comes from the base MAC, stable per device. */
        uint8_t mac[6] = { 0 };
        if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) {
            ESP_LOGW(TAG, "base MAC unreadable: station ID 0");
        }
        um98x_profile_default(&s_profile, um98x_station_id_from_mac(mac));
    }
    /* The signal view lists the profile's messages, and epoch gaps are
     * counted against its MSM periods. */
    gnss_obs_set_expected(s_profile.rtcm, s_profile.rtcm_count);
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

/* The stored profile with another signal group (the base position stays as stored). */
static bool store_profile_group(station_sg_t g)
{
    um98x_base_profile_t p = s_profile;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t blen = sizeof(buf);
    if (settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, buf, &blen) == ESP_OK) {
        (void)um98x_profile_decode(buf, blen, &p);
    }
    p.signal_group_master = g.master;
    p.signal_group_slave = g.slave;
    size_t len = um98x_profile_encode(&p, buf, sizeof(buf));
    esp_err_t err = len == 0 ? ESP_ERR_INVALID_SIZE : settings_set_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, buf, len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "signal group %u %u not written to the profile: %s", (unsigned)g.master, (unsigned)g.slave,
                 esp_err_to_name(err));
        return false;
    }
    s_profile = p;
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

static void on_rtcm_frame(const uint8_t *frame, size_t len, uint16_t msg_type, int64_t arrival_us,
                          bool arrival_exact, void *ctx)
{
    (void)ctx;
    /* If the hub is not ready the frame is lost; the hub counts the failed
     * publish and the link counters still show it as received, so health
     * sees the mismatch. The hub measures ages and its epoch gaps from the
     * frame's arrival (the UART interrupt's time when exact). */
    bool excluded = rtcm_type_excluded(msg_type);
    if (!excluded) {
        (void)rtcm_hub_publish(frame, len, msg_type, arrival_us, arrival_exact);
    }
    gnss_obs_feed_rtcm(frame, len, msg_type);
    /* A busy constellation comes as several MSMs of one type per epoch:
     * the observed-set tracker counts epochs. */
    gnss_rtcm3_msm_t msm;
    bool has_epoch = !excluded && gnss_rtcm3_msm_type(msg_type, NULL, NULL) && gnss_rtcm3_msm_decode(frame, len, &msm);
    int64_t t = arrival_us / 1000;   /* on the station's millisecond clock (esp_timer) */
    portENTER_CRITICAL(&s_lock);
    if (excluded) {
        s_rtcm_excluded++;
    }
    if (msg_type == 1005 || msg_type == 1006) {
        s_arp_frames++;
    }
    if (s_rxb_data_us == INT64_MAX) {
        s_rxb_data_us = arrival_us;   /* the first data of this boot (power-on verdict) */
    }
    stn_obs_feed(&s_obs, msg_type, has_epoch, has_epoch ? msm.epoch_time : 0, t);
    portEXIT_CRITICAL(&s_lock);
}

static void on_text_line(const char *line, size_t len, void *ctx)
{
    /* The first NMEA sentence of this boot (power-on verdict). Only this
     * task writes it: the unlocked check reads its own write. */
    if (s_rxb_data_us == INT64_MAX && stn_line_is_nmea(line, len)) {
        int64_t t = esp_timer_get_time();
        portENTER_CRITICAL(&s_lock);
        s_rxb_data_us = t;
        portEXIT_CRITICAL(&s_lock);
    }
    um98x_ctl_on_line(line, len, ctx);
    gnss_obs_feed_line(line, len);
    int64_t at = now_ms();
    portENTER_CRITICAL(&s_lock);
    s_line_ms = at;
    portEXIT_CRITICAL(&s_lock);
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
    int64_t t = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (s_rxb_restart_us == INT64_MAX) {
        s_rxb_restart_us = t;   /* the first restart evidence of this boot (power-on verdict) */
    }
    portEXIT_CRITICAL(&s_lock);
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

static uint32_t rtcm_age_ms(void)
{
    gnss_link_stats_t st;
    return gnss_link_get_stats(&st) == ESP_OK ? st.last_rtcm_age_ms : UINT32_MAX;
}

/* The receiver streams now: valid RTCM or a valid line within STN_FRESH_MS
 * (a receiver update starts only then). */
static bool receiver_streams(void)
{
    if (rtcm_age_ms() <= STN_FRESH_MS) {
        return true;
    }
    int64_t now = now_ms();
    portENTER_CRITICAL(&s_lock);
    int64_t line = s_line_ms;
    portEXIT_CRITICAL(&s_lock);
    return line != STN_NEVER && now - line <= STN_FRESH_MS;
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

/* ---- refused signal groups and the comparison's applies --------------------- */

static void sg_note_refused(station_sg_t refused, bool back_valid, station_sg_t back, const char *reply)
{
    portENTER_CRITICAL(&s_lock);
    s_sg_refused = true;
    s_sg_refused_group = refused;
    s_sg_back_valid = back_valid;
    s_sg_back = back;
    snprintf(s_sg_refused_reply, sizeof(s_sg_refused_reply), "%s", reply[0] != '\0' ? reply : "rejected");
    portEXIT_CRITICAL(&s_lock);
    journal_push(DIAG_EV_SG_TEST, sgt_note_a(SGT_EV_REFUSED, refused),
                 back_valid ? (int32_t)(((uint32_t)back.master << 8) | back.slave) : -1);
}

/* Any apply but the comparison's: the receiver refused the profile's group.
 * The group it runs (the session's read-back before the change, else the
 * group of the last successful apply) goes back into the profile and is
 * re-applied at once, without CONFIG_FAILED; with neither known it is an
 * ordinary failed apply. */
static void sg_refused_fallback(stn_result_t *r)
{
    station_sg_t refused = { .master = s_profile.signal_group_master, .slave = s_profile.signal_group_slave };
    uint16_t ok_pair = 0;
    bool ok_known = settings_get_u16(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_SG_OK, &ok_pair) == ESP_OK;
    station_sg_t back = { 0 };
    bool have = stn_sg_fallback(refused.master, refused.slave, s_apply.sg_before.known, s_apply.sg_before.master,
                                s_apply.sg_before.slave, ok_known, ok_pair, &back.master, &back.slave);
    if (have && !store_profile_group(back)) {
        have = false;
    }
    sg_note_refused(refused, have, back, s_apply.reason);
    if (have) {
        r->sg_back = true;
        ESP_LOGW(TAG, "this receiver firmware does not accept signal group %u %u (%s; build 25164 accepts only "
                      STATION_SG_KNOWN_PAIRS "): back to %u %u, applied again%s",
                 (unsigned)refused.master, (unsigned)refused.slave, s_apply.reason, (unsigned)back.master,
                 (unsigned)back.slave, s_apply.rolled_back ? "" : " (nothing changed on the receiver: no restart)");
    } else {
        ESP_LOGE(TAG, "this receiver firmware does not accept signal group %u %u (%s; build 25164 accepts only "
                      STATION_SG_KNOWN_PAIRS ") and the group it ran before is not known",
                 (unsigned)refused.master, (unsigned)refused.slave, s_apply.reason);
    }
}

/* The comparison's own switch ended: tell its core; a refused group goes
 * where the test goes next (its restore, or the group the receiver runs). */
static void sg_test_apply_done(const stn_job_t *job, stn_result_t *r)
{
    (void)job;
    sgt_apply_t a;
    memset(&a, 0, sizeof(a));
    a.ok = r->err == STN_OK;
    a.rejected = !a.ok && s_apply.signal_group_rejected;
    a.read_known = s_apply.sg_read.known && !r->nothing_sent;
    a.read = (station_sg_t){ .master = s_apply.sg_read.master, .slave = s_apply.sg_read.slave };
    snprintf(a.reason, sizeof(a.reason), "%s", s_apply.reason[0] != '\0' ? s_apply.reason : "failed");
    station_sg_t refused = { .master = s_profile.signal_group_master, .slave = s_profile.signal_group_slave };
    station_sg_t next = refused;
    bool want = false;
    bool idle = false;
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    sgt_apply_done(&s_sgt, &a, now_ms(), unix_now());
    if (s_sgt.want_switch) {
        want = true;
        next = s_sgt.target;
    } else if (!sgt_running(&s_sgt)) {
        idle = true;
        next = s_sgt.cur_known ? s_sgt.cur : s_sgt.before;
    }
    if (a.rejected && (want || idle)) {
        /* The profile gets the group the test goes to next, right below:
         * the record says so first (best effort: the refused group cannot
         * stay in the profile either way). */
        (void)sg_record_flush(now_ms());
    }
    xSemaphoreGive(s_sgt_mx);
    if (!a.rejected) {
        return;   /* a success, or another failure: the station core's usual path (the test restores itself) */
    }
    ESP_LOGW(TAG, "comparison: the receiver refused signal group %u %u (%s)", (unsigned)refused.master,
             (unsigned)refused.slave, a.reason);
    /* The profile holds the refused group: put back the group the test goes
     * to (or ends on) and let the station re-apply it without CONFIG_FAILED. */
    if (!want && !idle) {
        sg_refused_fallback(r);   /* not a switch the test waited for: as for any apply */
        return;
    }
    if (store_profile_group(next)) {
        r->sg_back = true;
        if (want) {
            xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
            sgt_switch_queued(&s_sgt);   /* the core queues the re-apply (STN_WHY_SG_TEST) in stn_done */
            xSemaphoreGive(s_sgt_mx);
        }
    }
}

/* "galileo E1 E5a E5b E6 (4 bands)" for each constellation of a report, into out. */
static void bands_text(const um98x_bands_t *b, char *out, size_t size)
{
    size_t n = 0;
    out[0] = '\0';
    for (unsigned sys = 0; sys < UM98X_BANDS_SYS && n < size; sys++) {
        if ((b->present & (1u << sys)) == 0) {
            continue;
        }
        int w = snprintf(out + n, size - n, "%s%s", n > 0 ? "; " : "", um98x_bands_sys_name(sys));
        n += w > 0 ? (size_t)w : 0;
        for (unsigned bit = 0; bit < 32 && n < size; bit++) {
            if ((b->mask[sys] & (1u << bit)) == 0) {
                continue;
            }
            const char *name = um98x_bands_signal_name(sys, bit);
            w = name != NULL ? snprintf(out + n, size - n, " %s", name) : snprintf(out + n, size - n, " bit%u", bit);
            n += w > 0 ? (size_t)w : 0;
        }
        if (n < size) {
            w = snprintf(out + n, size - n, " (%u bands)", (unsigned)um98x_bands_count(sys, b->mask[sys]));
            n += w > 0 ? (size_t)w : 0;
        }
    }
}

/* The session's signal report: kept for the status, judged against the
 * profile's main-antenna group (a mismatch raises the station's alert; an
 * unknown report clears it), logged. */
static void note_bands(void)
{
    if (!s_apply.bands_asked) {
        return;   /* the session ended before the query */
    }
    s_bands_asked = true;
    s_bands = s_apply.bands;
    s_bands_at_ms = now_ms();
    s_bands_unix = unix_now();
    s_bands_master = s_profile.signal_group_master;
    snprintf(s_bands_reply, sizeof(s_bands_reply), "%s", s_apply.bands_reply);
    uint32_t missing[UM98X_BANDS_SYS];
    s_bands_match = um98x_bands_match(s_bands_master, &s_bands, missing);
    int verdict = s_bands.state != UM98X_BANDS_OK ? -1 : s_bands_match ? 1 : 0;
    portENTER_CRITICAL(&s_lock);
    stn_note_signals(&s_core, verdict);
    portEXIT_CRITICAL(&s_lock);

    static char text[200];   /* station task only */
    switch ((um98x_bands_state_t)s_bands.state) {
    case UM98X_BANDS_OK:
        bands_text(&s_bands, text, sizeof(text));
        if (s_bands_match) {
            ESP_LOGI(TAG, "signal group %u configures: %s", (unsigned)s_bands.group, text);
        } else {
            ESP_LOGW(TAG, "the receiver reports signal group %u with %s, not what group %u should configure",
                     (unsigned)s_bands.group, text, (unsigned)s_bands_master);
        }
        break;
    case UM98X_BANDS_REFUSED:
        ESP_LOGI(TAG, "this receiver firmware does not report its signals per group (%s): unknown", s_bands_reply);
        break;
    default:
        ESP_LOGI(TAG, "the receiver's signals per group are unknown (%s)",
                 s_bands_reply[0] != '\0' ? s_bands_reply : "answer not read");
        break;
    }
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
    case STN_WHY_FLASH:
        return "after a receiver update";
    case STN_WHY_SG_TEST:
        return "signal-group comparison";
    case STN_WHY_SG_BACK:
        return "back to the receiver's signal group";
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
    /* A group to go back to if the receiver refuses this one (the last
     * successful apply's): a plain refusal then needs no RESET. */
    uint16_t ok_pair = 0;
    uint16_t want_pair = (uint16_t)(((unsigned)s_profile.signal_group_master << 8) | s_profile.signal_group_slave);
    bool fallback = settings_get_u16(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_SG_OK, &ok_pair) == ESP_OK && ok_pair != want_pair;
    um98x_apply_opts_t opts = { .force_mode = job->force_mode, .sg_fallback = fallback };
    esp_err_t err = um98x_ctl_apply_profile(s_lease, &s_profile, &opts, &s_apply);
    lease_drop();
    note_bands();

    if (s_apply.settings_read) {
        s_rx_valid = true;
        s_rx_settings = s_apply.settings;
        s_rx_at_ms = now_ms();
        s_rx_unix = unix_now();
    }
    s_ant_sent = s_apply.antenna_sent;
    s_ant_ok = s_apply.antenna_ok;
    snprintf(s_ant_reply, sizeof(s_ant_reply), "%s", s_apply.antenna_reply);
    s_b1c_sent = s_apply.b1cb2a_sent;
    s_b1c_ok = s_apply.b1cb2a_ok;
    snprintf(s_b1c_reply, sizeof(s_b1c_reply), "%s", s_apply.b1cb2a_reply);
    if (s_ant_sent && !s_ant_ok) {
        ESP_LOGW(TAG, "antenna model rejected: %s", s_ant_reply);
    }
    if (s_b1c_sent && !s_b1c_ok) {
        ESP_LOGW(TAG, "BeiDou B1C/B2a in RTCM not turned on: %s", s_b1c_reply);
    }
    r->mode_sent = s_apply.mode_sent;
    r->mode_ack_ms = s_apply.mode_ack_ms;
    r->restarted = s_apply.signal_group_sent || (s_apply.rolled_back && s_apply.rollback_answered);
    r->receiver_lost = s_apply.receiver_lost || err == ESP_ERR_NOT_FOUND;
    r->answered = s_apply.rolled_back ? s_apply.rollback_answered : answered_since(t0);
    switch (err) {
    case ESP_OK:
        r->err = STN_OK;
        gnss_obs_reset();   /* signals, bands and the 1005 point start over */
        first_apply_hold_update();   /* the apply's persist step erased gnss/hold */
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
    if (s_apply.sg_read.known) {
        s_rx_sg_valid = true;
        s_rx_sg = (station_sg_t){ .master = s_apply.sg_read.master, .slave = s_apply.sg_read.slave };
    }
    if (job->why == STN_WHY_SG_TEST) {
        sg_test_apply_done(job, r);
    } else if (err != ESP_OK && s_apply.signal_group_rejected) {
        sg_refused_fallback(r);
    }
    /* The failure is logged with what follows it once the core has
     * decided (STN_NOTE_APPLY_FAILED). */
}

static void job_restore(stn_result_t *r)
{
    if (lease_take("restore output") != ESP_OK) {
        r->err = STN_E_STATE;
        return;
    }
    ESP_LOGW(TAG, "no RTCM after the failed apply: restoring the RTCM outputs only");
    /* s_apply still holds the failed apply's result: what the receiver
     * refused there is left out, then the restore's own result replaces it. */
    esp_err_t err = um98x_ctl_restore_output(s_lease, &s_profile, &s_apply, &s_apply);
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

/* The fixed-position check's query: the receiver's own position, one line,
 * without a lease (nothing is held back). */
static void job_own_fix(const stn_job_t *job, stn_result_t *r)
{
    esp_err_t err = um98x_ctl_query_own_fix(NULL, job->sppnav, &r->own);
    switch (err) {
    case ESP_OK:
        r->err = STN_OK;
        ESP_LOGD(TAG, "own position (%s): %s type %u, %u satellites, sd %.1f/%.1f/%.1f m",
                 job->sppnav ? UM98X_CMD_SPPNAV : UM98X_CMD_BASEPOS, r->own.computed ? "computed" : "not computed",
                 (unsigned)r->own.pos_type, (unsigned)r->own.sats_used, r->own.sd_lat_m, r->own.sd_lon_m,
                 r->own.sd_hgt_m);
        break;
    case ESP_ERR_NOT_SUPPORTED:
        r->err = STN_E_FAIL;
        ESP_LOGI(TAG, "the receiver refused %s", job->sppnav ? UM98X_CMD_SPPNAV : UM98X_CMD_BASEPOS);
        break;
    case ESP_ERR_INVALID_STATE:
        r->err = STN_E_STATE;
        break;
    default:
        r->err = STN_E_TIMEOUT;   /* no log, one not read, or the command layer busy */
        break;
    }
}

/* The question to a receiver that never streamed this boot: the version
 * query without a lease, only so that its answer (a line like any other)
 * keeps "the receiver answers" fresh. Logged when the outcome changes. */
static void job_ask(stn_result_t *r)
{
    static int s_last_ok = -1;
    esp_err_t err = um98x_ctl_query_version(NULL, false, NULL);
    if (err == ESP_ERR_INVALID_STATE) {
        r->err = STN_E_STATE;   /* not allowed in the link's mode now: nothing was sent */
        return;
    }
    /* A refusal, or an acknowledgement without the log, is an answer too. */
    int ok = err == ESP_OK || err == ESP_FAIL || err == ESP_ERR_INVALID_RESPONSE;
    r->err = ok ? STN_OK : STN_E_TIMEOUT;
    if (ok != s_last_ok) {
        if (ok) {
            ESP_LOGI(TAG, "the receiver answers when asked (no RTCM this boot)");
        } else {
            ESP_LOGW(TAG, "no answer to the station's question (%s)", esp_err_to_name(err));
        }
        s_last_ok = ok;
    }
}

/* Satellite-in-view output for the live view: one command under a short
 * COMMAND lease (RTCM is held back meanwhile; the casters are paused). */
static void job_sky(const stn_job_t *job, stn_result_t *r)
{
    int64_t t0 = esp_timer_get_time();
    if (lease_take(job->sky_on ? "live view on" : "live view off") != ESP_OK) {
        r->err = STN_E_STATE;
        return;
    }
    if (job->sky_on) {
        /* Before the command: a reset from here on still turns it off. */
        stn_sky_rtc_write(&s_sky_rtc, lifecycle_rtc_stamp(), true);
    }
    char reply[sizeof(s_sky_reply)];
    esp_err_t err = um98x_ctl_sat_view(s_lease, job->sky_on, CONFIG_STATION_LIVE_VIEW_SKY_PERIOD_S, reply,
                                       sizeof(reply));
    lease_drop();
    int64_t ms = (esp_timer_get_time() - t0) / 1000;
    r->lease_ms = ms < 0 ? 0 : (ms > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)ms);
    switch (err) {
    case ESP_OK:
        r->err = STN_OK;
        break;
    case ESP_FAIL:
        r->err = STN_E_FAIL;
        break;
    case ESP_ERR_TIMEOUT:
        r->err = STN_E_TIMEOUT;
        break;
    default:
        r->err = STN_E_STATE;
        break;
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "satellite-in-view output %s for the live view (RTCM held %u ms)", job->sky_on ? "on" : "off",
                 (unsigned)r->lease_ms);
    } else {
        snprintf(s_sky_reply, sizeof(s_sky_reply), "%s", reply[0] != '\0' ? reply : esp_err_to_name(err));
    }
}

/* ---- receiver update sessions ---------------------------------------------------- */

static void flash_last_set(uint8_t outcome, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void flash_last_set(uint8_t outcome, const char *fmt, ...)
{
    station_flash_last_t l;
    memset(&l, 0, sizeof(l));
    portENTER_CRITICAL(&s_lock);
    l = s_flash_last;
    portEXIT_CRITICAL(&s_lock);
    l.outcome = outcome;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(l.reason, sizeof(l.reason), fmt, ap);
    va_end(ap);
    l.ended_unix = unix_now();
    portENTER_CRITICAL(&s_lock);
    s_flash_last = l;
    s_flash_last_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_lock);
    ESP_LOGW(TAG, "receiver update: %s", l.reason);
}

/* Progress to the web (STATION_EVENT_FLASH_PROGRESS): every phase change, else at most once a second. */
static void post_progress(const um98x_flash_progress_t *p, bool force)
{
    int64_t now = esp_timer_get_time();
    if (!force && p->phase == s_progress_phase && now - s_progress_post_us < 1000000) {
        return;
    }
    s_progress_phase = p->phase;
    s_progress_post_us = now;
    esp_err_t err = esp_event_post(STATION_EVENT, STATION_EVENT_FLASH_PROGRESS, p, sizeof(*p), 0);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "flash progress not posted: %s", esp_err_to_name(err));
    }
}

/* The job's last message (STATION_EVENT_FLASH_ENDED): a session, the
 * user's action on a waiting one, the journal's close or a package check
 * ended. A FAILED phase posted while a session runs (a transfer that failed
 * and is tried again) is progress, never this. */
static void post_ended(const um98x_flash_progress_t *p)
{
    s_progress_phase = p->phase;
    s_progress_post_us = esp_timer_get_time();
    esp_err_t err = esp_event_post(STATION_EVENT, STATION_EVENT_FLASH_ENDED, p, sizeof(*p), 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "flash end not posted: %s", esp_err_to_name(err));
    }
}

static void final_progress(um98x_flash_phase_t phase)
{
    um98x_flash_progress_t p;
    um98x_flash_get_progress(&p);
    p.phase = phase;
    post_ended(&p);
}

/* A phase change of the job itself (it goes on). */
static void phase_progress(um98x_flash_phase_t phase)
{
    um98x_flash_progress_t p;
    um98x_flash_get_progress(&p);
    p.phase = phase;
    post_progress(&p, true);
}

static bool main_journal_state(uint8_t st)
{
    return st >= UM98X_JOURNAL_PREPARED && st <= UM98X_JOURNAL_VERIFYING;
}

static void flash_progress_cb(const um98x_flash_progress_t *p, void *ctx)
{
    (void)ctx;
    if (p->journal_state != 0) {
        /* The journal as the status reports it (the record itself is the flasher's). */
        portENTER_CRITICAL(&s_lock);
        s_journal_err = ESP_OK;
        s_journal.state = p->journal_state;
        if (main_journal_state(p->journal_state)) {
            s_journal.phase = p->journal_state;
        }
        s_journal.transfers = p->transfers;
        s_journal.entry_attempts = p->entry_attempts;
        s_journal.failure_class = p->failure_class;
        portEXIT_CRITICAL(&s_lock);
    }
    post_progress(p, false);
}

/* Nothing of the session is left: no journal, the package released, the lockout ended. */
static void flash_forget(bool release_package)
{
    um98x_flash_discard();
    if (release_package) {
        esp_err_t rel = pkg_store_release();
        if (rel != ESP_OK) {
            ESP_LOGW(TAG, "staged receiver package not released: %s", esp_err_to_name(rel));
        }
    }
    portENTER_CRITICAL(&s_lock);
    s_journal_err = ESP_ERR_NOT_FOUND;
    memset(&s_journal, 0, sizeof(s_journal));
    s_journal_parked = false;
    portEXIT_CRITICAL(&s_lock);
    lockout_release();
}

/* The session left the journal waiting for the user: reduced lockout, the package stays pinned. */
static void flash_parked(const um98x_flash_result_t *res)
{
    portENTER_CRITICAL(&s_lock);
    s_journal_err = ESP_OK;
    s_journal = res->journal;
    s_journal_parked = false;
    portEXIT_CRITICAL(&s_lock);
    lockout_release();
    um98x_flash_discard();
    flash_last_set(STATION_FLASH_OUT_NEEDS_USER, "waits for you: %s", res->reason);
}

static void flash_result_common(const um98x_flash_result_t *res, stn_result_t *r)
{
    switch (res->end) {
    case UM98X_FLASH_END_DONE:
    case UM98X_FLASH_END_ABORTED:
        r->flash_end = res->end == UM98X_FLASH_END_DONE ? STN_FLASH_END_REAPPLY : STN_FLASH_END_REAPPLY_RTCM;
        r->supported = um98x_ctl_model_supported(&res->receiver);
        s_after = res->receiver;
        s_after_aborted = (res->end == UM98X_FLASH_END_ABORTED);
        snprintf(s_after_reason, sizeof(s_after_reason), "%s", res->reason);
        portENTER_CRITICAL(&s_lock);
        s_journal = res->journal;
        portEXIT_CRITICAL(&s_lock);
        um98x_flash_discard();
        /* The journal stays open, the lockout held, until the re-apply is checked (STN_NOTE_FLASH_CLOSE). */
        break;
    case UM98X_FLASH_END_NEEDS_USER:
        r->flash_end = STN_FLASH_END_NEEDS_USER;
        flash_parked(res);
        break;
    default:
        r->flash_end = STN_FLASH_END_NOTHING;
        flash_last_set(STATION_FLASH_OUT_NOT_STARTED, "%s", res->reason);
        (void)pkg_store_set_pinned(false);
        flash_forget(true);
        break;
    }
}

static void flash_new(gnss_link_lease_t lease, stn_result_t *r)
{
    station_flash_request_t req;
    uint32_t stop_after;
    portENTER_CRITICAL(&s_lock);
    req = s_flash_req;
    stop_after = s_bench_stop_after;
    s_bench_stop_after = 0;
    bool key_allowed = !s_core.loader_key_sent;
    portEXIT_CRITICAL(&s_lock);
    um98x_flash_progress_t p;
    memset(&p, 0, sizeof(p));
    p.phase = UM98X_FLASH_CHECKING;
    p.bytes_total = req.size;
    post_progress(&p, true);

    um98x_flash_ident_t id;
    esp_err_t err = um98x_flash_identify(lease, home_baud(), key_allowed, &id);
    if (err != ESP_OK || id.result != UM98X_FLASH_ID_OK) {
        r->flash_end = STN_FLASH_END_NOTHING;
        if (err == ESP_OK && id.result == UM98X_FLASH_ID_AT_LOADER) {
            r->loader_key_sent = id.loader_key_sent;
            r->in_loader = !id.app_back;
            flash_last_set(STATION_FLASH_OUT_NOT_STARTED, "the receiver was at its boot menu (%u baud)%s",
                           (unsigned)id.loader_baud, id.app_back ? "; it is back in its application" : "");
        } else {
            flash_last_set(STATION_FLASH_OUT_NOT_STARTED, "the receiver did not answer its version query (%s)",
                           esp_err_to_name(err));
        }
        flash_forget(true);
        return;
    }
    um98x_flash_expect_t e;
    memset(&e, 0, sizeof(e));
    memcpy(e.sha256, req.sha256, sizeof(e.sha256));
    e.size = req.size;
    snprintf(e.expected_fw, sizeof(e.expected_fw), "%s", req.expected_fw);
    e.expected_build = req.expected_build;
    e.receiver = id.info;
    e.cross_layout_confirmed = req.cross_layout_confirmed;
    um98x_flash_pkg_facts_t f;
    err = um98x_flash_prepare(&e, &f);
    if (err != ESP_OK) {
        r->flash_end = STN_FLASH_END_NOTHING;
        portENTER_CRITICAL(&s_lock);
        s_flash_last.failed_rule = f.failed_rule;
        s_flash_last.cross_layout = f.cross_layout;
        s_flash_last.check_only = false;
        portEXIT_CRITICAL(&s_lock);
        if (f.failed_rule != 0) {
            flash_last_set(STATION_FLASH_OUT_REJECTED, "package refused by rule %u: %s", (unsigned)f.failed_rule, f.why);
        } else {
            flash_last_set(STATION_FLASH_OUT_NOT_STARTED, "package not checked: %s", f.why[0] ? f.why : esp_err_to_name(err));
        }
        flash_forget(true);   /* a failed validation releases the package */
        return;
    }
    /* Pin, inhibit (the update slot is already ours), then the journal. */
    err = pkg_store_set_pinned(true);
    if (err != ESP_OK) {
        r->flash_end = STN_FLASH_END_NOTHING;
        flash_last_set(STATION_FLASH_OUT_NOT_STARTED, "the staged package could not be pinned (%s)", esp_err_to_name(err));
        flash_forget(true);
        return;
    }
    lockout_take();
    um98x_flash_request_t fr;
    memset(&fr, 0, sizeof(fr));
    um98x_flash_journal_t *j = &fr.journal;
    memcpy(j->sha256, f.sha256, sizeof(j->sha256));
    j->size = req.size;
    snprintf(j->expected_fw, sizeof(j->expected_fw), "%s", req.expected_fw);
    j->expected_build = req.expected_build;
    snprintf(j->pre_model, sizeof(j->pre_model), "%s", id.info.model);
    snprintf(j->pre_fw, sizeof(j->pre_fw), "%s", id.info.firmware);
    j->pre_build = id.info.build;
    j->app_baud = id.app_baud;
    fr.progress_cb = flash_progress_cb;
    fr.stop_after_blocks = stop_after;
    um98x_flash_result_t res;
    err = um98x_flash_run(lease, &fr, &res);
    if (err != ESP_OK) {
        r->flash_end = STN_FLASH_END_NOTHING;
        flash_last_set(STATION_FLASH_OUT_NOT_STARTED, "flasher refused: %s", esp_err_to_name(err));
        (void)pkg_store_set_pinned(false);
        flash_forget(true);
        return;
    }
    flash_result_common(&res, r);
}

/* A user action or the boot-time recovery could not run (nothing was sent):
 * the journal waits for the user, in reduced lockout (the update slot the
 * action claimed is given back). A recovery that never ran is parked like a
 * report-only one, so the journal shows as waiting instead of running. */
static void flash_not_run(uint8_t kind, const char *why)
{
    if (kind == STN_FLASH_RECOVER) {
        portENTER_CRITICAL(&s_lock);
        s_journal_parked = true;
        portEXIT_CRITICAL(&s_lock);
    }
    lockout_release();
    flash_last_set(STATION_FLASH_OUT_NEEDS_USER, "not run: %s", why);
}

/* A journal waiting for the user (or the boot-time recovery): the journal as read. */
static void flash_action(gnss_link_lease_t lease, uint8_t kind, stn_result_t *r)
{
    um98x_flash_request_t fr;
    memset(&fr, 0, sizeof(fr));
    portENTER_CRITICAL(&s_lock);
    fr.journal = s_journal;
    bool race = s_race_prompt;
    int plan = s_recover_plan;
    portEXIT_CRITICAL(&s_lock);
    fr.progress_cb = flash_progress_cb;
    lockout_take();   /* the receiver is worked on again: full lockout until it waits or closes */
    um98x_flash_result_t res;
    esp_err_t err;
    switch (kind) {
    case STN_FLASH_RETRY:
        err = um98x_flash_user_retry(lease, &fr, &res);
        break;
    case STN_FLASH_RESTART:
        err = um98x_flash_user_restart(lease, &fr, &res);
        break;
    default:
        err = um98x_flash_recover(lease, &fr, plan, race, &res);
        break;
    }
    if (err != ESP_OK) {
        flash_not_run(kind, esp_err_to_name(err));   /* nothing sent: the journal still waits */
        return;
    }
    if (res.end == UM98X_FLASH_END_NEEDS_USER && kind == STN_FLASH_RECOVER && res.journal.state != UM98X_JOURNAL_NEEDS_USER) {
        /* A plan that only stops (the race at the prompt, a check): the record is left as it was. */
        res.journal = fr.journal;
    }
    flash_result_common(&res, r);
}

static void job_flash(const stn_job_t *job, stn_result_t *r)
{
    r->err = STN_OK;
    r->flash_end = STN_FLASH_END_NOTHING;
    release_lease(&s_hold_lease, "listen-only");   /* the session's RAW lease replaces it */
    gnss_link_lease_t lease = NULL;
    esp_err_t err = gnss_link_acquire(GNSS_LINK_MODE_RAW, "receiver update", LEASE_WAIT_MS, &lease);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no RAW lease for the receiver update: %s", esp_err_to_name(err));
        if (job->flash_kind == STN_FLASH_NEW) {
            flash_last_set(STATION_FLASH_OUT_NOT_STARTED, "the receiver link was busy (%s)", esp_err_to_name(err));
            flash_forget(true);
        } else {
            char why[48];
            snprintf(why, sizeof(why), "the receiver link was busy (%s)", esp_err_to_name(err));
            flash_not_run(job->flash_kind, why);
        }
    } else {
        if (job->flash_kind == STN_FLASH_NEW) {
            flash_new(lease, r);
        } else {
            flash_action(lease, job->flash_kind, r);
        }
        release_lease(&lease, "receiver update");
    }
    bool waits = r->flash_end == STN_FLASH_END_NEEDS_USER ||
                 (job->flash_kind != STN_FLASH_NEW && r->flash_end == STN_FLASH_END_NOTHING);
    if (waits) {
        hold_take();   /* the journal waits: listen only */
    }
    portENTER_CRITICAL(&s_lock);
    s_flash_running = false;   /* the job is over: a waiting journal may be acted on (or cleared) again */
    portEXIT_CRITICAL(&s_lock);
    if (r->flash_end == STN_FLASH_END_REAPPLY || r->flash_end == STN_FLASH_END_REAPPLY_RTCM) {
        phase_progress(UM98X_FLASH_FINISHING);   /* the re-apply and its check follow; flash_close ends the job */
    } else {
        final_progress(waits ? UM98X_FLASH_NEEDS_USER : UM98X_FLASH_FAILED);
    }
}

/* STN_NOTE_FLASH_CLOSE: the re-apply was checked (ok) or gave up. */
static void flash_close(bool reapply_ok)
{
    uint8_t outcome;
    if (s_after_aborted) {
        outcome = STATION_FLASH_OUT_NOT_DONE;
    } else if (s_after.build != 0 && s_after.build == s_journal.expected_build) {
        outcome = STATION_FLASH_OUT_SUCCESS;
    } else if (s_after.build != 0 && s_after.build == s_journal.pre_build) {
        outcome = STATION_FLASH_OUT_OLD_FIRMWARE;
    } else {
        outcome = STATION_FLASH_OUT_UNEXPECTED;
    }
    uint8_t transfers = s_journal.transfers;
    /* The outcome reaches the event journal before the session's journal is
     * closed, and both before the lockout ends (flash_forget, last): a reboot
     * queued behind the inhibit may follow at once, and the shutdown writes
     * out what diag has queued, not what waits in this task's queue. */
    esp_err_t jr = diag_journal_append(DIAG_EV_GNSS_FLASH, outcome, transfers);
    if (jr != ESP_OK && jr != ESP_ERR_NOT_FOUND) {
        ESP_LOGW(TAG, "update outcome not in the event journal yet (%s): queued", esp_err_to_name(jr));
        journal_push(DIAG_EV_GNSS_FLASH, outcome, transfers);
    }
    esp_err_t err = um98x_flash_journal_close();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "receiver flash journal not closed: %s", esp_err_to_name(err));
    }
    /* The identity the receiver reported is the stored one from now on. */
    if (s_after.valid && settings_is_available()) {
        (void)settings_set_state_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MODEL, s_after.model);
        (void)settings_set_state_str(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_FW, s_after.firmware);
        load_stored_identity();
    }
    portENTER_CRITICAL(&s_lock);
    s_flash_last.reapply_ok = reapply_ok;
    s_flash_last.build = s_after.build;
    snprintf(s_flash_last.firmware, sizeof(s_flash_last.firmware), "%s", s_after.firmware);
    portEXIT_CRITICAL(&s_lock);
    static const char *const k_what[] = { "", "updated", "the old firmware still runs", "an unexpected build runs",
                                          "not done, receiver unchanged" };
    flash_last_set(outcome, "%s (%s)%s; %s", outcome < sizeof(k_what) / sizeof(k_what[0]) ? k_what[outcome] : "?",
                   s_after.firmware, reapply_ok ? "" : ", the station profile did not come back: check the receiver",
                   s_after_reason);
    flash_forget(true);   /* releases the lockout last */
    final_progress(outcome == STATION_FLASH_OUT_SUCCESS ? UM98X_FLASH_DONE : UM98X_FLASH_FAILED);
}

/* Check only: rules 1-12 on the RAM copy against this boot's confirmed identity. */
static void job_pkg_check(stn_result_t *r)
{
    station_flash_request_t req;
    um98x_info_t info;
    portENTER_CRITICAL(&s_lock);
    req = s_check_req;
    portEXIT_CRITICAL(&s_lock);
    bool have = um98x_ctl_get_info(&info) == ESP_OK && info.valid;
    um98x_flash_expect_t e;
    memset(&e, 0, sizeof(e));
    memcpy(e.sha256, req.sha256, sizeof(e.sha256));
    e.size = req.size;
    snprintf(e.expected_fw, sizeof(e.expected_fw), "%s", req.expected_fw);
    e.expected_build = req.expected_build;
    if (have) {
        e.receiver = info;
    }
    e.cross_layout_confirmed = req.cross_layout_confirmed;
    um98x_flash_pkg_facts_t f;
    esp_err_t err = um98x_flash_prepare(&e, &f);
    portENTER_CRITICAL(&s_lock);
    s_flash_last.failed_rule = f.failed_rule;
    s_flash_last.cross_layout = f.cross_layout;
    s_flash_last.check_only = true;
    s_flash_last.build = 0;
    s_flash_last.firmware[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
    if (err == ESP_OK) {
        flash_last_set(STATION_FLASH_OUT_CHECKED, "the package passes every rule against %s %s; receiver flashing is "
                       "not enabled in this build, nothing was sent",
                       info.model, info.firmware);
    } else if (f.failed_rule != 0) {
        flash_last_set(STATION_FLASH_OUT_REJECTED, "package refused by rule %u: %s%s", (unsigned)f.failed_rule, f.why,
                       have ? "" : " (the receiver's identity is not confirmed yet)");
    } else {
        flash_last_set(STATION_FLASH_OUT_REJECTED, "package not checked: %s", f.why[0] ? f.why : esp_err_to_name(err));
    }
    um98x_flash_discard();
    esp_err_t rel = pkg_store_release();   /* nothing stays staged without a session */
    if (rel != ESP_OK) {
        ESP_LOGW(TAG, "staged receiver package not released: %s", esp_err_to_name(rel));
    }
    portENTER_CRITICAL(&s_lock);
    s_check_running = false;
    portEXIT_CRITICAL(&s_lock);
    um98x_flash_progress_t p;
    memset(&p, 0, sizeof(p));
    p.phase = err == ESP_OK ? UM98X_FLASH_DONE : UM98X_FLASH_FAILED;
    p.bytes_total = req.size;
    post_ended(&p);
    r->err = STN_OK;
}

#if CONFIG_UM98X_FLASH_BYTE_CAPTURE
/* Bench: the report-only loader prompt observer at a chosen baud under a
 * COMMAND lease (casters paused); nothing is transmitted. */
static void bench_prompt_watch(const station_bench_req_t *req, station_bench_res_t *out)
{
#if CONFIG_GNSS_LINK_BENCH_HOOKS
    gnss_link_prompt_obs_t before;
    gnss_link_prompt_obs_t after;
    if (lease_take("bench prompt watch") != ESP_OK) {
        snprintf(out->line, sizeof(out->line), "FAIL promptwatch: no COMMAND lease");
        return;
    }
    (void)gnss_link_bench_prompt_status(&before);
    esp_err_t err = gnss_link_set_baud(s_lease, req->baud);
    if (err == ESP_OK && !before.on) {
        err = gnss_link_bench_prompt_observe(true);
    }
    if (err == ESP_OK) {
        int64_t end = now_ms() + (int64_t)req->seconds * 1000;
        while (now_ms() < end) {
            vTaskDelay(pdMS_TO_TICKS(LISTEN_POLL_MS));
            feed_watchdog();
        }
    }
    if (!before.on) {
        (void)gnss_link_bench_prompt_observe(false);
    }
    (void)gnss_link_set_baud(s_lease, home_baud());   /* where the link listens between jobs */
    (void)gnss_link_bench_prompt_status(&after);
    lease_drop();
    if (err != ESP_OK) {
        snprintf(out->line, sizeof(out->line), "FAIL promptwatch %u: %s", (unsigned)req->baud, esp_err_to_name(err));
        return;
    }
    uint32_t hits = after.hits - before.hits;
    char ctx[4 * GNSS_LINK_PROMPT_CTX + 1] = "";   /* every byte as \xNN at worst */
    size_t k = 0;
    for (size_t i = 0; hits > 0 && i < after.context_len && k + 4 < sizeof(ctx); i++) {
        uint8_t c = after.context[i];
        k += (size_t)snprintf(ctx + k, sizeof(ctx) - k, (c >= 0x20 && c < 0x7F && c != '\\') ? "%c" : "\\x%02x", c);
    }
    out->pass = true;
    snprintf(out->line, sizeof(out->line), "PASS promptwatch %u baud %u s: %u hit(s), nothing sent%s%s%s",
             (unsigned)req->baud, (unsigned)req->seconds, (unsigned)hits, hits ? "; last context \"" : "", ctx,
             hits ? "\"" : "");
#else
    (void)req;
    snprintf(out->line, sizeof(out->line), "FAIL promptwatch: the link's bench hooks are not built in");
#endif
}

static void job_bench(stn_result_t *r)
{
    station_bench_req_t req;
    portENTER_CRITICAL(&s_lock);
    req = s_bench_req;
    portEXIT_CRITICAL(&s_lock);
    station_bench_res_t out;
    memset(&out, 0, sizeof(out));
    if (req.op == STATION_BENCH_PROMPT_WATCH || req.op == STATION_BENCH_LINK_RESTART) {
        if (req.op == STATION_BENCH_PROMPT_WATCH) {
            bench_prompt_watch(&req, &out);
        } else {
            /* The ladder's link restart: the installed driver reconfigured in place, nothing sent. */
            esp_err_t err = gnss_link_restart(NULL);
            out.pass = err == ESP_OK;
            snprintf(out.line, sizeof(out.line), "%s linkrestart: %s", out.pass ? "PASS" : "FAIL", esp_err_to_name(err));
        }
        portENTER_CRITICAL(&s_lock);
        s_bench_res = out;
        portEXIT_CRITICAL(&s_lock);
        if (s_bench_done != NULL) {
            xSemaphoreGive(s_bench_done);
        }
        r->err = STN_OK;
        return;
    }
    gnss_link_lease_t lease = NULL;
    esp_err_t err = gnss_link_acquire(GNSS_LINK_MODE_RAW, "bench", LEASE_WAIT_MS, &lease);
    if (err != ESP_OK) {
        snprintf(out.line, sizeof(out.line), "FAIL no RAW lease: %s", esp_err_to_name(err));
    } else {
        um98x_bench_req_t b = {
            .op = req.op, .baud = req.baud, .seconds = req.seconds, .app_baud = home_baud(), .record_only = req.record_only,
        };
        um98x_bench_res_t br;
        err = um98x_flash_bench(lease, &b, &br);
        out.pass = (err == ESP_OK) && br.pass;
        snprintf(out.line, sizeof(out.line), "%s", err == ESP_OK ? br.line : esp_err_to_name(err));
        release_lease(&lease, "bench");
    }
    portENTER_CRITICAL(&s_lock);
    s_bench_res = out;
    portEXIT_CRITICAL(&s_lock);
    if (s_bench_done != NULL) {
        xSemaphoreGive(s_bench_done);
    }
    r->err = STN_OK;
}
#else
static void job_bench(stn_result_t *r)
{
    r->err = STN_E_STATE;
}
#endif

static void rxboot_tick(void);

static void run_job(const stn_job_t *job, stn_result_t *r)
{
    memset(r, 0, sizeof(*r));
    if (s_rxb_judging && (job->kind == STN_JOB_APPLY || job->kind == STN_JOB_RECEIVER_RESET ||
                          job->kind == STN_JOB_FLASH || job->kind == STN_JOB_BENCH ||
                          job->kind == STN_JOB_LEAVE_LOADER)) {
        /* This job may restart the receiver: a version log after a restart
         * of ours tells nothing about the power. What came before counts. */
        rxboot_tick();
        s_rxb_clock_closed = true;
    }
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
    case STN_JOB_SKY:
        job_sky(job, r);
        break;
    case STN_JOB_FLASH:
        job_flash(job, r);
        break;
    case STN_JOB_BENCH:
        job_bench(r);
        break;
    case STN_JOB_PKG_CHECK:
        job_pkg_check(r);
        break;
    case STN_JOB_OWN_FIX:
        job_own_fix(job, r);
        break;
    case STN_JOB_ASK:
        job_ask(r);
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
    in->profile_sky = s_profile.sat_view_output;
    in->fixed_valid = (s_profile.base_mode == UM98X_BASE_FIXED);
    if (in->fixed_valid) {
        in->fixed_llh[0] = s_profile.fixed_lat_deg;
        in->fixed_llh[1] = s_profile.fixed_lon_deg;
        in->fixed_llh[2] = s_profile.fixed_height_m;
    }
    portENTER_CRITICAL(&s_lock);
    s_obs_snap = s_obs;
    s_in_profile_sky = in->profile_sky;
    s_in_autocfg = in->autocfg;
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
    o->identity_confirmed = core.identity_ok;
    o->alerts = stn_alerts(&core);
    o->profile_applied = core.profile_applied;
    o->rtcm_differs = core.rtcm_differs;
    o->rtcm1230_supported = in->rtcm1230_ok;
    o->rtcm1230_known = rtcm1230_state() >= 0;
    o->config_failed = (core.alerts & STN_ALERT_CONFIG_FAILED) != 0;
    o->config_retry_in_s = stn_config_retry_in_s(&core, in->autocfg, in->now_ms);
    o->config_auto_retry = stn_config_auto_retry(&core, in->autocfg);
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
    o->detect_passes = core.detect_passes;
    o->next_detect_in_s = stn_next_detect_in_s(&core, in->now_ms);
    um98x_flash_get_progress(&o->flash);

    o->rx_settings_valid = s_rx_valid;
    if (s_rx_valid) {
        o->rx_settings = s_rx_settings;
        int64_t age = (in->now_ms - s_rx_at_ms) / 1000;
        o->rx_settings_age_s = age < 0 ? 0 : (age > (int64_t)UINT32_MAX ? UINT32_MAX : (uint32_t)age);
        o->rx_settings_unix = s_rx_unix;
    }
    o->antenna_sent = s_ant_sent;
    o->antenna_ok = s_ant_ok;
    memcpy(o->antenna_reply, s_ant_reply, sizeof(o->antenna_reply));
    o->b1cb2a_sent = s_b1c_sent;
    o->b1cb2a_ok = s_b1c_ok;
    memcpy(o->b1cb2a_reply, s_b1c_reply, sizeof(o->b1cb2a_reply));

    o->rx_sg_valid = s_rx_sg_valid;
    o->rx_sg = s_rx_sg;
    o->rx_bands_age_s = UINT32_MAX;
    if (s_bands_asked) {
        o->rx_bands = s_bands;
        int64_t age = (in->now_ms - s_bands_at_ms) / 1000;
        o->rx_bands_age_s = age < 0 ? 0 : (age >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)age);
        o->rx_bands_unix = s_bands_unix;
        o->rx_bands_profile_master = s_bands_master;
        o->rx_bands_match = s_bands_match;
        memcpy(o->rx_bands_reply, s_bands_reply, sizeof(o->rx_bands_reply));
    }
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    sgt_brief(&s_sgt, in->now_ms, &o->sg_test);
    xSemaphoreGive(s_sgt_mx);

    stn_fix_status_t fx;
    stn_fix_status(&core, in->now_ms, &fx);
    o->fix_state = fx.state;
    o->fix_horizontal_m = fx.offset.horizontal_m;
    o->fix_up_m = fx.offset.up_m;
    o->fix_distance_m = fx.offset.distance_m;
    o->fix_height_only = fx.height_only;
    o->fix_checked_age_s = fx.checked_age_s;

    o->first_apply_held = core.first_apply_held;
    o->rx_maintenance = core.maint;
    o->maintenance_left_s = stn_maint_left_s(&core, in->now_ms);
    o->maintenance_chosen_s = core.maint ? core.maint_chosen_s : 0;

    stn_live_status_t lv;
    stn_live_status(&core, in, &lv);
    o->live.open = lv.open;
    o->live.left_s = lv.left_s;
    o->live.idle_left_s = lv.idle_left_s;
    o->live.open_s = lv.open_s;
    o->live.max_s = (uint32_t)CONFIG_STATION_LIVE_VIEW_MAX_MIN * 60u;
    o->live.sky = lv.sky;
    o->live.sky_why = lv.why;
    o->live.sky_period_s = CONFIG_STATION_LIVE_VIEW_SKY_PERIOD_S;
    o->live.last_end = lv.last_end;
    if (lv.why == STN_SKY_WHY_REJECTED || lv.why == STN_SKY_WHY_NO_ANSWER) {
        memcpy(o->live.sky_reply, s_sky_reply, sizeof(o->live.sky_reply));
    }

    portENTER_CRITICAL(&s_lock);
    s_status = *o;
    s_status_valid = true;
    portEXIT_CRITICAL(&s_lock);
}

/* Fields every status read gets fresh (also uninitialised, safe mode). */
static void status_flash_fields(station_status_t *out)
{
    portENTER_CRITICAL(&s_lock);
    out->flash_last = s_flash_last;
    int64_t at = s_flash_last_us;
    out->pkg_check_running = s_check_running;
    portEXIT_CRITICAL(&s_lock);
    out->flash_available = station_flash_available();
    if (at == INT64_MIN) {
        out->flash_last.ended_age_s = UINT32_MAX;
    } else {
        int64_t s = (esp_timer_get_time() - at) / 1000000;
        out->flash_last.ended_age_s = s < 0 ? 0 : (s >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)s);
    }
}

static void journal_push(diag_event_t kind, int32_t a, int32_t b)
{
    if (s_jq_n >= JQ_LEN) {
        memmove(&s_jq[0], &s_jq[1], sizeof(s_jq[0]) * (JQ_LEN - 1u));   /* the oldest gives way */
        s_jq_n = JQ_LEN - 1u;
    }
    s_jq[s_jq_n++] = (jrec_t){ kind, a, b };
}

/* Write what the journal takes now; a rate-limited record waits for the
 * next loop, anything else that fails is dropped. */
static void journal_flush(void)
{
    while (s_jq_n > 0) {
        esp_err_t err = diag_journal_append(s_jq[0].kind, s_jq[0].a, s_jq[0].b);
        if (err == ESP_ERR_INVALID_STATE) {
            return;
        }
        memmove(&s_jq[0], &s_jq[1], sizeof(s_jq[0]) * (s_jq_n - 1u));
        s_jq_n--;
    }
}

static int32_t sky_detail(int32_t err)
{
    switch (err) {
    case STN_OK:
        return 0;
    case STN_E_FAIL:
        return 1;
    case STN_E_TIMEOUT:
        return 2;
    default:
        return 3;
    }
}

static const char *live_end_name(int32_t why)
{
    switch (why) {
    case STN_LIVE_END_USER:
        return "stopped";
    case STN_LIVE_END_EXPIRED:
        return "time up";
    case STN_LIVE_END_IDLE:
        return "nobody watching";
    case STN_LIVE_END_FLASH:
        return "receiver update";
    default:
        return "?";
    }
}

static void handle_notes(const stn_note_t *n, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        const stn_note_t *x = &n[i];
        switch (x->kind) {
        case STN_NOTE_MAINT_ON:
            ESP_LOGW(TAG, "receiver maintenance for %d min: nothing is sent to the receiver", (int)(x->a / 60));
            journal_push(DIAG_EV_RX_MAINT, 1, x->a);
            break;
        case STN_NOTE_MAINT_OFF:
            ESP_LOGW(TAG, "receiver maintenance ended (%s) after %d s: listening first, as at boot",
                     x->b == STN_MAINT_END_TIME    ? "time up"
                     : x->b == STN_MAINT_END_POWER ? "the power was cut: the receiver restarted with the ESP32"
                                                   : "by the user",
                     (int)x->a);
            journal_push(DIAG_EV_RX_MAINT, 0, x->a);
            break;
        case STN_NOTE_LIVE_OPENED:
            ESP_LOGI(TAG, "live view open for %d s", (int)x->a);
            journal_push(DIAG_EV_LIVE_VIEW, DIAG_LIVE_OPENED, x->a);
            break;
        case STN_NOTE_LIVE_EXTENDED:
            ESP_LOGI(TAG, "live view extended: %d s left", (int)x->a);
            journal_push(DIAG_EV_LIVE_VIEW, DIAG_LIVE_EXTENDED, x->a);
            break;
        case STN_NOTE_LIVE_CLOSED:
            ESP_LOGI(TAG, "live view closed (%s) after %d s", live_end_name(x->b), (int)x->a);
            journal_push(DIAG_EV_LIVE_VIEW, DIAG_LIVE_CLOSED | (int32_t)((uint32_t)x->b << 8), x->a);
            break;
        case STN_NOTE_SKY_ON:
        case STN_NOTE_SKY_OFF:
            journal_push(DIAG_EV_LIVE_VIEW,
                         (x->kind == STN_NOTE_SKY_ON ? DIAG_LIVE_SKY_ON : DIAG_LIVE_SKY_OFF) |
                             (int32_t)((uint32_t)sky_detail(x->b) << 8),
                         x->a);
            break;
        case STN_NOTE_SKY_LEFT_ON:
            ESP_LOGW(TAG, "satellite-in-view output not turned off after %d tries: it ends with the receiver's "
                          "next restart",
                     STN_SKY_OFF_TRIES);
            break;
        case STN_NOTE_FLASH_CLOSE:
            flash_close(x->a != 0);
            break;
        case STN_NOTE_FIXED_FAR:
            ESP_LOGW(TAG, "the fixed position is %d m from the receiver's own solution horizontally and %+d m in "
                          "height: check it (streaming goes on)",
                     (int)x->a, (int)x->b);
            journal_push(DIAG_EV_FIXED_POS, diag_fixed_a(DIAG_FIXED_FAR, x->b), x->a);
            break;
        case STN_NOTE_FIXED_NEAR:
            ESP_LOGI(TAG, "the fixed position is within the limits again (%d m horizontally, %+d m in height)",
                     (int)x->a, (int)x->b);
            journal_push(DIAG_EV_FIXED_POS, diag_fixed_a(DIAG_FIXED_NEAR, x->b), x->a);
            break;
        case STN_NOTE_FIXED_CHANGED:
            ESP_LOGI(TAG, "fixed position changed: its warning is cleared until it is checked again");
            journal_push(DIAG_EV_FIXED_POS, diag_fixed_a(DIAG_FIXED_CHANGED, 0), 0);
            break;
        case STN_NOTE_APPLY_FAILED: {
            char next[112];
            (void)stn_apply_next_text(x->a > 0 ? (uint32_t)x->a : 0u, next, sizeof(next));
            ESP_LOGE(TAG, "profile apply failed at \"%s\" (%s)%s: %s", s_apply.failed_cmd, s_apply.reason,
                     x->b ? ", receiver lost" : "", next);
            break;
        }
        default:
            break;
        }
    }
}

/* The stored copy follows the core: written when due (stn_maint_store_due),
 * erased once maintenance is off. A write that fails takes the old copy
 * away (past its period it could resume more than is left); both are tried
 * again after a while. Nothing after a factory reset. */
#define MAINT_STORE_RETRY_MS 10000

static void sync_maint_store(bool on, bool due, const stn_maint_rec_t *rec, int64_t now)
{
    if (s_maint_store_retry_ms != STN_NEVER && now < s_maint_store_retry_ms) {
        return;
    }
    s_maint_store_retry_ms = STN_NEVER;
    if (on && due && !s_factory_reset) {
        uint8_t buf[STN_MAINT_STORE_MAX];
        size_t len = stn_maint_store_encode(rec, buf, sizeof(buf));
        esp_err_t err = len > 0 ? settings_set_state_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MAINT, buf, len)
                                : ESP_ERR_INVALID_SIZE;
        if (err == ESP_OK) {
            s_maint_stored = true;
            s_maint_saved_ms = now;
            s_maint_saved_chosen = rec->chosen_s;
            return;
        }
        esp_err_t erase = settings_erase_state_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MAINT);
        if (erase == ESP_OK) {
            s_maint_stored = false;
        }
        s_maint_saved_ms = STN_NEVER;
        s_maint_store_retry_ms = now + MAINT_STORE_RETRY_MS;
        ESP_LOGW(TAG, "receiver maintenance not stored (%s; old copy %s): only a reset that keeps the RTC memory "
                      "keeps it",
                 esp_err_to_name(err), erase == ESP_OK ? "removed" : esp_err_to_name(erase));
        return;
    }
    if ((!on || s_factory_reset) && s_maint_stored) {
        esp_err_t err = settings_erase_state_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MAINT);
        if (err == ESP_OK) {
            s_maint_stored = false;
            s_maint_saved_ms = STN_NEVER;
        } else {
            s_maint_store_retry_ms = now + MAINT_STORE_RETRY_MS;
            ESP_LOGW(TAG, "receiver maintenance's stored copy not removed (%s): tried again shortly",
                     esp_err_to_name(err));
        }
    }
}

/* Maintenance: the link's gate, the ledger, the RTC record and the stored
 * copy follow the core. The gate is compared with the link's own state, not
 * with what this task last set: station_maintenance_start sets it from the
 * API task, and an "end now" handled before this ran would otherwise leave
 * it set. */
static void sync_maintenance(const stn_in_t *in)
{
    bool on;
    bool due;
    stn_maint_rec_t rec;
    stn_maint_rec_t store;
    int64_t utc = diag_utc_now();
    portENTER_CRITICAL(&s_lock);
    on = s_core.maint;
    stn_maint_record(&s_core, in->now_ms, utc, false, &rec);
    due = stn_maint_store_due(&s_core, s_maint_saved_chosen, s_maint_saved_ms, in->now_ms);
    stn_maint_record(&s_core, in->now_ms, utc, true, &store);
    portEXIT_CRITICAL(&s_lock);
    gnss_link_stats_t ls;
    bool gate = (gnss_link_get_stats(&ls) == ESP_OK) ? ls.hands_off : s_gate_on;
    if (on != gate) {
        esp_err_t err = gnss_link_set_hands_off(on);
        if (err == ESP_OK) {
            gate = on;
        } else {
            ESP_LOGW(TAG, "link hands-off %s: %s", on ? "not set" : "not cleared", esp_err_to_name(err));
        }
    }
    if (gate == on && on != s_gate_on) {
        s_gate_on = on;
        ledger_set_maintenance(on);
    }
    uint32_t stamp = lifecycle_rtc_stamp();
    if (s_factory_reset) {
        memset(&rec, 0, sizeof(rec));   /* "ended": the boot after the reset resumes nothing */
    }
    stn_maint_rec_t cur;
    if (rec.on || !stn_maint_rtc_read(&s_maint_rtc, &cur) || cur.on || cur.stamp != stamp) {
        rec.stamp = stamp;
        rec.rtc_us = esp_rtc_get_time_us();
        stn_maint_rtc_write(&s_maint_rtc, &rec);
    }
    store.stamp = stamp;
    sync_maint_store(on, due, &store, in->now_ms);
}

/* The casters pause for receiver work between two epochs, never inside one.
 * The hub is held at an epoch boundary (rtcm_hub_hold): each caster sends
 * every frame before that point, at its own pace, and nothing after it, so
 * RTCM that arrives until the receiver session takes the line (the next
 * epoch's station messages and first MSMs) never reaches a caster as an
 * epoch the session then cuts. An epoch in flight (an MSM announced more,
 * however quiet the line is now) goes out whole first. After EPOCH_WAIT_MS
 * the hold is forced: after an open epoch whose line went quiet for the
 * hub's gap rule with nothing waiting for the link's reader (its last MSM
 * was lost, or the receiver never clears the bit), else at that epoch's
 * start. Bytes waiting for the reader (gnss_link_rx_unread: in the UART
 * driver, or read and not yet framed) mean the rest of the epoch may be
 * among them, so the forced hold never goes after the epoch then. The
 * release (resume) starts every caster over at a whole epoch. */
static void hold_casters(void)
{
    int64_t start = now_ms();
    bool backlog = false;
    rtcm_hub_hold_t h;
    for (;;) {
        bool force = now_ms() - start >= EPOCH_WAIT_MS;
        bool unread = gnss_link_rx_unread() > 0;   /* read right before the decision */
        backlog = backlog || unread;
        h = rtcm_hub_hold(CONFIG_RTCM_HUB_BURST_GAP_MS, force, unread);
        if (h != RTCM_HUB_HOLD_NONE) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(EPOCH_POLL_MS));
    }
    unsigned waited = (unsigned)(now_ms() - start);
    const char *why = backlog ? " (bytes were waiting for the link's reader)" : "";
    switch (h) {
    case RTCM_HUB_HOLD_QUIET:
        ESP_LOGW(TAG, "casters held after an epoch without its last MSM: no epoch end within %u ms, the line "
                 "quiet for %u ms or more%s", (unsigned)EPOCH_WAIT_MS, (unsigned)CONFIG_RTCM_HUB_BURST_GAP_MS, why);
        break;
    case RTCM_HUB_HOLD_CUT:
        ESP_LOGW(TAG, "casters held at the start of the epoch in flight: no epoch end within %u ms%s",
                 (unsigned)EPOCH_WAIT_MS, why);
        break;
    default:
        if (waited >= HOLD_NOTE_MS) {
            /* A note, not a warning: an epoch in flight is normal; how long it
             * took tells whether the line or the reader was slow. */
            ESP_LOGI(TAG, "casters held at an epoch boundary after %u ms%s", waited, why);
        }
        break;
    }
}

static void after_step(const stn_in_t *in)
{
    bool paused;
    bool sky_on;
    station_state_t st;
    stn_note_t notes[STN_NOTES];
    unsigned n_notes;
    portENTER_CRITICAL(&s_lock);
    if (s_core.obs_rearm) {
        stn_obs_arm(&s_obs, s_profile.rtcm, s_profile.rtcm_count, in->now_ms);
        s_core.obs_rearm = false;
    }
    paused = s_core.paused;
    st = (station_state_t)s_core.state;
    sky_on = s_core.sky_on;
    n_notes = stn_take_notes(&s_core, notes, STN_NOTES);
    portEXIT_CRITICAL(&s_lock);

    sync_maintenance(in);
    uint32_t sky_stamp = 0;
    bool sky_rec = stn_sky_rtc_read(&s_sky_rtc, &sky_stamp);
    if (sky_on != sky_rec) {
        stn_sky_rtc_write(&s_sky_rtc, lifecycle_rtc_stamp(), sky_on);
    }
    handle_notes(notes, n_notes);
    journal_flush();

    if (paused != s_paused) {
        if (paused) {
            hold_casters();
        } else {
            rtcm_hub_release();   /* before the casters see the resume */
        }
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

/* ---- after a power-on reset: did the receiver keep running? ------------------ */

static int64_t us_to_ms(int64_t us)
{
    return us == INT64_MAX ? STN_NEVER : us / 1000;
}

/* What follows from the verdict (once per power-on boot). */
static void rxboot_decided(stn_rxboot_t v, stn_rxboot_by_t by)
{
    uint8_t st = v == STN_RXBOOT_KEPT        ? STATION_RX_BOOT_KEPT
                 : v == STN_RXBOOT_RESTARTED ? STATION_RX_BOOT_RESTARTED
                                             : STATION_RX_BOOT_UNKNOWN;
    portENTER_CRITICAL(&s_lock);
    s_rxb_state = st;
    s_rxb_by = (uint8_t)by;
    esp_err_t jerr = s_journal_err;
    um98x_flash_journal_t j = s_journal;
    portEXIT_CRITICAL(&s_lock);
    journal_push(DIAG_EV_RX_BOOT, (int32_t)v, (int32_t)by);

    unsigned up_s = (unsigned)(s_rxb_uptime_ms / 1000u);
    switch (v) {
    case STN_RXBOOT_KEPT:
        if (by == STN_RXBOOT_BY_UPTIME) {
            ESP_LOGW(TAG, "power-on reset, but the receiver kept running (it has run for %u s, since before the "
                          "ESP32 started): only the ESP32 was reset (its reset pin, such as the USB auto-reset); "
                          "not a power loss",
                     up_s);
        } else {
            ESP_LOGW(TAG, "power-on reset, but the receiver kept running (data at once, no restart banner): only "
                          "the ESP32 was reset (its reset pin, such as the USB auto-reset); not a power loss");
        }
        ledger_note_power_kept();
        break;
    case STN_RXBOOT_RESTARTED:
        s_maint_power_cut = true;   /* a maintenance resumed at this boot ends (service) */
        if (by == STN_RXBOOT_BY_UPTIME) {
            ESP_LOGW(TAG, "power-on reset: the receiver restarted too (it has run for %u s, less than the ESP32): "
                          "the power was cut",
                     up_s);
        } else {
            ESP_LOGW(TAG, "power-on reset: the receiver restarted too (its restart banner before any data): the "
                          "power was cut");
        }
        break;
    default:
        ESP_LOGW(TAG, "power-on reset: nothing the receiver sent in %u s tells whether it kept running; not counted "
                      "as a power loss",
                 (unsigned)(STN_RXBOOT_WINDOW_MS / 1000));
        break;
    }

    /* An open flash journal's boot plan took the power-on reset for a
     * receiver restart (station_early_boot): say what the verdict means for
     * it. Report-only builds log it; no build acts on the plan before it. */
    if (jerr == ESP_OK && j.state != UM98X_JOURNAL_NEEDS_USER && v != STN_RXBOOT_UNKNOWN) {
        const char *text = NULL;
        (void)um98x_flash_journal_plan(jerr, &j, v == STN_RXBOOT_RESTARTED, &text);
        ESP_LOGW(TAG, "receiver flash journal: by the receiver's word this was %s reset; that plan: %s",
                 v == STN_RXBOOT_RESTARTED ? "a power-on" : "an ESP32-only", text);
    }
}

/* Judge a power-on boot until there is a verdict (station task). */
static void rxboot_tick(void)
{
    if (!s_rxb_judging) {
        return;
    }
    int64_t now = now_ms();
    if (s_rxb_uptime_at_ms == STN_NEVER && !s_rxb_clock_closed) {
        um98x_ctl_stats_t st;
        if (um98x_ctl_get_stats(&st) == ESP_OK && st.version_age_ms != UINT32_MAX && st.rx_uptime_ms != UINT32_MAX) {
            s_rxb_uptime_at_ms = now - (int64_t)st.version_age_ms;
            s_rxb_uptime_ms = st.rx_uptime_ms;
        }
    }
    stn_rxboot_in_t in = { .now_ms = now, .uptime_at_ms = s_rxb_uptime_at_ms, .uptime_ms = s_rxb_uptime_ms };
    portENTER_CRITICAL(&s_lock);
    in.restart_ms = us_to_ms(s_rxb_restart_us);
    in.data_ms = us_to_ms(s_rxb_data_us);
    portEXIT_CRITICAL(&s_lock);
    stn_rxboot_by_t by = STN_RXBOOT_BY_NONE;
    stn_rxboot_t v = stn_rxboot_judge(&in, &by);
    if (v == STN_RXBOOT_PENDING) {
        return;
    }
    s_rxb_judging = false;
    rxboot_decided(v, by);
}

/* A maintenance resumed with its end by the clock, once the clock is set:
 * the ESP32 may have been off (or held in reset) for a while. */
static void maint_clock(const stn_in_t *in)
{
    int64_t utc = diag_utc_now();
    if (utc <= 0) {
        return;
    }
    bool checked;
    int64_t was;
    int64_t until;
    portENTER_CRITICAL(&s_lock);
    checked = s_core.maint && s_core.maint_end_unix > 0;
    was = s_core.maint_until_ms;
    stn_maint_clock(&s_core, utc, in->now_ms);
    until = s_core.maint_until_ms;
    portEXIT_CRITICAL(&s_lock);
    if (checked && until < was) {
        int64_t left = until - in->now_ms;
        ESP_LOGW(TAG, "receiver maintenance by the clock: %u s left, %u s less than counted (the ESP32 was off or held "
                      "in reset meanwhile)",
                 (unsigned)(left > 0 ? left / 1000 : 0), (unsigned)((was - until) / 1000));
    }
}

/* Flags from other tasks, the ledger and the survey fallback's 1005 feed. */
static void service(const stn_in_t *in)
{
    rxboot_tick();

    if (s_restart_pending) {
        s_restart_pending = false;
        ESP_LOGW(TAG, "the receiver restarted (its restart banner, its restarting answer or an unasked version log)");
        esp_err_t err = esp_event_post(STATION_EVENT, STATION_EVENT_RECEIVER_RESTARTED, NULL, 0, 0);
        if (err != ESP_OK) {
            ESP_LOGD(TAG, "restart event not posted: %s", esp_err_to_name(err));
        }
        portENTER_CRITICAL(&s_lock);
        stn_restart_seen(&s_core);
        portEXIT_CRITICAL(&s_lock);
    }

    if (s_maint_end_req) {
        s_maint_end_req = false;
        portENTER_CRITICAL(&s_lock);
        (void)stn_maint_end(&s_core, in);
        portEXIT_CRITICAL(&s_lock);
    }
    if (s_maint_power_cut) {
        s_maint_power_cut = false;
        portENTER_CRITICAL(&s_lock);
        (void)stn_maint_power_cut(&s_core, in);
        portEXIT_CRITICAL(&s_lock);
    }
    maint_clock(in);

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
              st.last_rtcm_age_ms <= STN_FRESH_MS;
    if (ok != s_rtcm_ok) {
        s_rtcm_ok = ok;
        ledger_set_rtcm_ok(ok);
    }
}

/* ---- the signal-group comparison ---------------------------------------------- */

/* The record buffer is used under s_sgt_mx only, encode and write (or read
 * and decode) together, so the shutdown hook and the station task never
 * share it half-written. */

/* Store the record now (s_sgt_mx held); marked saved only once it is. */
static esp_err_t sg_record_store(int64_t now)
{
    size_t len = sgt_encode(&s_sgt, now, s_sgt_rec, sizeof(s_sgt_rec));
    esp_err_t err = len == 0 ? ESP_ERR_INVALID_SIZE
                             : settings_set_state_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_SG_TEST, s_sgt_rec, len);
    if (err == ESP_OK) {
        sgt_saved(&s_sgt, now);
    }
    return err;
}

/* Store the record if it is due (s_sgt_mx held). A failed write is tried
 * again at the next transition or period, not at every pass of the loop;
 * while a switch waits for it, it stays due, so the switch never goes ahead
 * without its record (sg_service fails it instead). */
static esp_err_t sg_record_flush(int64_t now)
{
    if (!s_sgt.save_due) {
        return ESP_OK;
    }
    esp_err_t err = sg_record_store(now);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "comparison record not saved: %s", esp_err_to_name(err));
        if (!s_sgt.want_switch) {
            sgt_saved(&s_sgt, now);
        }
    }
    return err;
}

/* The record at boot: resume it; one that cannot be read ends the test. */
static void sg_test_load(void)
{
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    size_t len = sizeof(s_sgt_rec);
    esp_err_t err = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_SG_TEST, s_sgt_rec, &len);
    sgt_init(&s_sgt);
    bool bad = false;
    if (err == ESP_OK && !sgt_decode(&s_sgt, s_sgt_rec, len, now_ms())) {
        bad = true;
    } else if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        bad = err != ESP_ERR_INVALID_STATE;   /* settings unavailable: nothing to judge */
    }
    if (bad) {
        sgt_interrupted(&s_sgt, now_ms(), unix_now());
    }
    bool running = sgt_running(&s_sgt);
    uint8_t state = s_sgt.state;
    xSemaphoreGive(s_sgt_mx);
    if (bad) {
        esp_err_t e = settings_erase_state_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_SG_TEST);
        ESP_LOGW(TAG, "signal-group comparison: its record could not be read (%s): ended as interrupted%s",
                 esp_err_to_name(err), e == ESP_OK ? "" : " (record not erased)");
    } else if (running) {
        ESP_LOGW(TAG, "signal-group comparison resumes after the restart (state %u)", (unsigned)state);
    }
}

/* A planned restart (the user's, an update's): store the running comparison
 * with what it measured since its last periodic save, and a test that ended
 * (keep, stop) before the station task stored its end: otherwise the next
 * boot resumes the test from its older record. */
static void sg_test_on_shutdown(lifecycle_reboot_reason_t reason, uint32_t deadline_ms, void *ctx)
{
    (void)reason;
    (void)ctx;
    if (s_sgt_mx == NULL || xSemaphoreTake(s_sgt_mx, pdMS_TO_TICKS(deadline_ms < 200u ? deadline_ms : 200u)) != pdTRUE) {
        return;
    }
    bool due = sgt_store_at_shutdown(&s_sgt);
    esp_err_t err = due ? sg_record_store(now_ms()) : ESP_OK;
    xSemaphoreGive(s_sgt_mx);
    if (due) {
        ESP_LOGI(TAG, "comparison record %s before the restart", err == ESP_OK ? "stored" : "not stored");
    }
}

/* One sample for the comparison: the live totals and the observation summary. */
static bool sg_sample(sgt_sample_t *out, bool streaming)
{
    EXT_RAM_BSS_ATTR static gnss_obs_summary_t sum;   /* station task only */
    EXT_RAM_BSS_ATTR static gnss_obs_live_t live;
    memset(out, 0, sizeof(*out));
    out->streaming = streaming;
    size_t n = 0;
    if (gnss_obs_get_summary(&sum) != ESP_OK || gnss_obs_get_live(NULL, &n, &live) != ESP_OK) {
        return false;
    }
    out->epoch_age_ms = sum.epoch_age_ms;
    out->epoch_gaps = sum.epoch_gaps;
    for (unsigned s = 0; s < STATION_SGT_SYS; s++) {
        out->sys_sats[s] = live.sys_sats[s];
        out->signal_mask[s] = sum.sys[s].present ? sum.sys[s].signal_mask : 0;
        for (unsigned b = 0; b < STATION_SGT_BANDS; b++) {
            out->band_sats[s][b] = live.sys_band_sats[s][b];
            out->cnr_dbhz[s][b] = sum.sys[s].present ? sum.sys[s].cnr_dbhz[b] : 0;
        }
    }
    return true;
}

static const char *sgt_outcome_text(int32_t o)
{
    static const char *const k[] = { "none", "kept", "timed out", "cancelled", "rejected", "failed", "interrupted" };
    return (o >= 0 && (size_t)o < sizeof(k) / sizeof(k[0])) ? k[o] : "?";
}

/* The comparison's clock, samples, switches, record and journal (station task). */
static void sg_service(const stn_in_t *in)
{
    EXT_RAM_BSS_ATTR static sgt_sample_t smp;
    bool streaming;
    portENTER_CRITICAL(&s_lock);
    streaming = s_core.state == STN_STREAMING && !s_core.maint;
    portEXIT_CRITICAL(&s_lock);
    int64_t unix_s = unix_now();

    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    bool due = sgt_sample_due(&s_sgt, in->now_ms);
    xSemaphoreGive(s_sgt_mx);
    bool have = due && sg_sample(&smp, streaming);

    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    sgt_tick(&s_sgt, streaming, have ? &smp : NULL, in->now_ms, unix_s);
    bool want = s_sgt.want_switch;
    station_sg_t target = s_sgt.target;
    /* The record first: it names the switch before the profile does, so a
     * restart in between asks for the switch again (sg_test_core.h). */
    esp_err_t rec = sg_record_flush(in->now_ms);
    if (want && rec != ESP_OK) {
        /* No switch the stored record does not know of: it fails instead. */
        sgt_switch_queued(&s_sgt);
        sgt_apply_t a = { .ok = false };
        snprintf(a.reason, sizeof(a.reason), "comparison record not saved");
        sgt_apply_done(&s_sgt, &a, in->now_ms, unix_s);
        want = false;
    }
    xSemaphoreGive(s_sgt_mx);

    if (want) {
        /* The station's own reconfigure: casters paused, planned downtime. */
        bool ok = store_profile_group(target);
        xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
        if (s_sgt.want_switch && s_sgt.target.master == target.master && s_sgt.target.slave == target.slave) {
            sgt_switch_queued(&s_sgt);
            if (!ok) {
                sgt_apply_t a = { .ok = false };
                snprintf(a.reason, sizeof(a.reason), "profile not saved");
                sgt_apply_done(&s_sgt, &a, in->now_ms, unix_s);
            }
        }
        xSemaphoreGive(s_sgt_mx);
        if (ok) {
            portENTER_CRITICAL(&s_lock);
            stn_request_apply_why(&s_core, STN_WHY_SG_TEST);
            portEXIT_CRITICAL(&s_lock);
            ESP_LOGI(TAG, "comparison: switching the receiver to signal group %u %u", (unsigned)target.master,
                     (unsigned)target.slave);
        }
    }

    sgt_note_t notes[SGT_NOTES];
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    unsigned nn = sgt_take_notes(&s_sgt, notes, SGT_NOTES);
    (void)sg_record_flush(in->now_ms);   /* what the switch's result changed */
    xSemaphoreGive(s_sgt_mx);
    for (unsigned i = 0; i < nn; i++) {
        const sgt_note_t *n = &notes[i];
        unsigned m = ((uint32_t)n->a >> 8) & 0xFFu;
        unsigned sl = ((uint32_t)n->a >> 16) & 0xFFu;
        switch (n->event) {
        case SGT_EV_STARTED:
            ESP_LOGI(TAG, "comparison started: %u s per group, %u %u first", (unsigned)n->b, m, sl);
            break;
        case SGT_EV_SWITCHED:
            ESP_LOGI(TAG, "comparison: signal group %u %u runs (phase %d)", m, sl, (int)n->b);
            break;
        case SGT_EV_ENDED:
            ESP_LOGI(TAG, "comparison ended (%s) on signal group %u %u", sgt_outcome_text(n->b), m, sl);
            break;
        default:
            break;
        }
        if (n->event == SGT_EV_REFUSED) {
            /* The refusal also shows on the receiver page until the next profile save. */
            station_sg_t refused = { .master = (uint8_t)m, .slave = (uint8_t)sl };
            station_sg_t back = { .master = (uint8_t)((uint32_t)n->b >> 8), .slave = (uint8_t)n->b };
            sg_note_refused(refused, true, back, s_apply.reason);   /* journals it */
            continue;
        }
        journal_push(DIAG_EV_SG_TEST, n->a, n->b);
    }
}

/* Listen only while a journal is open and nothing runs: a COMMAND lease that
 * is never written to keeps the hub unfed and refuses every lease-free write. */
static void hold_take(void)
{
    if (s_hold_lease == NULL &&
        gnss_link_acquire(GNSS_LINK_MODE_COMMAND, "flash journal", LEASE_WAIT_MS, &s_hold_lease) != ESP_OK) {
        s_hold_lease = NULL;
        ESP_LOGE(TAG, "no listen-only lease for the open flash journal");
    }
}

/* An open journal: the UART is the flash session's; casters paused. With
 * the recovery actions built in, the recovery table's plan runs as the
 * first job (s_recover_wanted). Otherwise (report-only) the plan is logged,
 * and a session that is not already waiting for the user waits for the user
 * here (reduced lockout: the inhibit and the update slot are released, so
 * app updates and reboots work again); its record is left as it was. */
static bool s_recover_wanted;

static void flash_recovery_begin(void)
{
    release_lease(&s_boot_lease, "boot");
    hold_take();
    esp_err_t jerr;
    um98x_flash_journal_t j;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    j = s_journal;
    portEXIT_CRITICAL(&s_lock);
    if (jerr == ESP_OK && j.state != UM98X_JOURNAL_NEEDS_USER) {
        const char *text = NULL;
        lifecycle_boot_info_t boot;
        lifecycle_get_boot_info(&boot);
        /* See station_early_boot: "power" may be the reset pin on this chip. */
        bool power = boot.reset_class == LIFECYCLE_RESET_POWER_ON || boot.reset_class == LIFECYCLE_RESET_BROWNOUT;
        (void)um98x_flash_journal_plan(jerr, &j, power, &text);
        if (CONFIG_UM98X_FLASH_ENABLE && CONFIG_UM98X_FLASH_RECOVERY_ACTIONS) {
            ESP_LOGW(TAG, "interrupted receiver update (%s, %s reset): %s", um98x_flash_journal_state_name(j.state),
                     power ? "power-on" : "ESP32-only", text);
            s_recover_wanted = true;
            return;
        }
        ESP_LOGW(TAG,
                 "report-only: the interrupted receiver update (%s, phase %s, %u transfer(s), %s reset) would be "
                 "recovered here: %s. Nothing is sent to the receiver and the session waits for the user",
                 um98x_flash_journal_state_name(j.state), um98x_flash_journal_state_name(j.phase),
                 (unsigned)j.transfers, power ? "power-on" : "ESP32-only", text);
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
    if (s_recover_wanted) {
        s_recover_wanted = false;
        if (stn_request_flash(&s_core, STN_FLASH_RECOVER) == STN_OK) {
            s_flash_running = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    after_step(&in);

    for (;;) {
        feed_watchdog();
        fill_in(&in);
        service(&in);
        sg_service(&in);
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
    /* ASSUMPTION, wrong on this board: a power-on (or brownout) reset is
     * taken as a receiver restart, and the race into the loader below has
     * to start at once to catch its window. On the ESP32-S3 the EN pin (the
     * USB serial chip's auto-reset that every flash and capture uses, the
     * RST button) reads as power-on while the receiver keeps running (our
     * bench), so after such a reset the receiver may still sit in its
     * loader or in a transfer, and the race would send its stop key into
     * that. Only the recovery actions act on this, and no build turns them
     * on (CONFIG_UM98X_FLASH_RECOVERY_ACTIONS, report-only everywhere): they
     * must check the receiver first (listen for its stream or its loader)
     * before they can be turned on. The station judges afterwards whether
     * the receiver kept running (rxboot_decided logs the plan that fits). */
    bool receiver_was_reset =
        boot.reset_class == LIFECYCLE_RESET_POWER_ON || boot.reset_class == LIFECYCLE_RESET_BROWNOUT;
    const char *text = NULL;
    s_recover_plan = um98x_flash_journal_plan(jerr, &s_journal, receiver_was_reset, &text);
    ESP_LOGW(TAG, "receiver flash journal at boot (%s reset): %s",
             receiver_was_reset ? "power-on class: power cut, or on this chip the reset pin" : "ESP32-only", text);
    if (jerr != ESP_OK || !um98x_flash_journal_needs_race(&s_journal, receiver_was_reset)) {
        return ESP_OK;   /* the rest of the recovery table runs in the station task */
    }

    if (CONFIG_UM98X_FLASH_ENABLE && CONFIG_UM98X_FLASH_RECOVERY_ACTIONS) {
        esp_err_t err = board_init();
        if (err == ESP_OK) {
            err = gnss_link_init();
        }
        if (err == ESP_OK) {
            err = gnss_link_acquire(GNSS_LINK_MODE_RAW, "flash recovery", 0, &s_boot_lease);
        }
        if (err == ESP_OK) {
            err = um98x_flash_race(s_boot_lease, RACE_MAX_MS);
            s_race_prompt = (err == ESP_OK);
            ESP_LOGW(TAG, "boot race into the receiver's loader: %s", esp_err_to_name(err));
        } else {
            ESP_LOGE(TAG, "boot race into the receiver's loader not possible: %s", esp_err_to_name(err));
        }
        return err;
    }
    ESP_LOGW(TAG, "report-only: would race into the receiver's loader at %u baud for %u ms (journal state %s)",
             (unsigned)UM98X_FLASH_BAUD, (unsigned)RACE_MAX_MS, um98x_flash_journal_state_name(s_journal.state));
    return ESP_OK;
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
    if (s_sgt_mx == NULL) {
        s_sgt_mx = xSemaphoreCreateMutex();
        if (s_sgt_mx == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    sg_test_load();
    if (lifecycle_register_shutdown_hook(sg_test_on_shutdown, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "comparison record: no shutdown hook (it is stored on its own schedule only)");
    }
    if (!s_factory_hooked) {
        err = esp_event_handler_register(SETTINGS_EVENT, SETTINGS_EVENT_FACTORY_RESET, on_factory_reset, NULL);
        s_factory_hooked = err == ESP_OK;
        if (err != ESP_OK) {
            /* The boot after the reset still resumes nothing (its reboot reason). */
            ESP_LOGW(TAG, "factory reset event not registered: %s", esp_err_to_name(err));
        }
    }

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
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    stn_maint_boot_t mb;
    bool maint_found = false;
    maint_boot_decide(&mb, &maint_found);
    bool restore = mb.from != STN_MAINT_FROM_NONE;
    if (!restore) {
        /* "Ended" at once: a reset before the first step resumes nothing either. */
        stn_maint_rec_t off;
        memset(&off, 0, sizeof(off));
        off.stamp = lifecycle_rtc_stamp();
        stn_maint_rtc_write(&s_maint_rtc, &off);
    }
    /* A stored copy is rewritten at the first step when maintenance goes on;
     * one that does not go on goes now (else at the first step). */
    s_maint_stored = maint_found;
    s_maint_saved_ms = STN_NEVER;
    if (!restore && maint_found && settings_erase_state_key(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_MAINT) == ESP_OK) {
        s_maint_stored = false;
    }
    /* The live view's output after an ESP32-only reset: the receiver did not
     * restart, so it may still send it (a flash session restarts it). A
     * power-on reset may be the ESP32-S3's reset pin, with the receiver
     * running on: a record that survived is honoured then too. If the
     * receiver did restart, its banner clears it (stn_restart_seen), and an
     * "off" it no longer needs changes nothing. */
    uint32_t sky_stamp = 0;
    bool sky_restore = !journal_open(s_journal_err) && stn_sky_rtc_read(&s_sky_rtc, &sky_stamp) &&
                       lifecycle_rtc_stamp_ok(sky_stamp);
    if (!sky_restore) {
        stn_sky_rtc_write(&s_sky_rtc, 0, false);
    }
    /* A power-on boot is judged by what the receiver sends (rxboot_tick). */
    s_rxb_judging = boot.reset_class == LIFECYCLE_RESET_POWER_ON;
    portENTER_CRITICAL(&s_lock);
    s_rxb_state = s_rxb_judging ? STATION_RX_BOOT_PENDING : STATION_RX_BOOT_NONE;
    s_rxb_by = STATION_RX_BOOT_BY_NONE;
    portEXIT_CRITICAL(&s_lock);

    /* Before the station task starts: nothing has reached the receiver yet. */
    bool held = first_apply_hold_boot();

    portENTER_CRITICAL(&s_lock);
    stn_init(&s_core, journal_open(s_journal_err));
    stn_set_first_apply_held(&s_core, held);
    s_core.live_idle_ms = (uint32_t)CONFIG_STATION_LIVE_VIEW_IDLE_S * 1000u;
    s_core.live_max_ms = (uint32_t)CONFIG_STATION_LIVE_VIEW_MAX_MIN * 60000u;
    if (restore) {
        stn_maint_restore(&s_core, &mb, now_ms());
    }
    if (sky_restore) {
        stn_sky_restore(&s_core);
    }
    stn_obs_clear(&s_obs);
    portEXIT_CRITICAL(&s_lock);
    if (sky_restore) {
        ESP_LOGW(TAG, "the live view's satellite-in-view output may still be on after the reset: turned off once "
                      "the receiver streams");
    }
    if (restore) {
        /* Hands off from the first moment: the boot only listens. */
        if (gnss_link_set_hands_off(true) == ESP_OK) {
            s_gate_on = true;
            ledger_set_maintenance(true);
        }
        unsigned left = (unsigned)(mb.left_ms / 1000u);
        if (mb.from == STN_MAINT_FROM_RTC) {
            ESP_LOGW(TAG, "receiver maintenance continues after the reset: %u s left of %u (RTC record, %s), "
                          "listening only",
                     left, (unsigned)mb.chosen_s,
                     mb.elapsed_known ? "the time across the reset taken off" : "this boot's time taken off");
        } else {
            ESP_LOGW(TAG, "receiver maintenance continues after the reset: at most %u s left of %u (stored copy: "
                          "the RTC memory was lost, as by the EN pin), listening only%s",
                     left, (unsigned)mb.chosen_s, mb.end_unix > 0 ? "; checked by the clock once it is set" : "");
        }
        if (boot.reset_class == LIFECYCLE_RESET_POWER_ON) {
            ESP_LOGW(TAG, "power-on reset during receiver maintenance: it ends if the receiver restarted too (a "
                          "power cut), and goes on if it kept running (the ESP32's EN pin)");
        }
    } else if (mb.expired) {
        ESP_LOGW(TAG, "receiver maintenance was on before the reset, but its time ran out meanwhile");
        journal_push(DIAG_EV_RX_MAINT, 0, (int32_t)(mb.chosen_s > INT32_MAX ? INT32_MAX : mb.chosen_s));
    }
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
    bool running;
    int64_t released;
    uint64_t excluded;
    /* Whether the receiver answers: at read time, one rule for every reader. */
    uint32_t rtcm_age = s_inited ? rtcm_age_ms() : UINT32_MAX;
    portENTER_CRITICAL(&s_lock);
    have = s_status_valid;
    if (have) {
        *out = s_status;
        /* At read time, not as last published: the station task publishes
         * between its steps, and one step (a detection pass) can take most
         * of a minute; a live-view start or stop must show at once. */
        stn_in_t lin;
        memset(&lin, 0, sizeof(lin));
        lin.now_ms = now_ms();
        lin.profile_sky = s_in_profile_sky;
        lin.autocfg = s_in_autocfg;
        stn_live_status_t lv;
        stn_live_status(&s_core, &lin, &lv);
        out->live.open = lv.open;
        out->live.left_s = lv.left_s;
        out->live.idle_left_s = lv.idle_left_s;
        out->live.open_s = lv.open_s;
        out->live.sky = lv.sky;
        out->live.sky_why = lv.why;
        out->live.last_end = lv.last_end;
        if (lv.why != STN_SKY_WHY_REJECTED && lv.why != STN_SKY_WHY_NO_ANSWER) {
            out->live.sky_reply[0] = '\0';
        }
        out->alerts = stn_alerts(&s_core);
        out->next_detect_in_s = stn_next_detect_in_s(&s_core, lin.now_ms);
        out->first_apply_held = s_core.first_apply_held;
        /* Maintenance as the user's start or extension left it, not as the
         * station task last published it (it publishes after its own work). */
        out->rx_maintenance = s_core.maint;
        out->maintenance_left_s = stn_maint_left_s(&s_core, lin.now_ms);
        out->maintenance_chosen_s = s_core.maint ? s_core.maint_chosen_s : 0;
        out->rx_boot = s_rxb_state;
        out->rx_boot_by = s_rxb_by;
        out->receiver_responding = stn_responding(&s_core, lin.now_ms, rtcm_age, s_line_ms);
    }
    int64_t line_ms = s_line_ms;
    jerr = s_journal_err;
    journal = s_journal;
    read = s_journal_read;
    parked = s_journal_parked;
    running = s_flash_running;
    released = s_lease_release_us;
    excluded = s_rtcm_excluded;
    bool sg_refused = s_sg_refused;
    station_sg_t sg_refused_group = s_sg_refused_group;
    bool sg_back_valid = s_sg_back_valid;
    station_sg_t sg_back = s_sg_back;
    char sg_reply[sizeof(s_sg_refused_reply)];
    memcpy(sg_reply, s_sg_refused_reply, sizeof(sg_reply));
    portEXIT_CRITICAL(&s_lock);

    if (!have) {
        /* Not started (safe mode, or before station_start): state IDLE. */
        memset(out, 0, sizeof(*out));
        out->state = STATION_STATE_IDLE;
        out->next_detect_in_s = UINT32_MAX;
        out->rx_bands_age_s = UINT32_MAX;
        if (s_inited) {
            out->position_locked = (s_profile.base_mode == UM98X_BASE_FIXED);
            int r1230 = rtcm1230_state();
            out->rtcm1230_supported = r1230 == 1;
            out->rtcm1230_known = r1230 >= 0;
            out->reconfigure_refused = !autocfg_enabled();
            out->receiver = s_stored_info;
        }
        out->first_apply_held = first_apply_held_stored();
    }
    out->rtcm_excluded = excluded;
    out->autocfg_off_this_boot = s_autocfg_ram_off;
    if (line_ms == STN_NEVER) {
        out->rx_line_age_ms = UINT32_MAX;
    } else {
        int64_t age = now_ms() - line_ms;
        out->rx_line_age_ms = age < 0 ? 0u : age >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)age;
    }
    if (!have) {
        out->sg_test.phase = -1;   /* no comparison without the station */
        out->sg_test.left_s = UINT32_MAX;
        out->sg_test.decide_left_s = UINT32_MAX;
        out->sg_test.ended_age_s = UINT32_MAX;
    }
    /* At read time: a profile save clears it at once. */
    out->sg_refused = sg_refused;
    out->sg_refused_group = sg_refused ? sg_refused_group : (station_sg_t){ 0 };
    out->sg_back_valid = sg_refused && sg_back_valid;
    out->sg_back = out->sg_back_valid ? sg_back : (station_sg_t){ 0 };
    snprintf(out->sg_refused_reply, sizeof(out->sg_refused_reply), "%s", sg_refused ? sg_reply : "");
    /* Live, not from the last published status: a job takes and drops its
     * lease while it runs, between two publications. Every lease on the link
     * is the station's (its jobs, the boot lease, the listen-only lease). */
    gnss_link_mode_t mode = gnss_link_get_mode();
    out->lease_held = (mode == GNSS_LINK_MODE_COMMAND || mode == GNSS_LINK_MODE_RAW);
    /* Live as well: whether the receiver answers (health's reasons) and
     * whether it said it restarts, also in the middle of a long job. */
    um98x_ctl_stats_t cs;
    if (s_inited && um98x_ctl_get_stats(&cs) == ESP_OK) {
        out->rx_answers = cs.lines;
        out->rx_answer_age_ms = cs.last_answer_age_ms;
        out->receiver_restarting = cs.restarting;
    } else {
        out->rx_answers = 0;
        out->rx_answer_age_ms = UINT32_MAX;
        out->receiver_restarting = false;
    }
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
    out->flash_session_running = running;
    if (out->flash_journal_open && !journal_unknown(jerr) && !running &&
        (parked || jerr != ESP_OK || journal.state == UM98X_JOURNAL_NEEDS_USER)) {
        out->flash.phase = UM98X_FLASH_NEEDS_USER;
    }
    status_flash_fields(out);
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

static bool maint_on(void)
{
    portENTER_CRITICAL(&s_lock);
    bool on = s_core.maint;
    portEXIT_CRITICAL(&s_lock);
    return on;
}

static bool alert_raised(uint32_t alert)
{
    portENTER_CRITICAL(&s_lock);
    bool raised = (s_core.alerts & alert) != 0;
    portEXIT_CRITICAL(&s_lock);
    return raised;
}

void station_first_apply_hold_refresh(void)
{
    if (!s_inited) {
        return;   /* the status reads the key itself then */
    }
    s_autocfg_ram_off = false;   /* the user turned automatic configuration on */
    first_apply_hold_update();
}

/* ---- signal-group comparison: requests ------------------------------------------ */

static esp_err_t sgt_result(int rc, station_sgt_err_t *why)
{
    if (why != NULL) {
        *why = (station_sgt_err_t)rc;
    }
    if (rc == STATION_SGT_OK) {
        wake();
        return ESP_OK;
    }
    return (rc == STATION_SGT_E_GROUPS || rc == STATION_SGT_E_PHASE) ? ESP_ERR_INVALID_ARG : ESP_ERR_INVALID_STATE;
}

esp_err_t station_sg_test_start(const station_sg_t groups[2], uint32_t phase_s, station_sgt_err_t *why)
{
    int rc = sgt_check_args(groups, phase_s);
    if (rc != STATION_SGT_OK) {
        return sgt_result(rc, why);
    }
    if (!s_started || s_sgt_mx == NULL) {
        return sgt_result(lifecycle_in_safe_mode() ? STATION_SGT_E_SAFE_MODE : STATION_SGT_E_NOT_RUNNING, why);
    }
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    station_status_t *st = &s_sgt_st;
    if (station_get_status(st) != ESP_OK) {
        xSemaphoreGive(s_sgt_mx);
        return sgt_result(STATION_SGT_E_NOT_RUNNING, why);
    }
    um98x_base_profile_t p;
    uint8_t buf[UM98X_PROFILE_BLOB_MAX];
    size_t blen = sizeof(buf);
    bool have_profile = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_PROFILE, buf, &blen) == ESP_OK &&
                        um98x_profile_decode(buf, blen, &p);
    sgt_guard_t g = {
        .settings_ok = settings_is_available(),
        .first_apply_held = st->first_apply_held,
        .autocfg = autocfg_enabled(),
        .maintenance = st->rx_maintenance,
        .journal_open = st->flash_journal_open || st->flash_journal_settings_unavailable || st->flash_session_running,
        .safe_mode = lifecycle_in_safe_mode(),
        .no_receiver = st->state == STATION_STATE_NO_RECEIVER,
        /* Unsupported: an identified model this firmware does not configure
         * (a receiver not identified yet is identity_unconfirmed). */
        .supported = (st->alerts & STATION_ALERT_UNSUPPORTED_MODEL) == 0 &&
                     (!st->receiver.valid || um98x_ctl_model_supported(&st->receiver)),
        .identity_confirmed = st->identity_confirmed,
        .streaming = st->state == STATION_STATE_STREAMING,
        .config_failed = st->config_failed,
        .position_locked = have_profile && p.base_mode == UM98X_BASE_FIXED,
        .running = { .master = have_profile ? p.signal_group_master : 0, .slave = have_profile ? p.signal_group_slave : 0 },
    };
    rc = sgt_start(&s_sgt, &g, groups, phase_s, now_ms());
    xSemaphoreGive(s_sgt_mx);
    if (rc == STATION_SGT_OK) {
        ESP_LOGW(TAG, "comparison of signal groups %u %u and %u %u started by the user (%u s each)",
                 (unsigned)groups[0].master, (unsigned)groups[0].slave, (unsigned)groups[1].master,
                 (unsigned)groups[1].slave, (unsigned)phase_s);
    }
    return sgt_result(rc, why);
}

esp_err_t station_sg_test_keep(unsigned group, station_sgt_err_t *why)
{
    if (s_sgt_mx == NULL) {
        return sgt_result(STATION_SGT_E_NOT_DECIDING, why);
    }
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    int rc = sgt_keep(&s_sgt, group, now_ms(), unix_now());
    xSemaphoreGive(s_sgt_mx);
    return sgt_result(rc, why);
}

esp_err_t station_sg_test_stop(station_sgt_err_t *why)
{
    if (s_sgt_mx == NULL) {
        return sgt_result(STATION_SGT_E_IDLE, why);
    }
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    int rc = sgt_stop(&s_sgt, now_ms(), unix_now());
    xSemaphoreGive(s_sgt_mx);
    if (rc == STATION_SGT_OK) {
        ESP_LOGW(TAG, "comparison stopped by the user");
    }
    return sgt_result(rc, why);
}

esp_err_t station_sg_test_get(station_sg_test_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_sgt_mx == NULL) {
        EXT_RAM_BSS_ATTR static sgt_t idle;   /* the station does not run: an idle view */
        sgt_init(&idle);
        sgt_view(&idle, now_ms(), out);
        return ESP_OK;
    }
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    sgt_view(&s_sgt, now_ms(), out);
    xSemaphoreGive(s_sgt_mx);
    return ESP_OK;
}

bool station_sg_test_running(void)
{
    if (s_sgt_mx == NULL) {
        return false;
    }
    xSemaphoreTake(s_sgt_mx, portMAX_DELAY);
    bool running = sgt_running(&s_sgt);
    xSemaphoreGive(s_sgt_mx);
    return running;
}

void station_profile_saved(void)
{
    portENTER_CRITICAL(&s_lock);
    s_sg_refused = false;
    s_sg_back_valid = false;
    s_sg_refused_reply[0] = '\0';
    portEXIT_CRITICAL(&s_lock);
}

esp_err_t station_request_reconfigure(void)
{
    esp_err_t err = running_gate();
    if (err != ESP_OK) {
        return err;
    }
    if (maint_on()) {
        return ESP_ERR_INVALID_STATE;   /* receiver maintenance: nothing is sent */
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
    if (!station_flash_available()) {
        return ESP_ERR_NOT_SUPPORTED;   /* the caller may check the package instead */
    }
    if (!s_inited || !s_started || journal_open(jerr) || lifecycle_app_trial_active() || app_update_running() ||
        lifecycle_in_safe_mode() || state != STATION_STATE_STREAMING || !receiver_streams() || !supported ||
        maint_on()) {
        return ESP_ERR_INVALID_STATE;
    }
    /* A signal-group comparison owns the receiver until it ends (the web
     * answers 409 signal_test_running before this; an update that was
     * downloading when a comparison started, and the bench, end here). */
    if (station_sg_test_running()) {
        return ESP_ERR_INVALID_STATE;
    }
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    if (info.pinned) {
        return ESP_ERR_INVALID_STATE;
    }
    /* One update at a time across both chips: check and claim in one step. */
    if (lifecycle_update_claim(LIFECYCLE_UPDATE_RECEIVER) != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_lock);
    int rc = stn_request_flash(&s_core, STN_FLASH_NEW);
    if (rc == STN_OK) {
        s_flash_req = *req;
        memset(&s_flash_last, 0, sizeof(s_flash_last));
        s_flash_last_us = INT64_MIN;
        s_flash_running = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (rc != STN_OK) {
        lifecycle_update_release(LIFECYCLE_UPDATE_RECEIVER);
        return ESP_ERR_INVALID_STATE;
    }
    s_update_claimed = true;
    ESP_LOGW(TAG, "receiver update to %s requested by the user", req->expected_fw);
    wake();
    return ESP_OK;
}

bool station_flash_available(void)
{
    return um98x_flash_enabled();
}

esp_err_t station_request_package_check(const station_flash_request_t *req)
{
    if (req == NULL || req->size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t jerr;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    portEXIT_CRITICAL(&s_lock);
    if (!s_inited || !s_started || journal_open(jerr) || lifecycle_in_safe_mode()) {
        return ESP_ERR_INVALID_STATE;
    }
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_lock);
    int rc = s_check_running ? STN_E_STATE : stn_request_pkg_check(&s_core);
    if (rc == STN_OK) {
        s_check_req = *req;
        s_check_running = true;
    }
    portEXIT_CRITICAL(&s_lock);
    if (rc != STN_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    wake();
    return ESP_OK;
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
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    portEXIT_CRITICAL(&s_lock);

    if (!journal_open(jerr)) {
        if (action == STATION_FLASH_ACTION_RESTART_RECEIVER && alert_raised(STN_ALERT_RECEIVER_IN_LOADER)) {
            /* Receiver found at its loader without a session: one more
             * restart key at the user's request, then detection. */
            if (!s_inited || !s_started || maint_on()) {
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
    switch (action) {
    case STATION_FLASH_ACTION_CLEAR_JOURNAL: {
        /* Not while a session, a user action (queued or running) or the
         * boot-time recovery works on the receiver; always in safe mode,
         * where nothing runs. Not with settings unavailable: there is
         * nothing to clear and nothing could be written. Checked and
         * claimed in one step: no retry or restart is accepted until the
         * clear is done. */
        portENTER_CRITICAL(&s_lock);
        bool busy = journal_unknown(s_journal_err) || s_journal_clearing || s_flash_running ||
                    (s_inited && (!journal_waits_for_user(s_journal_err, &s_journal) || stn_flash_pending(&s_core)));
        if (!busy) {
            s_journal_clearing = true;
        }
        portEXIT_CRITICAL(&s_lock);
        if (busy) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = um98x_flash_journal_close();
        if (err != ESP_OK) {
            portENTER_CRITICAL(&s_lock);
            s_journal_clearing = false;
            portEXIT_CRITICAL(&s_lock);
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
        portENTER_CRITICAL(&s_lock);
        s_journal_clearing = false;
        portEXIT_CRITICAL(&s_lock);
        return ESP_OK;
    }
    case STATION_FLASH_ACTION_RETRY:
    case STATION_FLASH_ACTION_RESTART_RECEIVER: {
        portENTER_CRITICAL(&s_lock);
        bool waiting = !s_journal_clearing && journal_waits_for_user(s_journal_err, &s_journal);
        portEXIT_CRITICAL(&s_lock);
        if (!s_inited || !s_started || !waiting || jerr != ESP_OK || maint_on() || lifecycle_in_safe_mode()) {
            return ESP_ERR_INVALID_STATE;
        }
        if (action == STATION_FLASH_ACTION_RETRY && (lifecycle_app_trial_active() || app_update_running())) {
            return ESP_ERR_INVALID_STATE;
        }
        if (!station_flash_available()) {
            return ESP_ERR_NOT_SUPPORTED;   /* this build sends nothing to the receiver's loader */
        }
        /* The session is the receiver's again: one update at a time (an app update may not start meanwhile). */
        bool claimed_now = false;
        if (!s_update_claimed) {
            if (lifecycle_update_claim(LIFECYCLE_UPDATE_RECEIVER) != ESP_OK) {
                return ESP_ERR_INVALID_STATE;
            }
            s_update_claimed = true;
            claimed_now = true;
        }
        portENTER_CRITICAL(&s_lock);
        int rc = (s_journal_clearing || s_flash_running)
                     ? STN_E_STATE
                     : stn_request_flash(&s_core, action == STATION_FLASH_ACTION_RETRY ? STN_FLASH_RETRY
                                                                                       : STN_FLASH_RESTART);
        if (rc == STN_OK) {
            s_journal_parked = false;
            s_flash_running = true;   /* from now on the journal is worked on: not cleared, not waiting */
            /* The last outcome was the stop this action answers: the status
             * and the job's final message tell how the action ends. */
            memset(&s_flash_last, 0, sizeof(s_flash_last));
            s_flash_last_us = INT64_MIN;
        }
        portEXIT_CRITICAL(&s_lock);
        if (rc != STN_OK) {
            if (claimed_now) {
                s_update_claimed = false;
                lifecycle_update_release(LIFECYCLE_UPDATE_RECEIVER);
            }
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGW(TAG, "receiver update: the user's %s", action == STATION_FLASH_ACTION_RETRY ? "retry (same package)"
                                                                                        : "receiver restart");
        wake();
        return ESP_OK;
    }
    default:
        return ESP_ERR_INVALID_ARG;
    }
}

size_t station_get_flash_log(char *buf, size_t len)
{
    return um98x_flash_get_text(buf, len);
}

/* ---- bench console ------------------------------------------------------------------- */

#if CONFIG_UM98X_FLASH_BYTE_CAPTURE
esp_err_t station_bench_run(const station_bench_req_t *req, station_bench_res_t *out, uint32_t timeout_ms)
{
    if (req == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    esp_err_t jerr;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    portEXIT_CRITICAL(&s_lock);
    if (!s_inited || !s_started || journal_open(jerr) || lifecycle_in_safe_mode()) {
        snprintf(out->line, sizeof(out->line), "REFUSED: %s",
                 journal_open(jerr) ? "a receiver update journal is open" : "the station does not run");
        return ESP_ERR_INVALID_STATE;
    }
    if (s_bench_done == NULL) {
        s_bench_done = xSemaphoreCreateBinary();
        if (s_bench_done == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    (void)xSemaphoreTake(s_bench_done, 0);
    portENTER_CRITICAL(&s_lock);
    int rc = stn_request_bench(&s_core);
    if (rc == STN_OK) {
        s_bench_req = *req;
    }
    bool m = s_core.maint;
    portEXIT_CRITICAL(&s_lock);
    if (rc != STN_OK) {
        snprintf(out->line, sizeof(out->line), "REFUSED: %s",
                 m ? "receiver maintenance is on" : "another receiver session or job runs (or no receiver state for it)");
        return ESP_ERR_INVALID_STATE;
    }
    wake();
    if (xSemaphoreTake(s_bench_done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        snprintf(out->line, sizeof(out->line), "TIMEOUT waiting for the station (the operation may still run)");
        return ESP_ERR_TIMEOUT;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_bench_res;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t station_bench_boot_race(uint32_t max_ms, station_bench_res_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    if (!s_journal_read) {
        read_journal();
    }
    if (journal_open(s_journal_err)) {
        snprintf(out->line, sizeof(out->line), "REFUSED: a receiver update journal is open");
        return ESP_ERR_INVALID_STATE;
    }
    /* The race runs at the next boot of any kind. After an ESP32 reset
     * during receiver maintenance, that maintenance goes on (the user works
     * on the receiver over its other port, e.g. a PC flashing it): no stop
     * key and no "6" may reach the receiver then. */
    stn_maint_boot_t mb;
    bool found = false;
    maint_boot_decide(&mb, &found);
    if (mb.from != STN_MAINT_FROM_NONE) {
        snprintf(out->line, sizeof(out->line),
                 "REFUSED: receiver maintenance was on before this reset and goes on (%u s left); nothing sent",
                 (unsigned)(mb.left_ms / 1000u));
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = board_init();
    if (err == ESP_OK) {
        err = gnss_link_init();
    }
    gnss_link_lease_t lease = NULL;
    if (err == ESP_OK) {
        err = gnss_link_acquire(GNSS_LINK_MODE_RAW, "bench race", 0, &lease);
    }
    if (err != ESP_OK) {
        snprintf(out->line, sizeof(out->line), "FAIL link not opened: %s", esp_err_to_name(err));
        return err;
    }
    um98x_bench_res_t br;
    err = um98x_flash_bench_race(lease, max_ms, home_baud(), &br);
    out->pass = err == ESP_OK && br.pass;
    snprintf(out->line, sizeof(out->line), "%s", err == ESP_OK ? br.line : esp_err_to_name(err));
    (void)gnss_link_release(lease);
    return err;
}

esp_err_t station_bench_flash(const station_flash_request_t *req, uint32_t stop_after_blocks, uint8_t reset_at_state)
{
    um98x_flash_bench_reset_at(reset_at_state);
    portENTER_CRITICAL(&s_lock);
    s_bench_stop_after = stop_after_blocks;
    portEXIT_CRITICAL(&s_lock);
    esp_err_t err = station_request_flash(req);
    if (err != ESP_OK) {
        um98x_flash_bench_reset_at(0);
        portENTER_CRITICAL(&s_lock);
        s_bench_stop_after = 0;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}

esp_err_t station_bench_journal_set(uint8_t state)
{
    esp_err_t jerr;
    portENTER_CRITICAL(&s_lock);
    jerr = s_journal_err;
    bool busy = s_core.job_busy || s_core.state == STN_FLASHING;
    portEXIT_CRITICAL(&s_lock);
    if (!s_inited || busy || journal_open(jerr)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (state < UM98X_JOURNAL_PREPARED || state > UM98X_JOURNAL_NEEDS_USER) {
        return ESP_ERR_INVALID_ARG;
    }
    pkg_store_info_t info;
    if (pkg_store_get_info(&info) != ESP_OK || !info.valid) {
        return ESP_ERR_NOT_FOUND;
    }
    um98x_info_t rx;
    if (um98x_ctl_get_info(&rx) != ESP_OK || !rx.valid) {
        return ESP_ERR_INVALID_STATE;   /* the receiver's identity is not confirmed this boot */
    }
    um98x_flash_journal_t j;
    memset(&j, 0, sizeof(j));
    j.version = UM98X_FLASH_JOURNAL_VERSION;
    j.state = state;
    j.phase = (state == UM98X_JOURNAL_FAILED || state == UM98X_JOURNAL_NEEDS_USER) ? UM98X_JOURNAL_TRANSFERRING : state;
    memcpy(j.sha256, info.sha256, sizeof(j.sha256));
    j.size = info.size;
    snprintf(j.expected_fw, sizeof(j.expected_fw), "%s", rx.firmware);
    j.expected_build = rx.build;
    snprintf(j.pre_model, sizeof(j.pre_model), "%s", rx.model);
    snprintf(j.pre_fw, sizeof(j.pre_fw), "%s", rx.firmware);
    j.pre_build = rx.build;
    j.app_baud = home_baud();
    j.boot_baud = UM98X_FLASH_BAUD;
    j.transfers = (j.phase >= UM98X_JOURNAL_TRANSFERRING) ? 1 : 0;
    j.entry_attempts = 1;
    j.failure_class = state == UM98X_JOURNAL_FAILED ? UM98X_FLASH_FAIL_TRANSPORT : UM98X_FLASH_FAIL_NONE;
    j.changed_unix = unix_now();
    esp_err_t err = pkg_store_set_pinned(true);
    if (err == ESP_OK) {
        err = um98x_flash_journal_put(&j);
    }
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "bench: journal written in \"%s\" (nothing sent to the receiver); reset the ESP32 now",
                 um98x_flash_journal_state_name(state));
    }
    return err;
}
#else
esp_err_t station_bench_run(const station_bench_req_t *req, station_bench_res_t *out, uint32_t timeout_ms)
{
    (void)req;
    (void)out;
    (void)timeout_ms;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_bench_boot_race(uint32_t max_ms, station_bench_res_t *out)
{
    (void)max_ms;
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_bench_flash(const station_flash_request_t *req, uint32_t stop_after_blocks, uint8_t reset_at_state)
{
    (void)req;
    (void)stop_after_blocks;
    (void)reset_at_state;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_bench_journal_set(uint8_t state)
{
    (void)state;
    return ESP_ERR_NOT_SUPPORTED;
}
#endif

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
    if (maint_on()) {
        return ESP_ERR_INVALID_STATE;
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
    if (maint_on()) {
        return ESP_ERR_INVALID_STATE;
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

esp_err_t station_request_detect(void)
{
    esp_err_t err = running_gate();
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_lock);
    int rc = stn_request_detect(&s_core, now_ms());
    portEXIT_CRITICAL(&s_lock);
    if (rc != STN_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGI(TAG, "receiver search requested by the user");
    wake();
    return ESP_OK;
}

/* ---- receiver maintenance and live view -------------------------------------- */

esp_err_t station_maintenance_start(uint32_t duration_s, station_maint_why_t *why)
{
    station_maint_why_t w = STATION_MAINT_OK;
    esp_err_t ret = ESP_OK;
    if (duration_s == 0) {
        duration_s = STN_MAINT_DEFAULT_S;
    }
    if (!s_inited || !s_started || lifecycle_in_safe_mode()) {
        w = STATION_MAINT_REFUSED_NOT_RUNNING;
        ret = ESP_ERR_INVALID_STATE;
    } else {
        esp_err_t jerr;
        portENTER_CRITICAL(&s_lock);
        jerr = s_journal_err;
        portEXIT_CRITICAL(&s_lock);
        gnss_link_mode_t mode = gnss_link_get_mode();
        bool lease_held = (mode == GNSS_LINK_MODE_COMMAND || mode == GNSS_LINK_MODE_RAW);
        if (journal_open(jerr)) {
            w = STATION_MAINT_REFUSED_FLASH;
        } else {
            portENTER_CRITICAL(&s_lock);
            w = (station_maint_why_t)stn_maint_enter(&s_core, duration_s, lease_held, now_ms());
            portEXIT_CRITICAL(&s_lock);
        }
        if (w == STATION_MAINT_REFUSED_DURATION) {
            ret = ESP_ERR_INVALID_ARG;
        } else if (w != STATION_MAINT_OK) {
            ret = ESP_ERR_INVALID_STATE;
        } else {
            /* Hands off from the return on; the station task follows
             * (ledger, RTC record, journal). */
            esp_err_t err = gnss_link_set_hands_off(true);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "link hands-off not set yet: %s", esp_err_to_name(err));
            }
            wake();
        }
    }
    if (why != NULL) {
        *why = w;
    }
    return ret;
}

esp_err_t station_maintenance_end(void)
{
    if (!s_inited || !s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!maint_on()) {
        return ESP_ERR_NOT_FOUND;
    }
    s_maint_end_req = true;
    wake();
    return ESP_OK;
}

esp_err_t station_live_view_start(uint32_t minutes, station_live_why_t *why)
{
    station_live_why_t w = STATION_LIVE_WHY_NONE;
    esp_err_t ret = ESP_OK;
    if (!s_inited || !s_started || lifecycle_in_safe_mode()) {
        w = STATION_LIVE_WHY_NOT_RUNNING;
        ret = ESP_ERR_INVALID_STATE;
    } else {
        if (minutes == 0) {
            minutes = CONFIG_STATION_LIVE_VIEW_MIN;
        }
        if (minutes > CONFIG_STATION_LIVE_VIEW_MAX_MIN) {
            minutes = CONFIG_STATION_LIVE_VIEW_MAX_MIN;
        }
        uint8_t cw = STN_SKY_WHY_NONE;
        portENTER_CRITICAL(&s_lock);
        int rc = stn_live_start(&s_core, minutes * 60000u, now_ms(), &cw);
        portEXIT_CRITICAL(&s_lock);
        if (rc != STN_OK) {
            w = (station_live_why_t)cw;
            ret = ESP_ERR_INVALID_STATE;
        } else {
            wake();
        }
    }
    if (why != NULL) {
        *why = w;
    }
    return ret;
}

esp_err_t station_live_view_touch(void)
{
    if (!s_inited) {
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_lock);
    bool open = s_core.live_open;
    stn_live_touch(&s_core, now_ms());
    portEXIT_CRITICAL(&s_lock);
    return open ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t station_live_view_stop(void)
{
    if (!s_inited) {
        return ESP_ERR_NOT_FOUND;
    }
    portENTER_CRITICAL(&s_lock);
    bool open = s_core.live_open;
    stn_live_stop(&s_core, now_ms());
    portEXIT_CRITICAL(&s_lock);
    if (open) {
        wake();
    }
    return open ? ESP_OK : ESP_ERR_NOT_FOUND;
}
