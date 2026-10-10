/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "station.h"

#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "lifecycle.h"
#include "pkg_store.h"
#include "rtcm_hub.h"
#include "sdkconfig.h"
#include "settings.h"
#include "um98x_ctl.h"
#include "um98x_flash.h"

static const char *TAG = "station";

/* Boot-time race into the receiver's loader. PROVISIONAL, to be confirmed on hardware. */
#define RACE_MAX_MS 10000u

ESP_EVENT_DEFINE_BASE(STATION_EVENT);

static station_state_t s_state = STATION_STATE_IDLE;
static um98x_base_profile_t s_profile;
static bool s_inited;

/* Journal as read at boot (early step or init). */
static esp_err_t s_journal_err = ESP_ERR_NOT_FOUND;
static um98x_flash_journal_t s_journal;
static bool s_journal_read;

/* Reboot inhibit and update slot held for an open flash session in full
 * lockout (taken again at boot). */
static bool s_inhibit_held;
static lifecycle_inhibit_t s_inhibit;
static bool s_update_claimed;

/* station_alert_t flags; CONFIG_FAILED drives the apply backoff. */
static uint32_t s_alerts;

/* Loader prompt seen by the armed detector (RX task sets, station task reads). */
static volatile bool s_prompt_seen;

/* RAW lease taken by the early boot step for station_start(). */
static gnss_link_lease_t s_boot_lease;

/* RX task only. */
static uint64_t s_rtcm_excluded;

static bool journal_open(esp_err_t err)
{
    /* Only "not found" means no journal. Anything else (damaged, a newer
     * schema, degraded settings) may hide an interrupted flash. */
    return err != ESP_ERR_NOT_FOUND;
}

static bool journal_waits_for_user(esp_err_t err, const um98x_flash_journal_t *j)
{
    return err != ESP_OK || j->state == UM98X_JOURNAL_NEEDS_USER;
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

/* Automatic re-applies are refused while the user configures the receiver
 * (autocfg off) or while the apply backoff after a failure owns them. */
static bool reconfigure_refused(void)
{
    return !autocfg_enabled() || (s_alerts & STATION_ALERT_CONFIG_FAILED) != 0;
}

static bool rtcm1230_supported(void)
{
    uint8_t v = 0;
    return settings_get_u8(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_RTCM1230_OK, &v) == ESP_OK && v != 0;
}

static void read_journal(void)
{
    s_journal_err = um98x_flash_journal_get(&s_journal);
    s_journal_read = true;
    if (s_journal_err == ESP_OK) {
        ESP_LOGW(TAG, "receiver flash journal open (state %u, phase %u, %u transfer(s)): recovery runs first",
                 (unsigned)s_journal.state, (unsigned)s_journal.phase, (unsigned)s_journal.transfers);
    } else if (journal_unknown(s_journal_err)) {
        ESP_LOGW(TAG, "settings unavailable: receiver flash journal state unknown, nothing is sent to the receiver");
    } else if (journal_open(s_journal_err)) {
        ESP_LOGW(TAG, "receiver flash journal unusable (%s): waiting for the user, nothing is sent to the receiver",
                 esp_err_to_name(s_journal_err));
    }
}

/* RTCM types the hub never carries: 1101-1107 are SBAS MSM numbers the
 * receiver may reuse for its own messages. */
static bool rtcm_type_excluded(uint16_t msg_type)
{
    return msg_type >= 1101 && msg_type <= 1107;
}

static void on_rtcm_frame(const uint8_t *frame, size_t len, uint16_t msg_type, void *ctx)
{
    (void)ctx;
    /* Runs in the GNSS RX task: never block. If the hub is not ready the frame
     * is lost; the hub counts the failed publish and the link counters still
     * show it as received, so health sees the mismatch. */
    if (rtcm_type_excluded(msg_type)) {
        s_rtcm_excluded++;
    } else {
        (void)rtcm_hub_publish(frame, len, msg_type);
    }
    gnss_obs_feed_rtcm(frame, len, msg_type);
}

static void on_text_line(const char *line, size_t len, void *ctx)
{
    um98x_ctl_on_line(line, len, ctx);
    gnss_obs_feed_line(line, len);
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

esp_err_t station_early_boot(void)
{
    read_journal();
    if (!journal_open(s_journal_err)) {
        return ESP_OK;
    }

    /* Full lockout for a session that is not waiting for the user: no
     * reboot may cut the recovery run short and no app update may start. */
    if (!journal_waits_for_user(s_journal_err, &s_journal)) {
        lockout_take();
    }

    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    bool receiver_was_reset =
        boot.reset_class == LIFECYCLE_RESET_POWER_ON || boot.reset_class == LIFECYCLE_RESET_BROWNOUT;
    if (s_journal_err != ESP_OK || !um98x_flash_journal_needs_race(&s_journal, receiver_was_reset)) {
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
    load_profile();

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
    s_inited = true;
    ESP_LOGI(TAG, "stub: sinks wired, profile hash %08x, station ID %u", (unsigned)um98x_profile_hash(&s_profile),
             (unsigned)s_profile.station_id);
    return ESP_OK;
}

esp_err_t station_start(void)
{
    /* TODO: supervisor task + request queue; the rest of the recovery table
     * (with the early step's RAW lease) when the journal is open, otherwise
     * stream-first bring-up. */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_get_status(station_status_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    out->state = s_state;
    out->since_lease_release_ms = UINT32_MAX;
    out->rtcm_excluded = s_rtcm_excluded;
    out->alerts = s_alerts;
    out->config_failed = (s_alerts & STATION_ALERT_CONFIG_FAILED) != 0;
    if (s_inited) {
        out->position_locked = (s_profile.base_mode == UM98X_BASE_FIXED);
        out->rtcm1230_supported = rtcm1230_supported();
        out->reconfigure_refused = reconfigure_refused();
    }

    /* Safe mode: the station never ran, read the journal from settings
     * (no UART) so the UI can show it and offer "clear the journal". */
    esp_err_t jerr = s_journal_err;
    um98x_flash_journal_t journal = s_journal;
    if (!s_journal_read) {
        jerr = um98x_flash_journal_get(&journal);
    }
    out->flash_journal_open = journal_open(jerr);
    out->flash_journal_readable = (jerr == ESP_OK);
    out->flash_journal_settings_unavailable = journal_unknown(jerr);
    if (jerr == ESP_OK) {
        out->flash_journal = journal;
    }
    return ESP_OK;
}

esp_err_t station_request_reconfigure(void)
{
    return s_inited ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
}

esp_err_t station_request_flash(const station_flash_request_t *req)
{
    if (req == NULL || req->size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_inited || journal_open(s_journal_err) || lifecycle_app_trial_active() || app_update_running() ||
        s_state != STATION_STATE_STREAMING) {
        return ESP_ERR_INVALID_STATE;
    }
    /* TODO: valid RTCM or NMEA within the last 10 s, model gate, prepare
     * (rules on the RAM copy; release the package on failure), claim the
     * update slot (lockout_take: refused if an app update got there first),
     * pin, journal, RAW lease, run. */
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
        s_journal_err = um98x_flash_journal_get(&s_journal);
        s_journal_read = true;
    }
    if (!journal_open(s_journal_err)) {
        if (action == STATION_FLASH_ACTION_RESTART_RECEIVER && (s_alerts & STATION_ALERT_RECEIVER_IN_LOADER) != 0) {
            /* Receiver found at its loader without a session: one more
             * restart key at the user's request, then detection. */
            return s_inited ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_FOUND;
    }
    bool waiting = journal_waits_for_user(s_journal_err, &s_journal);

    switch (action) {
    case STATION_FLASH_ACTION_CLEAR_JOURNAL: {
        /* Not while a session or its recovery runs; always in safe mode,
         * where nothing runs. Not with settings unavailable: there is
         * nothing to clear and nothing could be written. */
        if ((s_inited && !waiting) || journal_unknown(s_journal_err)) {
            return ESP_ERR_INVALID_STATE;
        }
        esp_err_t err = um98x_flash_journal_close();
        if (err != ESP_OK) {
            return err;
        }
        ESP_LOGW(TAG, "receiver flash journal cleared by the user");
        s_journal_err = ESP_ERR_NOT_FOUND;
        memset(&s_journal, 0, sizeof(s_journal));
        esp_err_t rel = pkg_store_release();
        if (rel != ESP_OK) {
            ESP_LOGW(TAG, "staged receiver package not released: %s", esp_err_to_name(rel));
        }
        lockout_release();
        return ESP_OK;
    }
    case STATION_FLASH_ACTION_RETRY:
        if (!s_inited || !waiting || s_journal_err != ESP_OK || lifecycle_app_trial_active() ||
            app_update_running()) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_SUPPORTED;
    case STATION_FLASH_ACTION_RESTART_RECEIVER:
        if (!s_inited || !waiting) {
            return ESP_ERR_INVALID_STATE;
        }
        return ESP_ERR_NOT_SUPPORTED;
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
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (action == STATION_RECOVER_RECONFIGURE && reconfigure_refused()) {
        /* Sends nothing: the user configures the receiver, or the apply
         * backoff after a failure owns the re-applies. */
        return ESP_ERR_INVALID_STATE;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t station_lock_position(void)
{
    return s_inited ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
}

esp_err_t station_request_resurvey(void)
{
    return s_inited ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_STATE;
}
