/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Diagnostics: remote log access, an event journal, core dump access, task
 * statistics.
 *
 *  - Log tap: wraps the esp_log output function so each formatted line still
 *    goes to the UART console and is also appended to a ring with a sequence
 *    number. Readers poll with a cursor; nothing blocks the logger. The ring
 *    lives in PSRAM .noinit memory (EXT_RAM_NOINIT_ATTR,
 *    CONFIG_SPIRAM_ALLOW_NOINIT_SEG_EXTERNAL_MEMORY; the PSRAM memory test
 *    skips that range) with a magic/sequence/CRC header, so the lines leading
 *    up to a panic, watchdog reset or planned reboot survive into the next
 *    boot and can be read as "previous boot".
 *    PSRAM writes sit in the ESP32-S3's write-back data cache, and the
 *    restart path (including panic and watchdog resets) disables that cache
 *    without writing it back. So every appended record and the header are
 *    written back at once with esp_cache_msync(DIR_C2M | UNALIGNED, rounded
 *    to the 32-byte cache line); otherwise exactly the last lines before a
 *    crash would be lost.
 *    Limit: with XIP from PSRAM the ring's physical place depends on the
 *    image layout (code and rodata are copied to PSRAM first), so the
 *    previous boot's lines do not survive a change of app image (OTA, trial
 *    revert, rollback). The event journal below is layout-independent and
 *    records those events and the reset reason instead.
 *  - Event journal: rate-limited, fixed-size records in the raw "journal"
 *    partition (boots with reset class and reason, the previous session's
 *    uptime, reboot reason codes, ladder steps, app update/trial/revert/
 *    rollback results, receiver-flash results, safe-mode entries, Wi-Fi
 *    disconnect reasons, data-loss deltas). Survives power loss and image
 *    changes; the oldest records are overwritten. The rate limit is per kind
 *    (CONFIG_DIAG_JOURNAL_MIN_INTERVAL_MS); a record it drops is reported as
 *    not written, so a caller that accumulates deltas keeps them pending.
 *    Ladder step records are exempt: one step's result and the next step's
 *    start fall in the same second, and the ladder bounds their rate itself.
 *    The journal is a timeline, not the boot history: the last 16 boots are
 *    kept in settings (life/boots), because a busy journal can overwrite
 *    boot records within hours.
 *  - Core dump: reports whether the coredump partition holds a dump from an
 *    earlier crash, its summary (task, PC, backtrace) and lets the web UI
 *    download (for `idf.py coredump-info`) or erase it. A dump can contain
 *    secrets from RAM (Wi-Fi and caster passwords): admin only, with a
 *    warning in the UI.
 *  - Task stats: per-task state, priority, core (xTaskGetCoreID), stack
 *    high-water mark, and CPU % computed from the 64-bit run-time counter
 *    between two snapshots.
 *
 * Nothing logs from the GNSS RX hot path (counters only). The console runs at
 * 115200, so console output is best-effort: a line is written to the UART only
 * when its TX buffer has room, otherwise it is counted as a console drop. The
 * log ring always keeps the line, so a burst never stalls the task that logs.
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
    uint8_t cpu_pct;        /**< since the previous snapshot */
} diag_task_info_t;

/** Event journal record kinds. */
typedef enum {
    DIAG_EV_BOOT = 1,           /**< a = reset class, b = planned reboot reason (written at boot) */
    DIAG_EV_REBOOT,             /**< a = reason */
    /**
     * a = ladder (0 GNSS, 1 network, 2 caster), b = (outcome << 8) | code.
     * GNSS journal step codes (health.h names the rungs 1-5, 6a, 6b, 7):
     * 1-5 = rungs 1-5 (the recovery actions), 6 = rung 6a (ESP32 reboot),
     * 7 = rung 6b ("receiver needs a power cycle" alert), 8 = rung 7
     * (FAILED, retry pass pending), 9 = rung 6a blocked by the reboot cap.
     * Outcome: 0 started, 1 healthy again, 2 failed (also: refused by the
     * station without sending anything), 3 abandoned because the ladder was
     * suspended. Exempt from the per-kind rate limit.
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

/** One journal record as read back. */
typedef struct {
    uint32_t seq;
    int64_t unix_s;         /**< 0 if no trusted clock at the time */
    uint32_t uptime_s;
    uint16_t kind;          /**< diag_event_t */
    int32_t a;
    int32_t b;
} diag_journal_entry_t;

/** Install the log tap and attach to the ring. Safe to call before anything else. */
esp_err_t diag_init(void);

/**
 * Copy log text newer than *cursor into buf (whole lines, NUL-terminated).
 * Start with *cursor = 0; on return it points past the last copied line. If
 * the ring overwrote lines the reader had not seen, the gap is marked.
 *
 * @return number of bytes copied (excluding the terminator).
 */
size_t diag_log_read(uint32_t *cursor, char *buf, size_t buf_size);

/**
 * Same for the previous boot's log, preserved across the reset (0 if none,
 * e.g. after a power cut or when the previous boot ran a different app image).
 */
size_t diag_log_read_prev_boot(uint32_t *cursor, char *buf, size_t buf_size);

/** Change a tag's runtime log level ("*" = all). */
esp_err_t diag_set_log_level(const char *tag, esp_log_level_t level);

/**
 * Append a journal record (never blocks for long).
 * @return ESP_OK when written; ESP_ERR_INVALID_STATE when the per-kind rate
 *         limit dropped it (nothing written: keep the data and retry later;
 *         never for DIAG_EV_LADDER_STEP); other errors when the journal is
 *         unavailable.
 */
esp_err_t diag_journal_append(diag_event_t kind, int32_t a, int32_t b);

/** Read records with seq > *cursor, oldest first; *count in = capacity, out = filled. */
esp_err_t diag_journal_read(uint32_t *cursor, diag_journal_entry_t *out, size_t *count);

/** Summary of the stored core dump (present = false if none). */
esp_err_t diag_coredump_info(diag_coredump_info_t *out);

/** Read raw core dump bytes (ELF) for download. */
esp_err_t diag_coredump_read(size_t offset, void *buf, size_t len);

/** Erase the stored core dump. */
esp_err_t diag_coredump_erase(void);

/** Snapshot task statistics; *count in = capacity, out = filled. */
esp_err_t diag_task_stats(diag_task_info_t *tasks, size_t *count);

#ifdef __cplusplus
}
#endif
