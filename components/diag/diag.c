/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "diag.h"

#include <string.h>

#include "sdkconfig.h"

esp_err_t diag_init(void)
{
    /* TODO: log ring of CONFIG_DIAG_LOG_RING_KB in EXT_RAM_NOINIT_ATTR memory
     * (keep the previous boot's ring readable), install the log tap with
     * esp_log_set_vprintf() (chain to the previous vprintf), open the
     * journal partition. After each append, write the record and the header
     * back to PSRAM with esp_cache_msync(DIR_C2M | UNALIGNED) (esp_mm): the
     * S3 restart path drops dirty data-cache lines. */
    return ESP_OK;
}

size_t diag_log_read(uint32_t *cursor, char *buf, size_t buf_size)
{
    (void)cursor;
    if (buf != NULL && buf_size > 0) {
        buf[0] = '\0';
    }
    return 0;
}

size_t diag_log_read_prev_boot(uint32_t *cursor, char *buf, size_t buf_size)
{
    (void)cursor;
    if (buf != NULL && buf_size > 0) {
        buf[0] = '\0';
    }
    return 0;
}

esp_err_t diag_set_log_level(const char *tag, esp_log_level_t level)
{
    if (tag == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_log_level_set(tag, level);
    return ESP_OK;
}

esp_err_t diag_journal_append(diag_event_t kind, int32_t a, int32_t b)
{
    (void)kind;
    (void)a;
    (void)b;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t diag_journal_read(uint32_t *cursor, diag_journal_entry_t *out, size_t *count)
{
    (void)cursor;
    (void)out;
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t diag_coredump_info(diag_coredump_info_t *out)
{
    if (out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));
    /* TODO: esp_core_dump_image_check() + esp_core_dump_get_summary(). */
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t diag_coredump_read(size_t offset, void *buf, size_t len)
{
    (void)offset;
    (void)buf;
    (void)len;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t diag_coredump_erase(void)
{
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t diag_task_stats(diag_task_info_t *tasks, size_t *count)
{
    (void)tasks;
    if (count != NULL) {
        *count = 0;
    }
    return ESP_ERR_NOT_SUPPORTED;
}
