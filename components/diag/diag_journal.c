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
 *
 * diag_journal_clear() empties the journal for the user: the core opens a
 * new sector at once (so the next read already finds nothing) and the
 * records queued before the clear are dropped with the rest, then it erases
 * the other sectors one per lock, so a reader or the writer waits for one
 * sector erase at most. A clear that cannot open its sector clears nothing:
 * the queue stays too. It blocks its caller for every erase (the web server
 * runs it on its worker task).
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
#define WRITER_STACK  4096
#define WRITER_PRIO   5
#define FLUSH_WAIT_MS 100
#define INFO_WAIT_MS  1000      /* longer than one sector erase, the longest the writer or a clear holds the journal */
#define READ_CHUNK    16u

/* Kinds that carry a caster slot or a rover connection in the low bits of
 * `a`: one restart ends every session, and each one's record must get in
 * (the per-kind interval holds per slot or connection, dj_rate_key_kinds). */
#define RATE_KEYED_KINDS                                                                                       \
    ((1u << DIAG_EV_CASTER_DOWN) | (1u << DIAG_EV_CASTER_UP) | (1u << DIAG_EV_CASTER_STOP) |                   \
     (1u << DIAG_EV_ROVER_UP) | (1u << DIAG_EV_ROVER_DOWN))
_Static_assert(DIAG_EV_ROVER_DOWN < DJ_RATE_KINDS, "keyed kinds need their own rate entry");
#if defined(CONFIG_NTRIP_MAX_CASTERS)
_Static_assert(CONFIG_NTRIP_MAX_CASTERS <= DJ_RATE_KEYS, "every caster slot needs its own rate key");
#endif

typedef enum {
    J_PENDING = 0,      /* before diag_init: appends wait in the queue */
    J_READY,
    J_UNAVAILABLE,
} jstate_t;

/* Queue and rate limits: s_qlock. */
static portMUX_TYPE s_qlock = portMUX_INITIALIZER_UNLOCKED;
static dj_queue_t s_q;
static uint32_t s_queue_full;
static dj_rate_t s_rate;
static bool s_rate_ready;
static volatile jstate_t s_state;
static bool s_clearing;           /* a clear runs (s_qlock) */
static volatile uint32_t s_clears; /* clears since boot (written under s_jmux) */

/* Journal core and flash: s_jmux. */
static SemaphoreHandle_t s_jmux;
static dj_t s_j;
/* The core's counts as diag_journal_info last read them (s_qlock): what it
 * gives while the journal is held longer than it waits. */
static diag_journal_info_t s_info;
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

static bool pop(dj_entry_t *out)
{
    portENTER_CRITICAL(&s_qlock);
    bool got = dj_queue_pop(&s_q, out);
    portEXIT_CRITICAL(&s_qlock);
    return got;
}

/* With s_jmux held. Without `may_erase`, stop where a sector would have to
 * be opened (the restart path: no erase, a millisecond per record at most). */
static void drain_locked(bool may_erase)
{
    dj_entry_t e;
    while ((may_erase || !dj_needs_open(&s_j)) && pop(&e)) {
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
    dj_entry_t r = {
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
                     (1u << DIAG_EV_LADDER_STEP) | (1u << DIAG_EV_GNSS_FLASH) | (1u << DIAG_EV_RX_BOOT));
        dj_rate_key_kinds(&s_rate, RATE_KEYED_KINDS);
        s_rate_ready = true;
    }
    if (s_q.count >= DJ_QUEUE_LEN) {
        s_queue_full++;
        err = ESP_ERR_NO_MEM;
    } else if (dj_rate_check_rec(&s_rate, (uint16_t)kind, a, (uint32_t)(now_us / 1000)) != DJ_RATE_OK) {
        err = ESP_ERR_INVALID_STATE;
    } else {
        (void)dj_queue_push(&s_q, &r);
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

esp_err_t diag_journal_clear(void)
{
    if (s_state != J_READY) {
        return ESP_ERR_NOT_FOUND;
    }
    bool busy;
    portENTER_CRITICAL(&s_qlock);
    busy = s_clearing;
    s_clearing = true;
    portEXIT_CRITICAL(&s_qlock);
    if (busy) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(s_jmux, portMAX_DELAY);
    /* What waits in the queue now was written before the clear. With
     * s_jmux held the writer cannot take any of it; appends may add more,
     * which stay whatever happens. */
    portENTER_CRITICAL(&s_qlock);
    uint32_t queued = s_q.count;
    portEXIT_CRITICAL(&s_qlock);
    int r = dj_clear_start(&s_j);
    portENTER_CRITICAL(&s_qlock);
    dj_queue_clear_started(&s_q, queued, r);   /* dropped only if the clear started */
    portEXIT_CRITICAL(&s_qlock);
    if (r == DJ_OK) {
        s_clears++;
    }
    uint32_t from = s_j.next_seq;
    xSemaphoreGive(s_jmux);
    if (r != DJ_OK) {
        portENTER_CRITICAL(&s_qlock);
        s_clearing = false;
        portEXIT_CRITICAL(&s_qlock);
        ESP_LOGW(TAG, "event journal not cleared: its next sector could not be opened");
        return ESP_FAIL;
    }
    bool failed = false;
    int left = 1;
    while (left != 0) {
        xSemaphoreTake(s_jmux, portMAX_DELAY);
        left = dj_clear_step(&s_j);
        xSemaphoreGive(s_jmux);
        if (left < 0) {
            failed = true;   /* that sector is given up; the others go on */
            left = 1;
        }
    }
    portENTER_CRITICAL(&s_qlock);
    s_clearing = false;
    portEXIT_CRITICAL(&s_qlock);
    if (failed) {
        ESP_LOGW(TAG, "event journal cleared with flash errors");
        return ESP_ERR_NOT_FINISHED;
    }
    ESP_LOGI(TAG, "event journal cleared; records go on from %u", (unsigned)from);
    return ESP_OK;
}

uint32_t diag_journal_clear_count(void)
{
    return s_clears;
}

/* With s_jmux held: the core's counts. */
static void core_counts(diag_journal_info_t *c)
{
    c->available = true;
    c->capacity = dj_capacity(&s_j);
    c->oldest_seq = dj_oldest_seq(&s_j);
    c->newest_seq = (c->oldest_seq != 0) ? s_j.next_seq - 1u : 0;
    c->sector_cycles = (s_j.sectors != 0) ? s_j.top_sector_seq / s_j.sectors : 0;
    c->written = s_j.written;
    c->io_errors = s_j.io_errors;
}

/*
 * Available whenever the journal is: a clear or the writer holds it for one
 * sector erase at a time, which it waits for; held longer than that (a
 * failing flash), the counts are those of the previous read.
 */
void diag_journal_info(diag_journal_info_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_state == J_READY) {
        diag_journal_info_t c = { 0 };
        bool fresh = xSemaphoreTake(s_jmux, pdMS_TO_TICKS(INFO_WAIT_MS)) == pdTRUE;
        if (fresh) {
            core_counts(&c);
            xSemaphoreGive(s_jmux);
        }
        portENTER_CRITICAL(&s_qlock);
        if (fresh) {
            s_info = c;
        }
        *out = s_info;
        portEXIT_CRITICAL(&s_qlock);
    }
    portENTER_CRITICAL(&s_qlock);
    out->queued = s_q.count;
    out->queue_full = s_queue_full;
    out->refused_kind = s_rate.refused_kind;
    out->refused_budget = s_rate.refused_budget;
    portEXIT_CRITICAL(&s_qlock);
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
        dj_queue_reset(&s_q);
        portEXIT_CRITICAL(&s_qlock);
        ESP_LOGE(TAG, "event journal unavailable (%s)", s_part == NULL ? "no journal partition" : "mount failed");
        return ESP_ERR_NOT_FOUND;
    }
    xSemaphoreTake(s_jmux, portMAX_DELAY);
    core_counts(&s_info);   /* read before diag_journal_info can */
    xSemaphoreGive(s_jmux);
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
