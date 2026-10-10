/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Event journal around the pure core (diag_journal_core.c): the raw
 * "journal" partition, the rate limits, a RAM queue and the writer task.
 *
 * diag_journal_append() never touches flash: it checks the rate limits,
 * stamps the record (uptime, UTC if known) and queues it under a spinlock.
 * The writer task ("diag_jw", internal-RAM stack because it writes flash)
 * moves queued records to flash, one 32-byte write each, and erases the
 * next sector when the ring comes round to it. Appends made before
 * diag_init() wait in the queue until the partition is mounted. Before a
 * planned restart a shutdown handler writes out what is still queued, as far
 * as the sector being written has room (the restart never waits for an
 * erase).
 */
#include <string.h>

#include "diag.h"
#include "diag_internal.h"
#include "diag_journal_core.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "diag";

#define PART_TYPE     ((esp_partition_type_t)0x40)
#define PART_SUBTYPE  ((esp_partition_subtype_t)0x01)
#define PART_LABEL    "journal"
#define QUEUE_LEN     64u
#define WRITER_STACK  4096
#define WRITER_PRIO   5
#define FLUSH_WAIT_MS 100
#define READ_CHUNK    16u

typedef struct {
    uint32_t uptime_s;
    int64_t unix_s;
    uint16_t kind;
    int32_t a;
    int32_t b;
} qrec_t;

typedef enum {
    J_PENDING = 0,      /* before diag_init: appends wait in the queue */
    J_READY,
    J_UNAVAILABLE,
} jstate_t;

/* Queue and rate limits: s_qlock. */
static portMUX_TYPE s_qlock = portMUX_INITIALIZER_UNLOCKED;
static qrec_t s_queue[QUEUE_LEN];
static uint32_t s_qhead;
static uint32_t s_qcount;
static uint32_t s_queue_full;
static dj_rate_t s_rate;
static bool s_rate_ready;
static volatile jstate_t s_state;

/* Journal core and flash: s_jmux. */
static SemaphoreHandle_t s_jmux;
static dj_t s_j;
static const esp_partition_t *s_part;
static TaskHandle_t s_writer;

/* ---- flash access ------------------------------------------------------------------------ */

static int fl_read(void *ctx, uint32_t off, void *buf, size_t len)
{
    return esp_partition_read((const esp_partition_t *)ctx, off, buf, len) == ESP_OK ? 0 : -1;
}

static int fl_write(void *ctx, uint32_t off, const void *buf, size_t len)
{
    return esp_partition_write((const esp_partition_t *)ctx, off, buf, len) == ESP_OK ? 0 : -1;
}

static int fl_erase(void *ctx, uint32_t off, size_t len)
{
    return esp_partition_erase_range((const esp_partition_t *)ctx, off, len) == ESP_OK ? 0 : -1;
}

/* ---- queue -------------------------------------------------------------------------------- */

static bool pop(qrec_t *out)
{
    bool got = false;
    portENTER_CRITICAL(&s_qlock);
    if (s_qcount > 0) {
        *out = s_queue[s_qhead];
        s_qhead = (s_qhead + 1u) % QUEUE_LEN;
        s_qcount--;
        got = true;
    }
    portEXIT_CRITICAL(&s_qlock);
    return got;
}

/* With s_jmux held. Without `may_erase`, stop where a sector would have to
 * be opened (the restart path: no erase, a millisecond per record at most). */
static void drain_locked(bool may_erase)
{
    qrec_t r;
    while ((may_erase || !dj_needs_open(&s_j)) && pop(&r)) {
        dj_entry_t e = {
            .uptime_s = r.uptime_s,
            .unix_s = r.unix_s,
            .kind = r.kind,
            .a = r.a,
            .b = r.b,
        };
        /* A failure is counted by the core (io_errors); the record is gone. */
        (void)dj_append(&s_j, &e);
    }
}

static void writer_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (xSemaphoreTake(s_jmux, portMAX_DELAY) == pdTRUE) {
            drain_locked(true);
            xSemaphoreGive(s_jmux);
        }
    }
}

void diag_journal_flush(void)
{
    if (s_state != J_READY || s_jmux == NULL) {
        return;
    }
    if (xSemaphoreTake(s_jmux, pdMS_TO_TICKS(FLUSH_WAIT_MS)) == pdTRUE) {
        drain_locked(false);
        xSemaphoreGive(s_jmux);
    }
}

/* ---- API ------------------------------------------------------------------------------------ */

esp_err_t diag_journal_append(diag_event_t kind, int32_t a, int32_t b)
{
    if (s_state == J_UNAVAILABLE) {
        return ESP_ERR_NOT_FOUND;
    }
    int64_t now_us = esp_timer_get_time();
    qrec_t r = {
        .uptime_s = (uint32_t)(now_us / 1000000),
        .unix_s = diag_utc_now(),
        .kind = (uint16_t)kind,
        .a = a,
        .b = b,
    };
    esp_err_t err = ESP_OK;
    portENTER_CRITICAL_SAFE(&s_qlock);
    if (!s_rate_ready) {
        dj_rate_init(&s_rate, CONFIG_DIAG_JOURNAL_MIN_INTERVAL_MS, CONFIG_DIAG_JOURNAL_BUDGET_PER_H,
                     1u << DIAG_EV_LADDER_STEP);
        s_rate_ready = true;
    }
    if (s_qcount >= QUEUE_LEN) {
        s_queue_full++;
        err = ESP_ERR_NO_MEM;
    } else if (dj_rate_check(&s_rate, (uint16_t)kind, (uint32_t)(now_us / 1000)) != DJ_RATE_OK) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        s_queue[(s_qhead + s_qcount) % QUEUE_LEN] = r;
        s_qcount++;
    }
    portEXIT_CRITICAL_SAFE(&s_qlock);
    if (err == ESP_OK && s_writer != NULL) {
        if (xPortInIsrContext()) {
            BaseType_t woken = pdFALSE;
            vTaskNotifyGiveFromISR(s_writer, &woken);
            portYIELD_FROM_ISR(woken);
        } else {
            xTaskNotifyGive(s_writer);
        }
    }
    return err;
}

esp_err_t diag_journal_read(uint32_t *cursor, diag_journal_entry_t *out, size_t *count)
{
    if (count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t cap = *count;
    *count = 0;
    if (cursor == NULL || (out == NULL && cap > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_state != J_READY) {
        return ESP_ERR_NOT_FOUND;
    }
    dj_entry_t tmp[READ_CHUNK];
    size_t n = 0;
    xSemaphoreTake(s_jmux, portMAX_DELAY);
    while (n < cap) {
        size_t want = cap - n;
        if (want > READ_CHUNK) {
            want = READ_CHUNK;
        }
        size_t got = dj_read(&s_j, cursor, tmp, want, NULL);
        for (size_t i = 0; i < got; i++) {
            out[n + i] = (diag_journal_entry_t){
                .seq = tmp[i].seq,
                .unix_s = tmp[i].unix_s,
                .uptime_s = tmp[i].uptime_s,
                .kind = tmp[i].kind,
                .a = tmp[i].a,
                .b = tmp[i].b,
            };
        }
        n += got;
        if (got < want) {
            break;
        }
    }
    xSemaphoreGive(s_jmux);
    *count = n;
    return ESP_OK;
}

void diag_journal_info(diag_journal_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    portENTER_CRITICAL(&s_qlock);
    out->queued = s_qcount;
    out->queue_full = s_queue_full;
    out->refused_kind = s_rate.refused_kind;
    out->refused_budget = s_rate.refused_budget;
    portEXIT_CRITICAL(&s_qlock);
    if (s_state != J_READY || xSemaphoreTake(s_jmux, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    out->available = true;
    out->capacity = dj_capacity(&s_j);
    out->oldest_seq = dj_oldest_seq(&s_j);
    out->newest_seq = (out->oldest_seq != 0) ? s_j.next_seq - 1u : 0;
    out->sector_cycles = (s_j.sectors != 0) ? s_j.top_sector_seq / s_j.sectors : 0;
    out->written = s_j.written;
    out->io_errors = s_j.io_errors;
    xSemaphoreGive(s_jmux);
}

esp_err_t diag_journal_init(void)
{
    if (s_state != J_PENDING) {
        return (s_state == J_READY) ? ESP_OK : ESP_ERR_NOT_FOUND;
    }
    s_part = esp_partition_find_first(PART_TYPE, PART_SUBTYPE, PART_LABEL);
    s_jmux = xSemaphoreCreateMutex();
    int r = DJ_ERR_STATE;
    if (s_part != NULL && s_jmux != NULL) {
        dj_flash_t fl = {
            .read = fl_read,
            .write = fl_write,
            .erase = fl_erase,
            .ctx = (void *)s_part,
        };
        r = dj_mount(&s_j, &fl, s_part->size);
    }
    if (r == DJ_OK &&
        xTaskCreatePinnedToCore(writer_task, "diag_jw", WRITER_STACK, NULL, WRITER_PRIO, &s_writer, tskNO_AFFINITY) !=
            pdPASS) {
        r = DJ_ERR_STATE;
    }
    if (r != DJ_OK) {
        s_state = J_UNAVAILABLE;
        portENTER_CRITICAL(&s_qlock);
        s_qcount = 0;
        portEXIT_CRITICAL(&s_qlock);
        ESP_LOGE(TAG, "event journal unavailable (%s)", s_part == NULL ? "no journal partition" : "mount failed");
        return ESP_ERR_NOT_FOUND;
    }
    s_state = J_READY;
    uint32_t oldest = dj_oldest_seq(&s_j);
    if (oldest != 0) {
        ESP_LOGI(TAG, "event journal: records %u..%u, %u erase cycles per sector", (unsigned)oldest,
                 (unsigned)(s_j.next_seq - 1u), (unsigned)(s_j.top_sector_seq / s_j.sectors));
    } else {
        ESP_LOGI(TAG, "event journal: empty, room for %u records", (unsigned)dj_capacity(&s_j));
    }
    xTaskNotifyGive(s_writer);   /* anything queued before init */
    return ESP_OK;
}
