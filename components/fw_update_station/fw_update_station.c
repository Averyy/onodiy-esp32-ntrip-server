/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The station as fw_update's product (fw_update_set_product).
 */
#include "fw_update_station.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "fw_station_map.h"
#include "fw_update.h"
#include "network.h"
#include "ntrip_server.h"
#include "rtcm_hub.h"
#include "station.h"
#include "um98x_flash.h"

static const char *TAG = "fw_update";

static void health(fw_update_health_t *out, void *ctx)
{
    (void)ctx;
    out->network_up = network_is_online();
    rtcm_hub_stats_t hub;
    rtcm_hub_get_stats(&hub);
    out->data_fed = fw_station_hub_fed(hub.last_frame_age_ms);

    ntrip_caster_status_t *cst = malloc(sizeof(*cst));
    if (cst != NULL) {
        size_t n = ntrip_server_slot_count();
        for (size_t i = 0; i < n && i < 8; i++) {
            if (ntrip_server_get_status(i, cst) == ESP_OK && cst->state == NTRIP_STATE_STREAMING) {
                out->outputs |= (uint8_t)(1u << i);
            }
        }
        free(cst);
    }
    station_status_t *st = malloc(sizeof(*st));
    if (st != NULL) {
        out->receiver_problem = station_get_status(st) == ESP_OK && fw_station_receiver_problem(st->state);
        free(st);
    }
}

static esp_err_t receiver(fw_update_receiver_t *out, void *ctx)
{
    (void)ctx;
    station_status_t *st = malloc(sizeof(*st));
    if (st == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = station_get_status(st);
    if (err == ESP_OK) {
        out->valid = st->receiver.valid;
        strlcpy(out->model, st->receiver.model, sizeof(out->model));
        strlcpy(out->firmware, st->receiver.firmware, sizeof(out->firmware));
        out->build = st->receiver.build;
        out->ready = fw_station_receiver_ready(st->state);
        out->journal_open = st->flash_journal_open;
    }
    free(st);
    return err;
}

static bool model_supported(const char *model, void *ctx)
{
    (void)ctx;
    return um98x_flash_model_supported(model);
}

static uint32_t downtime_s(uint32_t size, void *ctx)
{
    (void)ctx;
    return station_expected_flash_downtime_s(size);
}

static bool waits_for_package(um98x_flash_journal_t *j, void *ctx)
{
    (void)ctx;
    return station_flash_waits_for_package(j);
}

static void request_of(const fw_update_receiver_job_t *job, station_flash_request_t *req)
{
    memset(req, 0, sizeof(*req));
    memcpy(req->sha256, job->sha256, sizeof(req->sha256));
    req->size = job->size;
    strlcpy(req->expected_fw, job->expected_fw, sizeof(req->expected_fw));
    req->expected_build = job->expected_build;
    req->cross_layout_confirmed = job->cross_layout_confirmed;
}

static esp_err_t start(const fw_update_receiver_job_t *job, void *ctx)
{
    (void)ctx;
    if (station_sg_test_running()) {
        /* Started while the package downloaded: the comparison owns the
         * receiver until it ends (the web refuses an update during one). */
        ESP_LOGE(TAG, "a signal-group comparison is running: receiver update not started (start it again "
                      "when the comparison has ended)");
    }
    station_flash_request_t req;
    request_of(job, &req);
    return station_request_flash(&req);
}

static esp_err_t check(const fw_update_receiver_job_t *job, void *ctx)
{
    (void)ctx;
    station_flash_request_t req;
    request_of(job, &req);
    return station_request_package_check(&req);
}

static esp_err_t retry_restaged(void *ctx)
{
    (void)ctx;
    return station_flash_retry_restaged();
}

esp_err_t fw_update_station_init(void)
{
    const fw_update_product_t product = {
        .health = health,
        .receiver = receiver,
        .model_supported = model_supported,
        .receiver_downtime_s = downtime_s,
        .receiver_waits_for_package = waits_for_package,
        .receiver_start = start,
        .receiver_check = check,
        .receiver_retry_restaged = retry_restaged,
    };
    esp_err_t err = fw_update_set_product(&product);
    return err == ESP_OK ? fw_update_init() : err;
}
