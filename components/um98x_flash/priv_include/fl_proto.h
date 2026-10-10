/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The receiver side of a flash session as protocol steps (pure logic,
 * host-tested against a simulated receiver loader): finding and identifying
 * the receiver, quiescing it, moving its COM1 to the loader baud (with a
 * verification and a restore), the reset and the stop key into its resident
 * loader, the loader menu, the wait after the transfer and its outcome
 * table, and leaving the loader with the version check.
 *
 * Receiver text is matched NUL-safely across reads (fl_win_t). "The
 * receiver's application answers" means a CRC-valid RTCM frame, a
 * checksum-valid '$' sentence or "$devicename" decoded at the baud being
 * listened at; the version query goes out only after that (never into a
 * loader that might be listening at another baud), and its reply counts only
 * when its CRC32 is valid (the shared version log parser).
 *
 * What may be sent where (nothing else ever is):
 *   application      VERSIONA, unlog, config com1 <baud>, the GGA request,
 *                    the reset command, the stop key
 *   loader menu      "\r\n", "\r\n2\r\n", "6\r\n"
 *   firmware submenu "q\r\n"
 *   transfer         frames, EOT, CAN CAN, one ACK (xmodem_tx.h)
 *   after the transfer  nothing until the outcome is known
 * Menu items 3, 4, 5, 8, 9 and 10 are never sent.
 *
 * PROVISIONAL, to be confirmed on hardware: every time limit below, the
 * loader's texts and whether it answers on COM1 at 460800 after a soft
 * reset at all.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fl_io.h"
#include "fl_rx.h"
#include "gnss_framing.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- time limits (PROVISIONAL, to be confirmed on hardware) ------------- */
#define FL_VER_WAIT_MS          2000u    /**< version reply window per query */
#define FL_QUIESCE_GAP_MS       100u
#define FL_BAUD_SETTLE_MS       1000u    /**< after config com1 */
#define FL_VERIFY_MS            1500u    /**< sentence request after the baud change */
#define FL_PRE_RESET_MS         100u
#define FL_ENTRY_MAX_MS         30000u   /**< one entry attempt, from the end of the reset write */
#define FL_ENTRY_LATE_MS        8001u    /**< data this late without the prompt: the attempt is lost */
#define FL_STOPKEY_GAP_MS       5u       /**< idle between stop-key bursts (each written after the last drained) */
#define FL_MENU_QUIET_MS        200u     /**< RX quiet before the first "\r\n" at the prompt */
#define FL_MENU_QUIET_MAX_MS    10000u   /**< ... but no longer than this */
#define FL_PROMPT_MS            10000u   /**< waits for the prompt in the menu */
#define FL_MENU_RESEND_MS       3000u    /**< menu item resent only after this much silence */
#define FL_MENU_SENDS           3u
#define FL_MENU_SETTLE_MS       1000u    /**< a prompt after the item that stays the last text this long: the item did nothing */
#define FL_READY_MS             15000u   /**< ready line after the last menu item */
#define FL_NUDGE_MS             3000u    /**< silence before one "\r\n" while waiting for the prompt */
#define FL_RESETTING_MS         10000u   /**< "resetting the cpu" after "6" */
#define FL_APP_WAIT_MS          20000u   /**< the application's first output after leaving the loader */
#define FL_VER_RETRY_MS         2000u
#define FL_VER_TOTAL_MS         60000u
#define FL_PRE_EXIT_APP_MS      30000u   /**< application after "6" before the point of no return */
#define FL_LOADER_RESTART_MS    15000u   /**< application after the one "6" when the receiver was found at its loader */
#define FL_T1_MS                180000u  /**< success text expected within this after the last EOT */
#define FL_T2_MS                600000u  /**< silent listening before "no answer" */
#define FL_POST_NUDGE_WAIT_MS   10000u   /**< after the one "\r\n" at T2 */
#define FL_POST_FAIL_PROMPT_MS  60000u   /**< after a failed transfer: back at the prompt within this */
#define FL_POST_FAIL_NUDGE_MS   10000u
#define FL_APPCHECK_MS          10000u   /**< recovery: listen-only for the application */
#define FL_APPCHECK_LONG_MS     30000u
#define FL_SCAN_LISTEN_MS       3000u    /**< per baud in the listen-first version scan */
#define FL_RACE_MAX_MS          10000u   /**< boot-time race into the loader */
#define FL_MISMATCH_LISTEN      32u      /**< mismatch-pattern bytes that mean "another baud" */

#define FL_BAUD_APP_DEFAULT     115200u
#define FL_BAUD_LOADER          460800u

/** Watched receiver texts (index = value). */
typedef enum {
    FL_S_PROMPT = 0,        /* "boot>" */
    FL_S_DEVICENAME,        /* "$devicename" */
    FL_S_READY,             /* "download to" */
    FL_S_BEGIN,             /* "begin to update flash" */
    FL_S_INVALID,           /* "Invalid input" */
    FL_S_TOO_LONG,          /* "input is too long" */
    FL_S_INPUT_TIMEOUT,     /* "Get input timeout" */
    FL_S_SUBMENU,           /* "Choose firmware for update" */
    FL_S_ABORTED,           /* "## Binary download aborted" */
    FL_S_RESETTING,         /* "resetting the cpu" */
    FL_S_BACKUP_OK,         /* "backup succeed" */
    FL_S_RECOVER_WARN,      /* "recover warinnig" (spelled as the loader prints it) */
    FL_S_UART_BAUD,         /* "uart baudrate:" */
    FL_S_TIMEOUT_CFG,       /* "Timeout:" */
    FL_S_DEFAULT_ACTION,    /* "Default action:" */
    FL_S_LAST_MENU,         /* "efuse from uart" */
    FL_S_GGA,
    FL_S_GSV,
    FL_S_RMC,
    /* hard errors after the transfer (FL_S_ERR_FIRST .. FL_S_ERR_LAST) */
    FL_S_ERR_PKG_HEADER,    /* "wrong pkg header" */
    FL_S_ERR_SUM32,         /* "sum32 check err" */
    FL_S_ERR_DECOMP,        /* "decompression failed" */
    FL_S_ERR_LZOP,          /* "lzop_decompress_block Error" */
    FL_S_ERR_SIG,           /* "signature failed" */
    FL_S_ERR_SIG2,          /* "Signature Check Failed" */
    FL_S_ERR_AUTH,          /* "authcode check err" */
    FL_S_ERR_BACKUP,        /* "backup fail" */
    FL_S_ERR_LOAD,          /* "fail to load" */
    FL_S_ERR_BOOTCFG,       /* "fail to update boot config" */
    FL_S_ERR_VERSION,       /* "version is different" */
    FL_S_ERR_ABORTED,       /* "## Binary download aborted" (after the transfer) */
    FL_S_ERR_XMODEM,        /* "xmodem error" */
    FL_S_COUNT,
} fl_str_t;

#define FL_S_ERR_FIRST FL_S_ERR_PKG_HEADER
#define FL_S_ERR_LAST  FL_S_ERR_XMODEM

const char *fl_str(fl_str_t s);

/**
 * Listener: reads from the io into the text window (every watched string)
 * and through a framer that recognises the application's output.
 */
typedef struct {
    const fl_io_t *io;
    fl_win_t win;
    gnss_framer_t framer;
    uint32_t baud;               /**< host baud now (as last set through fl_lis_set_baud) */
    uint32_t rtcm;               /**< CRC-valid RTCM frames since fl_lis_evidence_reset */
    uint32_t sentences;          /**< checksum-valid '$' sentences since then */
    uint32_t mismatch;           /**< mismatch-pattern bytes since then */
    uint32_t err_base;
    bool ver_ok;                 /**< a CRC-valid version log since then */
    gnss_versiona_t ver;
    uint32_t bytes;              /**< bytes since then */
    uint32_t last_rx_ms;         /**< when the last byte arrived */
    bool any_rx;                 /**< any byte since then */
    uint32_t text_bytes;         /**< printable non-blank bytes since then */
    fl_tap_t tap;                /**< every byte read also goes here (when set) */
    void *tap_ctx;
} fl_lis_t;

void fl_lis_init(fl_lis_t *lis, const fl_io_t *io, uint32_t baud);
/** Bytes read elsewhere (the transfer's classifier): into the window and the framer. */
void fl_lis_feed(fl_lis_t *lis, const uint8_t *data, size_t len);
/** fl_tap_t adapter: ctx is the fl_lis_t. */
void fl_lis_tap(void *ctx, const uint8_t *data, size_t len);
/** Read and discard into the window for ms (listen only). */
void fl_lis_listen(fl_lis_t *lis, uint32_t ms);
/** Forget the application evidence and counters (not the window or its watches). */
void fl_lis_evidence_reset(fl_lis_t *lis);
/** One read (up to wait_ms, at most one slice). Returns the bytes read. */
size_t fl_lis_read(fl_lis_t *lis, uint32_t wait_ms, uint8_t *copy, size_t copy_cap);
/** Stream offset of the next byte (a mark for "after this write"). */
uint64_t fl_lis_mark(const fl_lis_t *lis);
bool fl_lis_seen_after(const fl_lis_t *lis, fl_str_t s, uint64_t mark);
bool fl_lis_seen(const fl_lis_t *lis, fl_str_t s);
/** The application answers at this baud (RTCM, '$' sentence or "$devicename" since the evidence reset). */
bool fl_lis_app_evidence(const fl_lis_t *lis);
/** Change the host baud (and remember it); clears the evidence. 0 = ok. */
int fl_lis_set_baud(fl_lis_t *lis, uint32_t baud);
/** Listen until the application answers or ms pass. */
bool fl_lis_wait_app(fl_lis_t *lis, uint32_t ms);
/** One version query: write VERSIONA, wait FL_VER_WAIT_MS for a CRC-valid reply. */
bool fl_query_version(fl_lis_t *lis, gnss_versiona_t *out);

/* ---- find and identify ------------------------------------------------- */

typedef enum {
    FL_ID_OK = 0,           /**< a CRC-valid version log at app_baud */
    FL_ID_NO_ANSWER,        /**< no version log at any baud */
    FL_ID_AT_LOADER,        /**< the loader prompt answered: the session does not start */
    FL_ID_IO,
} fl_id_result_t;

typedef struct {
    uint32_t app_baud;
    gnss_versiona_t ver;
    uint32_t loader_baud;    /**< FL_ID_AT_LOADER: where the prompt was seen */
    bool loader_key_sent;    /**< ... "6" was sent there (the boot's one restart from the loader) */
    bool app_back;           /**< ... and the application came back at the stored baud */
} fl_ident_t;

/**
 * VERSIONA at the stored baud (twice), then 115200, 460800, 921600. Any
 * prompt in a reply stops the queries: "6" once at that baud when
 * loader_key_allowed, then up to FL_LOADER_RESTART_MS for the application at
 * the stored baud.
 */
fl_id_result_t fl_identify(fl_lis_t *lis, uint32_t stored_baud, bool loader_key_allowed, fl_ident_t *out);

/* ---- quiesce, loader baud, reset and stop key ------------------------------ */

/** Three times "\r\nunlog\r\n", FL_QUIESCE_GAP_MS apart. */
int fl_quiesce(fl_lis_t *lis);

typedef enum {
    FL_BAUD_OK = 0,         /**< COM1 at the loader baud, verified */
    FL_BAUD_RESTORED,       /**< not verified; COM1 back at the application baud (abort the session) */
    FL_BAUD_STRANDED,       /**< not verified and not restored: receiver COM1 at an unsaved baud (needs the user) */
    FL_BAUD_IO,
} fl_baud_result_t;

/** "config com1 <loader>" (COM1 only, never saved), host switch, verification, restore on failure. */
fl_baud_result_t fl_raise_baud(fl_lis_t *lis, uint32_t app_baud, uint32_t loader_baud);

/** The restore ladder alone (also the boot-time recovery of an interrupted baud change). */
fl_baud_result_t fl_restore_baud(fl_lis_t *lis, uint32_t app_baud, uint32_t loader_baud);

/** Last "\r\nunlog\r\n", FL_PRE_RESET_MS, input cleared. */
int fl_pre_reset_quiet(fl_lis_t *lis);

typedef enum {
    FL_ENTRY_PROMPT = 0,    /**< the loader prompt was seen */
    FL_ENTRY_TIMEOUT,       /**< FL_ENTRY_MAX_MS without it */
    FL_ENTRY_APP,           /**< "$devicename": the application restarted instead */
    FL_ENTRY_LATE,          /**< data after FL_ENTRY_LATE_MS without the prompt */
    FL_ENTRY_IO,
} fl_entry_result_t;

typedef struct {
    uint32_t t0_ms;          /**< end of the reset write */
    uint32_t prompt_ms;      /**< from t0 to the prompt */
    uint32_t bursts;
    uint32_t key_bytes;
    bool baud_text;          /**< "uart baudrate:" seen before the prompt */
    bool timeout_text;       /**< "Timeout:" */
    bool action_text;        /**< "Default action:" */
    bool last_menu_line;     /**< "efuse from uart" */
} fl_entry_stats_t;

/** Stop-key burst for a baud: 8 bytes at 115200, 32 at 460800, 64 at 921600. */
size_t fl_stop_key_burst(uint32_t baud);

/** "\r\nreset\r\nreset\r\n" in one write, then the stop key until the prompt (or a limit). */
fl_entry_result_t fl_reset_and_stop(fl_lis_t *lis, uint32_t loader_baud, fl_entry_stats_t *st);

/** Stream the stop key for up to max_ms looking for the prompt (no reset; boot-time race and bench). */
fl_entry_result_t fl_stop_key_stream(fl_lis_t *lis, uint32_t max_ms, bool late_rule, fl_entry_stats_t *st);

/* ---- the loader menu --------------------------------------------------------- */

typedef enum {
    FL_PROMPT_SEEN = 0,
    FL_PROMPT_TIMEOUT,
    FL_PROMPT_LEFT,         /**< "$devicename", a '$' sentence, RTCM or the mismatch pattern: the loader is gone */
} fl_prompt_result_t;

/** Wait up to max_ms for the prompt after `mark`; one "\r\n" after nudge_ms of silence (0 = never). */
fl_prompt_result_t fl_wait_prompt(fl_lis_t *lis, uint64_t mark, uint32_t max_ms, uint32_t nudge_ms);

typedef enum {
    FL_MENU_READY = 0,      /**< the ready line arrived: the handshake follows */
    FL_MENU_FAILED,         /**< this attempt failed (the loader is at its prompt or silent) */
    FL_MENU_SUBMENU,        /**< the firmware submenu: "q" sent; the session ends ("6" at the prompt) */
    FL_MENU_COPY_CHANGED,   /**< the RAM copy no longer matches its digest */
    FL_MENU_JOURNAL,        /**< the "menu-2-sent" record could not be written */
    FL_MENU_IO,
} fl_menu_result_t;

typedef struct {
    bool confirm_prompt;     /**< identify: quiet, "\r\n", a fresh prompt (not on a retry after a failed transfer) */
    void *ctx;
    bool (*before_item)(void *ctx);      /**< persist "menu-2-sent"; false: nothing sent */
    bool (*copy_ok)(void *ctx);          /**< recompute the RAM copy's digest */
} fl_menu_job_t;

typedef struct {
    uint32_t sends;          /**< menu items sent */
    bool begin_seen;
    bool at_prompt;          /**< the prompt is known to be the last thing seen */
    char text[96];           /**< the text that decided a failure */
} fl_menu_stats_t;

/** Menu item 2 up to the ready line; bytes after its LF go into rx. */
fl_menu_result_t fl_menu_download(fl_lis_t *lis, fl_rx_t *rx, const fl_menu_job_t *job, fl_menu_stats_t *st);

/**
 * After a failed handshake: back to the prompt (a prompt after `mark`, the
 * end of the ready line, counts; else FL_PROMPT_MS with one nudge after
 * FL_NUDGE_MS of silence).
 */
fl_prompt_result_t fl_after_handshake(fl_lis_t *lis, uint64_t mark);

/* ---- after the transfer ---------------------------------------------------------- */

typedef enum {
    FL_OUT_S = 0,           /**< success text, then the prompt, within T1, no error */
    FL_OUT_L,               /**< the same after T1 but before T2 (treated as S) */
    FL_OUT_E,               /**< a hard error string */
    FL_OUT_U,               /**< the prompt without the success text (or unknown text only) */
    FL_OUT_N_APP,           /**< nothing by T2; the application answers at its baud */
    FL_OUT_N_SILENT,        /**< nothing by T2 and nothing afterwards */
} fl_outcome_t;

typedef struct {
    fl_outcome_t outcome;
    fl_str_t error;          /**< FL_OUT_E: the first hard error seen */
    bool error_retryable;    /**< a transport error report, not a package or write error */
    bool prompt_after_error; /**< FL_OUT_E: the prompt came back */
    uint32_t success_ms;     /**< from the last EOT to the success text (0 = not seen) */
    uint32_t prompt_ms;      /**< ... to the prompt */
    bool recover_warning;    /**< the loader's A/B recovery warning was seen (not an error) */
    bool success_this_boot;  /**< the success text was seen in this run */
} fl_post_t;

/**
 * Classify the receiver's text after the transfer. `post` already holds
 * every byte read from the first EOT on (the post-transfer buffer, its
 * watches registered); t_eot is the last EOT write. Sends nothing before T2.
 * Only text seen in this run counts, so after an ESP32 reset in "eot-sent"
 * a prompt without a success text seen in this boot is U (the text may have
 * come before the reset), never an automatic retry unless a hard error is
 * seen. No answer by T2: one "\r\n" at the loader baud and
 * FL_POST_NUDGE_WAIT_MS; then FL_APPCHECK_MS listen-only at app_baud.
 */
void fl_post_wait(fl_lis_t *post, uint32_t t_eot, uint32_t app_baud, fl_post_t *out);

/** True if this hard error is a transport report (the same package may be sent again). */
bool fl_error_retryable(fl_str_t s);

/* ---- leaving the loader ------------------------------------------------------------ */

typedef enum {
    FL_EXIT_APP = 0,        /**< the application answered the version query (out->ver) */
    FL_EXIT_NO_APP_PROMPT,  /**< no application, the loader prompt answers: "the application does not start" */
    FL_EXIT_SILENT,         /**< nothing answers: the user must power-cycle the receiver */
    FL_EXIT_IO,
} fl_exit_result_t;

typedef struct {
    bool resetting_seen;
    uint32_t app_baud_found; /**< where the version log came from */
    gnss_versiona_t ver;
} fl_exit_t;

/**
 * Leave the loader after a successful transfer (or the user's restart):
 * "6\r\n" x3 at the loader baud, FL_RESETTING_MS for "resetting the cpu"
 * (once more "6\r\n" if only the prompt answered), host to the application
 * baud, FL_APP_WAIT_MS for its first output, the version query every
 * FL_VER_RETRY_MS for FL_VER_TOTAL_MS (only once the application answers at
 * that baud), then a listen-first scan of the other bauds, then FL_APPCHECK_MS
 * listen-only at the loader baud for the prompt.
 */
fl_exit_result_t fl_leave_loader(fl_lis_t *lis, uint32_t loader_baud, uint32_t app_baud, fl_exit_t *out);

/**
 * Leave the loader before the point of no return: "6\r\n" once, the
 * application within FL_PRE_EXIT_APP_MS at its baud; otherwise listen at the
 * loader baud for the prompt and, if it answers, "\r\n" and "6\r\n" once
 * more and wait for the application again.
 */
fl_exit_result_t fl_leave_loader_early(fl_lis_t *lis, uint32_t loader_baud, uint32_t app_baud, fl_exit_t *out);

/**
 * APP-CHECK of the recovery table: listen-only at app_baud for up to
 * listen_ms; the version query goes out only after the application answered
 * there.
 */
bool fl_app_check(fl_lis_t *lis, uint32_t app_baud, uint32_t listen_ms, gnss_versiona_t *out);

/* ---- bench observations (no data frame is ever sent) ------------------------------------ */

typedef struct {
    uint32_t c_count;
    uint32_t nak_count;
    uint32_t first_ms;       /**< first start byte after the ready line (UINT32_MAX none) */
    uint32_t interval_ms;    /**< between the first two start bytes of one kind (0 unknown) */
    uint32_t other_bytes;
    uint32_t lines;
} fl_startbytes_t;

/** Record the start bytes (and text) for ms after the ready line; sends nothing. */
void fl_watch_start_bytes(fl_lis_t *lis, fl_rx_t *rx, uint32_t ms, fl_startbytes_t *out);

#ifdef __cplusplus
}
#endif
