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

#include "esp_attr.h"
#include "esp_core_dump.h"
#include "esp_heap_caps.h"
#include "esp_ipc.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/freertos_debug.h"
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

/*
 * Every task's figures, read with no task running on either core and
 * interrupts on throughout. uxTaskGetSystemState() measures each task's free
 * stack inside the kernel's critical section: some 2 ms of scanning 70+ KB
 * with interrupts masked on both cores (the other core spins for the kernel
 * lock with them masked), longer than UART1's FIFO lasts at 460800 baud
 * above its 64-byte threshold (1.4 ms): the receiver link lost bytes at
 * almost every hardware FIFO overflow seen. Here the other core is held in
 * hold_core() (IPC) and this one's scheduler is suspended, so no task is
 * created, deleted or freed meanwhile (the pattern of IDF's
 * esp_backtrace_print_all_tasks), and the stacks are scanned outside the
 * kernel's lock. The collector runs pinned to core 0 (TASKS_CORE), so the
 * core it holds is never its own.
 */
#define TASKS_CORE        0
#define TASKS_STACK       3072

typedef struct {
    TaskStatus_t *st;
    UBaseType_t room;
    UBaseType_t got;
    configRUN_TIME_COUNTER_TYPE total;
    SemaphoreHandle_t done;
#if !CONFIG_FREERTOS_UNICORE
    volatile bool held;            /* the other core is in hold_core() with its scheduler suspended */
    volatile bool release;
    volatile bool let_go;          /* hold_core() read `release` and touches the job no more */
#endif
} task_job_t;

#if !CONFIG_FREERTOS_UNICORE
static void hold_core(void *arg)
{
    task_job_t *j = arg;
    vTaskSuspendAll();
    j->held = true;
    while (!j->release) {
    }
    /* The job lives on the waiting caller's stack, gone once it returns: say so before the last word of it. */
    j->let_go = true;
    (void)xTaskResumeAll();
}
#endif

static void collect_tasks(void *arg)
{
    task_job_t *j = arg;
    j->got = 0;
    TaskSnapshot_t *snap = heap_caps_malloc(j->room * sizeof(TaskSnapshot_t), MALLOC_CAP_8BIT);
#if !CONFIG_FREERTOS_UNICORE
    if (snap != NULL && esp_ipc_call(TASKS_CORE == 0 ? 1 : 0, hold_core, j) != ESP_OK) {
        free(snap);
        snap = NULL;
    }
    while (snap != NULL && !j->held) {
    }
#endif
    if (snap != NULL) {
        vTaskSuspendAll();
        UBaseType_t got = uxTaskGetSnapshotAll(snap, j->room, NULL);
        if (got > j->room) {
            got = j->room;
        }
        TaskHandle_t self = xTaskGetCurrentTaskHandle();
        UBaseType_t n = 0;
        for (UBaseType_t i = 0; i < got; i++) {
            TaskHandle_t t = (TaskHandle_t)snap[i].pxTCB;
            if (t == self) {
                continue;   /* this collector, gone in a moment */
            }
            vTaskGetInfo(t, &j->st[n], pdFALSE, eReady);   /* without the stack scan: that is the long part */
            j->st[n].usStackHighWaterMark = (configSTACK_DEPTH_TYPE)uxTaskGetStackHighWaterMark(t);
            n++;
        }
        j->total = portGET_RUN_TIME_COUNTER_VALUE();
        j->got = n;
        (void)xTaskResumeAll();
#if !CONFIG_FREERTOS_UNICORE
        j->release = true;
        while (!j->let_go) {
        }
#endif
        free(snap);
    }
    xSemaphoreGive(j->done);
    vTaskDelete(NULL);
}

/* collect_tasks() on core 0, waited for. @return the tasks read, 0 when it could not run. */
static UBaseType_t task_states(TaskStatus_t *st, UBaseType_t room, configRUN_TIME_COUNTER_TYPE *total)
{
    task_job_t j = { .st = st, .room = room };
    j.done = xSemaphoreCreateBinary();
    if (j.done == NULL) {
        return 0;
    }
    if (xTaskCreatePinnedToCore(collect_tasks, "diag_tasks", TASKS_STACK, &j, uxTaskPriorityGet(NULL), NULL,
                                TASKS_CORE) != pdPASS) {
        vSemaphoreDelete(j.done);
        return 0;
    }
    (void)xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    *total = j.total;
    return j.got;
}

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
    UBaseType_t got = task_states(st, room, &total);
    if (got == 0) {
        free(st);
        free(smp);
        return ESP_ERR_NO_MEM;
    }
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

/* Tags with a fixed level (diag_log_fix_level), applied again after every
 * "*" change: esp_log_level_set("*") drops every per-tag level. */
#define FIXED_TAG_LEN 16
typedef struct {
    char tag[FIXED_TAG_LEN];
    esp_log_level_t level;
} fixed_level_t;

static portMUX_TYPE s_level_lock = portMUX_INITIALIZER_UNLOCKED;
EXT_RAM_BSS_ATTR static fixed_level_t s_fixed[DIAG_LOG_FIXED_MAX];   /* under s_level_lock */
static size_t s_fixed_n;

static esp_err_t level_ok(esp_log_level_t level)
{
    if (level > ESP_LOG_VERBOSE) {
        return ESP_ERR_INVALID_ARG;
    }
    return level > (esp_log_level_t)CONFIG_LOG_MAXIMUM_LEVEL ? ESP_ERR_NOT_SUPPORTED : ESP_OK;
}

esp_err_t diag_set_log_level(const char *tag, esp_log_level_t level)
{
    if (tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = level_ok(level);
    if (err != ESP_OK) {
        return err;
    }
    esp_log_level_set(tag, level);
    if (strcmp(tag, "*") != 0) {
        return ESP_OK;
    }
    fixed_level_t fixed[DIAG_LOG_FIXED_MAX];
    portENTER_CRITICAL(&s_level_lock);
    size_t n = s_fixed_n;
    memcpy(fixed, s_fixed, n * sizeof(fixed[0]));
    portEXIT_CRITICAL(&s_level_lock);
    for (size_t i = 0; i < n; i++) {
        esp_log_level_set(fixed[i].tag, fixed[i].level);
    }
    return ESP_OK;
}

esp_err_t diag_log_fix_level(const char *tag, esp_log_level_t level)
{
    if (tag == NULL || tag[0] == '\0' || strcmp(tag, "*") == 0 || strnlen(tag, FIXED_TAG_LEN) >= FIXED_TAG_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = level_ok(level);
    if (err != ESP_OK) {
        return err;
    }
    err = ESP_ERR_NO_MEM;
    portENTER_CRITICAL(&s_level_lock);
    for (size_t i = 0; i < s_fixed_n; i++) {
        if (strcmp(s_fixed[i].tag, tag) == 0) {
            s_fixed[i].level = level;
            err = ESP_OK;
            break;
        }
    }
    if (err != ESP_OK && s_fixed_n < DIAG_LOG_FIXED_MAX) {
        strlcpy(s_fixed[s_fixed_n].tag, tag, sizeof(s_fixed[s_fixed_n].tag));
        s_fixed[s_fixed_n].level = level;
        s_fixed_n++;
        err = ESP_OK;
    }
    portEXIT_CRITICAL(&s_level_lock);
    if (err == ESP_OK) {
        esp_log_level_set(tag, level);
    }
    return err;
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
