/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics: remote log access, an event journal, core dump access, task
 * and heap statistics.
 *
 *  - Log tap: wraps the esp_log output function (esp_log_set_vprintf; the
 *    function it replaced is kept and used until the console is set up).
 *    Each formatted line is appended to a ring with a sequence number and
 *    then offered to the UART console. Readers poll with a cursor; nothing
 *    blocks the logger. The ring lives in PSRAM .noinit memory
 *    (EXT_RAM_NOINIT_ATTR, CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY; the
 *    PSRAM memory test skips that range) with a magic/sequence/CRC header and
 *    a check per line, in two banks: the one this boot writes and the
 *    previous boot's, so the lines leading up to a panic, watchdog reset or
 *    planned reboot can be read as "previous boot". At boot the previous
 *    bank is validated (header CRC, then each line; a line cut by the reset
 *    ends it) and a bank with another layout or size is discarded.
 *    PSRAM writes sit in the ESP32-S3's write-back data cache, and the
 *    restart path (including panic and watchdog resets) disables that cache
 *    without writing it back. So every appended record and the header are
 *    written back at once with esp_cache_msync(DIR_C2M | UNALIGNED, rounded
 *    to the cache line); otherwise exactly the last lines before a crash
 *    would be lost.
 *    Limit: with XIP from PSRAM the ring's physical place depends on the
 *    image layout (code and rodata are copied to PSRAM first), so the
 *    previous boot's lines do not survive a change of app image (OTA, trial
 *    revert, rollback). The event journal below is layout-independent and
 *    records those events and the reset reason instead.
 *    Lines longer than CONFIG_DIAG_LOG_LINE_MAX - 1 characters are cut
 *    (ending in "..."), on the console and in the ring alike.
 *  - Console: UART at 115200 through the UART driver with a TX buffer of
 *    CONFIG_DIAG_CONSOLE_TX_BUF bytes. Output is best-effort: a line goes to
 *    the UART only when the buffer has room for all of it, otherwise it is
 *    counted as a console drop (diag_log_stats) and the next line that fits
 *    is preceded by a note saying how many were dropped. The ring always
 *    keeps the line, so a burst never stalls the task that logs. Before a
 *    planned restart the buffer is given up to 300 ms to drain.
 *  - Event journal: fixed-size records in the raw "journal" partition
 *    (boots with reset class and reason, the previous session's uptime,
 *    reboot reason codes, ladder steps, app update/trial/revert/rollback
 *    results, receiver-flash results, safe-mode entries, Wi-Fi disconnect
 *    reasons, data-loss deltas). Survives power loss and image changes; the
 *    oldest records are overwritten, one 4 KB sector (127 records) at a
 *    time, in strict ring order so every sector wears equally. Appends are
 *    queued and written by a small task of diag's, so a caller never waits
 *    for flash; before a planned restart the queue is written out too (into
 *    the sector being written: the restart never waits for an erase).
 *    Two rate limits protect the flash: per kind
 *    (CONFIG_DIAG_JOURNAL_MIN_INTERVAL_MS) and an hourly budget over all
 *    kinds (CONFIG_DIAG_JOURNAL_BUDGET_PER_H, of which up to ten minutes'
 *    worth may come in one burst). A record either limit refuses is
 *    reported as not written (ESP_ERR_INVALID_STATE), so a caller that
 *    accumulates deltas keeps them pending. Ladder step records are exempt
 *    from both: one step's result and the next step's start fall in the
 *    same second, and the ladder bounds their rate itself.
 *    Records carry the uptime and, once a trusted clock is known
 *    (diag_set_utc, fed by the availability ledger), the UTC time.
 *    The journal is a timeline, not the boot history: the last 16 boots are
 *    kept in settings (life/boots), because a busy journal can overwrite
 *    boot records within hours.
 *  - Core dump: reports whether the coredump partition holds a dump from an
 *    earlier crash, its summary (task, PC, backtrace; checked once at boot)
 *    and lets the web UI download (for `idf.py coredump-info`) or erase it.
 *    A dump can contain secrets from RAM (Wi-Fi and caster passwords): admin
 *    only, with a warning in the UI.
 *  - Task stats: per-task priority, core, stack high-water mark, and CPU %
 *    from the 64-bit run-time counter between two snapshots.
 *  - Heap stats: free, minimum and largest free block, internal and PSRAM.
 *
 * Nothing logs from the GNSS RX hot path (counters only), and nothing here
 * logs on behalf of a caller.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Core dump summary. */
typedef struct {
    bool present;
    size_t size;
    char task[16];          /**< task that crashed */
    uint32_t pc;            /**< program counter at the fault */
    uint32_t backtrace[16];
    uint8_t backtrace_depth;
} diag_coredump_info_t;

/** One task's statistics. */
typedef struct {
    char name[16];
    uint8_t priority;
    int8_t core;            /**< -1 = not pinned */
    uint32_t stack_free_min;/**< bytes, high-water mark */
    uint8_t cpu_pct;        /**< since the previous snapshot, share of both cores together */
} diag_task_info_t;

/** Event journal record kinds. */
typedef enum {
    DIAG_EV_BOOT = 1,           /**< a = reset class, b = planned reboot reason (written at boot) */
    DIAG_EV_REBOOT,             /**< a = reason */
    /**
     * a = ladder (diag_ladder_t: 0 GNSS, 1 network, 2 caster),
     * b = (outcome << 8) | code; diag_ladder_b() builds it. The network
     * ladder adds its reason in bits 16-23 (see network.h).
     * GNSS journal step codes (diag_gnss_step_t; health.h names the rungs
     * 1-5, 6a, 6b, 7): 1-5 = rungs 1-5 (the recovery actions), 6 = rung 6a
     * (ESP32 reboot), 7 = rung 6b ("receiver needs a power cycle" alert),
     * 8 = rung 7 (FAILED, retry pass pending), 9 = rung 6a blocked by the
     * reboot cap. Outcome (diag_step_outcome_t): 0 started, 1 healthy again,
     * 2 failed (also: refused by the station without sending anything),
     * 3 abandoned because the ladder was suspended. Exempt from both rate
     * limits.
     */
    DIAG_EV_LADDER_STEP,
    DIAG_EV_APP_UPDATE,         /**< a = result (esp_err_t) */
    DIAG_EV_APP_REVERT,         /**< a = trial stage */
    DIAG_EV_GNSS_FLASH,         /**< a = result (esp_err_t), b = attempt */
    DIAG_EV_SAFE_MODE,          /**< a = episode */
    DIAG_EV_WIFI_DISCONNECT,    /**< a = wifi_err_reason_t, b = RSSI */
    DIAG_EV_DATA_LOSS,          /**< a = diag_loss_id_t, b = increase since the previous record for that id (clamped to INT32_MAX) */
    DIAG_EV_SESSION,            /**< right after DIAG_EV_BOOT: a = previous session uptime in s (-1 = unknown), b = raw esp_reset_reason_t */
} diag_event_t;

/**
 * Counter ids for DIAG_EV_DATA_LOSS records (health journals the deltas,
 * never the task that increments the counter). Units in brackets.
 */
typedef enum {
    DIAG_LOSS_HW_FIFO = 1,         /**< UART hardware FIFO overflows [events] */
    DIAG_LOSS_RX_BUFFER_FULL,      /**< UART driver buffer full [events] */
    DIAG_LOSS_LINE_ERRORS,         /**< framing/parity/break [events] */
    DIAG_LOSS_RTCM_CRC,            /**< RTCM frames failing CRC [frames] */
    DIAG_LOSS_FRAMER_DISCARDED,    /**< framer discarded bytes [bytes] */
    DIAG_LOSS_HUB_PUBLISH,         /**< frames the hub failed to publish [frames] */
    DIAG_LOSS_BAUD_SWITCH,         /**< bytes discarded after a baud change [bytes]; expected, never alerted */
    DIAG_LOSS_RX_EVENTS_DROPPED,   /**< UART event queue found full [episodes]; the event counts above may be low */
    DIAG_LOSS_CASTER_BASE = 16,    /**< ids from here on are reserved for per-caster losses */
} diag_loss_id_t;

/** DIAG_EV_LADDER_STEP `a`: which ladder. */
typedef enum {
    DIAG_LADDER_GNSS = 0,
    DIAG_LADDER_NETWORK = 1,
    DIAG_LADDER_CASTER = 2,
} diag_ladder_t;

/** GNSS ladder journal step codes (low byte of `b`). */
typedef enum {
    DIAG_GNSS_STEP_RESYNC = 1,            /**< rung 1 */
    DIAG_GNSS_STEP_LINK_RESTART = 2,      /**< rung 2 */
    DIAG_GNSS_STEP_RESCAN = 3,            /**< rung 3 */
    DIAG_GNSS_STEP_RECONFIGURE = 4,       /**< rung 4 */
    DIAG_GNSS_STEP_RECEIVER_RESET = 5,    /**< rung 5 */
    DIAG_GNSS_STEP_REBOOT = 6,            /**< rung 6a: ESP32 reboot */
    DIAG_GNSS_STEP_POWER_CYCLE = 7,       /**< rung 6b: "receiver needs a power cycle" alert */
    DIAG_GNSS_STEP_FAILED = 8,            /**< rung 7: FAILED, retry pass pending */
    DIAG_GNSS_STEP_REBOOT_BLOCKED = 9,    /**< rung 6a blocked by the reboot cap */
} diag_gnss_step_t;

/** Ladder step outcome (second byte of `b`). */
typedef enum {
    DIAG_STEP_STARTED = 0,
    DIAG_STEP_HEALTHY = 1,
    DIAG_STEP_FAILED = 2,       /**< also: refused without sending anything */
    DIAG_STEP_ABANDONED = 3,    /**< the ladder was suspended */
} diag_step_outcome_t;

/** `b` of a DIAG_EV_LADDER_STEP record. */
static inline int32_t diag_ladder_b(diag_step_outcome_t outcome, uint32_t code)
{
    return (int32_t)((((uint32_t)outcome & 0xFFu) << 8) | (code & 0xFFu));
}

/** One journal record as read back. */
typedef struct {
    uint32_t seq;
    int64_t unix_s;         /**< 0 if no trusted clock at the time */
    uint32_t uptime_s;
    uint16_t kind;          /**< diag_event_t */
    int32_t a;
    int32_t b;
} diag_journal_entry_t;

/** Journal state and counters (since boot unless noted). */
typedef struct {
    bool available;          /**< partition found and mounted */
    uint32_t capacity;       /**< records the ring holds (127 per 4 KB sector) */
    uint32_t oldest_seq;     /**< 0 = empty */
    uint32_t newest_seq;     /**< 0 = empty */
    uint32_t sector_cycles;  /**< erase cycles each sector has seen (over the journal's whole life) */
    uint32_t written;
    uint32_t queued;         /**< accepted, not yet on flash */
    uint32_t refused_kind;   /**< refused by the per-kind interval */
    uint32_t refused_budget; /**< refused by the hourly budget */
    uint32_t queue_full;     /**< refused because the write queue was full */
    uint32_t io_errors;
} diag_journal_info_t;

/** Which log ring to read. */
typedef enum {
    DIAG_LOG_THIS_BOOT = 0,
    DIAG_LOG_PREV_BOOT,
} diag_log_src_t;

/** What a log read found. */
typedef struct {
    bool gap;               /**< lines were overwritten before they were read (or the cursor was from another boot) */
    bool more;              /**< more lines are waiting: read again at once */
} diag_log_read_info_t;

/** Log counters since boot. */
typedef struct {
    uint32_t lines;                 /**< lines logged */
    uint32_t ring_overwritten;      /**< lines the ring dropped to make room */
    uint32_t cut;                   /**< lines cut to CONFIG_DIAG_LOG_LINE_MAX */
    uint32_t console_dropped;       /**< lines not sent to the console (TX buffer full) */
    uint32_t console_dropped_bytes;
    bool prev_boot_available;       /**< the previous boot's lines are readable */
    uint32_t prev_boot_lines;
} diag_log_stats_t;

/** Heap figures in bytes. */
typedef struct {
    uint32_t internal_free;
    uint32_t internal_min;          /**< low-water mark since boot */
    uint32_t internal_largest;      /**< largest free block (fragmentation) */
    uint32_t psram_free;
    uint32_t psram_min;
    uint32_t psram_largest;
} diag_heap_info_t;

/**
 * Install the log tap, set up the console, attach to the log ring (keeping
 * the previous boot's lines readable), mount the journal and start its
 * writer, check for a core dump. Call once, first thing in app_main.
 */
esp_err_t diag_init(void);

/**
 * Copy log text newer than *cursor into buf (whole lines, NUL-terminated).
 * Start with *cursor = 0; on return it points past the last copied line. If
 * the ring overwrote lines the reader had not seen, a note line saying so
 * comes first.
 *
 * @return number of bytes copied (excluding the terminator).
 */
size_t diag_log_read(uint32_t *cursor, char *buf, size_t buf_size);

/**
 * Same for the previous boot's log, preserved across the reset (0 if none,
 * e.g. after a power cut or when the previous boot ran a different app image).
 */
size_t diag_log_read_prev_boot(uint32_t *cursor, char *buf, size_t buf_size);

/**
 * Read either ring without note lines: gaps and "more" are reported in
 * `info` (optional). Same cursor rules as diag_log_read().
 */
size_t diag_log_read_ex(diag_log_src_t src, uint32_t *cursor, char *buf, size_t buf_size,
                        diag_log_read_info_t *info);

/** Log counters. */
void diag_log_stats(diag_log_stats_t *out);

/** Change a tag's runtime log level ("*" = all). */
esp_err_t diag_set_log_level(const char *tag, esp_log_level_t level);

/**
 * Append a journal record (never blocks; written to flash by diag's writer
 * task within milliseconds). Not from an interrupt handler.
 * @return ESP_OK when accepted; ESP_ERR_INVALID_STATE when a rate limit (per
 *         kind or the hourly budget) refused it (nothing written: keep the
 *         data and retry later; never for DIAG_EV_LADDER_STEP);
 *         ESP_ERR_NO_MEM when the write queue is full; ESP_ERR_NOT_FOUND
 *         when there is no usable journal partition.
 */
esp_err_t diag_journal_append(diag_event_t kind, int32_t a, int32_t b);

/**
 * Read records with seq > *cursor, oldest first; *count in = capacity, out =
 * filled. *cursor becomes the last returned seq (unchanged if none).
 * @return ESP_OK (also with 0 records); ESP_ERR_NOT_FOUND without a journal.
 */
esp_err_t diag_journal_read(uint32_t *cursor, diag_journal_entry_t *out, size_t *count);

/** Journal state and counters. */
void diag_journal_info(diag_journal_info_t *out);

/**
 * Trusted UTC now (seconds since 1970), from GNSS or SNTP. Journal records
 * carry it (extrapolated with the monotonic clock) from then on; before the
 * first call they carry 0. The availability ledger feeds it.
 */
void diag_set_utc(int64_t unix_s);

/** Summary of the stored core dump (present = false if none). */
esp_err_t diag_coredump_info(diag_coredump_info_t *out);

/** Read raw core dump bytes (ELF) for download; reads past the end fail with ESP_ERR_INVALID_SIZE. */
esp_err_t diag_coredump_read(size_t offset, void *buf, size_t len);

/** Erase the stored core dump. */
esp_err_t diag_coredump_erase(void);

/**
 * Snapshot task statistics; *count in = capacity, out = filled.
 * @return ESP_ERR_INVALID_SIZE (with the first *count tasks filled) when
 *         more tasks exist than fit.
 */
esp_err_t diag_task_stats(diag_task_info_t *tasks, size_t *count);

/** Heap figures. */
void diag_heap_stats(diag_heap_info_t *out);

#ifdef __cplusplus
}
#endif
