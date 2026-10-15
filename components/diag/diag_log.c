/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Log tap: the log rings (this boot's and the previous boot's, in PSRAM
 * .noinit memory; logbanks_t in diag_logring.h) and the console.
 * The previous boot's lines become readable only once lifecycle_init has
 * checked their image generation (diag_log_check_stamp).
 *
 * Every log line is formatted once, on the logging task's stack
 * (CONFIG_DIAG_LOG_LINE_MAX bytes), and appended to the ring under a
 * spinlock (memcpy plus a write-back of the touched cache lines, a few
 * microseconds). The console then sends the ring's records in order
 * (diag_console_core.h): the logger writes its own line to the UART driver's
 * transmit buffer only when nothing older waits and the whole line fits;
 * otherwise the line waits in the ring and the "console" task (stack in
 * PSRAM, priority below every station task) copies it out when the buffer
 * has room, blocking only itself. A line is lost only when the ring
 * overwrote it before it was sent, and one note at a line boundary says how
 * many. A mutex keeps the fast path and the writer apart; a logger never
 * waits for it for more than CONSOLE_WAIT_MS (the line then waits in the
 * ring instead).
 *
 * Text that is not a log line (diag_console_write: the bench console's
 * replies, gathered into whole lines first when printed in parts) goes to
 * the driver whole, under the same mutex and with the same line endings, so
 * no log line lands inside one of its lines; it may overtake log lines that
 * wait in the ring. printf and the other stdout calls still write to the
 * driver directly, one byte per driver call (the UART VFS), so a log line
 * can land inside their lines.
 *
 * With CONFIG_DIAG_CONSOLE_USB_MIRROR every write to the UART driver above
 * (wire_write) is copied to the built-in USB Serial/JTAG as well, under the
 * same mutex (diag_mirror_core.h). The copy goes to the USB driver's
 * transmit buffer whole or not at all and never waits: its driver's write
 * with no wait takes a mutex of its own that only a drain holds, and puts
 * the bytes in a ring buffer only when all of them fit (usb_serial_jtag.c,
 * usb_serial_jtag_write_bytes; ringbuf.c, xRingbufferSend). Without a host
 * (no USB frames: usb_serial_jtag_is_connected) nothing is copied. The
 * stdout calls above are not copied.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "diag.h"
#include "diag_console_core.h"
#include "diag_internal.h"
#include "diag_logring.h"
#include "diag_mirror_core.h"

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
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
#if CONFIG_DIAG_CONSOLE_USB_MIRROR
#include "driver/usb_serial_jtag.h"
#endif

#define BANK_BYTES        ((size_t)CONFIG_DIAG_LOG_RING_KB * 1024u)
#define READ_SLICE        (LOGRING_MAX_TEXT + 1u)   /* bytes copied per lock hold: one longest record */
#define LINE_SPARE        8u                        /* room to turn up to 8 newlines into CR LF in place */
#define CONSOLE_RX_BUF    256                       /* the driver needs more than the 128-byte FIFO */
#define CONSOLE_MARGIN    64u                       /* TX buffer item headers of one write */
#define CONSOLE_WAIT_MS   5                         /* another logger finishing its copy */
#define CONSOLE_TASK_PRIO 1                         /* below every station task (IDLE is 0) */
#define CONSOLE_POLL_MS   200                       /* the writer looks at the ring at least this often */
#define CONSOLE_RETRY_MS  10                        /* no room: about 115 bytes drain meanwhile */
#define DRAIN_STEP_MS     5
#define OTHER_WAIT_TRIES  50                        /* console text waits up to 0.5 s for room, then in the driver */
#define USB_RX_BUF        128                       /* the USB driver needs more than its 64-byte endpoint; input there is not read */
#define USB_DRAIN_MAX_MS  50                        /* a reading host takes the USB buffer in a few ms */

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
/* ring: this boot's, under s_lock. prev: the previous boot's; its records
 * are read-only after init, its read hints under s_lock. prev_ok is set
 * under s_lock by diag_log_check_stamp, before any reader starts. */
static logbanks_t s_banks;
static bool s_ring_ok;
static uint32_t s_cache_line = 32;

static diag_log_stats_t s_stats;    /* under s_lock */

#if CONFIG_ESP_CONSOLE_UART
#define CONSOLE_PORT ((uart_port_t)CONFIG_ESP_CONSOLE_UART_NUM)
#endif
static vprintf_like_t s_chain;      /* output function before ours: console until the driver is up */
static volatile bool s_console;     /* driver installed: write through it, never block */
static SemaphoreHandle_t s_console_mux;
static uint32_t s_console_pending;  /* under s_console_mux: drops not yet reported (no ring: best effort) */

/* The console writer (diag_console_core.h), under s_console_mux. */
static dcon_t s_con;
EXT_RAM_BSS_ATTR static char s_con_buf[DCON_BUF_SIZE];
static TaskHandle_t s_con_task;
static volatile bool s_con_ordered;  /* the console sends from the ring (else best effort, as without one) */
static volatile bool s_con_backlog;  /* hint: records wait for the writer, so loggers leave their lines to it */

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

    /* Each bank is stamped with its index and this image: when the image
     * changes, the banks' physical place can move by whole MMU pages (one
     * page can be one bank), so an old bank must not pass for the other
     * bank or for this image's. Whether this image's own bank comes from the
     * run right before this boot is decided later (diag_log_check_stamp). */
    uint8_t image[LOGRING_IMAGE_ID];
    memcpy(image, esp_app_get_description()->app_elf_sha256, sizeof(image));
    logbanks_open(&s_banks, mem, BANK_BYTES, image, BANKS_PERSIST != 0, ring_sync, NULL);
    s_ring_ok = true;
    s_stats.prev_boot_available = false;
    s_stats.prev_boot_lines = 0;
    return ESP_OK;
}

/* ---- console ----------------------------------------------------------------------------- */

#if CONFIG_DIAG_CONSOLE_USB_MIRROR
/* The copy on the built-in USB (diag_mirror_core.h), under s_console_mux. */
static bool s_usb;                  /* the USB driver is up: copy (set before the console starts) */
static dmir_t s_mir;
/* Scratch for one write with its line endings and a note: never more than
 * the USB driver's buffer holds, or no write of that size could ever fit. */
#define USB_MIR_BUF (DMIR_BUF_SIZE(DCON_BUF_SIZE) < (size_t)CONFIG_DIAG_CONSOLE_USB_TX_BUF \
                         ? DMIR_BUF_SIZE(DCON_BUF_SIZE)                                    \
                         : (size_t)CONFIG_DIAG_CONSOLE_USB_TX_BUF)
EXT_RAM_BSS_ATTR static char s_mir_buf[USB_MIR_BUF];

static bool usb_connected(void *ctx)
{
    (void)ctx;
    return usb_serial_jtag_is_connected();
}

static bool usb_put(void *ctx, const char *s, size_t len)
{
    (void)ctx;
    return usb_serial_jtag_write_bytes(s, len, 0) == (int)len;
}

static const dmir_port_t s_usb_port = {
    .connected = usb_connected,
    .put = usb_put,
    .ctx = NULL,
#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
    .eol = DMIR_EOL_CRLF,
#elif CONFIG_LIBC_STDOUT_LINE_ENDING_CR
    .eol = DMIR_EOL_CR,
#else
    .eol = DMIR_EOL_LF,
#endif
};

/* A console write, copied to the USB port (text before the line-ending rule). */
static void usb_copy(const char *s, size_t len)
{
    if (!s_usb || len == 0) {
        return;
    }
    uint32_t lost = s_mir.lost;
    dmir_write(&s_mir, &s_usb_port, s, len);
    if (s_mir.lost != lost) {
        portENTER_CRITICAL_SAFE(&s_lock);
        s_stats.usb_dropped = s_mir.lost;
        s_stats.usb_dropped_bytes = s_mir.lost_bytes > UINT32_MAX ? UINT32_MAX : (uint32_t)s_mir.lost_bytes;
        portEXIT_CRITICAL_SAFE(&s_lock);
    }
}

/* The driver for the copy, before the console starts. */
static esp_err_t usb_start(void)
{
    esp_err_t err = ESP_OK;
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = {
            .tx_buffer_size = CONFIG_DIAG_CONSOLE_USB_TX_BUF,
            .rx_buffer_size = USB_RX_BUF,
        };
        err = usb_serial_jtag_driver_install(&cfg);
    }
    if (err == ESP_OK) {
        dmir_init(&s_mir, s_mir_buf, sizeof(s_mir_buf));
        s_usb = true;
        portENTER_CRITICAL(&s_lock);
        s_stats.usb_mirror = true;
        portEXIT_CRITICAL(&s_lock);
    }
    return err;
}
#endif /* CONFIG_DIAG_CONSOLE_USB_MIRROR */

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
 * (room to expand LF to CR LF in place). Under s_console_mux. */
static void wire_write(char *s, size_t len, size_t cap)
{
#if CONFIG_DIAG_CONSOLE_USB_MIRROR
    usb_copy(s, len);   /* first: the line endings below are expanded in s itself */
#endif
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

#if CONFIG_ESP_CONSOLE_UART
/* Without a log ring (it could not be set up): best effort as the only way,
 * a line that does not fit is dropped and counted, and the next one that
 * fits says how many. Under s_console_mux. */
static void best_effort_put(char *s, size_t len, size_t cap)
{
    size_t room = 0;
    if (uart_get_tx_buffer_free_size(CONSOLE_PORT, &room) != ESP_OK) {
        room = 0;
    }
    size_t need = wire_len(s, len) + CONSOLE_MARGIN;
    if (s_console_pending > 0) {
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
}

/* ---- the console writer (ordered from the ring) ---- */

/* Line endings expanded into one buffer, so a batch costs one driver write
 * (each write takes item headers of the driver's transmit buffer). */
EXT_RAM_BSS_ATTR static char s_con_wire[2u * DCON_BUF_SIZE];

static size_t con_room(void *ctx)
{
    (void)ctx;
    size_t room = 0;
    if (uart_get_tx_buffer_free_size(CONSOLE_PORT, &room) != ESP_OK) {
        room = 0;
    }
    return room;
}

static void con_write(void *ctx, const char *s, size_t len)
{
    (void)ctx;
    if (len > DCON_BUF_SIZE) {
        len = DCON_BUF_SIZE;   /* a batch is never longer */
    }
    memcpy(s_con_wire, s, len);
    wire_write(s_con_wire, len, sizeof(s_con_wire));
}

static const dcon_tx_t s_con_tx = {
    .room = con_room,
    .write = con_write,
    .ctx = NULL,
    .total = CONFIG_DIAG_CONSOLE_TX_BUF,
    .margin = CONSOLE_MARGIN,
#if CONFIG_LIBC_STDOUT_LINE_ENDING_CRLF
    .crlf = true,
#else
    .crlf = false,
#endif
};

static void ring_lock(void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
}

static void ring_unlock(void *ctx)
{
    (void)ctx;
    portEXIT_CRITICAL(&s_lock);
}

/* One writer step. Only from a task that may block. */
static dcon_pump_t con_pump(TickType_t wait)
{
    if (xSemaphoreTake(s_console_mux, wait) != pdTRUE) {
        return DCON_BUSY;
    }
    dcon_loss_t loss = { 0 };
    dcon_pump_t r = dcon_pump(&s_con, &s_banks.ring, ring_lock, ring_unlock, NULL, &s_con_tx, &loss);
    if (r == DCON_IDLE) {
        s_con_backlog = false;
    }
    xSemaphoreGive(s_console_mux);
    if (loss.lost > 0) {
        portENTER_CRITICAL(&s_lock);
        s_stats.console_dropped += loss.lost;
        uint64_t b = (uint64_t)s_stats.console_dropped_bytes + loss.lost_bytes;
        s_stats.console_dropped_bytes = b > UINT32_MAX ? UINT32_MAX : (uint32_t)b;
        portEXIT_CRITICAL(&s_lock);
    }
    return r;
}

static void console_task(void *arg)
{
    (void)arg;
    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CONSOLE_POLL_MS));
        dcon_pump_t r;
        while ((r = con_pump(portMAX_DELAY)) != DCON_IDLE) {
            if (r == DCON_BUSY) {
                vTaskDelay(pdMS_TO_TICKS(CONSOLE_RETRY_MS));
            }
        }
    }
}

static void wake_writer(void)
{
    TaskHandle_t t = s_con_task;
    if (t != NULL) {
        xTaskNotifyGive(t);
    }
}

/* Text that is not a log line, under s_console_mux: through the batch's
 * wire buffer (free while the mutex is held), the line endings expanded as
 * for the log. Every driver write of it happens under the mutex, so no log
 * line comes between them. */
static void other_write(const char *s, size_t len)
{
    while (len > 0) {
        size_t k = len < DCON_BUF_SIZE ? len : DCON_BUF_SIZE;
        memcpy(s_con_wire, s, k);
        wire_write(s_con_wire, k, sizeof(s_con_wire));
        s += k;
        len -= k;
    }
}
#endif /* CONFIG_ESP_CONSOLE_UART */

void diag_console_write(const char *s, size_t len)
{
    if (s == NULL || len == 0 || xPortInIsrContext()) {
        return;
    }
    if (!s_console) {
        /* No console of ours (yet): stdout as it is. */
        (void)fwrite(s, 1, len, stdout);
        (void)fflush(stdout);
        return;
    }
#if CONFIG_ESP_CONSOLE_UART
    if (!xPortCanYield() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return;   /* would have to wait for the mutex or the driver */
    }
    /* The mutex is held only while all of it fits the transmit buffer at
     * once, so a logger waiting for the mutex never waits for the UART:
     * without the room, let go and look again (the writer and the fast path
     * go on meanwhile). Text that can never fit at once, or that waited
     * OTHER_WAIT_TRIES times, waits in the driver under the mutex: loggers
     * then leave their lines to the writer after CONSOLE_WAIT_MS. */
    size_t need = wire_len(s, len) + 2u + CONSOLE_MARGIN * (2u + len / DCON_BUF_SIZE);
    for (unsigned tries = 0;; tries++) {
        (void)xSemaphoreTake(s_console_mux, portMAX_DELAY);
        if (con_room(NULL) >= need || need > CONFIG_DIAG_CONSOLE_TX_BUF || tries >= OTHER_WAIT_TRIES) {
            break;
        }
        xSemaphoreGive(s_console_mux);
        vTaskDelay(pdMS_TO_TICKS(CONSOLE_RETRY_MS));
    }
    if (s_con_ordered && dcon_other_text(&s_con, s, len)) {
        other_write("\n", 1);
    }
    other_write(s, len);
    xSemaphoreGive(s_console_mux);
#endif
}

/* ---- lines gathered from pieces (diag_console_printf) ---------------------------------- */

static void lines_emit(void *ctx, const char *s, size_t len)
{
    (void)ctx;
    diag_console_write(s, len);
}

static void lines_view(diag_console_lines_t *l, dcon_lines_t *g)
{
    dcon_lines_init(g, l->buf, sizeof(l->buf));
    g->len = l->len < sizeof(l->buf) ? l->len : sizeof(l->buf);
}

void diag_console_put(diag_console_lines_t *l, const char *s, size_t len)
{
    if (l == NULL) {
        return;
    }
    dcon_lines_t g;
    lines_view(l, &g);
    dcon_lines_put(&g, s, len, lines_emit, NULL);
    l->len = g.len;
}

void diag_console_vprintf(diag_console_lines_t *l, const char *fmt, va_list ap)
{
    if (l == NULL || fmt == NULL) {
        return;
    }
    dcon_lines_t g;
    lines_view(l, &g);
    /* Straight into the gatherer when it fits (with its terminator), else
     * formatted again into a heap buffer of its length. */
    size_t room = g.cap - g.len;
    va_list cp;
    va_copy(cp, ap);
    int n = vsnprintf(g.buf + g.len, room, fmt, cp);
    va_end(cp);
    if (n < 0) {
        return;
    }
    if ((size_t)n < room) {
        dcon_lines_added(&g, (size_t)n, lines_emit, NULL);
        l->len = g.len;
        return;
    }
    char *tmp = malloc((size_t)n + 1u);
    if (tmp != NULL) {
        va_copy(cp, ap);
        int m = vsnprintf(tmp, (size_t)n + 1u, fmt, cp);
        va_end(cp);
        if (m > 0) {
            dcon_lines_put(&g, tmp, (size_t)m < (size_t)n ? (size_t)m : (size_t)n, lines_emit, NULL);
        }
        free(tmp);
    } else {
        /* No memory for it: the part that fit, gathered as it stands. */
        dcon_lines_added(&g, room > 0 ? room - 1u : 0, lines_emit, NULL);
    }
    l->len = g.len;
}

void diag_console_printf(diag_console_lines_t *l, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    diag_console_vprintf(l, fmt, ap);
    va_end(ap);
}

void diag_console_flush(diag_console_lines_t *l, bool end_line)
{
    if (l == NULL) {
        return;
    }
    dcon_lines_t g;
    lines_view(l, &g);
    dcon_lines_flush(&g, end_line, lines_emit, NULL);
    l->len = g.len;
}

/* Record `seq` of the ring (0 = not in the ring) to the console. */
static void console_put(uint32_t seq, char *s, size_t len, size_t cap, const char *fmt, va_list ap)
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
    (void)fmt;
    (void)ap;
    /* Never wait inside a critical section or with the scheduler stopped. */
    bool can_wait = xPortCanYield() && xTaskGetSchedulerState() == taskSCHEDULER_RUNNING;
    if (!s_con_ordered || seq == 0) {
        if (!can_wait || xSemaphoreTake(s_console_mux, pdMS_TO_TICKS(CONSOLE_WAIT_MS)) != pdTRUE) {
            count_drop(len);
            return;
        }
        best_effort_put(s, len, cap);
        xSemaphoreGive(s_console_mux);
        return;
    }
    /* The line is in the ring: it reaches the console now, or from there
     * through the writer, in record order either way. */
    if (!can_wait) {
        s_con_backlog = true;   /* the writer looks again within CONSOLE_POLL_MS */
        return;
    }
    if (s_con_backlog || xSemaphoreTake(s_console_mux, pdMS_TO_TICKS(CONSOLE_WAIT_MS)) != pdTRUE) {
        s_con_backlog = true;
        wake_writer();
        return;
    }
    if (dcon_direct_ok(&s_con, seq, wire_len(s, len), con_room(NULL), CONSOLE_MARGIN)) {
        dcon_sent(&s_con, seq, s, len);   /* before the line endings are expanded in place */
        wire_write(s, len, cap);
    } else {
        s_con_backlog = true;
        wake_writer();
    }
    xSemaphoreGive(s_console_mux);
#else
    (void)seq;
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

    uint32_t seq = 0;
    if (s_ring_ok) {
        portENTER_CRITICAL_SAFE(&s_lock);
        uint32_t lost = s_banks.ring.hdr->lost;
        seq = logring_append(&s_banks.ring, line, len);
        s_stats.lines++;
        s_stats.ring_overwritten += s_banks.ring.hdr->lost - lost;
        s_stats.cut += cut ? 1u : 0u;
        portEXIT_CRITICAL_SAFE(&s_lock);
    }
    console_put(seq, line, len, sizeof(line), fmt, ap);
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
#if CONFIG_DIAG_CONSOLE_USB_MIRROR
    esp_err_t uerr = ESP_ERR_INVALID_STATE;   /* the console did not start */
#endif
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
    bool writer = false;
    bool internal_stack = false;
    if (cerr == ESP_OK) {
        /* The console sends the ring's records from here on (nothing is in
         * the ring yet: the tap is installed below). */
        dcon_init(&s_con, s_banks.ring.hdr->next_seq - 1u, s_con_buf, sizeof(s_con_buf));
        TaskHandle_t t = NULL;
        /* The writer's stack in PSRAM: it only copies lines into the UART
         * driver's transmit buffer and never touches flash, so the cache is
         * never off while it runs. Internal RAM only when PSRAM has no room. */
        writer = xTaskCreatePinnedToCoreWithCaps(console_task, "console", CONFIG_DIAG_CONSOLE_TASK_STACK, NULL,
                                                 CONSOLE_TASK_PRIO, &t, tskNO_AFFINITY,
                                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS;
        if (!writer) {
            internal_stack = true;
            writer = xTaskCreatePinnedToCore(console_task, "console", CONFIG_DIAG_CONSOLE_TASK_STACK, NULL,
                                             CONSOLE_TASK_PRIO, &t, tskNO_AFFINITY) == pdPASS;
        }
        s_con_task = writer ? t : NULL;
        s_con_ordered = writer;
#if CONFIG_DIAG_CONSOLE_USB_MIRROR
        uerr = usb_start();
#endif
        /* printf and friends go through the driver too, so their bytes queue
         * behind the log lines handed over so far instead of racing them
         * into the FIFO (lines still waiting in the ring can come later). */
        uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
        s_console = true;
    }
#endif
    s_chain = esp_log_set_vprintf(diag_vprintf);
#if CONFIG_ESP_CONSOLE_UART
    if (cerr != ESP_OK) {
        ESP_LOGE("diag", "console driver not installed (%s): console output may block", esp_err_to_name(cerr));
    } else if (!writer) {
        ESP_LOGE("diag", "console task not started: lines that do not fit the transmit buffer are dropped");
    } else if (internal_stack) {
        ESP_LOGW("diag", "console task stack in internal RAM: no room for it in PSRAM");
    }
#if CONFIG_DIAG_CONSOLE_USB_MIRROR
    if (uerr == ESP_OK) {
        ESP_LOGI("diag", "console copied to the built-in USB (%s now)",
                 usb_serial_jtag_is_connected() ? "a host is attached" : "no host");
    } else {
        ESP_LOGW("diag", "console not copied to the built-in USB: %s", esp_err_to_name(uerr));
    }
#endif
#endif
    if (s_banks.prev_found) {
        ESP_LOGI("diag", "previous boot's log found: %u lines (readable once its image generation is checked)",
                 (unsigned)logring_count(&s_banks.prev));
    }
    return ESP_OK;
}

void diag_log_drain_console(uint32_t ms)
{
#if CONFIG_ESP_CONSOLE_UART
    if (!s_console || !xPortCanYield() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
        return;
    }
    TickType_t t0 = xTaskGetTickCount();
    TickType_t budget = pdMS_TO_TICKS(ms);
    /* First what still waits in the ring, written from this task (the
     * console task's low priority must not decide whether a planned
     * restart's last lines go out), then the transmit buffer itself. */
    while (s_con_ordered && (xTaskGetTickCount() - t0) < budget) {
        dcon_pump_t r = con_pump(budget - (xTaskGetTickCount() - t0));
        if (r == DCON_IDLE) {
            break;
        }
        if (r == DCON_BUSY) {
            vTaskDelay(pdMS_TO_TICKS(DRAIN_STEP_MS));
        }
    }
    TickType_t used = xTaskGetTickCount() - t0;
    if (used < budget) {
        (void)uart_wait_tx_done(CONSOLE_PORT, budget - used);
    }
#if CONFIG_DIAG_CONSOLE_USB_MIRROR
    /* Then the USB copy's buffer, briefly and only to a host that is there
     * (one that does not read would hold it the whole time). */
    used = xTaskGetTickCount() - t0;
    if (s_usb && used < budget && usb_serial_jtag_is_connected()) {
        TickType_t left = budget - used;
        TickType_t cap = pdMS_TO_TICKS(USB_DRAIN_MAX_MS);
        (void)usb_serial_jtag_wait_tx_done(left < cap ? left : cap);
    }
#endif
#else
    (void)ms;
#endif
}

/* ---- readers ------------------------------------------------------------------------------ */

static void read_lock(void *ctx)
{
    (void)ctx;
    portENTER_CRITICAL(&s_lock);
}

static void read_unlock(void *ctx)
{
    (void)ctx;
    portEXIT_CRITICAL(&s_lock);
}

void diag_log_check_stamp(bool (*stamp_ok)(uint32_t stamp), uint32_t stamp)
{
    if (!s_ring_ok) {
        return;
    }
    logbanks_verdict_t v = logbanks_check_stamp(&s_banks, stamp_ok, stamp, read_lock, read_unlock, NULL);
    uint32_t lines = (v != LOGBANKS_PREV_NONE) ? logring_count(&s_banks.prev) : 0;
    portENTER_CRITICAL(&s_lock);
    s_stats.prev_boot_available = (v == LOGBANKS_PREV_KEPT);
    s_stats.prev_boot_lines = (v == LOGBANKS_PREV_KEPT) ? lines : 0;
    portEXIT_CRITICAL(&s_lock);
    if (v == LOGBANKS_PREV_KEPT) {
        ESP_LOGI("diag", "previous boot's log kept: %u lines", (unsigned)lines);
    } else if (v == LOGBANKS_PREV_STALE) {
        ESP_LOGW("diag", "previous boot's log set aside (%u lines): written before another app image ran, or "
                         "not checkable without settings",
                 (unsigned)lines);
    }
}

size_t diag_log_read_ex(diag_log_src_t src, uint32_t *cursor, char *buf, size_t buf_size,
                        diag_log_read_info_t *info)
{
    diag_log_read_info_t res = { 0 };
    if (buf != NULL && buf_size > 0) {
        buf[0] = '\0';
    }
    bool live = (src == DIAG_LOG_THIS_BOOT);
    if (cursor == NULL || buf == NULL || buf_size < 2 || !(live ? s_ring_ok : s_banks.prev_ok)) {
        if (info != NULL) {
            *info = res;
        }
        return 0;
    }
    /* Short lock holds: one slice at a time, so a long read never delays a
     * logger much; the read hints keep each hold from walking the ring. The
     * previous boot's ring takes the lock too: readers on two tasks share
     * its hints. */
    logring_read_t ri;
    size_t pos = logring_read_sliced(live ? &s_banks.ring : &s_banks.prev, cursor, buf, buf_size, READ_SLICE,
                                     read_lock, read_unlock, NULL, &ri);
    res.gap = ri.gap;
    res.more = ri.more;
    if (info != NULL) {
        *info = res;
    }
    return pos;
}

/* Would a read from this cursor skip lines that were overwritten? */
static bool gap_ahead(const logring_t *r, uint32_t cursor)
{
    if (cursor == 0) {
        return false;
    }
    portENTER_CRITICAL(&s_lock);
    uint32_t first = r->hdr->first_seq;
    uint32_t next = r->hdr->next_seq;
    portEXIT_CRITICAL(&s_lock);
    return cursor + 1u < first || cursor + 1u > next;
}

static size_t read_with_note(diag_log_src_t src, uint32_t *cursor, char *buf, size_t buf_size)
{
    static const char k_note[] = "[diag: earlier lines were overwritten before they were read]\n";
    bool live = (src == DIAG_LOG_THIS_BOOT);
    if (cursor != NULL && buf != NULL && buf_size > sizeof(k_note) + 1u && (live ? s_ring_ok : s_banks.prev_ok) &&
        gap_ahead(live ? &s_banks.ring : &s_banks.prev, *cursor)) {
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

uint32_t diag_log_head(void)
{
    if (!s_ring_ok) {
        return 0;
    }
    portENTER_CRITICAL(&s_lock);
    uint32_t first = s_banks.ring.hdr->first_seq;
    uint32_t next = s_banks.ring.hdr->next_seq;
    portEXIT_CRITICAL(&s_lock);
    return next == first ? (first > 1u ? first - 1u : 0u) : next - 1u;
}

void diag_log_stats(diag_log_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
#if CONFIG_DIAG_CONSOLE_USB_MIRROR
    out->usb_connected = out->usb_mirror && usb_serial_jtag_is_connected();
#endif
}
