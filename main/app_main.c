/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Firmware entry point: bring up the system services every component relies
 * on, then initialise and start the station components in dependency order.
 *
 * Failure policy:
 *  - Boot accounting runs first, before anything that could crash, so a crash
 *    in any later step counts toward safe mode. Right after the settings,
 *    lifecycle_init() checks that accounting's RTC state against the image
 *    generation (state from before another app image's run starts over),
 *    before the safe-mode decision or any other RTC block is read, and has
 *    diag's previous-boot log checked the same way.
 *  - NVS is never erased just because it is unreadable. Written by a newer
 *    app (rolled back): boot that app if the other slot still holds it and it
 *    is newer, otherwise run without persistence. Erased only when it is full or
 *    corrupt, after one retry. Without persistence the station never opens
 *    an unprotected setup AP on its own (see network.h).
 *  - The default event loop and esp_netif are mandatory: if they fail, the
 *    boot is reported as failed and retried after a delay (counts as a crash,
 *    so it ends in safe mode or an app rollback, never a tight loop).
 *  - Every station component is optional at boot: a failing one is logged
 *    and skipped so the device still comes up far enough (Wi-Fi + web UI) to
 *    be diagnosed and fixed remotely.
 *  - Safe mode skips the receiver path and the casters entirely (init and
 *    start): they are what safe mode protects against, and their init runs
 *    before network and web, so a crash there would otherwise keep the UI
 *    from ever coming up.
 *  - An open receiver-flash journal is read right after the settings, before
 *    anything that could transmit or start Wi-Fi (station_early_boot): an
 *    interrupted receiver update decides the first bytes on the receiver's
 *    UART. Not in safe mode, where nothing is sent to the receiver; there
 *    station_safe_mode_boot() only reads the journal and, with none open,
 *    releases a receiver package left staged.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "nvs_flash.h"

#include "boot_info.h"

#include "bench.h"
#include "board.h"
#include "diag.h"
#include "fw_update.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "health.h"
#include "ledger.h"
#include "lifecycle.h"
#include "local_caster.h"
#include "monitor.h"
#include "network.h"
#include "ntrip_server.h"
#include "pkg_store.h"
#include "rtcm_hub.h"
#include "settings.h"
#include "station.h"
#include "um98x_ctl.h"
#include "um98x_flash.h"
#include "web.h"

static const char *TAG = "app";

typedef struct {
    const char *name;
    esp_err_t (*fn)(void);
    bool skip_in_safe_mode;
} boot_step_t;

/*
 * Availability ledger and planned reboots. The ledger (L1) cannot see
 * lifecycle (L1), so the wiring is here: its shutdown hook is registered
 * before any other (hooks run in registration order), so the books are
 * stored, and a reboot the user asked for starts counting as planned, before
 * the casters close; and how lifecycle says the previous session ended (a
 * planned reboot, or a crash or reboot that was not) is reported to it
 * before the casters report their slots. Without a record of lifecycle's
 * (power-on, another image) the ledger's own books decide. Lifecycle's RTC
 * stamp check is handed over too: the ledger uses its RTC copy only when it
 * is from the run right before this boot.
 */
static void ledger_reboot_hook(lifecycle_reboot_reason_t reason, uint32_t deadline_ms, void *ctx)
{
    (void)deadline_ms;
    (void)ctx;
    ledger_shutdown(lifecycle_reboot_user_planned(reason));
}

static esp_err_t ledger_step(void)
{
    esp_err_t err = ledger_init(lifecycle_rtc_stamp_ok, lifecycle_rtc_stamp());
    if (err != ESP_OK) {
        return err;
    }
    lifecycle_boot_info_t boot;
    lifecycle_get_boot_info(&boot);
    switch (lifecycle_prev_session_end(boot.reset_class, boot.prev_reboot_reason)) {
    case LIFECYCLE_PREV_PLANNED:
        ledger_note_prev_reboot(true);
        break;
    case LIFECYCLE_PREV_UNPLANNED:
        ledger_note_prev_reboot(false);
        break;
    case LIFECYCLE_PREV_UNKNOWN:
    default:
        break;
    }
    return lifecycle_register_shutdown_hook(ledger_reboot_hook, NULL);
}

/* The board, and in the journal anything off about which one it is (board is below diag). */
static esp_err_t board_step(void)
{
    esp_err_t err = board_init();
    int32_t a = 0;
    int32_t b = 0;
    if (board_journal_note(&a, &b)) {
        (void)diag_journal_append(DIAG_EV_BOARD, a, b);
    }
    return err;
}

/* Initialisation: allocate, load settings, register handlers. No tasks yet
 * (except the lifecycle service, started by lifecycle_init() right after the
 * settings). fw_update goes first so a failing app trial is reverted before
 * the steps that might be what is failing. The receiver path and the casters
 * are skipped in safe mode; their status getters work uninitialised. */
static const boot_step_t s_init_steps[] = {
    { "fw_update",    fw_update_init,    false }, /* app trial bookkeeping, revert */
    { "board",        board_step,        false }, /* which board, its pins */
    { "ledger",       ledger_step,       false }, /* first shutdown hook; planned-reboot accounting */
    { "pkg_store",    pkg_store_init,    false },
    { "rtcm_hub",     rtcm_hub_init,     true  },
    { "gnss_obs",     gnss_obs_init,     true  },
    { "gnss_link",    gnss_link_init,    true  },
    { "um98x_ctl",    um98x_ctl_init,    true  },
    { "um98x_flash",  um98x_flash_init,  true  },
    { "station",      station_init,      true  }, /* wires GNSS link sinks to hub + ctl */
    { "ntrip_server", ntrip_server_init, true  },
    { "local_caster", local_caster_init, true  },
    { "network",      network_init,      false },
    { "health",       health_init,       false },
    { "web",          web_init,          false },
};

/* Start: spawn tasks. Network and web first so the UI is reachable early;
 * health last so it starts watching a fully started system (only the
 * monitor's sampling for the Status page comes after it). The update
 * check and user-initiated app installs keep running in safe mode so a fix
 * can arrive; receiver flashing stays off there because the station does
 * not run, and that includes finishing an interrupted receiver flash: it
 * waits for the next normal boot (the safe-mode retry, or clearing safe mode
 * from the UI), since the crash being escaped may be in the flash path
 * itself. */
static const boot_step_t s_start_steps[] = {
    { "network",      network_start,      false },
    { "web",          web_start,          false },
    { "station",      station_start,      true  }, /* starts the GNSS RX task: stream-first bring-up */
    { "ntrip_server", ntrip_server_start, true  },
    { "local_caster", local_caster_start, true  },
    { "fw_update",    fw_update_start,    false },
    { "health",       health_start,       false },
    { "monitor",      monitor_start,      true  }, /* the Status page's ping, signal, CPU and memory figures */
};

/*
 * Could the other slot's image be the app that wrote this NVS? It needs a
 * different IDF version (same NVS code would fail the same way) and, when both
 * are releases, a higher version. A dev build on either side has no
 * comparable version, so the IDF difference alone decides.
 */
static bool other_is_newer(const esp_app_desc_t *other)
{
    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(other->idf_ver, running->idf_ver, sizeof(other->idf_ver)) == 0) {
        return false;
    }
    int cmp = 0;
    if (fw_update_version_compare(other->version, running->version, &cmp) == ESP_OK) {
        return cmp > 0;
    }
    return true;
}

/*
 * NVS written by a newer app (the running one was rolled back or reverted).
 * If the other OTA slot still holds a confirmed (VALID) image that is newer,
 * boot it once per power cycle. A reverted image is marked invalid by
 * fw_update, so this never re-enters an image that was rejected.
 *
 * The switch marks this image invalid instead of calling
 * esp_ota_set_boot_partition(): with rollback enabled, that would write the
 * target's entry as a new image, i.e. a first-boot trial that the next reset
 * of any kind rolls back, leaving both slots unusable. Marking the running
 * image invalid makes the bootloader take the other otadata entry with its
 * VALID state untouched (esp_ota_get_state_partition() above found that
 * entry). This older image, which cannot read the settings anyway, is then
 * not booted again unless it is installed again.
 */
static void try_boot_other_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *other = esp_ota_get_next_update_partition(running);
    esp_ota_img_states_t state;
    esp_app_desc_t desc;
    if (other == NULL || esp_ota_get_state_partition(other, &state) != ESP_OK || state != ESP_OTA_IMG_VALID ||
        esp_ota_get_partition_description(other, &desc) != ESP_OK) {
        return;
    }
    if (!other_is_newer(&desc)) {
        ESP_LOGW(TAG, "other slot holds %s (IDF %s), not a newer app: staying here", desc.version, desc.idf_ver);
        return;
    }
    if (!lifecycle_claim_once(LIFECYCLE_ONCE_NVS_SLOT_SWITCH)) {
        return;
    }
    esp_err_t err = esp_ota_mark_app_invalid_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "cannot switch to %s: %s", other->label, esp_err_to_name(err));
        return;
    }
    ESP_LOGW(TAG, "settings belong to a newer app: booting %s from %s", desc.version, other->label);
    (void)lifecycle_reboot(LIFECYCLE_REBOOT_NVS_SLOT_SWITCH, true);   /* does not return */
}

/* @return true if NVS is usable. */
static bool init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
        ESP_LOGW(TAG, "NVS reports no free pages, retrying once");
        err = nvs_flash_init();
        if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
            ESP_LOGE(TAG, "NVS full or corrupt: erasing, all settings reset to defaults");
            err = nvs_flash_erase();
            if (err == ESP_OK) {
                err = nvs_flash_init();
            }
        }
    } else if (err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG, "NVS was written by a newer app; it is kept untouched");
        try_boot_other_slot();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS unusable (%s): running without saved settings, nothing will be stored",
                 esp_err_to_name(err));
        return false;
    }
    return true;
}

static int run_steps(const char *phase, const boot_step_t *steps, size_t count, bool safe_mode)
{
    int failures = 0;
    for (size_t i = 0; i < count; i++) {
        if (safe_mode && steps[i].skip_in_safe_mode) {
            ESP_LOGW(TAG, "%s %s: skipped (safe mode)", phase, steps[i].name);
            continue;
        }
        esp_err_t err = steps[i].fn();
        if (err != ESP_OK) {
            failures++;
            ESP_LOGE(TAG, "%s %s failed: %s", phase, steps[i].name, esp_err_to_name(err));
        }
    }
    return failures;
}

void app_main(void)
{
    lifecycle_early_boot();

    /* Log tap next, so everything after it reaches the web log view. */
    esp_err_t err = diag_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "init diag failed: %s", esp_err_to_name(err));
    }

    boot_info_log();

    err = esp_event_loop_create_default();
    if (err != ESP_OK) {
        lifecycle_fatal_boot_error("default event loop", err);
    }

    bool nvs_ok = init_nvs();
    if (nvs_ok) {
        err = settings_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "settings unusable (%s): running on defaults", esp_err_to_name(err));
        }
    }
    /* The receiver wiring saved on the station, before any step checks the pins (board_init). */
    board_wiring_t wiring;
    if (station_wiring_saved(&wiring)) {
        board_use_wiring(&wiring);
    }

    /* Before anything reads the boot accounting, the safe-mode decision or an
     * RTC block: RTC state from before another app image's run is set aside
     * here (checked without settings too), then the service task starts and
     * the boot is recorded. */
    int failures = 0;
    err = lifecycle_init();
    if (err != ESP_OK) {
        failures++;
        ESP_LOGE(TAG, "init lifecycle failed: %s", esp_err_to_name(err));
    }

    bool safe_mode = lifecycle_in_safe_mode();
    if (safe_mode) {
        ESP_LOGW(TAG, "SAFE MODE: repeated crashes; receiver path and casters stay off, update checks and app "
                      "installs stay on");
    } else {
        /* Bench builds only, and only when armed for this one boot: the
         * receiver's stop key from the earliest point of the boot (refused
         * while a receiver update journal is open). */
        err = bench_boot_early();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "bench stop key from boot: %s", esp_err_to_name(err));
        }
        /* Before Wi-Fi and every other init step: an interrupted receiver
         * update owns the receiver's UART first. */
        err = station_early_boot();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "receiver flash recovery at boot: %s", esp_err_to_name(err));
        }
    }

    err = esp_netif_init();
    if (err != ESP_OK) {
        lifecycle_fatal_boot_error("esp_netif", err);
    }

    failures += run_steps("init", s_init_steps, sizeof(s_init_steps) / sizeof(s_init_steps[0]), safe_mode);
    if (safe_mode) {
        /* The station does not run, but a receiver package left staged
         * without an open journal must not wait out the whole episode. */
        err = station_safe_mode_boot();
        if (err != ESP_OK) {
            failures++;
            ESP_LOGE(TAG, "init station (safe mode) failed: %s", esp_err_to_name(err));
        }
    }
    failures += run_steps("start", s_start_steps, sizeof(s_start_steps) / sizeof(s_start_steps[0]), safe_mode);

    if (failures == 0) {
        ESP_LOGI(TAG, "startup complete");
    } else {
        ESP_LOGW(TAG, "startup complete with %d failed step(s)", failures);
    }

    /* Bench builds only: the bench console's command line. */
    err = bench_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bench console: %s", esp_err_to_name(err));
    }
}
