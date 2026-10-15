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
 *    revert, rollback): each bank's header names its bank and the image
 *    that wrote it, and a bank from another image, or another bank's copy
 *    moved into its place, is discarded. The event journal below is
 *    layout-independent and records those events and the reset reason
 *    instead.
 *    Image generation: when this image runs again after another one ran (A,
 *    then B, then A), A can find its own banks from before B intact, and
 *    they would pass for the previous boot's although B's boot came after
 *    them. So each bank also carries the image generation of the run that
 *    wrote it (lifecycle's, the RTC blocks' stamp). diag starts before the
 *    settings, when the generation is not known yet: the previous boot's
 *    lines stay unreadable until lifecycle_init has checked their stamp
 *    (diag_log_check_stamp), and lines from before another image's run are
 *    then reported as unavailable, as after a power cut (never shown with a
 *    label: a reader cannot tell how much older they are).
 *    Lines longer than CONFIG_DIAG_LOG_LINE_MAX - 1 characters are cut
 *    (ending in "..."), on the console and in the ring alike.
 *  - Console: UART at 115200 through the UART driver with a TX buffer of
 *    CONFIG_DIAG_CONSOLE_TX_BUF bytes, fed from the log ring in record
 *    order: a line goes straight to the buffer only when nothing older
 *    waits and all of it fits; otherwise it waits in the ring and a
 *    low-priority "console" task sends it as the buffer drains, so a burst
 *    never stalls the task that logs and nothing is dropped while the ring
 *    still holds it. Lines are lost only when the ring overwrote them before
 *    they were sent: they count as console drops (diag_log_stats), and one
 *    note "[diag: N console lines dropped]" says so, always at the start of
 *    a line. Console text that is not a log line and must stay readable
 *    (the bench console's replies, the receiver flasher's byte echo in bench
 *    builds) goes through diag_console_write: whole, under the same lock,
 *    so no log line lands inside one of its lines. printf output goes to
 *    the driver directly, one byte per driver write: a log line can land
 *    inside a printf line, and printf output can overtake lines that wait
 *    in the ring. Before a planned restart the waiting lines and the buffer
 *    are given up to 300 ms to go out.
 *    With CONFIG_DIAG_CONSOLE_USB_MIRROR (bench builds) everything this
 *    console writes (not printf output) is copied to the chip's built-in
 *    USB Serial/JTAG too, which the board's ESP32S3 USB-C port reaches with
 *    the plug the other way round. The copy never waits: a write goes to
 *    the USB driver's buffer whole or not at all; without a host there
 *    nothing is sent, and what a host that does not read leaves no room for
 *    is dropped, counted (diag_log_stats: usb_dropped) and announced there
 *    by "[diag: N lines not sent on USB]". Input stays on UART0.
 *  - Event journal: fixed-size records in the raw "journal" partition
 *    (boots with reset class and reason, the previous session's uptime,
 *    reboot reason codes, ladder steps, app update/trial/revert/rollback
 *    results, receiver-flash results, safe-mode entries, Wi-Fi disconnect
 *    reasons, data-loss deltas, and the connection history: caster sessions
 *    ending and starting again, the station getting its IP back). Survives power loss and image changes; the
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
 *    same second, and the ladder bounds their rate itself. Receiver-flash
 *    results are exempt too: one per session the user started, and the
 *    station writes it before it closes the session's own record, so a
 *    refused one would leave no trace of how the update ended. So is the
 *    power-on verdict (DIAG_EV_RX_BOOT): one per power-on boot, and the
 *    power-lost alert counts a boot only with it.
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

#include <stdarg.h>
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
     * ladder adds its reason in bits 16-23 (see network.h); its code 0
     * marks a pass start, with the triggers holding then in the reason
     * (bit 0 protected caster down, bit 1 associated without an IP, bit 2 a
     * slot not reaching its caster).
     * GNSS journal step codes (diag_gnss_step_t; health.h names the rungs
     * 1-5, 6a, 6b, 7): 1-5 = rungs 1-5 (the recovery actions), 6 = rung 6a
     * (ESP32 reboot), 7 = rung 6b ("receiver needs a power cycle" alert),
     * 8 = rung 7 (FAILED, retry pass pending), 9 = rung 6a blocked by the
     * reboot cap or a held reboot inhibit (or, with outcome 4, not taken
     * because of the user's recovery mode, or, with outcome 5, held back
     * for now). Code 9 with outcome 0 names what blocked it in bits 16-23
     * of b: 1 the cap's hourly limit, 2 its daily limit (3 in 24 h), 3 a
     * restart lock (a reboot inhibit held); 0 = not recorded (older
     * records). Outcome (diag_step_outcome_t): 0 started, 1 healthy again,
     * 2 failed (also: refused by the station without sending anything),
     * 3 abandoned because the ladder was suspended, 4 not run because of
     * the user's recovery mode (gnss/recovery: the step the full ladder
     * would have started), 5 held (code 9 only): b = (hold << 16) |
     * (5 << 8) | 9, the hold bits 0 first setup not finished, 1 a client on
     * the station's access point, 2 the web page in use, 3 the first hour
     * after power-on with nothing heard from the receiver. Exempt from both
     * rate limits.
     */
    DIAG_EV_LADDER_STEP,
    DIAG_EV_APP_UPDATE,         /**< a = result (esp_err_t) */
    DIAG_EV_APP_REVERT,         /**< a = trial stage (fw_update_trial_t; 0 = the user's rollback), b = 1 for the user's
                                     rollback; a stage-1 rollback the bootloader did: a = confirming, b = the attempt
                                     (1, 2); a revert with no other image to go back to: b = the esp_err_t; else 0 */
    DIAG_EV_GNSS_FLASH,         /**< a = outcome (station_flash_outcome_t), b = transfers */
    DIAG_EV_SAFE_MODE,          /**< a = episode */
    DIAG_EV_WIFI_DISCONNECT,    /**< a = wifi_err_reason_t, b = RSSI */
    DIAG_EV_DATA_LOSS,          /**< a = diag_loss_id_t, b = increase since the previous record for that id (clamped to INT32_MAX) */
    DIAG_EV_SESSION,            /**< right after DIAG_EV_BOOT: a = previous session uptime in s (-1 = unknown), b = raw esp_reset_reason_t */
    /**
     * A caster session that reached STREAMING ended (any reason):
     * a = diag_caster_a(slot, end class, short sessions not journalled since
     * the slot's previous DOWN), b = session length in s. End class:
     * ntrip_fail_class_t of a failure; 0 when the end was not a caster
     * failure (pause, configuration change, health restart, shutdown, the
     * receiver's RTCM stopping, the network going down: the records beside
     * it tell which). Sessions shorter than 60 s are journalled only when
     * the slot wrote no DOWN in the last 10 min; the skipped ones are counted
     * into the next DOWN written.
     */
    DIAG_EV_CASTER_DOWN = 11,
    /**
     * A caster session reached STREAMING after at least one failed attempt,
     * or at least 10 s after the slot's previous session ended:
     * a = diag_caster_a(slot, 0, failed attempts since that session),
     * b = outage in s (from the previous session's end, or from the first
     * failed attempt when the slot had not streamed this boot). After a
     * session shorter than 60 s it is journalled only when the slot wrote no
     * UP in the last 10 min.
     */
    DIAG_EV_CASTER_UP = 12,
    DIAG_EV_WIFI_UP = 13,       /**< the station got its IP back (not the first IP of a boot): a = connect attempts during the outage, b = outage in s */
    /**
     * The Onocoy login guard stopped a caster slot (no attempt until the user
     * saves it, retries it or disables it): a = slot index (bits 0-7) | stop
     * reason (ntrip_stop_t, bits 8-15), b = 0. Written once per stop, not
     * again when a stopped slot is restored at boot.
     */
    DIAG_EV_CASTER_STOP = 14,
    /**
     * Receiver maintenance state (the user works on the receiver through
     * its other port; nothing is sent on the station's link): a = 1 entry,
     * 0 exit; b = the chosen duration (entry) or the time spent (exit), in
     * seconds. Kind values are stored in the journal: never renumbered.
     */
    DIAG_EV_RX_MAINT = 20,
    /** Live view: a = diag_live_ev_t | (detail << 8), b as diag_live_ev_t says. */
    DIAG_EV_LIVE_VIEW = 21,
    /**
     * Signal groups: a = event (bits 0-7) | master << 8 | slave << 16.
     * Events: 1 a comparison started (the group measured first; b = the
     * measuring time per group in s), 2 it switched the receiver (b = the
     * phase, 0 or 1), 3 the receiver refused a signal group (in a
     * comparison or any apply; b = (master << 8) | slave of the group the
     * profile went back to, -1 none), 4 a comparison ended (the group it
     * ended on; b = outcome: 1 kept, 2 timed out, 3 cancelled, 4 rejected,
     * 5 failed, 6 interrupted).
     */
    DIAG_EV_SG_TEST = 22,
    /**
     * After a power-on reset, whether the receiver kept running (written once
     * per such boot by the station, when it knows): a = diag_rx_boot_t,
     * b = diag_rx_boot_by_t. On the ESP32-S3 a reset by the EN pin (the USB
     * serial chip's auto-reset, the RST button) reads as power-on while the
     * receiver keeps running; only DIAG_RX_BOOT_RESTARTED is a power loss
     * (health's power-lost alert).
     */
    DIAG_EV_RX_BOOT = 23,
} diag_event_t;

/** DIAG_EV_RX_BOOT `a`. */
typedef enum {
    DIAG_RX_BOOT_KEPT = 1,       /**< the receiver kept running: only the ESP32 was reset (its reset pin) */
    DIAG_RX_BOOT_RESTARTED = 2,  /**< the receiver restarted with the ESP32: the power was cut */
    DIAG_RX_BOOT_UNKNOWN = 3,    /**< nothing the receiver sent told within 30 s */
} diag_rx_boot_t;

/** DIAG_EV_RX_BOOT `b`: what told it. */
typedef enum {
    DIAG_RX_BOOT_BY_NONE = 0,
    DIAG_RX_BOOT_BY_RESTART = 1, /**< its restart banner (or restarting answer, an unasked version log) before any data */
    DIAG_RX_BOOT_BY_UPTIME = 2,  /**< its version log's time since its restart */
    DIAG_RX_BOOT_BY_STREAM = 3,  /**< RTCM or NMEA at once */
} diag_rx_boot_by_t;

/** DIAG_EV_LIVE_VIEW events (low byte of `a`). */
typedef enum {
    DIAG_LIVE_OPENED = 1,       /**< b = window length (s) */
    DIAG_LIVE_EXTENDED = 2,     /**< b = time left (s) */
    DIAG_LIVE_CLOSED = 3,       /**< detail = why (the station's live-view end reason), b = time open (s) */
    /**
     * Satellite-in-view output turned on for the window, never saved:
     * detail = 0 accepted, 1 rejected, 2 no answer, 3 not sent (no lease or
     * a link error); b = how long the command lease held RTCM back (ms).
     */
    DIAG_LIVE_SKY_ON = 4,
    DIAG_LIVE_SKY_OFF = 5,      /**< turned off again: detail and b as DIAG_LIVE_SKY_ON */
} diag_live_ev_t;

/**
 * `a` of DIAG_EV_CASTER_DOWN / _UP: slot index in bits 0-7, end class in bits
 * 8-15, a count in bits 16-30 (saturating at 32 767, so `a` stays >= 0).
 */
static inline int32_t diag_caster_a(uint32_t slot, uint32_t cls, uint32_t count)
{
    if (count > 32767u) {
        count = 32767u;
    }
    return (int32_t)((slot & 0xFFu) | ((cls & 0xFFu) << 8) | (count << 16));
}

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
    DIAG_GNSS_STEP_REBOOT_BLOCKED = 9,    /**< rung 6a blocked by the reboot cap or a held reboot inhibit (outcome 0; why in bits 16-23 of b), not taken (4), held back for now (5) */
} diag_gnss_step_t;

/** Ladder step outcome (second byte of `b`). */
typedef enum {
    DIAG_STEP_STARTED = 0,
    DIAG_STEP_HEALTHY = 1,
    DIAG_STEP_FAILED = 2,       /**< also: refused without sending anything */
    DIAG_STEP_ABANDONED = 3,    /**< the ladder was suspended */
    DIAG_STEP_NOT_RUN = 4,      /**< not run: the user's recovery mode (gnss/recovery) */
    DIAG_STEP_HELD = 5,         /**< GNSS code 9 only: the restart is held back for now (reasons in bits 16-23 of b) */
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
    uint32_t lines;                 /**< lines logged (newlines written: a line can take more than one record) */
    uint32_t ring_overwritten;      /**< records the ring dropped to make room (a record is a line or a part of one) */
    uint32_t cut;                   /**< lines cut to CONFIG_DIAG_LOG_LINE_MAX */
    uint32_t console_dropped;       /**< lines never sent to the console: the ring overwrote them first */
    uint32_t console_dropped_bytes;
    bool prev_boot_available;       /**< the previous boot's lines are readable (once diag_log_check_stamp kept them) */
    uint32_t prev_boot_lines;       /**< 0 when not available */
    bool usb_mirror;                /**< the console is copied to the built-in USB (CONFIG_DIAG_CONSOLE_USB_MIRROR) */
    bool usb_connected;             /**< a USB host is attached there now (it may not be reading) */
    uint32_t usb_dropped;           /**< lines not copied there although a host was attached (it did not read) */
    uint32_t usb_dropped_bytes;
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
 * the previous boot's lines for diag_log_check_stamp), mount the journal and
 * start its writer, check for a core dump. Call once, first thing in
 * app_main (after lifecycle_early_boot).
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
 * Same for the previous boot's log, preserved across the reset (0 if none:
 * after a power cut, when the previous boot ran a different app image, when
 * this image's lines predate another image's run, and before
 * diag_log_check_stamp()).
 */
size_t diag_log_read_prev_boot(uint32_t *cursor, char *buf, size_t buf_size);

/**
 * Image generation check of the previous boot's log; lifecycle_init calls it
 * once, right after the settings, before any reader starts. The previous
 * boot's lines are kept (readable from then on) only if
 * `stamp_ok(their stamp)` (lifecycle_rtc_stamp_ok; NULL = never), i.e. they
 * come from the run right before this boot. Otherwise they predate another
 * image's run, or cannot be checked without settings, and stay unavailable
 * for this boot (diag_log_stats, diag_log_read_prev_boot), as after a power
 * cut. `stamp` (lifecycle_rtc_stamp(), 0 = none) is stamped into this boot's
 * bank; a boot that ends before this call leaves it unstamped, and the next
 * boot treats it as any unstamped RTC block. Calling it again decides the
 * same way.
 */
void diag_log_check_stamp(bool (*stamp_ok)(uint32_t stamp), uint32_t stamp);

/**
 * Read either ring without note lines: gaps and "more" are reported in
 * `info` (optional). Same cursor rules as diag_log_read().
 */
size_t diag_log_read_ex(diag_log_src_t src, uint32_t *cursor, char *buf, size_t buf_size,
                        diag_log_read_info_t *info);

/**
 * Cursor of the newest line this boot: a reader that starts from it gets
 * only lines logged afterwards (0 before the first line).
 */
uint32_t diag_log_head(void);

/** Log counters. */
void diag_log_stats(diag_log_stats_t *out);

/**
 * Console text that is not a log line, whole: handed to the console UART's
 * driver in one piece under the console's lock, with the log's line ending
 * ("\n" goes out as "\r\n" when the console is set to CR LF), so no log
 * line can land inside it. `s` should end at a line end (several lines go
 * out together); text without one is sent as it is and taken for a prompt
 * that waits on its line, which the typed input's echo ends (a log line
 * that comes first continues that line). When the console stopped inside a log
 * line (one longer than the transmit buffer, or a library's line logged in
 * fragments), a line break comes first, so the text starts at a line
 * start. Unlike a log line it is never dropped: the call blocks until the
 * driver has taken every byte, so only from a task that may block (from an
 * interrupt, a critical section or with the scheduler stopped it does
 * nothing). Before the console is set up, and in a build without the UART
 * console, it goes to stdout.
 */
void diag_console_write(const char *s, size_t len);

/** Longest line diag_console_printf keeps whole (a longer one goes out in parts of this size). */
#define DIAG_CONSOLE_LINE_MAX 1024

/**
 * A command line's replies printed in parts (several printf calls per line):
 * each part is gathered here until its line ends, and every whole line then
 * goes to diag_console_write. One writer per gatherer; zero-initialised is
 * empty.
 */
typedef struct {
    char buf[DIAG_CONSOLE_LINE_MAX];
    size_t len;
} diag_console_lines_t;

/** printf into the gatherer. */
void diag_console_printf(diag_console_lines_t *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void diag_console_vprintf(diag_console_lines_t *l, const char *fmt, va_list ap) __attribute__((format(printf, 2, 0)));
/** `len` bytes of text into the gatherer. */
void diag_console_put(diag_console_lines_t *l, const char *s, size_t len);
/**
 * Send what is gathered of a line that has not ended: as it is (a prompt
 * that waits on its line for input), or with `end_line` ended first (the
 * end of a reply). Nothing when nothing is gathered.
 */
void diag_console_flush(diag_console_lines_t *l, bool end_line);

/**
 * Change a tag's runtime log level ("*" = all). "*" sets the default for
 * every tag and then applies the fixed levels (diag_log_fix_level) again, so
 * those tags keep theirs; an explicit level for a fixed tag holds until the
 * next "*" change or restart.
 * @return ESP_ERR_INVALID_ARG for NULL or a level past ESP_LOG_VERBOSE,
 *         ESP_ERR_NOT_SUPPORTED for a level above CONFIG_LOG_MAXIMUM_LEVEL
 *         (this build leaves those lines out: setting it would change
 *         nothing).
 */
esp_err_t diag_set_log_level(const char *tag, esp_log_level_t level);

/** Most tags with a fixed level. */
#define DIAG_LOG_FIXED_MAX 8

/**
 * Give a tag a fixed level, applied now and again after every "*" change
 * (a component whose own warnings are expected noise, such as the HTTP
 * server's about clients that went away). Setting a fixed tag again
 * replaces its level.
 * @return ESP_ERR_INVALID_ARG for NULL, "", "*", a tag of 16 or more
 *         characters or a level past ESP_LOG_VERBOSE; ESP_ERR_NOT_SUPPORTED
 *         above CONFIG_LOG_MAXIMUM_LEVEL; ESP_ERR_NO_MEM when
 *         DIAG_LOG_FIXED_MAX tags are fixed already.
 */
esp_err_t diag_log_fix_level(const char *tag, esp_log_level_t level);

/**
 * Append a journal record (never blocks; written to flash by diag's writer
 * task within milliseconds). Not from an interrupt handler.
 * @return ESP_OK when accepted; ESP_ERR_INVALID_STATE when a rate limit (per
 *         kind or the hourly budget) refused it (nothing written: keep the
 *         data and retry later; never for DIAG_EV_LADDER_STEP,
 *         DIAG_EV_GNSS_FLASH or DIAG_EV_RX_BOOT);
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

/** Trusted UTC now (seconds since 1970), extrapolated from the last diag_set_utc(); 0 before it. */
int64_t diag_utc_now(void);

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
