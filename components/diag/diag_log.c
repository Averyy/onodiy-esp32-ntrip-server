/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Log tap: the log rings (this boot's and the previous boot's, in PSRAM
 * .noinit memory) and the best-effort console.
 *
 * Every log line is formatted once, on the logging task's stack
 * (CONFIG_DIAG_LOG_LINE_MAX bytes), appended to the ring under a spinlock
 * (memcpy plus a write-back of the touched cache lines, a few microseconds)
 * and then offered to the console UART: written only if the driver's TX
 * buffer has room for the whole line, else counted as dropped. A mutex
 * keeps two loggers from both seeing the same free room; nobody holds it
 * for longer than a copy into the TX buffer.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "diag.h"
#include "diag_internal.h"
#include "diag_logring.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_attr.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"
#include "sdkconfig.h"

#define BANK_BYTES        ((size_t)CONFIG_DIAG_LOG_RING_KB * 1024u)
#define READ_SLICE        (LOGRING_MAX_TEXT + 1u)   /* bytes copied per lock hold: one longest record */
#define LINE_SPARE        8u                        /* room to turn up to 8 newlines into CR LF in place */
#define CONSOLE_RX_BUF    256                       /* the driver needs more than the 128-byte FIFO */
#define CONSOLE_MARGIN    64u                       /* TX buffer item headers of one write */
#define CONSOLE_WAIT_MS   5                         /* another logger finishing its copy */

_Static_assert(CONFIG_DIAG_LOG_LINE_MAX <= LOGRING_MAX_TEXT, "a log line must fit one ring record");
_Static_assert(BANK_BYTES > sizeof(logring_hdr_t) + LOGRING_MAX_TEXT, "log ring too small");

/* Two banks: the one this boot writes and the previous boot's. They keep
 * their content across software, panic and watchdog resets. */
#if CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY
#define BANKS_PERSIST 1
static EXT_RAM_NOINIT_ATTR uint8_t s_bank_mem[2][BANK_BYTES] __attribute__((aligned(64)));
#else
#define BANKS_PERSIST 0
#endif

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static logring_t s_ring;            /* this boot; under s_lock */
static logring_t s_prev;            /* previous boot; read-only after init */
static bool s_ring_ok;
static bool s_prev_ok;
static uint32_t s_cache_line = 32;

static diag_log_stats_t s_stats;    /* under s_lock */

#if CONFIG_ESP_CONSOLE_UART
#define CONSOLE_PORT ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#endif
static vprintf_like_t s_chain;      /* output function before ours: console until the driver is up */
static volatile bool s_console;     /* driver installed: write through it, never block */
static SemaphoreHandle_t s_console_mux;
static uint32_t s_console_pending;  /* under s_console_mux: drops not yet reported on the console */

/* ---- ring ------------------------------------------------------------------------------ */

static void ring_sync(void *ctx, const void *addr, size_t len)
{
    (void)ctx;
#if BANKS_PERSIST
    /* Rounded to whole cache lines; DIR_C2M writes dirty lines back to PSRAM. */
    uintptr_t line = s_cache_line;
    uintptr_t start = (uintptr_t)addr & ~(line - 1u);
    uintptr_t end = ((uintptr_t)addr + len + line - 1u) & ~(line - 1u);
    (void)esp_cache_msync((void *)start, end - start,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#else
    (void)addr;
    (void)len;
#endif
}

static esp_err_t rings_init(void)
{
    uint8_t *mem[2];
#if BANKS_PERSIST
    mem[0] = s_bank_mem[0];
    mem[1] = s_bank_mem[1];
    uint32_t line = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA);
    if (line != 0 && (line & (line - 1u)) == 0) {
        s_cache_line = line;
    }
#else
    /* No persistent memory: this boot's ring only. */
    mem[0] = heap_caps_malloc(BANK_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (mem[0] == NULL) {
        mem[0] = heap_caps_malloc(BANK_BYTES, MALLOC_CAP_8BIT);
    }
    if (mem[0] == NULL) {
        return ESP_ERR_NO_MEM;
    }
    mem[1] = NULL;
#endif

    logring_t bank[2];
    bool ok[2] = { false, false };
    for (int i = 0; i < 2; i++) {
        if (mem[i] != NULL) {
            logring_attach(&bank[i], mem[i], BANK_BYTES, ring_sync, NULL);
#if BANKS_PERSIST
            ok[i] = logring_recover(&bank[i]);
#endif
        }
    }

    /* The valid bank with the newer boot_id is the previous boot's. */
    int prev = -1;
    if (ok[0] && ok[1]) {
        prev = ((int32_t)(bank[1].hdr->boot_id - bank[0].hdr->boot_id) > 0) ? 1 : 0;
    } else if (ok[0]) {
        prev = 0;
    } else if (ok[1]) {
        prev = 1;
    }
    int active = (prev == 0) ? 1 : 0;
    uint32_t boot_id = 1;
    uint32_t first_seq = 1;
    if (prev >= 0) {
        s_prev = bank[prev];
        s_prev_ok = logring_count(&s_prev) > 0;
        boot_id = s_prev.hdr->boot_id + 1u;
        first_seq = s_prev.hdr->next_seq;
    }
    s_ring = bank[active];
    logring_format(&s_ring, boot_id == 0 ? 1u : boot_id, first_seq);
    s_ring_ok = true;
    s_stats.prev_boot_available = s_prev_ok;
    s_stats.prev_boot_lines = s_prev_ok ? logring_count(&s_prev) : 0;
    return ESP_OK;
}

/* ---- console ----------------------------------------------------------------------------- */

#if CONFIG_ESP_CONSOLE_UART
static size_t newlines(const char *s, size_t len)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        n += (s[i] == '\n');
    }
    return n;
}

/* Bytes on the wire after the console's line-ending rule. */
static size_t wire_len(const char *s, size_t len)
{
#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
    return len + newlines(s, len);
#else
    (void)s;
    return len;
#endif
}

/* Write with the console's line-ending rule; `cap` = size of the buffer behind s
 * (room to expand LF to CR LF in place). */
static void wire_write(char *s, size_t len, size_t cap)
{
#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
    size_t nl = newlines(s, len);
    if (nl == 0) {
        uart_write_bytes(CONSOLE_PORT, s, len);
    } else if (len + nl <= cap) {
        size_t j = len + nl;
        for (size_t i = len; i-- > 0;) {
            s[--j] = s[i];
            if (s[i] == '\n') {
                s[--j] = '\r';
            }
        }
        uart_write_bytes(CONSOLE_PORT, s, len + nl);
    } else {
        size_t start = 0;
        for (size_t i = 0; i < len; i++) {
            if (s[i] == '\n') {
                if (i > start) {
                    uart_write_bytes(CONSOLE_PORT, s + start, i - start);
                }
                uart_write_bytes(CONSOLE_PORT, "\r\n", 2);
                start = i + 1u;
            }
        }
        if (start < len) {
            uart_write_bytes(CONSOLE_PORT, s + start, len - start);
        }
    }
#elif CONFIG_LIBC_STDOUT_LINE_ENDING_CR
    (void)cap;
    for (size_t i = 0; i < len; i++) {
        if (s[i] == '\n') {
            s[i] = '\r';
        }
    }
    uart_write_bytes(CONSOLE_PORT, s, len);
#else
    (void)cap;
    uart_write_bytes(CONSOLE_PORT, s, len);
#endif
}
#endif /* CONFIG_ESP_CONSOLE_UART */

static void count_drop(size_t len)
{
    portENTER_CRITICAL_SAFE(&s_lock);
    s_stats.console_dropped++;
    s_stats.console_dropped_bytes += (uint32_t)len;
    portEXIT_CRITICAL_SAFE(&s_lock);
}

static void console_put(char *s, size_t len, size_t cap, const char *fmt, va_list ap)
{
    if (!s_console) {
        if (s_chain != NULL) {
            va_list cp;
            va_copy(cp, ap);
            s_chain(fmt, cp);
            va_end(cp);
        }
        return;
    }
#if CONFIG_ESP_CONSOLE_UART
    /* Never wait inside a critical section or with the scheduler stopped. */
    if (!xPortCanYield() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING ||
        xSemaphoreTake(s_console_mux, pdMS_TO_TICKS(CONSOLE_WAIT_MS)) != pdTRUE) {
        count_drop(len);
        return;
    }
    size_t room = 0;
    if (uart_get_tx_buffer_free_size(CONSOLE_PORT, &room) != ESP_OK) {
        room = 0;
    }
    size_t need = wire_len(s, len) + CONSOLE_MARGIN;
    if (s_console_pending > 0) {
        /* Say what was lost before the output resumes. */
        char note[64];
        int n = snprintf(note, sizeof(note) - 1u, "[diag: %u console line%s dropped]\n",
                         (unsigned)s_console_pending, s_console_pending == 1 ? "" : "s");
        size_t note_len = (n > 0) ? (size_t)n : 0;
        size_t note_need = wire_len(note, note_len) + CONSOLE_MARGIN;
        if (room >= note_need + need) {
            wire_write(note, note_len, sizeof(note));
            room -= note_need;
            s_console_pending = 0;
        }
    }
    if (s_console_pending == 0 && room >= need) {
        wire_write(s, len, cap);
    } else {
        s_console_pending++;
        count_drop(len);
    }
    xSemaphoreGive(s_console_mux);
#else
    (void)s;
    (void)len;
    (void)cap;
    (void)fmt;
    (void)ap;
#endif
}

/* ---- the log output function ----------------------------------------------------------- */

/* A line cut to the buffer ends in "..." (and keeps its newline). */
static void mark_cut(char *line, size_t len, const char *fmt)
{
    size_t fl = strlen(fmt);
    if (fl > 0 && fmt[fl - 1u] == '\n') {
        memcpy(line + len - 4u, "...\n", 4);
    } else {
        memcpy(line + len - 3u, "...", 3);
    }
}

static int diag_vprintf(const char *fmt, va_list ap)
{
    if (xPortInIsrContext()) {
        /* Logging from an interrupt is not supported by esp_log either; keep
         * it from doing harm. */
        return esp_rom_vprintf(fmt, ap);
    }
    char line[CONFIG_DIAG_LOG_LINE_MAX + LINE_SPARE];
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(line, CONFIG_DIAG_LOG_LINE_MAX, fmt, cp);
    va_end(cp);
    if (n <= 0) {
        return n;
    }
    size_t len = (size_t)n;
    bool cut = false;
    if (len >= CONFIG_DIAG_LOG_LINE_MAX) {
        len = CONFIG_DIAG_LOG_LINE_MAX - 1u;
        mark_cut(line, len, fmt);
        cut = true;
    }

    if (s_ring_ok) {
        portENTER_CRITICAL_SAFE(&s_lock);
        uint32_t lost = s_ring.hdr->lost;
        (void)logring_append(&s_ring, line, len);
        s_stats.lines++;
        s_stats.ring_overwritten += s_ring.hdr->lost - lost;
        s_stats.cut += cut ? 1u : 0u;
        portEXIT_CRITICAL_SAFE(&s_lock);
    }
    console_put(line, len, sizeof(line), fmt, ap);
    return n;
}

esp_err_t diag_log_init(void)
{
    if (s_ring_ok) {
        return ESP_OK;
    }
    esp_err_t err = rings_init();
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_ESP_CONSOLE_UART
    esp_err_t cerr = ESP_OK;
    if (!uart_is_driver_installed(CONSOLE_PORT)) {
        /* Let the lines printed so far leave the FIFO before the driver takes over. */
        fflush(stdout);
        fsync(fileno(stdout));
        cerr = uart_driver_install(CONSOLE_PORT, CONSOLE_RX_BUF, CONFIG_DIAG_CONSOLE_TX_BUF, 0, NULL, 0);
    }
    if (cerr == ESP_OK) {
        s_console_mux = xSemaphoreCreateMutex();
        cerr = (s_console_mux != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (cerr == ESP_OK) {
        /* printf and friends go through the driver too, so their bytes queue
         * behind the log lines instead of racing them into the FIFO. */
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
        s_console = true;
    }
#endif
    s_chain = esp_log_set_vprintf(diag_vprintf);
#if CONFIG_ESP_CONSOLE_UART
    if (cerr != ESP_OK) {
        ESP_LOGE("diag", "console driver not installed (%s): console output may block", esp_err_to_name(cerr));
    }
#endif
    if (s_prev_ok) {
        ESP_LOGI("diag", "previous boot's log kept: %u lines", (unsigned)s_stats.prev_boot_lines);
    }
    return ESP_OK;
}

void diag_log_drain_console(uint32_t ms)
{
#if CONFIG_ESP_CONSOLE_UART
    if (s_console) {
        (void)uart_wait_tx_done(CONSOLE_PORT, pdMS_TO_TICKS(ms));
    }
#else
    (void)ms;
#endif
}

/* ---- readers ------------------------------------------------------------------------------ */

size_t diag_log_read_ex(diag_log_src_t src, uint32_t *cursor, char *buf, size_t buf_size,
                        diag_log_read_info_t *info)
{
    diag_log_read_info_t res = { 0 };
    size_t pos = 0;
    if (buf != NULL && buf_size > 0) {
        buf[0] = '\0';
    }
    bool live = (src == DIAG_LOG_THIS_BOOT);
    if (cursor == NULL || buf == NULL || buf_size < 2 || !(live ? s_ring_ok : s_prev_ok)) {
        if (info != NULL) {
            *info = res;
        }
        return 0;
    }
    const logring_t *r = live ? &s_ring : &s_prev;
    /* Short lock holds: one slice at a time, so a long read never delays a logger much. */
    while (pos + 1u < buf_size) {
        size_t slice = buf_size - pos;
        if (slice > READ_SLICE) {
            slice = READ_SLICE;
        }
        logring_read_t ri;
        if (live) {
            portENTER_CRITICAL(&s_lock);
        }
        size_t n = logring_read(r, cursor, buf + pos, slice, &ri);
        if (live) {
            portEXIT_CRITICAL(&s_lock);
        }
        pos += n;
        res.gap |= ri.gap;
        res.more = ri.more;
        if (!ri.more || ri.records == 0) {
            break;
        }
    }
    if (info != NULL) {
        *info = res;
    }
    return pos;
}

/* Would a read from this cursor skip lines that were overwritten? */
static bool gap_ahead(const logring_t *r, bool live, uint32_t cursor)
{
    if (cursor == 0) {
        return false;
    }
    if (live) {
        portENTER_CRITICAL(&s_lock);
    }
    uint32_t first = r->hdr->first_seq;
    uint32_t next = r->hdr->next_seq;
    if (live) {
        portEXIT_CRITICAL(&s_lock);
    }
    return cursor + 1u < first || cursor + 1u > next;
}

static size_t read_with_note(diag_log_src_t src, uint32_t *cursor, char *buf, size_t buf_size)
{
    static const char k_note[] = "[diag: earlier lines were overwritten before they were read]\n";
    bool live = (src == DIAG_LOG_THIS_BOOT);
    if (cursor != NULL && buf != NULL && buf_size > sizeof(k_note) + 1u && (live ? s_ring_ok : s_prev_ok) &&
        gap_ahead(live ? &s_ring : &s_prev, live, *cursor)) {
        memcpy(buf, k_note, sizeof(k_note) - 1u);
        size_t n = diag_log_read_ex(src, cursor, buf + sizeof(k_note) - 1u, buf_size - (sizeof(k_note) - 1u), NULL);
        return n + sizeof(k_note) - 1u;
    }
    return diag_log_read_ex(src, cursor, buf, buf_size, NULL);
}

size_t diag_log_read(uint32_t *cursor, char *buf, size_t buf_size)
{
    return read_with_note(DIAG_LOG_THIS_BOOT, cursor, buf, buf_size);
}

size_t diag_log_read_prev_boot(uint32_t *cursor, char *buf, size_t buf_size)
{
    return read_with_note(DIAG_LOG_PREV_BOOT, cursor, buf, buf_size);
}

void diag_log_stats(diag_log_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
}
