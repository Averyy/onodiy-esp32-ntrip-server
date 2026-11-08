/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The receiver components in another application (docs/component-reuse.md):
 * the order of initialisation, the link's sinks, receiver transactions on
 * the application's own task under a COMMAND lease, and corrections written
 * lease-free while the link streams. CI builds it (tools/ci/external_app.sh)
 * so that this contract keeps compiling; this repository does not run it on
 * hardware.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_event.h"
#include "esp_log.h"
#include "example_receiver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gnss_link.h"
#include "gnss_obs.h"
#include "leds.h"
#include "nvs_flash.h"
#include "settings.h"
#include "settings_schema.h"
#include "um98x_ctl.h"
#if EXAMPLE_UPDATER
#include "pkg_store.h"
#include "um98x_flash.h"
#endif
#if EXAMPLE_APP_UPDATE
#include "fw_update.h"
#include "lifecycle.h"
#endif

static const char *TAG = "example";

#define LEASE_WAIT_MS    2000u
#define CORRECTION_TX_MS 100u
#define SUPERVISOR_STACK 6144
#define SUPERVISOR_PRIO  4

/* Set once the task runs; read by the restart callback on the RX task. */
static TaskHandle_t s_supervisor;

/* ---- the link's sinks: on the RX task; never block, never take a lease -- */

static void on_rtcm(const uint8_t *frame, size_t len, uint16_t msg_type, int64_t arrival_us, bool arrival_exact,
                    void *ctx)
{
    (void)arrival_us;
    (void)arrival_exact;
    (void)ctx;
    gnss_obs_feed_rtcm(frame, len, msg_type);
}

static void on_line(const char *line, size_t len, void *ctx)
{
    /* The command layer's replies first; what it does not claim is receiver output. */
    if (!um98x_ctl_on_line(line, len, ctx)) {
        gnss_obs_feed_line(line, len);
    }
}

static void on_reset(void *ctx)
{
    (void)ctx;
    /* The framer was reset (baud change, lease release, overflow): what came
     * before is discontinuous. A product drops its partial input here. */
}

static void on_prompt(void *ctx)
{
    (void)ctx;
    um98x_ctl_notify_prompt();   /* the call in flight stops at once */
}

/* um98x_ctl's restart callback: on the RX task too, once per restart counted. */
static void on_restart(void *ctx)
{
    (void)ctx;
    TaskHandle_t t = __atomic_load_n(&s_supervisor, __ATOMIC_ACQUIRE);
    if (t != NULL) {
        xTaskNotifyGive(t);
    }
}

/* ---- receiver transactions: the application's own task ----------------- */

/* Identity and the mode read-back under one COMMAND lease, with the loader
 * prompt detector armed: once the loader shows, nothing more is sent. */
static void identify(bool after_restart)
{
    gnss_link_lease_t lease = NULL;
    esp_err_t err = gnss_link_acquire(GNSS_LINK_MODE_COMMAND, "example", LEASE_WAIT_MS, &lease);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no COMMAND lease: %s", esp_err_to_name(err));
        return;
    }
    um98x_info_t info = { 0 };
    bool known = false;
    err = gnss_link_set_prompt_detect(lease, true);
    if (err == ESP_OK && !after_restart) {
        um98x_detect_result_t found;
        err = um98x_ctl_detect(lease, false, &found);   /* found: gnss/baud persisted */
        if (err == ESP_OK && found.identity) {
            info = found.info;
            known = true;
        }
    }
    if (err == ESP_OK && !known) {
        /* After a restart: polled until 20 s after its evidence, no detection pass. */
        err = um98x_ctl_query_version(lease, after_restart, &info);   /* gnss/model, gnss/fw persisted */
        known = (err == ESP_OK);
    }
    char mode[128] = "";
    esp_err_t mode_err = known ? um98x_ctl_query_mode(lease, mode, sizeof(mode)) : ESP_ERR_INVALID_STATE;
    bool loader = gnss_link_prompt_seen(lease);
    /* Releasing resets the framer; on_reset has run when this returns. */
    ESP_ERROR_CHECK(gnss_link_release(lease));

    if (loader) {
        ESP_LOGW(TAG, "receiver at its loader prompt: nothing more was sent");
    } else if (known) {
        ESP_LOGI(TAG, "receiver %s %s (configurable here: %d), mode '%s'", info.model, info.firmware,
                 um98x_ctl_model_supported(&info), mode_err == ESP_OK ? mode : "?");
    } else {
        ESP_LOGW(TAG, "receiver not identified: %s", esp_err_to_name(err));
    }
}

#if EXAMPLE_UPDATER
/* Before any ordinary write: an open receiver update decides what may be
 * sent. Only ESP_ERR_NOT_FOUND means none is open; an unreadable journal,
 * one of a newer schema or settings unavailable count as open: send nothing
 * and leave the journal as it is (the user acts). */
static bool update_open(void)
{
    um98x_flash_journal_t j;
    esp_err_t err = um98x_flash_journal_get(&j);
    if (err == ESP_ERR_NOT_FOUND) {
        return false;
    }
    const char *plan = NULL;
    (void)um98x_flash_journal_plan(err, &j, false, &plan);
    ESP_LOGW(TAG, "receiver update open (%s, %s): nothing ordinary is sent", esp_err_to_name(err),
             plan != NULL ? plan : "?");
    return true;
}

esp_err_t example_update_receiver(const um98x_flash_expect_t *confirmed, um98x_flash_result_t *res)
{
    gnss_link_lease_t lease = NULL;
    esp_err_t err = gnss_link_acquire(GNSS_LINK_MODE_RAW, "update", LEASE_WAIT_MS, &lease);
    if (err != ESP_OK) {
        return err;
    }
    uint32_t stored_baud = 0;
    (void)settings_get_u32(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_BAUD, &stored_baud);
    um98x_flash_ident_t id;
    err = um98x_flash_identify(lease, stored_baud, false, &id);
    if (err == ESP_OK && id.result != UM98X_FLASH_ID_OK) {
        err = ESP_ERR_INVALID_STATE;   /* no answer, or at its loader: no session */
    }
    um98x_flash_expect_t e = *confirmed;
    um98x_flash_pkg_facts_t facts;
    if (err == ESP_OK) {
        e.receiver = id.info;   /* the live identity: a wrong model is refused here */
        err = um98x_flash_prepare(&e, &facts);
    }
    if (err == ESP_OK) {
        err = pkg_store_set_pinned(true);   /* before the journal opens */
    }
    if (err == ESP_OK) {
        um98x_flash_request_t req = { 0 };
        memcpy(req.journal.sha256, facts.sha256, sizeof(req.journal.sha256));
        req.journal.size = e.size;
        snprintf(req.journal.expected_fw, sizeof(req.journal.expected_fw), "%s", e.expected_fw);
        req.journal.expected_build = e.expected_build;
        snprintf(req.journal.pre_model, sizeof(req.journal.pre_model), "%s", id.info.model);
        snprintf(req.journal.pre_fw, sizeof(req.journal.pre_fw), "%s", id.info.firmware);
        req.journal.pre_build = id.info.build;
        req.journal.app_baud = id.app_baud;
        err = um98x_flash_run(lease, &req, res);
        if (err != ESP_OK) {
            (void)pkg_store_set_pinned(false);   /* refused before the journal opened */
        }
    }
    um98x_flash_discard();
    ESP_ERROR_CHECK(gnss_link_release(lease));
    /* DONE / ABORTED: res->receiver is what runs now; the product checks its
     * profile, then closes the journal and releases the package
     * (um98x_flash_journal_close, pkg_store_release). NEEDS_USER: the journal
     * and the pinned package wait for the user. */
    return err;
}
#endif

static void supervisor(void *arg)
{
    (void)arg;
#if EXAMPLE_UPDATER
    if (update_open()) {
        vTaskDelete(NULL);   /* the product's maintenance flow takes over; nothing ordinary goes out */
    }
#endif
    identify(false);
    for (;;) {
        if (ulTaskNotifyTake(pdTRUE, portMAX_DELAY) != 0) {
            identify(true);   /* a restart was reported: wait it out */
        }
    }
}

esp_err_t example_send_correction(const uint8_t *frame, size_t len, gnss_link_tx_t *tx)
{
    /* One complete frame per call; no retry: an uncertain write may already
     * have reached the receiver. Refused (nothing sent) under a lease. */
    return gnss_link_write_ex(NULL, frame, len, CORRECTION_TX_MS, tx);
}

#if EXAMPLE_APP_UPDATE
/* The product's side of the app updater: an offline device with no casters.
 * Left false, nothing here is asked of a new image: it must start (confirmed
 * only after fw_update_start and its confirm time) and not crash. A product
 * that has a network or a data path reports them, and the new image must
 * reach again what was true when the update was installed. */
static void product_health(fw_update_health_t *out, void *ctx)
{
    (void)ctx;
    out->network_up = false;
    out->data_fed = false;
    out->outputs = 0;
    out->receiver_problem = false;
}
#endif

/* ---- start -------------------------------------------------------------- */

static esp_err_t receiver_start(void)
{
    /* A wiring saved on the device (gnss/wiring) replaces the board's, given before board_init(). */
    uint8_t blob[64];
    size_t len = sizeof(blob);
    esp_err_t err = settings_get_blob(SETTINGS_NS_GNSS, SETTINGS_KEY_GNSS_WIRING, blob, &len);
    if (err == ESP_OK) {
        board_wiring_t w;
        if (!board_wiring_decode(blob, len, &w)) {
            ESP_LOGE(TAG, "saved wiring does not decode: receiver link stays closed");
            return ESP_ERR_INVALID_STATE;
        }
        board_use_wiring(&w);
    } else if (err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "saved wiring unreadable (%s): receiver link stays closed", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    err = board_init();
    const char *off = board_link_off_reason();
    if (err != ESP_OK || off != NULL) {
        ESP_LOGE(TAG, "receiver link stays closed: %s", off != NULL ? off : esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }
    const board_desc_t *b = board_get();

    /* The board's LEDs with the product's own pattern (a no-op without LEDs):
     * a slow white blink, the GNSS LED following the receiver as on the station. */
    ESP_ERROR_CHECK(leds_init());
    static const leds_pattern_t k_idle = {
        .rgb = { { 40, 40, 40, 100 }, { 0, 0, 0, 1900 } },
        .rgb_steps = 2,
        .gnss_mode = LEDS_GNSS_RECEIVER,
    };
    ESP_ERROR_CHECK(leds_set_pattern(&k_idle));

    ESP_ERROR_CHECK(gnss_obs_init());
    ESP_ERROR_CHECK(um98x_ctl_init());
#if EXAMPLE_UPDATER
    ESP_ERROR_CHECK(pkg_store_init());   /* no package partition (8 MB): staged in PSRAM if built so, else none */
    ESP_ERROR_CHECK(um98x_flash_init());
    um98x_flash_journal_t j;
    if (um98x_flash_journal_get(&j) == ESP_ERR_NOT_FOUND) {
        (void)pkg_store_release();   /* no open update: a package left over goes, pinned or not */
    }
#endif
    um98x_ctl_set_restart_cb(on_restart, NULL);
    um98x_ctl_set_port((uint8_t)b->gnss_port);
    err = gnss_link_init();
    if (err != ESP_OK) {
        /* Initialised closed: once started, the link reopens the UART on its own. */
        ESP_LOGW(TAG, "receiver UART not open yet: %s", esp_err_to_name(err));
    }
    const gnss_link_sinks_t sinks = {
        .on_rtcm = on_rtcm,
        .on_line = on_line,
        .on_reset = on_reset,
        .on_prompt = on_prompt,
    };
    ESP_ERROR_CHECK(gnss_link_set_sinks(&sinks));
    ESP_ERROR_CHECK(gnss_link_start());
    ESP_LOGI(TAG, "%s: UART%d RX %d TX %d, receiver COM%d", b->id, b->gnss_uart_num, b->gnss_rx_gpio,
             b->gnss_tx_gpio, b->gnss_port);
    return ESP_OK;
}

void app_main(void)
{
#if EXAMPLE_APP_UPDATE
    lifecycle_early_boot();   /* first: the reset class and reboot gate that installs and the crash watch need */
#endif
    /* NVS holds the station's shared device keys: never erased here. */
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    if (settings_init() != ESP_OK) {
        ESP_LOGW(TAG, "settings unavailable: the components run on their defaults, nothing is stored");
    }
#if EXAMPLE_APP_UPDATE
    /* First, so a failing trial is reverted before the steps that may be what fails. */
    const fw_update_product_t product = { .health = product_health };   /* no receiver updates through it */
    ESP_ERROR_CHECK(lifecycle_init());
    ESP_ERROR_CHECK(fw_update_set_product(&product));
    ESP_ERROR_CHECK(fw_update_init());
#endif
    if (receiver_start() != ESP_OK) {
        return;
    }
    TaskHandle_t t = NULL;
    if (xTaskCreate(supervisor, "receiver", SUPERVISOR_STACK, NULL, SUPERVISOR_PRIO, &t) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the receiver task");
        return;
    }
    __atomic_store_n(&s_supervisor, t, __ATOMIC_RELEASE);
#if EXAMPLE_APP_UPDATE
    ESP_ERROR_CHECK(fw_update_start());   /* the start is complete: the trial may confirm this image */
#endif
}
