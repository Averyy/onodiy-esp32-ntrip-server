/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * App trial after an update (confirm, verify, crash watch, revert), the
 * rolled-back list and manual rollback. Decisions are in app_trial.c.
 */
#include <stdlib.h>
#include <string.h>

#include "diag.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "fw_internal.h"
#include "network.h"
#include "ntrip_server.h"
#include "sdkconfig.h"
#include "settings.h"
#include "station.h"

static const char *TAG = "fw_update";

#define WATCH_S ((uint32_t)CONFIG_FW_UPDATE_CRASH_WATCH_H * 3600u)

/* DIAG_EV_APP_REVERT `a`: the stage that gave the image up (0 = the user). */
#define REVERT_BY_USER 0

/* ---- persistence -------------------------------------------------------------------- */

void fw_trial_save_rec(const fw_trial_rec_t *rec)
{
    uint8_t buf[FW_TRIAL_REC_MAX_BLOB];
    size_t n = fw_trial_rec_encode(rec, buf, sizeof(buf));
    esp_err_t err = (n == 0) ? ESP_ERR_INVALID_SIZE : settings_set_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_TRIAL, buf, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "trial record not stored: %s", esp_err_to_name(err));
    }
}

void fw_trial_save_bad(const fw_bad_versions_t *bad)
{
    uint8_t buf[FW_BAD_VER_MAX_BLOB];
    size_t n = fw_bad_encode(bad, buf, sizeof(buf));
    esp_err_t err =
        (n == 0) ? ESP_ERR_INVALID_SIZE : settings_set_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_BAD_VER, buf, n);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "rolled-back list not stored: %s", esp_err_to_name(err));
    }
}

static void load_rec(fw_trial_rec_t *rec)
{
    memset(rec, 0, sizeof(*rec));
    uint8_t buf[FW_TRIAL_REC_MAX_BLOB * 2];
    size_t len = sizeof(buf);
    if (settings_get_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_TRIAL, buf, &len) == ESP_OK &&
        !fw_trial_rec_decode(buf, len, rec)) {
        ESP_LOGW(TAG, "trial record unreadable: ignored");
        memset(rec, 0, sizeof(*rec));
    }
}

static void load_bad(fw_bad_versions_t *bad)
{
    memset(bad, 0, sizeof(*bad));
    uint8_t *buf = malloc(FW_BAD_VER_MAX_BLOB * 2);
    if (buf == NULL) {
        return;
    }
    size_t len = FW_BAD_VER_MAX_BLOB * 2;
    if (settings_get_blob(SETTINGS_NS_OTA, SETTINGS_KEY_OTA_BAD_VER, buf, &len) == ESP_OK &&
        !fw_bad_decode(buf, len, bad)) {
        ESP_LOGW(TAG, "rolled-back list unreadable: ignored");
        memset(bad, 0, sizeof(*bad));
    }
    free(buf);
}

/* ---- helpers ---------------------------------------------------------------------- */

static void inhibit_take(void)
{
    fw_lock();
    bool held = g_fw.trial_inhibit_held;
    fw_unlock();
    if (held) {
        return;
    }
    lifecycle_inhibit_t tok;
    esp_err_t err = lifecycle_inhibit_acquire("app update trial", &tok);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "trial reboot inhibit not taken: %s", esp_err_to_name(err));
        return;
    }
    fw_lock();
    g_fw.trial_inhibit = tok;
    g_fw.trial_inhibit_held = true;
    fw_unlock();
}

static void inhibit_drop(void)
{
    fw_lock();
    bool held = g_fw.trial_inhibit_held;
    lifecycle_inhibit_t tok = g_fw.trial_inhibit;
    g_fw.trial_inhibit_held = false;
    fw_unlock();
    if (held) {
        lifecycle_inhibit_release(tok);
    }
}

static void set_run(fw_trial_run_t run)
{
    fw_lock();
    g_fw.run = run;
    fw_unlock();
    lifecycle_set_app_trial(run != FW_TRIAL_RUN_NONE);
}

static void journal(diag_event_t kind, int32_t a, int32_t b)
{
    esp_err_t err = diag_journal_append(kind, a, b);
    if (err != ESP_OK) {
        /* The log line next to every call says the same. */
        ESP_LOGD(TAG, "journal record %d not written: %s", (int)kind, esp_err_to_name(err));
    }
}

/* Station state that means "no RTCM because of the receiver". */
static bool receiver_problem(void)
{
    station_status_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        return false;
    }
    bool problem = false;
    if (station_get_status(st) == ESP_OK) {
        switch (st->state) {
        case STATION_STATE_DETECTING:
        case STATION_STATE_CONFIGURING:
        case STATION_STATE_RECOVERING:
        case STATION_STATE_NO_RECEIVER:
        case STATION_STATE_FLASHING:
        case STATION_STATE_FLASH_RECOVERY:
            problem = true;
            break;
        case STATION_STATE_IDLE:
        case STATION_STATE_STREAMING:
            break;
        }
    }
    free(st);
    return problem;
}

/* Caster slots in `mask` that stream now. */
static uint8_t streaming_slots(uint8_t mask)
{
    if (mask == 0) {
        return 0;
    }
    ntrip_caster_status_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        return 0;
    }
    uint8_t now = 0;
    size_t n = ntrip_server_slot_count();
    for (size_t i = 0; i < n && i < 8; i++) {
        if ((mask & (1u << i)) && ntrip_server_get_status(i, st) == ESP_OK && st->state == NTRIP_STATE_STREAMING) {
            now |= (uint8_t)(1u << i);
        }
    }
    free(st);
    return now;
}

/*
 * Give the running image up: mark it invalid so the bootloader starts the
 * other slot, put its version on the rolled-back list, reboot. `stage` is
 * what gave it up (journal). At boot (from_init) the reboot runs right here.
 */
static void revert(fw_trial_run_t stage, const char *why, bool from_init)
{
    const char *version = esp_app_get_description()->version;
    /* No receiver flash may start before the reboot. */
    (void)lifecycle_update_claim(LIFECYCLE_UPDATE_APP);
    esp_err_t err = esp_ota_mark_app_invalid_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app %s %s, but there is no other image to go back to (%s): keeping it", version, why,
                 esp_err_to_name(err));
        journal(DIAG_EV_APP_REVERT, (int32_t)stage, (int32_t)err);
        lifecycle_update_release(LIFECYCLE_UPDATE_APP);
        fw_lock();
        g_fw.rec.stage = FW_TRIAL_REC_NONE;
        fw_trial_rec_t rec = g_fw.rec;
        fw_unlock();
        fw_trial_save_rec(&rec);
        inhibit_drop();
        set_run(FW_TRIAL_RUN_NONE);
        return;
    }
    fw_lock();
    fw_bad_add(&g_fw.bad, version);
    fw_bad_versions_t bad = g_fw.bad;
    strlcpy(g_fw.rec.version, version, sizeof(g_fw.rec.version));
    g_fw.rec.stage = FW_TRIAL_REC_REVERTED;
    fw_trial_rec_t rec = g_fw.rec;
    fw_unlock();
    fw_trial_save_bad(&bad);
    fw_trial_save_rec(&rec);
    journal(DIAG_EV_APP_REVERT, (int32_t)stage, 0);
    ESP_LOGE(TAG, "app %s %s: going back to the previous image", version, why);
    inhibit_drop();
    if (from_init && lifecycle_reboot(LIFECYCLE_REBOOT_APP_REVERT, false) == ESP_OK) {
        return;   /* not reached */
    }
    (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_APP_REVERT);
}

/* ---- boot ------------------------------------------------------------------------- */

void fw_trial_boot(void)
{
    const char *running = esp_app_get_description()->version;
    const esp_partition_t *part = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    bool pending = part != NULL && esp_ota_get_state_partition(part, &state) == ESP_OK &&
                   state == ESP_OTA_IMG_PENDING_VERIFY;

    char invalid_version[sizeof(((esp_app_desc_t *)0)->version)] = "";
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (invalid != NULL && esp_ota_get_partition_description(invalid, &desc) == ESP_OK) {
        strlcpy(invalid_version, desc.version, sizeof(invalid_version));
    }

    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);

    fw_trial_rec_t rec;
    fw_bad_versions_t bad;
    load_rec(&rec);
    load_bad(&bad);

    fw_trial_boot_in_t in = {
        .running_version = running,
        .pending_verify = pending,
        .crash_reset = boot.reset_class == LIFECYCLE_RESET_CRASH,
        .prev_uptime_s = boot.prev_session_uptime_s,
        .last_invalid_version = invalid_version,
        .crash_revert_count = CONFIG_FW_UPDATE_CRASH_REVERT_COUNT,
    };
    fw_trial_boot_out_t out;
    fw_trial_on_boot(&rec, &in, &out);

    if (out.add_bad && fw_bad_add(&bad, rec.version)) {
        fw_trial_save_bad(&bad);
    }
    if (out.persist) {
        fw_trial_save_rec(&rec);
    }
    if (out.stage1_rollback) {
        ESP_LOGW(TAG, "update to %s was rolled back during its first minute (a crash or a reset)%s", rec.version,
                 out.add_bad ? ": the second time, not offered again" : "");
        journal(DIAG_EV_APP_REVERT, (int32_t)FW_TRIAL_CONFIRMING, (int32_t)rec.attempt);
    }

    fw_lock();
    g_fw.rec = rec;
    g_fw.bad = bad;
    fw_unlock();
    set_run(out.run);

    switch (out.run) {
    case FW_TRIAL_RUN_CONFIRMING:
        ESP_LOGW(TAG, "new app image %s on trial: confirmed after %d s if nothing resets it", running,
                 CONFIG_FW_UPDATE_CONFIRM_S);
        break;
    case FW_TRIAL_RUN_VERIFYING:
    case FW_TRIAL_RUN_WATCHING:
        ESP_LOGW(TAG, "app %s on trial (boot %u, %u crash reset(s) of %d allowed)", running, (unsigned)rec.boots,
                 (unsigned)rec.crashes, CONFIG_FW_UPDATE_CRASH_REVERT_COUNT - 1);
        break;
    case FW_TRIAL_RUN_NONE:
        break;
    }
    if (out.revert) {
        revert(out.run, "kept crashing", true);
    }
}

void fw_trial_start(void)
{
    fw_lock();
    fw_trial_run_t run = g_fw.run;
    if (run == FW_TRIAL_RUN_VERIFYING || run == FW_TRIAL_RUN_WATCHING) {
        /* A later boot of the image on trial: stage 2 runs from boot on. */
        g_fw.stage2_start_s = 0;
        g_fw.window_start_s = 0;
        memset(&g_fw.obs, 0, sizeof(g_fw.obs));
    }
    fw_unlock();
    if (run == FW_TRIAL_RUN_VERIFYING) {
        inhibit_take();
    }
}

/* ---- running trial ------------------------------------------------------------------ */

static void confirm(uint32_t uptime, bool trusted, int64_t now)
{
    static bool s_failed_logged;
    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        if (!s_failed_logged) {
            ESP_LOGE(TAG, "confirming the new image failed (%s): retrying", esp_err_to_name(err));
            s_failed_logged = true;
        }
        return;
    }
    fw_lock();
    g_fw.rec.stage = FW_TRIAL_REC_VERIFYING;
    g_fw.rec.confirm_uptime_s = uptime;
    g_fw.rec.watch_start_unix = trusted ? now : 0;
    fw_trial_rec_t rec = g_fw.rec;
    g_fw.stage2_start_s = uptime;
    g_fw.window_start_s = uptime;
    memset(&g_fw.obs, 0, sizeof(g_fw.obs));
    g_fw.rollback_known = false;   /* the otadata changed */
    fw_unlock();
    fw_trial_save_rec(&rec);
    inhibit_take();
    set_run(FW_TRIAL_RUN_VERIFYING);
    ESP_LOGI(TAG, "app %s confirmed; verifying it reaches the station's state from before the update (%d s)",
             rec.version, CONFIG_FW_UPDATE_VERIFY_WINDOW_S);
}

void fw_trial_tick_worker(void)
{
    fw_lock();
    fw_trial_run_t run = g_fw.run;
    bool started = g_fw.started;
    fw_update_state_t state = g_fw.state;
    fw_trial_rec_t rec = g_fw.rec;
    fw_unlock();
    /* An activated update replaces this image's trial; nothing to do until the reboot. */
    if (run == FW_TRIAL_RUN_NONE || state == FW_UPDATE_REBOOT_PENDING) {
        return;
    }
    uint32_t uptime = fw_uptime_s();
    int64_t now = 0;
    bool trusted = fw_clock(&now);

    if (run == FW_TRIAL_RUN_VERIFYING) {
        bool online = network_is_online();
        bool fed = fw_hub_fed_now();
        uint8_t slots = streaming_slots(rec.streaming_slots);
        bool problem = receiver_problem();
        fw_lock();
        g_fw.obs.sta_online_seen |= online;
        g_fw.obs.hub_fed_seen |= fed;
        g_fw.obs.streaming_seen |= slots;
        g_fw.obs.receiver_problem = problem;
        fw_unlock();
    }

    fw_lock();
    if ((run == FW_TRIAL_RUN_VERIFYING || run == FW_TRIAL_RUN_WATCHING) && trusted && g_fw.rec.watch_start_unix == 0) {
        /* First trusted time in stage 2: anchor the wall-clock start. */
        uint32_t so_far = fw_trial_watch_elapsed_s(&g_fw.rec, uptime, g_fw.stage2_start_s, false, 0);
        g_fw.rec.watch_start_unix = now - (int64_t)so_far;
        rec = g_fw.rec;
        fw_unlock();
        fw_trial_save_rec(&rec);
        fw_lock();
    }
    fw_trial_obs_t obs = g_fw.obs;
    fw_trial_tick_in_t in = {
        .run = run,
        .uptime_s = uptime,
        .init_complete = started,
        .confirm_s = CONFIG_FW_UPDATE_CONFIRM_S,
        .window_start_s = g_fw.window_start_s,
        .verify_window_s = CONFIG_FW_UPDATE_VERIFY_WINDOW_S,
        .watch_elapsed_s = fw_trial_watch_elapsed_s(&g_fw.rec, uptime, g_fw.stage2_start_s, trusted, now),
        .watch_s = WATCH_S,
    };
    rec = g_fw.rec;
    fw_unlock();

    switch (fw_trial_tick(&in, &rec, &obs)) {
    case FW_TRIAL_ACT_NONE:
        break;
    case FW_TRIAL_ACT_CONFIRM:
        confirm(uptime, trusted, now);
        break;
    case FW_TRIAL_ACT_VERIFIED:
        fw_lock();
        g_fw.rec.stage = FW_TRIAL_REC_WATCHING;
        rec = g_fw.rec;
        fw_unlock();
        fw_trial_save_rec(&rec);
        inhibit_drop();
        set_run(FW_TRIAL_RUN_WATCHING);
        ESP_LOGI(TAG, "app %s %s; crash watch until %d h after the update", rec.version,
                 fw_trial_baseline_reached(&rec, &obs) ? "reached the station's state from before the update"
                                                       : "verified (the receiver is not delivering; not held against it)",
                 CONFIG_FW_UPDATE_CRASH_WATCH_H);
        break;
    case FW_TRIAL_ACT_REVERT:
        ESP_LOGE(TAG, "baseline missed: network %s, hub %s, caster slots %02x of %02x",
                 obs.sta_online_seen ? "ok" : "never online", obs.hub_fed_seen ? "fed" : "not fed",
                 (unsigned)(obs.streaming_seen & rec.streaming_slots), (unsigned)rec.streaming_slots);
        revert(FW_TRIAL_RUN_VERIFYING, "did not reach the station's state from before the update", false);
        break;
    case FW_TRIAL_ACT_DONE:
        fw_lock();
        g_fw.rec.stage = FW_TRIAL_REC_NONE;
        rec = g_fw.rec;
        fw_unlock();
        fw_trial_save_rec(&rec);
        set_run(FW_TRIAL_RUN_NONE);
        ESP_LOGI(TAG, "app %s accepted: trial over", rec.version);
        break;
    }
}

void fw_trial_refresh_rollback(void)
{
    bool possible = esp_ota_check_rollback_is_possible();
    fw_lock();
    g_fw.rollback_available = possible;
    g_fw.rollback_known = true;
    fw_unlock();
}

void fw_trial_record_install(const char *version, uint8_t baseline, uint8_t slots)
{
    int64_t now = 0;
    bool trusted = fw_clock(&now);
    fw_lock();
    fw_trial_rec_t prev = g_fw.rec;
    fw_trial_rec_t rec;
    fw_trial_rec_new(&rec, &prev, version, baseline, slots, trusted ? now : 0);
    g_fw.rec = rec;
    g_fw.run = FW_TRIAL_RUN_NONE;   /* this image's own trial (if any) is superseded */
    fw_unlock();
    fw_trial_save_rec(&rec);
}

bool fw_trial_blocks_install_locked(void)
{
    return g_fw.run == FW_TRIAL_RUN_CONFIRMING || g_fw.run == FW_TRIAL_RUN_VERIFYING;
}

void fw_trial_status_locked(fw_update_status_t *out, bool clock_trusted, int64_t now_unix)
{
    uint32_t uptime = fw_uptime_s();
    fw_trial_run_t run = g_fw.run;
    uint32_t elapsed = fw_trial_watch_elapsed_s(&g_fw.rec, uptime, g_fw.stage2_start_s, clock_trusted, now_unix);
    out->trial = fw_trial_public(run);
    switch (run) {
    case FW_TRIAL_RUN_CONFIRMING:
        out->trial_left_s = uptime < (uint32_t)CONFIG_FW_UPDATE_CONFIRM_S ? CONFIG_FW_UPDATE_CONFIRM_S - uptime : 0;
        break;
    case FW_TRIAL_RUN_VERIFYING: {
        uint32_t end = g_fw.window_start_s + CONFIG_FW_UPDATE_VERIFY_WINDOW_S;
        out->trial_left_s = uptime < end ? end - uptime : 0;
        break;
    }
    case FW_TRIAL_RUN_WATCHING:
        out->trial_left_s = elapsed < WATCH_S ? WATCH_S - elapsed : 0;
        break;
    case FW_TRIAL_RUN_NONE:
        break;
    }
    if (out->gnss_blocked == FW_GNSS_BLOCK_APP_TRIAL) {
        out->gnss_blocked_left_s = fw_trial_left_s(run, uptime, CONFIG_FW_UPDATE_CONFIRM_S, elapsed, WATCH_S);
    }
    if (g_fw.rec.stage == FW_TRIAL_REC_REVERTED || g_fw.rec.stage == FW_TRIAL_REC_ROLLED_BACK) {
        strlcpy(out->reverted_version, g_fw.rec.version, sizeof(out->reverted_version));
        out->reverted_in_stage1 =
            g_fw.rec.stage == FW_TRIAL_REC_ROLLED_BACK && !fw_bad_contains(&g_fw.bad, g_fw.rec.version);
    }
    out->rollback_available = g_fw.rollback_known && g_fw.rollback_available;
}

/* ---- manual rollback ---------------------------------------------------------------- */

esp_err_t fw_update_rollback(void)
{
    if (g_fw.mtx == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!esp_ota_check_rollback_is_possible()) {
        return ESP_ERR_NOT_FOUND;
    }
    fw_lock();
    bool busy = g_fw.busy || g_fw.state == FW_UPDATE_REBOOT_PENDING;
    if (!busy) {
        g_fw.busy = true;   /* nothing else starts meanwhile */
        g_fw.job = 0;
    }
    fw_unlock();
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_OK;
    /* The trial's own inhibit does not count against the user. */
    fw_lock();
    bool had_inhibit = g_fw.trial_inhibit_held;
    fw_unlock();
    inhibit_drop();
    if (lifecycle_reboot_inhibited(NULL)) {
        err = ESP_ERR_INVALID_STATE;
    } else if (lifecycle_update_claim(LIFECYCLE_UPDATE_APP) != ESP_OK) {
        err = ESP_ERR_INVALID_STATE;   /* a receiver flash session holds the update slot */
    } else {
        err = esp_ota_mark_app_invalid_rollback();
        if (err != ESP_OK) {
            lifecycle_update_release(LIFECYCLE_UPDATE_APP);
            if (err == ESP_ERR_OTA_ROLLBACK_FAILED) {
                err = ESP_ERR_NOT_FOUND;
            }
        }
    }
    if (err != ESP_OK) {
        if (had_inhibit) {
            inhibit_take();
        }
        fw_lock();
        g_fw.busy = false;
        fw_unlock();
        return err;
    }

    const char *version = esp_app_get_description()->version;
    fw_lock();
    fw_bad_add(&g_fw.bad, version);
    fw_bad_versions_t bad = g_fw.bad;
    memset(&g_fw.rec, 0, sizeof(g_fw.rec));
    strlcpy(g_fw.rec.version, version, sizeof(g_fw.rec.version));
    g_fw.rec.stage = FW_TRIAL_REC_REVERTED;
    fw_trial_rec_t rec = g_fw.rec;
    g_fw.run = FW_TRIAL_RUN_NONE;
    fw_unlock();
    fw_trial_save_bad(&bad);
    fw_trial_save_rec(&rec);
    journal(DIAG_EV_APP_REVERT, REVERT_BY_USER, 0);
    fw_set_state(FW_UPDATE_REBOOT_PENDING, ESP_OK);
    ESP_LOGW(TAG, "rollback of %s requested by the user: rebooting into the previous image", version);
    /* The update slot stays claimed until the reboot: no receiver flash starts. */
    (void)lifecycle_request_reboot(LIFECYCLE_REBOOT_APP_REVERT);
    return ESP_OK;
}
