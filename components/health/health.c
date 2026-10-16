/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Health monitor task: samples the station once a second, runs the GNSS
 * recovery ladder (health_ladder.c) and the monitor helpers (health_mon.c),
 * feeds the network ladder, supervises the caster slots, watches the heap
 * and the BOOT button, journals loss counters and ladder steps, reads the
 * journal's boot records for the power-lost alert, and drives
 * ledger_tick(). The logic lives in the pure cores; this file only samples
 * and acts. Thresholds and loss rules are PROVISIONAL, to be confirmed on
 * hardware.
 */
#include "health.h"

#include <inttypes.h>
#include <string.h>

#include "board.h"
#include "diag.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "health_ladder.h"
#include "health_mon.h"
#include "ledger.h"
#include "lifecycle.h"
#include "network.h"
#include "ntrip_server.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "settings.h"
#include "settings_schema.h"
#include "station.h"

static const char *TAG = "health";

#define TASK_PRIO          12
#define TICK_MS            1000
#define STALL_MS           ((uint32_t)CONFIG_HEALTH_GNSS_STALL_S * 1000u)
#define HEAP_WARN_BYTES    (16u * 1024u)
#define HEAP_REBOOT_BYTES  (8u * 1024u)
#define FRAG_BYTES         ((uint32_t)CONFIG_HEALTH_HEAP_FRAGMENT_KB * 1024u)
#define CFG_REFRESH_MS     10000
#define LEDGER_OVF_PER_TICK 100u
#define MAX_SLOTS          CONFIG_NTRIP_MAX_CASTERS
#define JOURNAL_CHUNK      32      /* journal records per read (power-lost alert) */
#define JOURNAL_READS_PER_TICK 8   /* a full 128 KB journal is read in about 16 s at start, bounded per tick */
#define SETUP_REREAD_MS    60000   /* sys/setup_done read again at least this often (change events can be dropped) */

/* Debounce of the alerts that come from sampled states (ms). */
#define BANDS_ON_MS        60000u
#define BANDS_OFF_MS       60000u
#define DRIFT_ON_MS        90000u
#define DRIFT_OFF_MS       10000u
#define FRAG_ON_MS         10000u
#define FRAG_OFF_MS        60000u
#define CASTER_ON_MS       30000u
#define CASTER_OFF_MS      10000u

/* The cores mirror these enums by value. */
_Static_assert(HL_STN_IDLE == (int)STATION_STATE_IDLE && HL_STN_DETECTING == (int)STATION_STATE_DETECTING &&
                   HL_STN_CONFIGURING == (int)STATION_STATE_CONFIGURING &&
                   HL_STN_STREAMING == (int)STATION_STATE_STREAMING &&
                   HL_STN_FLASHING == (int)STATION_STATE_FLASHING &&
                   HL_STN_FLASH_RECOVERY == (int)STATION_STATE_FLASH_RECOVERY &&
                   HL_STN_RECOVERING == (int)STATION_STATE_RECOVERING &&
                   HL_STN_NO_RECEIVER == (int)STATION_STATE_NO_RECEIVER,
               "ladder station states mirror station_state_t");
_Static_assert(HL_CODE_RESYNC == (int)STATION_RECOVER_RESYNC + 1 &&
                   HL_CODE_LINK_RESTART == (int)STATION_RECOVER_LINK_RESTART + 1 &&
                   HL_CODE_RESCAN == (int)STATION_RECOVER_RESCAN + 1 &&
                   HL_CODE_RECONFIGURE == (int)STATION_RECOVER_RECONFIGURE + 1 &&
                   HL_CODE_RECEIVER_RESET == (int)STATION_RECOVER_RECEIVER_RESET + 1,
               "ladder steps are station_recovery_t + 1");
_Static_assert(HL_CODE_RESYNC == (int)HEALTH_STEP_RESYNC && HL_CODE_LINK_RESTART == (int)HEALTH_STEP_LINK_RESTART &&
                   HL_CODE_RESCAN == (int)HEALTH_STEP_RESCAN && HL_CODE_RECONFIGURE == (int)HEALTH_STEP_RECONFIGURE &&
                   HL_CODE_RECEIVER_RESET == (int)HEALTH_STEP_RECEIVER_RESET &&
                   HL_CODE_REBOOT == (int)HEALTH_STEP_REBOOT,
               "ladder steps mirror health_step_t");
_Static_assert(HL_LEVEL_OK == (int)HEALTH_OK && HL_LEVEL_DEGRADED == (int)HEALTH_DEGRADED &&
                   HL_LEVEL_RECOVERING == (int)HEALTH_RECOVERING && HL_LEVEL_FAILED == (int)HEALTH_FAILED,
               "ladder levels mirror health_level_t");
_Static_assert(HM_CST_DISABLED == (int)NTRIP_STATE_DISABLED && HM_CST_WAIT_NETWORK == (int)NTRIP_STATE_WAIT_NETWORK &&
                   HM_CST_WAIT_DATA == (int)NTRIP_STATE_WAIT_DATA && HM_CST_CONNECTING == (int)NTRIP_STATE_CONNECTING &&
                   HM_CST_STREAMING == (int)NTRIP_STATE_STREAMING && HM_CST_BACKOFF == (int)NTRIP_STATE_BACKOFF &&
                   HM_CST_AUTH_FAILED == (int)NTRIP_STATE_AUTH_FAILED && HM_CST_PAUSED == (int)NTRIP_STATE_PAUSED &&
                   HM_CST_CONFIG_ERROR == (int)NTRIP_STATE_CONFIG_ERROR && HM_CST_BANNED == (int)NTRIP_STATE_BANNED &&
                   HM_CST_STOPPED == (int)NTRIP_STATE_STOPPED,
               "caster states mirror ntrip_state_t");
_Static_assert(HM_FAIL_NONE == (int)NTRIP_FAIL_NONE && HM_FAIL_DNS == (int)NTRIP_FAIL_DNS &&
                   HM_FAIL_CONNECT == (int)NTRIP_FAIL_CONNECT && HM_FAIL_TLS == (int)NTRIP_FAIL_TLS &&
                   HM_FAIL_TIMEOUT == (int)NTRIP_FAIL_TIMEOUT && HM_FAIL_REMOTE_CLOSE == (int)NTRIP_FAIL_REMOTE_CLOSE &&
                   HM_FAIL_WRITE_STALL == (int)NTRIP_FAIL_WRITE_STALL &&
                   HM_FAIL_KEEPALIVE == (int)NTRIP_FAIL_KEEPALIVE && HM_FAIL_AUTH == (int)NTRIP_FAIL_AUTH &&
                   HM_FAIL_MOUNT_BUSY == (int)NTRIP_FAIL_MOUNT_BUSY && HM_FAIL_CONFIG == (int)NTRIP_FAIL_CONFIG &&
                   HM_FAIL_BANNED == (int)NTRIP_FAIL_BANNED &&
                   HM_FAIL_LOGIN_BLOCKED == (int)NTRIP_FAIL_LOGIN_BLOCKED &&
                   HM_FAIL_IN_USE == (int)NTRIP_FAIL_IN_USE && HM_FAIL_REPLACED == (int)NTRIP_FAIL_REPLACED &&
                   HM_FAIL_LOCAL == (int)NTRIP_FAIL_LOCAL && HM_FAIL_CLASS_COUNT == (int)NTRIP_FAIL_CLASS_COUNT,
               "failure classes mirror ntrip_fail_class_t");
_Static_assert(DIAG_LOSS_HW_FIFO == 1 && DIAG_LOSS_RX_BUFFER_FULL == 2 && DIAG_LOSS_LINE_ERRORS == 3 &&
                   DIAG_LOSS_RTCM_CRC == 4 && DIAG_LOSS_FRAMER_DISCARDED == 5 && DIAG_LOSS_HUB_PUBLISH == 6 &&
                   DIAG_LOSS_BAUD_SWITCH == 7 && DIAG_LOSS_RX_EVENTS_DROPPED == HM_LOSS_IDS,
               "loss journal ids are diag_loss_id_t 1..8");
_Static_assert(HL_CODE_RESYNC == (int)DIAG_GNSS_STEP_RESYNC && HL_CODE_LINK_RESTART == (int)DIAG_GNSS_STEP_LINK_RESTART &&
                   HL_CODE_RESCAN == (int)DIAG_GNSS_STEP_RESCAN && HL_CODE_RECONFIGURE == (int)DIAG_GNSS_STEP_RECONFIGURE &&
                   HL_CODE_RECEIVER_RESET == (int)DIAG_GNSS_STEP_RECEIVER_RESET &&
                   HL_CODE_REBOOT == (int)DIAG_GNSS_STEP_REBOOT && HL_CODE_POWER_ALERT == (int)DIAG_GNSS_STEP_POWER_CYCLE &&
                   HL_CODE_FAILED == (int)DIAG_GNSS_STEP_FAILED &&
                   HL_CODE_REBOOT_BLOCKED == (int)DIAG_GNSS_STEP_REBOOT_BLOCKED,
               "ladder journal codes are diag_gnss_step_t");
_Static_assert(HL_OUT_STARTED == (int)DIAG_STEP_STARTED && HL_OUT_HEALTHY == (int)DIAG_STEP_HEALTHY &&
                   HL_OUT_FAILED == (int)DIAG_STEP_FAILED && HL_OUT_ABANDONED == (int)DIAG_STEP_ABANDONED &&
                   HL_OUT_NOT_RUN == (int)DIAG_STEP_NOT_RUN && HL_OUT_HELD == (int)DIAG_STEP_HELD,
               "ladder outcomes are diag_step_outcome_t");
_Static_assert(HL_HOLD_SETUP == (int)HEALTH_HOLD_SETUP && HL_HOLD_AP_CLIENT == (int)HEALTH_HOLD_AP_CLIENT &&
                   HL_HOLD_ADMIN == (int)HEALTH_HOLD_ADMIN && HL_HOLD_POWER_ON == (int)HEALTH_HOLD_POWER_ON &&
                   HM_HOLD_SETUP == HL_HOLD_SETUP && HM_HOLD_AP_CLIENT == HL_HOLD_AP_CLIENT &&
                   HM_HOLD_ADMIN == HL_HOLD_ADMIN && HM_HOLD_POWER_ON == HL_HOLD_POWER_ON,
               "restart hold bits match across the cores and health.h");
_Static_assert(HL_MODE_FULL == (int)HEALTH_RECOVERY_FULL && HL_MODE_NO_REBOOT == (int)HEALTH_RECOVERY_NO_REBOOT &&
                   HL_MODE_ALERTS == (int)HEALTH_RECOVERY_ALERTS,
               "ladder modes are health_recovery_mode_t");
_Static_assert(HM_RESET_POWER_ON == (int)LIFECYCLE_RESET_POWER_ON && HM_EV_BOOT == (int)DIAG_EV_BOOT,
               "power-lost alert reads lifecycle's boot records");
_Static_assert(HM_EV_RX_BOOT == (int)DIAG_EV_RX_BOOT && HM_RXBOOT_RESTARTED == (int)DIAG_RX_BOOT_RESTARTED,
               "power-lost alert reads the station's power-on verdicts");

/* ---- shared state ----------------------------------------------------------- */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static health_status_t s_pub;        /* published snapshot (under s_lock) */
static bool s_pub_valid;
static bool s_inited;
static TaskHandle_t s_task;

/* Station transitions into and out of RECOVERING (event loop task writes,
 * health task reads), so a step shorter than one tick is still seen. */
static uint32_t s_rec_enters;
static uint32_t s_rec_exits;
static station_state_t s_ev_state = STATION_STATE_IDLE;   /* event loop task only */
static bool s_cfg_dirty = true;                          /* caster configs to re-read */
static bool s_setup_dirty = true;                        /* sys/setup_done to read again (settings events) */
static uint32_t s_admin_at_s;                            /* last admin activity: uptime s + 1 (0 = none this boot) */

/* ---- health task only --------------------------------------------------------- */

static hl_t s_ladder;
static hl_in_t s_in;
static hm_hold_t s_hold;
static bool s_setup_done;               /* sys/setup_done == 1, as last read */
static int64_t s_setup_read_ms = HM_NEVER;
static uint8_t s_prev_hold;
static hm_loss_t s_loss;
static hm_link_t s_link;
static hm_slot_t s_slot[MAX_SLOTS];
static bool s_slot_enabled[MAX_SLOTS];
static bool s_slot_protected[MAX_SLOTS];
static int64_t s_cfg_at_ms;
static hm_deb_t s_deb_bands;
static hm_deb_t s_deb_drift;
static hm_deb_t s_deb_frag;
static hm_deb_t s_deb_casters;
static hm_button_t s_button;
static uint32_t s_caster_restarts;
static uint32_t s_admin_resets;
static uint64_t s_ledger_owed;
static bool s_heap_warned;
static hm_heap_t s_heap;
static bool s_heap_held_logged;
static int64_t s_factory_refused_ms = HM_NEVER;
static uint8_t s_prev_level;
static uint32_t s_prev_alerts;
static uint8_t s_prev_why;
static bool s_prev_suspended;
static uint8_t s_mode = HL_MODE_FULL;   /* recovery mode in effect */
static int s_mode_bad = -1;             /* last out-of-range stored value logged */
static hm_power_t s_power;
static uint32_t s_jcursor;              /* journal records read so far (seq) */
static uint32_t s_jclears;              /* the journal's clear count those records belong to */
static bool s_power_caught_up;          /* every record written before the reads began was fed once */
static bool s_power_alert;              /* last judgement (kept until the reader caught up) */
static diag_journal_entry_t s_jbuf[JOURNAL_CHUNK];

/* Large snapshots, kept off the task stack. */
static station_status_t s_st;
static ntrip_caster_status_t s_cst;
static ntrip_caster_cfg_t s_ccfg;
static gnss_link_stats_t s_link_stats;
static network_status_t s_net;

static int64_t now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *step_name(uint8_t code)
{
    static const char *const k[] = { "none", "resync", "link restart", "rescan", "reconfigure",
                                     "receiver reset", "ESP32 reboot", "power-cycle alert", "failed",
                                     "reboot blocked" };
    return code < sizeof(k) / sizeof(k[0]) ? k[code] : "?";
}

static const char *outcome_name(uint8_t o)
{
    static const char *const k[] = { "started", "healthy again", "failed", "abandoned (suspended)",
                                     "not run (user setting)", "held" };
    return o < sizeof(k) / sizeof(k[0]) ? k[o] : "?";
}

/* "first setup not finished, the web page is in use" */
static void hold_text(uint8_t hold, char *out, size_t size)
{
    static const char *const k[] = { "first setup not finished", "someone is on the station's access point",
                                     "the web page is in use", "first hour after power-on" };
    size_t n = 0;
    out[0] = '\0';
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        if ((hold & (1u << i)) != 0 && n < size) {
            int w = snprintf(out + n, size - n, "%s%s", n > 0 ? ", " : "", k[i]);
            n += w > 0 ? (size_t)w : 0;
        }
    }
}

/* ---- ladder callbacks ----------------------------------------------------------- */

static hl_req_t op_request(void *ctx, uint8_t step)
{
    (void)ctx;
    station_recovery_t action = (station_recovery_t)(step - 1u);
    esp_err_t err = station_request_recovery(action);
    if (err == ESP_OK) {
        return HL_REQ_ACCEPTED;
    }
    if (action == STATION_RECOVER_RECONFIGURE && err == ESP_ERR_INVALID_STATE &&
        station_get_status(&s_st) == ESP_OK && s_st.reconfigure_refused) {
        return HL_REQ_REFUSED;
    }
    ESP_LOGD(TAG, "station did not take step %u: %s", step, esp_err_to_name(err));
    return HL_REQ_REJECTED;
}

static bool op_reboot(void *ctx)
{
    (void)ctx;
    /* Checked again right before the request: the ladder's sample is from
     * the start of the tick, and a reboot is never queued behind an inhibit. */
    if (!lifecycle_ladder_reboot_allowed(NULL) || lifecycle_reboot_inhibited(NULL)) {
        ESP_LOGW(TAG, "no receiver: restart to look for it blocked after all (reboot cap or a running update)");
        return false;
    }
    ESP_LOGE(TAG, "no receiver after the recovery steps: restarting the ESP32 to look for it");
    (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_GNSS_LADDER);
    return true;
}

/* One line per journal record, in words (the journal itself keeps the codes). */
static void ladder_log(uint8_t code, uint8_t outcome, uint8_t detail)
{
    if (code == HL_CODE_REBOOT && outcome == HL_OUT_STARTED) {
        return;   /* op_reboot says it when it asks for the restart */
    }
    if (code == HL_CODE_REBOOT_BLOCKED) {
        if (outcome == HL_OUT_HELD) {
            char why[128];
            hold_text(detail, why, sizeof(why));
            ESP_LOGW(TAG, "no receiver: restart to look for it postponed (%s)", why);
        } else if (outcome == HL_OUT_NOT_RUN) {
            ESP_LOGW(TAG, "no receiver: no restart to look for it (recovery mode: no ESP32 restart)");
        } else if (detail == HL_CAP_LOCK) {
            ESP_LOGW(TAG, "no receiver: restart to look for it postponed (an update or receiver session holds "
                          "restarts back)");
        } else {
            uint32_t wait = 0;
            (void)lifecycle_ladder_reboot_allowed(&wait);
            ESP_LOGW(TAG, "no receiver: restart to look for it postponed (%s; possible in %" PRIu32 " min)",
                     detail == HL_CAP_DAY ? "at most 3 in 24 hours" : "at most one an hour", (wait + 59u) / 60u);
        }
        return;
    }
    if (code == HL_CODE_FAILED && outcome == HL_OUT_STARTED) {
        if (s_st.state == STATION_STATE_NO_RECEIVER) {
            ESP_LOGW(TAG, "no receiver: the station keeps looking for it on its own schedule");
        } else if (s_ladder.no_rtcm) {
            /* Set before this record (health task: the ladder runs in it). */
            ESP_LOGW(TAG, "GNSS recovery: the receiver answers but sends no RTCM; the configuration and the receiver "
                          "reset are not repeated (a check every %u min)",
                     (unsigned)(HL_FAILED_RETRY_MS / 60000u));
        } else {
            ESP_LOGW(TAG, "GNSS recovery: every step tried; the next round starts in %u min",
                     (unsigned)(HL_FAILED_RETRY_MS / 60000u));
        }
        return;
    }
    if (code == HL_CODE_POWER_ALERT && outcome == HL_OUT_STARTED) {
        ESP_LOGW(TAG, "receiver sends bytes but nothing usable: it needs a power cycle (an ESP32 restart cannot fix that)");
        return;
    }
    if (outcome == HL_OUT_HEALTHY) {
        ESP_LOGI(TAG, "GNSS ladder: %s: %s", step_name(code), outcome_name(outcome));
    } else {
        ESP_LOGW(TAG, "GNSS ladder: %s: %s", step_name(code), outcome_name(outcome));
    }
}

static void op_journal(void *ctx, uint8_t code, uint8_t outcome, uint8_t detail)
{
    (void)ctx;
    ladder_log(code, outcome, detail);
    /* A held restart carries its reasons in bits 16-23 (the network ladder's reason byte). */
    int32_t b = (int32_t)(((uint32_t)detail << 16) | (uint32_t)diag_ladder_b((diag_step_outcome_t)outcome, code));
    esp_err_t err = diag_journal_append(DIAG_EV_LADDER_STEP, DIAG_LADDER_GNSS, b);
    if (err != ESP_OK) {
        ESP_LOGD(TAG, "ladder record not journalled: %s", esp_err_to_name(err));
    }
}

/* ---- events ------------------------------------------------------------------- */

static void on_station_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id != STATION_EVENT_STATE_CHANGED || data == NULL) {
        return;
    }
    station_state_t st = *(const station_state_t *)data;
    if (st == STATION_STATE_RECOVERING && s_ev_state != STATION_STATE_RECOVERING) {
        __atomic_add_fetch(&s_rec_enters, 1u, __ATOMIC_SEQ_CST);
    } else if (st != STATION_STATE_RECOVERING && s_ev_state == STATION_STATE_RECOVERING) {
        __atomic_add_fetch(&s_rec_exits, 1u, __ATOMIC_SEQ_CST);
    }
    s_ev_state = st;
}

static void on_ntrip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)id;
    (void)data;
    __atomic_store_n(&s_cfg_dirty, true, __ATOMIC_SEQ_CST);
}

/* sys/setup_done changed (or several keys at once, or a factory reset): the
 * health task reads it again. */
static void on_settings_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    if (id == SETTINGS_EVENT_CHANGED && data != NULL) {
        const settings_changed_t *c = data;
        if (strcmp(c->ns, SETTINGS_NS_SYS) != 0 || (c->key[0] != '\0' && strcmp(c->key, SETTINGS_KEY_SETUP_DONE) != 0)) {
            return;
        }
    }
    __atomic_store_n(&s_setup_dirty, true, __ATOMIC_SEQ_CST);
}

/* ---- one tick, part by part --------------------------------------------------- */

/* Loss counters: one journal record per tick at most, and per counter at
 * most one a minute (health_mon.h); FIFO overflows also go to the ledger's
 * daily count. */
static void loss_tick(const gnss_link_stats_t *link, const rtcm_hub_stats_t *hub, int64_t now)
{
    const uint64_t vals[HM_LOSS_IDS] = {
        link->hw_fifo_overflows,      link->rx_buffer_full,    link->line_errors,
        link->framing.rtcm_crc_errors, link->framing.bytes_discarded, hub->publish_failures,
        link->baud_switch_bytes,      link->rx_events_dropped,
    };
    uint64_t inc[HM_LOSS_IDS];
    hm_loss_sample(&s_loss, vals, now, inc);

    s_ledger_owed += inc[DIAG_LOSS_HW_FIFO - 1];
    for (unsigned i = 0; i < LEDGER_OVF_PER_TICK && s_ledger_owed > 0; i++, s_ledger_owed--) {
        ledger_count_overflow();
    }

    uint8_t id;
    int32_t delta;
    if (hm_loss_next(&s_loss, now, &id, &delta)) {
        esp_err_t err = diag_journal_append(DIAG_EV_DATA_LOSS, id, delta);
        if (err == ESP_OK) {
            hm_loss_written(&s_loss, id, delta, now);
        }
        /* refused (rate limit) or no journal: the increase stays pending */
    }
}

/* Caster slots: budgets, "down" for the level, the protected slot and the
 * longest run of failures that never reached a caster for the network
 * ladder, and whether any slot streams (upstream evidently works). */
static void casters_tick(int64_t now, bool *any_down, bool *protected_down, bool *any_streaming,
                         uint32_t *unreached_s)
{
    *any_down = false;
    *protected_down = false;
    *any_streaming = false;
    *unreached_s = 0;
    size_t n = ntrip_server_slot_count();
    if (n > MAX_SLOTS) {
        n = MAX_SLOTS;
    }
    if (__atomic_exchange_n(&s_cfg_dirty, false, __ATOMIC_SEQ_CST) || now - s_cfg_at_ms >= CFG_REFRESH_MS) {
        s_cfg_at_ms = now;
        for (size_t i = 0; i < n; i++) {
            bool ok = ntrip_server_get_config(i, &s_ccfg, false) == ESP_OK;
            s_slot_enabled[i] = ok && s_ccfg.enabled;
            s_slot_protected[i] = ok && s_ccfg.protected_slot;
        }
        memset(&s_ccfg, 0, sizeof(s_ccfg));
    }
    for (size_t i = 0; i < n; i++) {
        if (ntrip_server_get_status(i, &s_cst) != ESP_OK) {
            continue;
        }
        hm_slot_in_t si = {
            .enabled = s_slot_enabled[i] && s_cst.state != NTRIP_STATE_DISABLED,
            .state = (uint8_t)s_cst.state,
            .state_age_s = s_cst.state_age_s,
            .progress_age_ms = s_cst.progress_age_ms,
            .connect_budget_s = CONFIG_NTRIP_CONNECT_BUDGET_S,
            .send_timeout_ms = CONFIG_NTRIP_SEND_TIMEOUT_MS,
        };
        if (hm_slot_overrun(&s_slot[i], &si, now)) {
            ESP_LOGW(TAG, "caster slot %u over its budget (state %d for %" PRIu32 " s, no progress for %" PRIu32
                     " ms): restarting it",
                     (unsigned)i, (int)s_cst.state, s_cst.state_age_s, s_cst.progress_age_ms);
            if (ntrip_server_restart_slot(i) == ESP_OK) {
                s_caster_restarts++;
            }
        }
        if (hm_slot_down(&si)) {
            *any_down = true;
        }
        if (si.enabled && s_cst.state == NTRIP_STATE_STREAMING) {
            *any_streaming = true;
        }
        if (si.enabled && s_cst.unreached_s > *unreached_s) {
            *unreached_s = s_cst.unreached_s;
        }
        if (hm_protected_down(si.enabled, s_slot_protected[i], si.state, (uint8_t)s_cst.retry_reason)) {
            /* not while stopped by the login guard or waiting on a caster's
             * reply: a network action cannot help (the retry's class, not
             * the last failure, which outlives it) */
            *protected_down = true;
        }
    }
}

/* Network ladder inputs; the ladder itself runs in the network component.
 * @return the "no upstream" alert: an IP, the last probe reached nothing
 *         beyond the router, and no caster streams. */
static bool network_tick(bool have, const rtcm_hub_stats_t *hub, bool protected_down, bool any_streaming,
                         uint32_t unreached_s)
{
    network_ladder_input_t nin = {
        .caster_down = protected_down,
        .rtcm_healthy = hub->last_frame_age_ms != UINT32_MAX && hub->last_frame_age_ms <= STALL_MS,
        .slot_unreached_s = unreached_s,
        .flash_journal_open = s_st.flash_journal_open,
        .flash_journal_unknown = s_st.flash_journal_settings_unavailable,
        .min_reboot_interval_s = (uint32_t)CONFIG_HEALTH_NET_REBOOT_MIN_INTERVAL_H * 3600u,
    };
    esp_err_t err = network_ladder_tick(&nin);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "network ladder: %s", esp_err_to_name(err));
    }
    return have && s_net.online && s_net.probe == NETWORK_PROBE_NO_UPSTREAM && !any_streaming;
}

static bool heap_tick(int64_t now, health_status_t *o)
{
    o->heap_internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    o->heap_internal_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    o->heap_internal_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    o->heap_psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (o->heap_internal_min < HEAP_WARN_BYTES && !s_heap_warned) {
        s_heap_warned = true;
        ESP_LOGW(TAG, "internal heap low-water %" PRIu32 " B (free %" PRIu32 " B)", o->heap_internal_min,
                 o->heap_internal_free);
    }
    if (hm_heap_reboot_due(&s_heap, o->heap_internal_min, HEAP_REBOOT_BYTES)) {
        /* Allowed even with a receiver flash journal open: lifecycle waits
         * for the reboot inhibit, and a starved heap would take the web UI
         * down with it. */
        ESP_LOGE(TAG, "internal heap low-water %" PRIu32 " B: rebooting", o->heap_internal_min);
        (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_LOW_HEAP);
    } else if (o->heap_internal_min < HEAP_REBOOT_BYTES && !s_heap.done && !s_heap_held_logged) {
        s_heap_held_logged = true;
        ESP_LOGW(TAG, "internal heap low-water %" PRIu32 " B: %s, so no reboot", o->heap_internal_min,
                 s_heap.held ? "the previous boot already rebooted for low heap soon after its start"
                             : "reached while the firmware started (a reboot would repeat it)");
    }
    return hm_deb(&s_deb_frag, o->heap_internal_largest < FRAG_BYTES, now, FRAG_ON_MS, FRAG_OFF_MS);
}

/* A receiver flash session in full lockout (not waiting for the user). */
static bool receiver_update_running(void)
{
    if (lifecycle_update_active() == LIFECYCLE_UPDATE_RECEIVER) {
        return true;
    }
    return s_st.flash_journal_open && s_st.flash_journal_readable &&
           s_st.flash_journal.state != UM98X_JOURNAL_NEEDS_USER && s_st.flash.phase != UM98X_FLASH_NEEDS_USER;
}

static void admin_reset(void)
{
    ESP_LOGW(TAG, "BOOT button: clearing the admin password and opening the recovery AP");
    s_admin_resets++;   /* the web drops a password held only in RAM and ends every session */
    if (settings_is_available()) {
        esp_err_t err = settings_erase_key(SETTINGS_NS_SYS, SETTINGS_KEY_ADMIN_HASH);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "admin password not cleared: %s", esp_err_to_name(err));
        }
    } else {
        ESP_LOGW(TAG, "settings unavailable: no stored admin password to clear");
    }
    esp_err_t err = network_start_setup_ap(true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "recovery AP not started: %s", esp_err_to_name(err));
    }
}

static void factory_reset(int64_t now)
{
    if (receiver_update_running()) {
        s_factory_refused_ms = now;
        ESP_LOGE(TAG, "BOOT button: factory reset refused while a receiver update runs (it needs the stored "
                      "profile); try again when it has finished");
        return;
    }
    ESP_LOGW(TAG, "BOOT button: factory reset");
    esp_err_t err = settings_factory_reset();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "factory reset failed: %s", esp_err_to_name(err));
        return;
    }
    (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_FACTORY_RESET);
}

static void button_tick(int64_t now)
{
    const board_desc_t *b = board_get();
    if (!board_is_valid() || b->button_gpio < 0) {
        return;
    }
    bool pressed = gpio_get_level((gpio_num_t)b->button_gpio) == 0;   /* active low */
    switch (hm_button(&s_button, pressed, now, (uint32_t)CONFIG_HEALTH_BUTTON_ADMIN_S * 1000u,
                      (uint32_t)CONFIG_HEALTH_BUTTON_FACTORY_S * 1000u)) {
    case HM_BTN_ADMIN_REACHED:
        ESP_LOGW(TAG, "BOOT held %d s: release now to clear the admin password (keep holding to %d s for a "
                      "factory reset)",
                 CONFIG_HEALTH_BUTTON_ADMIN_S, CONFIG_HEALTH_BUTTON_FACTORY_S);
        break;
    case HM_BTN_FACTORY_REACHED:
        ESP_LOGW(TAG, "BOOT held %d s: release now for a factory reset", CONFIG_HEALTH_BUTTON_FACTORY_S);
        break;
    case HM_BTN_ADMIN:
        admin_reset();
        break;
    case HM_BTN_FACTORY:
        factory_reset(now);
        break;
    default:
        break;
    }
}

/* sys/setup_done, cached: read at start, on a settings event, and once a minute. */
static bool setup_done_now(int64_t now)
{
    if (__atomic_exchange_n(&s_setup_dirty, false, __ATOMIC_SEQ_CST) || s_setup_read_ms == HM_NEVER ||
        now - s_setup_read_ms >= SETUP_REREAD_MS) {
        uint8_t v = 0;
        s_setup_done = settings_is_available() &&
                       settings_get_u8(SETTINGS_NS_SYS, SETTINGS_KEY_SETUP_DONE, &v) == ESP_OK && v == 1;
        s_setup_read_ms = now;
    }
    return s_setup_done;
}

/* A caster slot is enabled (the cached configs casters_tick reads). */
static bool any_caster_enabled(void)
{
    for (size_t i = 0; i < MAX_SLOTS; i++) {
        if (s_slot_enabled[i]) {
            return true;
        }
    }
    return false;
}

/* Why the ladder's ESP32 restart waits right now (health_mon.h). Logged
 * only during a ladder episode in full recovery (`restarts`), where a
 * restart may be pending: outside one every admin visit would log the admin
 * hold coming and going, and the other modes never restart. */
static uint8_t hold_tick(int64_t now, bool have_net, bool episode, bool restarts, uint32_t *left_s)
{
    uint32_t admin = __atomic_load_n(&s_admin_at_s, __ATOMIC_SEQ_CST);
    hm_hold_in_t hi = {
        .now_ms = now,
        .episode = episode,
        .settings_available = settings_is_available(),
        .setup_done = setup_done_now(now),
        .ap_clients = have_net ? s_net.ap_clients : 0,
        .admin_at_ms = admin == 0 ? HM_NEVER : (int64_t)(admin - 1u) * 1000,
        .admin_window_ms = (uint32_t)CONFIG_HEALTH_REBOOT_HOLD_ADMIN_S * 1000u,
        .presence_max_ms = (uint32_t)CONFIG_HEALTH_REBOOT_HOLD_MAX_MIN * 60000u,
        .station_time_s = lifecycle_station_time_s(),
        .power_on_grace_s = (uint32_t)CONFIG_HEALTH_REBOOT_POWER_ON_GRACE_MIN * 60u,
        .heard = s_link_stats.last_rtcm_age_ms != UINT32_MAX || s_link_stats.framing.text_lines > 0,
        .casters_enabled = any_caster_enabled(),
    };
    uint8_t hold = hm_reboot_hold(&s_hold, &hi, left_s);
    uint8_t shown = (episode && restarts) ? hold : 0u;
    if (shown != s_prev_hold) {
        char was[128];
        char now_text[128];
        hold_text(s_prev_hold, was, sizeof(was));
        hold_text(shown, now_text, sizeof(now_text));
        ESP_LOGI(TAG, "GNSS ladder restart hold: %s -> %s", s_prev_hold ? was : "none", shown ? now_text : "none");
        s_prev_hold = shown;
    }
    return hold;
}

/* gnss/recovery, read before every ladder decision; out of range reads as full. */
static uint8_t read_recovery_mode(void)
{
    uint8_t v = HL_MODE_FULL;
    if (!settings_is_available() || settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RECOVERY, &v) != ESP_OK) {
        return HL_MODE_FULL;
    }
    if (v > HL_MODE_ALERTS) {
        if (s_mode_bad != (int)v) {
            s_mode_bad = (int)v;
            ESP_LOGW(TAG, "gnss/recovery = %u is not a recovery mode: full recovery", (unsigned)v);
        }
        return HL_MODE_FULL;
    }
    return v;
}

static const char *mode_name(uint8_t m)
{
    static const char *const k[] = { "full", "no ESP32 reboot", "alerts only" };
    return m < sizeof(k) / sizeof(k[0]) ? k[m] : "?";
}

/*
 * The journal's boot records, read a bounded number per tick (the whole
 * journal over the first ticks, then what is new). Until the reader has
 * caught up (a read came back short while nothing was waiting in the
 * journal's queue, so this boot's own boot record, appended before the
 * health task started, has been fed), boots whose dating records were not
 * read yet would count as "now": the alert keeps its last value (off at
 * start) meanwhile. After that the running boot is dated from the trusted
 * clock itself, so a boot that writes no dated record still ends the alert
 * 24 h after it started. When the user clears the journal, the boots read
 * from it are forgotten too and the reading starts again from what follows
 * the clear (the running boot's own records went with it, so the alert is
 * off until new power losses are recorded).
 */
static bool power_tick(void)
{
    diag_journal_info_t ji;
    bool flushed = false;
    uint32_t clears = diag_journal_clear_count();
    if (clears != s_jclears) {
        s_jclears = clears;
        hm_power_init(&s_power);
        s_jcursor = 0;
    }
    if (!s_power_caught_up) {
        diag_journal_info(&ji);
        flushed = ji.available && ji.queued == 0;
    }
    bool short_read = false;
    for (int i = 0; i < JOURNAL_READS_PER_TICK; i++) {
        size_t n = JOURNAL_CHUNK;
        if (diag_journal_read(&s_jcursor, s_jbuf, &n) != ESP_OK) {
            break;
        }
        for (size_t k = 0; k < n; k++) {
            hm_power_feed(&s_power, s_jbuf[k].kind, s_jbuf[k].a, s_jbuf[k].unix_s, s_jbuf[k].uptime_s);
        }
        if (n < JOURNAL_CHUNK) {
            short_read = true;
            break;
        }
    }
    if (!s_power_caught_up) {
        if (!(flushed && short_read)) {
            return s_power_alert;
        }
        s_power_caught_up = true;
    }
    int64_t utc = diag_utc_now();
    hm_power_running(&s_power, utc, (uint32_t)(esp_timer_get_time() / 1000000));
    s_power_alert = hm_power_alert(&s_power, utc);
    return s_power_alert;
}

/* The ladder's reason in words, once when it changes. */
static void log_why(uint8_t why)
{
    if (why == s_prev_why) {
        return;
    }
    s_prev_why = why;
    switch (why) {
    case HL_WHY_HELD:
        ESP_LOGI(TAG, "no RTCM: the receiver's first configuration waits for you (the receiver page); not a failure");
        break;
    case HL_WHY_SILENT:
        ESP_LOGW(TAG, "no RTCM and nothing usable from the receiver, no answer to commands: power-cycle the station");
        break;
    case HL_WHY_NO_SKY:
        ESP_LOGW(TAG, "no RTCM: the receiver answers but uses no satellites; check that the antenna is on the socket "
                      "marked M and sees the sky");
        break;
    case HL_WHY_NO_RTCM:
        ESP_LOGW(TAG, "no RTCM although the receiver answers: check that the antenna is on the socket marked M and "
                      "sees the sky");
        break;
    default:
        break;   /* none, or 6b (its own log line) */
    }
}

static void log_changes(const health_status_t *o)
{
    if (o->level != s_prev_level) {
        static const char *const k[] = { "ok", "degraded", "recovering", "failed" };
        ESP_LOGI(TAG, "level %s -> %s", k[s_prev_level & 3u], k[o->level & 3u]);
        s_prev_level = (uint8_t)o->level;
    }
    if (o->alerts != s_prev_alerts) {
        ESP_LOGI(TAG, "alerts 0x%03" PRIx32 " -> 0x%03" PRIx32, s_prev_alerts, o->alerts);
        s_prev_alerts = o->alerts;
    }
    if (o->gnss_ladder_suspended != s_prev_suspended) {
        ESP_LOGD(TAG, "GNSS ladder %s", o->gnss_ladder_suspended ? "suspended" : "running");
        s_prev_suspended = o->gnss_ladder_suspended;
    }
}

static void tick(void)
{
    int64_t now = now_ms();
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);

    (void)gnss_link_get_stats(&s_link_stats);
    rtcm_hub_stats_t hub;
    rtcm_hub_get_stats(&hub);

    /* Transition counters first, then the status: a transition counted here
     * is already in the status read after it. */
    uint32_t enters = __atomic_load_n(&s_rec_enters, __ATOMIC_SEQ_CST);
    uint32_t exits = __atomic_load_n(&s_rec_exits, __ATOMIC_SEQ_CST);
    if (station_get_status(&s_st) != ESP_OK) {
        memset(&s_st, 0, sizeof(s_st));
        s_st.state = STATION_STATE_IDLE;
        s_st.since_lease_release_ms = UINT32_MAX;
    }

    hl_in_t *in = &s_in;
    in->now_ms = now;
    in->stall_ms = STALL_MS;
    in->rtcm_age_ms = s_link_stats.last_rtcm_age_ms;
    in->rx_age_ms = s_link_stats.last_rx_age_ms;
    in->rx_bytes = s_link_stats.rx_bytes;
    in->text_lines = s_link_stats.framing.text_lines;
    in->stn_state = (uint8_t)s_st.state;
    in->rec_enters = enters;
    in->rec_exits = exits;
    in->lease_held = s_st.lease_held;
    in->since_release_ms = s_st.since_lease_release_ms;
    in->journal_open = s_st.flash_journal_open;
    in->journal_unknown = s_st.flash_journal_settings_unavailable;
    in->safe_mode = boot.safe_mode;
    in->detect_saw_bytes = s_st.detect_saw_bytes;
    in->reconfigure_refused = s_st.reconfigure_refused;
    in->surveying = s_st.surveying;
#if CONFIG_HEALTH_SURVEY_HOLDS_RTCM
    in->survey_holds_rtcm = true;
#else
    in->survey_holds_rtcm = false;
#endif
    uint32_t cap_wait = 0;
    lifecycle_cap_t cap = lifecycle_ladder_reboot_cap(&cap_wait);
    in->reboot_allowed = cap == LIFECYCLE_CAP_NONE;
    in->reboot_cap_why = cap == LIFECYCLE_CAP_DAY ? HL_CAP_DAY : cap == LIFECYCLE_CAP_HOUR ? HL_CAP_HOUR : HL_CAP_UNKNOWN;
    in->reboot_inhibited = lifecycle_reboot_inhibited(NULL);
    in->detect_passes = s_st.detect_passes;
    in->answers = s_st.rx_answers;
    in->rx_held = s_st.first_apply_held;
    gnss_obs_fix_t fix;
    in->no_sky = gnss_obs_get_fix(&fix) == ESP_OK && fix.age_ms <= STALL_MS && fix.quality == 0 && fix.satellites == 0;
    bool have_net = network_get_status(&s_net) == ESP_OK;
    hl_status_t prev;
    hl_get_status(&s_ladder, in, &prev);
    uint8_t mode = read_recovery_mode();
    if (mode != s_mode) {
        ESP_LOGI(TAG, "automatic receiver recovery: %s -> %s", mode_name(s_mode), mode_name(mode));
        s_mode = mode;
    }
    in->recovery_mode = mode;
    /* Only full recovery restarts the ESP32 (out-of-range values read as full). */
    bool restarts = mode == HL_MODE_FULL;
    uint32_t hold_left = UINT32_MAX;
    in->reboot_hold = hold_tick(now, have_net, prev.episode, restarts, &hold_left);
    hl_tick(&s_ladder, in);
    hl_status_t ls;
    hl_get_status(&s_ladder, in, &ls);

    loss_tick(&s_link_stats, &hub, now);
    bool link_loss = hm_link_tick(&s_link, s_link_stats.hw_fifo_overflows, s_link_stats.framing.rtcm_crc_errors,
                                  s_link_stats.framing.rtcm_frames, ls.suspended, now);

    bool any_down;
    bool protected_down;
    bool any_streaming;
    uint32_t unreached_s;
    casters_tick(now, &any_down, &protected_down, &any_streaming, &unreached_s);
    bool casters_down = hm_deb(&s_deb_casters, any_down, now, CASTER_ON_MS, CASTER_OFF_MS);
    bool no_upstream = network_tick(have_net, &hub, protected_down, any_streaming, unreached_s);
    bool power_lost = power_tick();

    gnss_obs_summary_t obs;
    bool bands_raw = gnss_obs_get_summary(&obs) == ESP_OK && obs.epochs > 0 && obs.epoch_age_ms <= STALL_MS &&
                     (obs.best_bands < 4 || obs.e6_lost);
    bool bands = hm_deb(&s_deb_bands, bands_raw, now, BANDS_ON_MS, BANDS_OFF_MS);

    /* Only RTCM that flows and differs: no RTCM at all (no antenna, a
     * silent receiver) is a stall, which no_rtcm, receiver_silent or
     * no_satellites explain. */
    bool drift_raw = s_st.state == STATION_STATE_STREAMING && s_st.identity_confirmed && s_st.rtcm_differs &&
                     (s_st.alerts & (STATION_ALERT_UNSUPPORTED_MODEL | STATION_ALERT_CONFIG_FAILED)) == 0;
    bool drift = hm_deb(&s_deb_drift, drift_raw, now, DRIFT_ON_MS, DRIFT_OFF_MS);

    health_status_t o;
    memset(&o, 0, sizeof(o));
    bool fragmented = heap_tick(now, &o);
    button_tick(now);
    uint32_t gps_tow;
    uint32_t gps_age;
    if (gnss_obs_get_gps_time(&gps_tow, &gps_age) == ESP_OK) {
        ledger_set_gps_time(gps_tow, gps_age);   /* the ledger's GNSS clock */
    }
    ledger_tick();

    uint32_t alerts = 0;
    if (boot.brownouts_since_power_on > 0) {
        alerts |= HEALTH_ALERT_POWER_SUPPLY;
    }
    if (ls.power_alert) {
        alerts |= HEALTH_ALERT_RECEIVER_POWER;
    }
    if (bands) {
        alerts |= HEALTH_ALERT_BANDS;
    }
    if (drift) {
        alerts |= HEALTH_ALERT_PROFILE_DRIFT;
    }
    if (fragmented) {
        alerts |= HEALTH_ALERT_HEAP_FRAGMENTED;
    }
    if (boot.safe_mode) {
        alerts |= HEALTH_ALERT_SAFE_MODE;
    }
    if (!settings_is_available()) {
        alerts |= HEALTH_ALERT_SETTINGS_DEGRADED;
    }
    if (no_upstream) {
        alerts |= HEALTH_ALERT_NO_UPSTREAM;
    }
    if (link_loss) {
        alerts |= HEALTH_ALERT_LINK_LOSS;
    }
    if (power_lost) {
        alerts |= HEALTH_ALERT_POWER_LOST;
    }
    /* Why the receiver path is down: a FAILED level never comes bare. */
    switch (ls.why) {
    case HL_WHY_HELD:
        alerts |= HEALTH_ALERT_RX_HELD;
        break;
    case HL_WHY_SILENT:
        alerts |= HEALTH_ALERT_RECEIVER_SILENT;
        break;
    case HL_WHY_NO_SKY:
        alerts |= HEALTH_ALERT_NO_SATELLITES;
        break;
    case HL_WHY_NO_RTCM:
        alerts |= HEALTH_ALERT_NO_RTCM;
        break;
    default:
        break;   /* none, or 6b (HEALTH_ALERT_RECEIVER_POWER above) */
    }
    log_why(ls.why);

    /* The ladder's reason alerts come with its own level (FAILED, or DEGRADED while only reported or held). */
    const uint32_t ladder_alerts = HEALTH_ALERT_RECEIVER_POWER | HEALTH_ALERT_RECEIVER_SILENT | HEALTH_ALERT_NO_SATELLITES |
                                   HEALTH_ALERT_NO_RTCM | HEALTH_ALERT_RX_HELD;
    bool degraded = (alerts & ~ladder_alerts) != 0 || casters_down;
    o.level = (health_level_t)hm_level(ls.level, degraded);
    o.alerts = alerts;
    o.last_step = (health_step_t)ls.last_step;
    o.gnss_step = (health_step_t)ls.doing;
    o.gnss_step_phase = ls.doing == 0 ? HEALTH_STEP_PHASE_NONE
                        : ls.waiting  ? HEALTH_STEP_PHASE_WAITING
                                      : HEALTH_STEP_PHASE_RUNNING;
    o.gnss_ladder_suspended = ls.suspended;
    o.uptime_s = (uint64_t)(esp_timer_get_time() / 1000000);
    o.gnss_stalls = ls.stalls;
    o.recoveries = ls.recoveries;
    o.caster_restarts = s_caster_restarts;
    o.network_recoveries = s_net.ladder_actions;
    o.rtcm_gap_ms = s_link_stats.last_rtcm_age_ms;
    o.gnss_retry_in_s = ls.retry_in_s;
    o.admin_resets = s_admin_resets;
    o.recovery_mode = (health_recovery_mode_t)mode;
    o.gnss_reboot_recheck = ls.reboot_recheck;
    hm_hold_view_t hv;
    hm_hold_view(ls.restart_possible, restarts, in->reboot_hold, hold_left, in->reboot_allowed, cap_wait, &hv);
    o.gnss_reboot_hold = hv.hold;
    o.gnss_reboot_hold_left_s = hv.hold_left_s;
    o.gnss_reboot_cap_wait_s = hv.cap_wait_s;
    o.gnss_reboot_cap_limit = hv.cap_wait_s == UINT32_MAX ? HEALTH_CAP_NONE
                              : cap == LIFECYCLE_CAP_DAY  ? HEALTH_CAP_DAY
                                                          : HEALTH_CAP_HOUR;
    if (s_factory_refused_ms == HM_NEVER) {
        o.factory_reset_refused_age_s = UINT32_MAX;
    } else {
        int64_t age = (now - s_factory_refused_ms) / 1000;
        o.factory_reset_refused_age_s = age >= (int64_t)UINT32_MAX ? UINT32_MAX - 1u : (uint32_t)age;
    }

    portENTER_CRITICAL(&s_lock);
    s_pub = o;
    s_pub_valid = true;
    portEXIT_CRITICAL(&s_lock);
    log_changes(&o);
}

static void health_task(void *arg)
{
    (void)arg;
    bool wdt = esp_task_wdt_add(NULL) == ESP_OK;
    if (!wdt) {
        ESP_LOGW(TAG, "not on the task watchdog");
    }
    TickType_t last = xTaskGetTickCount();
    for (;;) {
        tick();
        if (wdt) {
            (void)esp_task_wdt_reset();
        }
        vTaskDelayUntil(&last, pdMS_TO_TICKS(TICK_MS));
    }
}

/* ---- public API -------------------------------------------------------------- */

esp_err_t health_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    int64_t now = now_ms();
    const hl_ops_t ops = { .request = op_request, .reboot = op_reboot, .journal = op_journal, .ctx = NULL };
    hl_init(&s_ladder, &ops, now);
    hm_loss_init(&s_loss);
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    hm_heap_init(&s_heap, boot.prev_reboot_reason == LIFECYCLE_REBOOT_LOW_HEAP, boot.prev_session_uptime_s);
    hm_link_init(&s_link, now);
    for (size_t i = 0; i < MAX_SLOTS; i++) {
        hm_slot_init(&s_slot[i]);
    }
    hm_deb_init(&s_deb_bands);
    hm_deb_init(&s_deb_drift);
    hm_deb_init(&s_deb_frag);
    hm_deb_init(&s_deb_casters);
    hm_button_init(&s_button);
    hm_power_init(&s_power);
    s_power_caught_up = false;
    s_power_alert = false;
    s_mode = read_recovery_mode();
    hm_hold_init(&s_hold);

    esp_err_t err = esp_event_handler_register(STATION_EVENT, STATION_EVENT_STATE_CHANGED, on_station_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "station events not received (%s): ladder steps are followed by polling only",
                 esp_err_to_name(err));
    }
    err = esp_event_handler_register(NTRIP_EVENT, NTRIP_EVENT_STATE_CHANGED, on_ntrip_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "caster events not received (%s)", esp_err_to_name(err));
    }
    err = esp_event_handler_register(SETTINGS_EVENT, ESP_EVENT_ANY_ID, on_settings_event, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "settings events not received (%s): first-setup state read once a minute", esp_err_to_name(err));
    }
    s_inited = true;
    ESP_LOGI(TAG, "stall after %d s without RTCM; BOOT button %d s / %d s", CONFIG_HEALTH_GNSS_STALL_S,
             CONFIG_HEALTH_BUTTON_ADMIN_S, CONFIG_HEALTH_BUTTON_FACTORY_S);
    return ESP_OK;
}

esp_err_t health_start(void)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_task != NULL) {
        return ESP_OK;
    }
    TaskHandle_t t = NULL;
    if (xTaskCreate(health_task, "health", CONFIG_HEALTH_TASK_STACK, NULL, TASK_PRIO, &t) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    s_task = t;
    return ESP_OK;
}

esp_err_t health_get_status(health_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    bool have;
    portENTER_CRITICAL(&s_lock);
    have = s_pub_valid;
    if (have) {
        *out = s_pub;
    }
    portEXIT_CRITICAL(&s_lock);
    if (have) {
        out->uptime_s = (uint64_t)(esp_timer_get_time() / 1000000);
        return ESP_OK;
    }

    /* Before the first tick (or without the monitor task): what is known
     * without sampling. FAILED is reserved for the ladder; safe mode is an
     * alert. */
    memset(out, 0, sizeof(*out));
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    if (boot.safe_mode) {
        out->alerts |= HEALTH_ALERT_SAFE_MODE;
    }
    if (boot.brownouts_since_power_on > 0) {
        out->alerts |= HEALTH_ALERT_POWER_SUPPLY;
    }
    if (!settings_is_available()) {
        out->alerts |= HEALTH_ALERT_SETTINGS_DEGRADED;
    }
    out->level = out->alerts != 0 ? HEALTH_DEGRADED : HEALTH_OK;
    out->uptime_s = (uint64_t)(esp_timer_get_time() / 1000000);
    out->recovery_mode = (health_recovery_mode_t)read_recovery_mode();
    out->rtcm_gap_ms = UINT32_MAX;
    out->gnss_reboot_hold_left_s = UINT32_MAX;
    out->gnss_reboot_cap_wait_s = UINT32_MAX;
    out->factory_reset_refused_age_s = UINT32_MAX;
    out->heap_internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->heap_psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    return ESP_OK;
}

void health_note_admin_activity(void)
{
    uint32_t up = (uint32_t)(esp_timer_get_time() / 1000000);
    __atomic_store_n(&s_admin_at_s, up + 1u, __ATOMIC_SEQ_CST);
}
