/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * diag start-up, the trusted clock for journal records, core dump access,
 * task and heap statistics. The log tap is in diag_log.c, the journal in
 * diag_journal.c.
 */
#include "diag.h"

#include <stdlib.h>
#include <string.h>

#include "diag_internal.h"
#include "diag_task_core.h"

#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "diag";

#define CONSOLE_DRAIN_MS 300

/* ---- trusted clock ------------------------------------------------------------------------ */

static portMUX_TYPE s_time_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t s_utc_offset_us;     /* UTC microseconds minus esp_timer time */
static bool s_utc_valid;

void diag_set_utc(int64_t unix_s)
{
    if (unix_s <= 0) {
        return;
    }
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&s_time_lock);
    s_utc_offset_us = unix_s * 1000000 - now;
    s_utc_valid = true;
    portEXIT_CRITICAL_SAFE(&s_time_lock);
}

int64_t diag_utc_now(void)
{
    int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_SAFE(&s_time_lock);
    int64_t v = s_utc_valid ? (now + s_utc_offset_us) / 1000000 : 0;
    portEXIT_CRITICAL_SAFE(&s_time_lock);
    return v;
}

/* ---- core dump ---------------------------------------------------------------------------- */

static portMUX_TYPE s_cd_lock = portMUX_INITIALIZER_UNLOCKED;
static diag_coredump_info_t s_cd;   /* checked once at boot (the summary maps the partition briefly) */

static void coredump_check(void)
{
    diag_coredump_info_t cd = { 0 };
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t addr = 0;
    size_t size = 0;
    if (esp_core_dump_image_check() == ESP_OK && esp_core_dump_image_get(&addr, &size) == ESP_OK && size > 0) {
        cd.present = true;
        cd.size = size;
#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
        esp_core_dump_summary_t sum;
        if (esp_core_dump_get_summary(&sum) == ESP_OK) {
            strlcpy(cd.task, sum.exc_task, sizeof(cd.task));
            cd.pc = sum.exc_pc;
            uint32_t depth = sum.exc_bt_info.depth;
            if (depth > sizeof(cd.backtrace) / sizeof(cd.backtrace[0])) {
                depth = sizeof(cd.backtrace) / sizeof(cd.backtrace[0]);
            }
            memcpy(cd.backtrace, sum.exc_bt_info.bt, depth * sizeof(cd.backtrace[0]));
            cd.backtrace_depth = (uint8_t)depth;
        }
#endif
        ESP_LOGW(TAG, "core dump stored: %u bytes, task \"%s\", PC 0x%08lx", (unsigned)cd.size, cd.task,
                 (unsigned long)cd.pc);
    }
#endif
    portENTER_CRITICAL(&s_cd_lock);
    s_cd = cd;
    portEXIT_CRITICAL(&s_cd_lock);
}

esp_err_t diag_coredump_info(diag_coredump_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_cd_lock);
    *out = s_cd;
    portEXIT_CRITICAL(&s_cd_lock);
    return ESP_OK;
}

esp_err_t diag_coredump_read(size_t offset, void *buf, size_t len)
{
    if (buf == NULL && len > 0) {
        return ESP_ERR_INVALID_ARG;
    }
    diag_coredump_info_t cd;
    (void)diag_coredump_info(&cd);
    if (!cd.present) {
        return ESP_ERR_NOT_FOUND;
    }
    if (offset > cd.size || len > cd.size - offset) {
        return ESP_ERR_INVALID_SIZE;
    }
    /* The image starts at the beginning of the coredump partition. */
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (part == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    return esp_partition_read(part, offset, buf, len);
}

esp_err_t diag_coredump_erase(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    esp_err_t err = esp_core_dump_image_erase();
#else
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#endif
    if (err == ESP_OK) {
        portENTER_CRITICAL(&s_cd_lock);
        memset(&s_cd, 0, sizeof(s_cd));
        portEXIT_CRITICAL(&s_cd_lock);
    }
    return err;
}

/* ---- task and heap statistics ------------------------------------------------------------ */

static SemaphoreHandle_t s_task_mux;
static dts_state_t s_dts;           /* under s_task_mux */

esp_err_t diag_task_stats(diag_task_info_t *tasks, size_t *count)
{
    if (count == NULL || (tasks == NULL && *count > 0)) {
        return ESP_ERR_INVALID_ARG;
    }
    size_t cap = *count;
    *count = 0;
    if (s_task_mux == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    UBaseType_t room = uxTaskGetNumberOfTasks() + 4u;
    TaskStatus_t *st = heap_caps_malloc(room * sizeof(TaskStatus_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (st == NULL) {
        st = malloc(room * sizeof(TaskStatus_t));
    }
    dts_sample_t *smp = heap_caps_malloc(DTS_MAX_TASKS * (sizeof(dts_sample_t) + 1u), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (smp == NULL) {
        smp = malloc(DTS_MAX_TASKS * (sizeof(dts_sample_t) + 1u));
    }
    if (st == NULL || smp == NULL) {
        free(st);
        free(smp);
        return ESP_ERR_NO_MEM;
    }
    uint8_t *pct = (uint8_t *)(smp + DTS_MAX_TASKS);

    configRUN_TIME_COUNTER_TYPE total = 0;
    UBaseType_t got = uxTaskGetSystemState(st, room, &total);
    size_t m = (got > DTS_MAX_TASKS) ? DTS_MAX_TASKS : got;
    for (size_t i = 0; i < m; i++) {
        smp[i].id = (uint32_t)st[i].xTaskNumber;
        smp[i].runtime = (uint64_t)st[i].ulRunTimeCounter;
    }
    xSemaphoreTake(s_task_mux, portMAX_DELAY);
    dts_update(&s_dts, smp, m, (uint64_t)total, configNUMBER_OF_CORES, pct);
    xSemaphoreGive(s_task_mux);

    size_t n = (m < cap) ? m : cap;
    for (size_t i = 0; i < n; i++) {
        diag_task_info_t *t = &tasks[i];
        memset(t, 0, sizeof(*t));
        strlcpy(t->name, st[i].pcTaskName != NULL ? st[i].pcTaskName : "", sizeof(t->name));
        t->priority = (uint8_t)st[i].uxCurrentPriority;
#if configTASKLIST_INCLUDE_COREID
        t->core = (st[i].xCoreID == tskNO_AFFINITY) ? -1 : (int8_t)st[i].xCoreID;
#else
        t->core = -1;
#endif
        t->stack_free_min = (uint32_t)st[i].usStackHighWaterMark;
        t->cpu_pct = pct[i];
    }
    *count = n;
    free(st);
    free(smp);
    return (got > n) ? ESP_ERR_INVALID_SIZE : ESP_OK;
}

void diag_heap_stats(diag_heap_info_t *out)
{
    if (out == NULL) {
        return;
    }
    out->internal_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->internal_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->internal_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->psram_min = (uint32_t)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    out->psram_largest = (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

/* ---- log level, start-up ------------------------------------------------------------------ */

esp_err_t diag_set_log_level(const char *tag, esp_log_level_t level)
{
    if (tag == NULL || level > ESP_LOG_VERBOSE) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_log_level_set(tag, level);
    return ESP_OK;
}

/* Last IDF shutdown handler to run (registered first): journal records still
 * queued go to flash, and the console gets a moment to empty its buffer. */
static void on_restart(void)
{
    diag_journal_flush();
    diag_log_drain_console(CONSOLE_DRAIN_MS);
}

esp_err_t diag_init(void)
{
    static bool s_started;
    if (s_started) {
        return ESP_OK;
    }
    s_started = true;

    esp_err_t err = diag_log_init();
    s_task_mux = xSemaphoreCreateMutex();
    esp_err_t jerr = diag_journal_init();
    coredump_check();
    esp_err_t serr = esp_register_shutdown_handler(on_restart);
    if (serr != ESP_OK) {
        ESP_LOGW(TAG, "no restart hook (%s): queued journal records may be lost at a reboot",
                 esp_err_to_name(serr));
    }
    if (err != ESP_OK) {
        return err;
    }
    return jerr;
}
