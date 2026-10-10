/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * UM98x firmware flasher: writes the package staged in pkg_store to the
 * receiver over the GNSS link (XMODEM-1K over UART, public protocol), and the
 * receiver-flash journal that makes the session crash-safe.
 *
 * Runs synchronously in the caller's task (the station supervisor) while the
 * caller holds a RAW lease on the GNSS link. Reports progress through a
 * callback so the web UI can show it.
 *
 * Every receiver update is started by the user (an Update click after the
 * daily check's banner, or a confirmed manual upload). Nothing here starts a
 * flash on its own; the only automatic transfers are the bounded retries of
 * the same pinned package after the point of no return, which finish a
 * session the user started.
 *
 * Build options: CONFIG_UM98X_FLASH_ENABLE (default off) builds the receiver
 * flasher in; without it the package can still be inspected and checked,
 * and nothing is ever sent to the receiver by this component.
 * CONFIG_UM98X_FLASH_RECOVERY_ACTIONS (needs it) lets the boot-time recovery
 * act; otherwise the recovery table is only logged.
 *
 * Validation (um98x_flash_prepare): the staged package is loaded into one
 * PSRAM buffer (the session is refused if the allocation fails) and every
 * pre-flight rule runs on that RAM copy, which is then the only copy sent:
 * block structure and header checks, payload sums and trailers, the
 * compressed sections decoded and checked, the loader baud and a known
 * partition table, the model string ("UM982" exactly, equal to the live
 * version query of this session), the UM982 product tags, a length that is
 * not a multiple of 1024, size and SHA-256 equal to the release manifest
 * entry or to what the user confirmed for an upload, and a build not lower
 * than the receiver's (a package with a different partition layout than the
 * receiver's current build is refused unless the build allows it, and then
 * needs its own confirmation). The rule checks are a pure core with host
 * tests (pkg_check.c, lzo1x.c).
 *
 * Session (um98x_flash_run): the receiver is identified first
 * (um98x_flash_identify, before the journal opens), the journal is opened in
 * "prepared" and written before every action that starts a new state; the
 * loader is entered with the soft reset and the stop key, menu item 2 is
 * selected, the RAM copy is sent, the receiver's report is classified, the
 * loader is left and the application confirmed with its version. The
 * protocol steps and the session policy (attempts, the point of no return,
 * retries within the caps, needs-user) are pure cores with a simulated
 * receiver in the host tests (fl_proto.c, xmodem_tx.c, fl_session.c). What
 * comes after a confirmed application (re-applying the station profile, its
 * check, closing the journal) is the station's part.
 *
 * Journal (gnss/flash_job): a versioned record with the fields of
 * um98x_flash_journal_t, written before each action that starts a new state.
 * It exists only while a session is open. The station reads it at boot (right
 * after the settings, before anything can transmit) and decides recovery
 * (flash_journal_plan); an unreadable journal, or one from a newer schema,
 * counts as open and waiting for the user: nothing is sent to the receiver.
 * The journal closes once the receiver's application is confirmed running
 * (any build) and the station profile has been re-applied and checked, or
 * after the abort path before the point of no return; closing it releases
 * the staged package.
 *
 * Loader entry runs at UM98X_FLASH_BAUD. With
 * CONFIG_UM98X_FLASH_FALLBACK_115200, an entry attempt that follows a failed
 * attempt which never reached the loader prompt may run at 115200 instead; it
 * is one of the session's three entry attempts under the same journal, never
 * a second session.
 *
 * Interrupting a transfer can leave the receiver without valid firmware, so:
 *  - um98x_flash_cancel() is honoured only before the reset;
 *  - the station opens the journal and pins the staged package before the
 *    first action, and holds a reboot inhibit until the session ends, the
 *    journal reaches "needs user", or the post-flash re-apply has been
 *    checked;
 *  - recovery runs take the model and the package SHA-256 from the journal,
 *    never from a request or the stored gnss/model. A journal allows at most
 *    3 transfers in total (the user's first plus up to 2 automatic retries,
 *    counted across reboots); more need a user confirmation each.
 *
 * Watchdog: every wait (block acknowledgements up to 60 s, the handshake,
 * the up to 600 s wait after the transfer, and the slices of package
 * validation) blocks in slices of at most 1 s and feeds the calling task's
 * watchdog; not only between blocks.
 *
 * Status: PROVISIONAL, to be confirmed on hardware. Loader entry, baud
 * switching, framing details, retry and timeout values, completion check and
 * recovery must be bench-tested before field use.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_link.h"
#include "gnss_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Loader baud for entry and transfer (both sides). PROVISIONAL, to be confirmed on hardware. */
#define UM98X_FLASH_BAUD 460800u

/** Journal blob layout version written by this firmware. */
#define UM98X_FLASH_JOURNAL_VERSION 1u

/** Last receiver text kept for the UI (bytes). */
#define UM98X_FLASH_TEXT_MAX 4096u

/** Progress callback, called from the flashing task; keep it short. */
typedef void (*um98x_flash_progress_cb_t)(const um98x_flash_progress_t *p, void *ctx);

/** What the user confirmed and what the receiver reports, for um98x_flash_prepare(). */
typedef struct {
    uint8_t sha256[32];           /**< from the release manifest, or confirmed by the user for an upload */
    uint32_t size;
    char expected_fw[48];         /**< firmware string the update should produce */
    uint32_t expected_build;
    um98x_info_t receiver;        /**< live identity from this session's version query (recovery: the journalled model) */
    bool cross_layout_confirmed;  /**< the user confirmed a package with a different partition layout */
    bool recovery;                /**< recovery gate: rules 1-11 only, the model from the journal */
} um98x_flash_expect_t;

/** Facts about a package, for the upload page and the confirmation dialog. */
typedef struct {
    uint32_t size;
    uint8_t sha256[32];
    char model[16];               /**< model string from the package header, "" if unreadable */
    int8_t layout;                /**< index of the known partition table it carries, -1 = unknown */
    uint32_t layout_build;        /**< receiver build that table belongs to, 0 = unknown */
    uint32_t stamps[5];           /**< build date stamps of the five blocks, 0 if unreadable */
    bool cross_layout;            /**< (prepare) layout differs from the receiver's current build */
    uint8_t failed_rule;          /**< (prepare) first rule that failed, 1..12; 0 = all passed */
    char why[80];                 /**< (prepare) what failed, "" when nothing did */
} um98x_flash_pkg_facts_t;

/** How a session ended. */
typedef enum {
    UM98X_FLASH_END_DONE = 0,     /**< the application answered after a transfer: re-apply, compare the build, close */
    UM98X_FLASH_END_ABORTED,      /**< ended before the point of no return, application confirmed: re-apply, check RTCM, close */
    UM98X_FLASH_END_NEEDS_USER,   /**< the journal waits for the user (reason) */
    UM98X_FLASH_END_NOT_STARTED,  /**< nothing was sent and no journal exists (the first journal write failed) */
} um98x_flash_end_t;

/** Outcome of a session (or a recovery run, or a user action). */
typedef struct {
    um98x_flash_end_t end;
    uint8_t failure;              /**< um98x_flash_failure_t */
    char reason[96];
    um98x_info_t receiver;        /**< DONE / ABORTED: the application's version log */
    uint32_t app_baud;            /**< where it answered */
    um98x_flash_journal_t journal;/**< the journal as the session left it */
    bool ponr;                    /**< a data frame went out under this journal */
    char outcome;                 /**< after the transfer: 'S', 'L', 'E', 'U', 'N' (application answered: 'A'), 0 = none */
    uint32_t handshake_ms;
    char xm_mode;                 /**< 'C' (CRC) or 'S' (checksum), 0 = none */
    uint32_t transfer_ms;         /**< first frame to the last acknowledgement */
    uint32_t success_ms;          /**< last EOT to the success text */
    uint32_t prompt_ms;           /**< last EOT to the prompt */
    uint32_t entry_prompt_ms;     /**< reset to the loader prompt (last attempt) */
    uint32_t stop_key_bytes;      /**< stop-key bytes of the last attempt */
} um98x_flash_result_t;

/** A session request: the journal's identity fields (or the journal as read, for recovery). */
typedef struct {
    um98x_flash_journal_t journal;
    um98x_flash_progress_cb_t progress_cb;  /**< optional */
    void *progress_ctx;
    uint32_t stop_after_blocks;   /**< bench only: stop sending after this many acknowledged blocks (0 = never) */
} um98x_flash_request_t;

/** Result of the identification before a session. */
typedef enum {
    UM98X_FLASH_ID_OK = 0,        /**< a CRC-valid version log (info, app_baud) */
    UM98X_FLASH_ID_NO_ANSWER,     /**< no version log at any baud */
    UM98X_FLASH_ID_AT_LOADER,     /**< the receiver sat at its loader prompt: no session */
    UM98X_FLASH_ID_ERROR,
} um98x_flash_id_t;

typedef struct {
    um98x_flash_id_t result;
    um98x_info_t info;
    uint32_t app_baud;
    uint32_t loader_baud;         /**< AT_LOADER: where */
    bool loader_key_sent;         /**< AT_LOADER: the restart key went out (the boot's one) */
    bool app_back;                /**< AT_LOADER: the application came back */
} um98x_flash_ident_t;

/** Prepare internal state. */
esp_err_t um98x_flash_init(void);

/** Receiver flashing is built in (CONFIG_UM98X_FLASH_ENABLE). */
bool um98x_flash_enabled(void);

/**
 * Load the staged package into one PSRAM buffer and run every pre-flight
 * rule on that copy (see above). The buffer stays allocated for the run
 * that follows; um98x_flash_discard() frees it. Feeds the calling task's
 * watchdog (the compressed blocks take seconds).
 *
 * @return ESP_ERR_NOT_FOUND (nothing staged), ESP_ERR_NO_MEM (no PSRAM
 *         buffer: never streamed from flash instead), ESP_ERR_INVALID_CRC
 *         (SHA-256 or size differ from what was confirmed),
 *         ESP_ERR_INVALID_VERSION (wrong model, a downgrade, or a cross-layout
 *         package not confirmed or not allowed), ESP_ERR_INVALID_RESPONSE
 *         (structure, sums or compressed data wrong); out->failed_rule says which.
 */
esp_err_t um98x_flash_prepare(const um98x_flash_expect_t *expect, um98x_flash_pkg_facts_t *out);

/**
 * Read the staged package's facts without the receiver (upload page: SHA-256,
 * size, model string, partition layout, date stamps). Does not keep a buffer.
 */
esp_err_t um98x_flash_inspect_staged(um98x_flash_pkg_facts_t *out);

/** Free the prepared PSRAM copy (after a session, or when the user cancels). */
void um98x_flash_discard(void);

/**
 * Identify the receiver before a session (no journal yet): the version query
 * at stored_baud, then 115200, 460800, 921600. A loader prompt in any reply
 * stops the queries; then "6" once at that baud when loader_key_allowed (the
 * boot's one restart from the loader) and a wait for the application.
 * ESP_ERR_NOT_SUPPORTED without CONFIG_UM98X_FLASH_ENABLE.
 */
esp_err_t um98x_flash_identify(gnss_link_lease_t raw_lease, uint32_t stored_baud, bool loader_key_allowed,
                               um98x_flash_ident_t *out);

/**
 * A new session with the prepared copy. Blocks until it ends (minutes);
 * feeds the calling task's watchdog in every wait (at most 1 s slices).
 * Opens the journal in "prepared" and writes it before every new state.
 * ESP_ERR_INVALID_STATE without a prepared copy; ESP_ERR_NOT_SUPPORTED
 * without CONFIG_UM98X_FLASH_ENABLE.
 * @param raw_lease RAW lease on the GNSS link, held by the caller throughout.
 */
esp_err_t um98x_flash_run(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req, um98x_flash_result_t *out);

/** The user's "retry flash (same package)" for a journal waiting for the user (the copy is prepared through the recovery gate). */
esp_err_t um98x_flash_user_retry(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req,
                                 um98x_flash_result_t *out);

/** The user's "restart receiver" for a journal waiting for the user: "6" and the exit checks. */
esp_err_t um98x_flash_user_restart(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req,
                                   um98x_flash_result_t *out);

/**
 * Boot-time recovery of an open journal (actions enabled only): run the
 * recovery-table plan (see flash_journal_plan). race_prompt: the early race
 * reached the loader prompt.
 */
esp_err_t um98x_flash_recover(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req, int plan,
                              bool race_prompt, um98x_flash_result_t *out);

/**
 * Boot-time race into the loader after a power cut during a session: stream
 * the stop key at UM98X_FLASH_BAUD for up to max_ms, looking for the loader
 * prompt. Called by the station's early boot step before anything else uses
 * the UART. PROVISIONAL, to be confirmed on hardware.
 * @return ESP_OK when the prompt was seen, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t um98x_flash_race(gnss_link_lease_t raw_lease, uint32_t max_ms);

/** Request cancellation; only effective before the reset. */
void um98x_flash_cancel(void);

/** Latest progress (safe from any task). */
void um98x_flash_get_progress(um98x_flash_progress_t *out);

/** Copy the last receiver text of the session (up to UM98X_FLASH_TEXT_MAX, NUL-terminated). @return bytes copied. */
size_t um98x_flash_get_text(char *buf, size_t len);

/**
 * Read the open journal (works without um98x_flash_init(), e.g. in safe mode).
 * @return ESP_OK (journal open), ESP_ERR_NOT_FOUND (no journal),
 *         ESP_ERR_INVALID_VERSION (written by a newer schema),
 *         ESP_ERR_INVALID_SIZE (unreadable: damaged or a read error),
 *         ESP_ERR_INVALID_STATE (settings unavailable this boot: state
 *         unknown). Every result but ESP_ERR_NOT_FOUND counts as open and the
 *         last three as waiting for the user: send nothing. Only the
 *         unreadable kinds can be cleared; with settings unavailable there is
 *         nothing to clear.
 */
esp_err_t um98x_flash_journal_get(um98x_flash_journal_t *out);

/** Write the journal (version set to UM98X_FLASH_JOURNAL_VERSION). */
esp_err_t um98x_flash_journal_put(const um98x_flash_journal_t *j);

/** Close the journal (erase the record). ESP_OK also when none was open. */
esp_err_t um98x_flash_journal_close(void);

/**
 * Recovery table, first decision: does this journal call for the boot-time
 * race into the loader? True only when both chips were reset (power-on or
 * brownout) and the session had reached the loader menu (menu item sent or
 * later, or any state after a transfer), including "needs user" with such a
 * phase. An ESP32-only reset never races: the receiver did not reset.
 */
bool um98x_flash_journal_needs_race(const um98x_flash_journal_t *j, bool receiver_was_reset);

/**
 * The whole recovery table for the journal read at boot (the plan as an
 * int: the station only passes it on), and its description for the log.
 * read_err is um98x_flash_journal_get()'s result.
 */
int um98x_flash_journal_plan(esp_err_t read_err, const um98x_flash_journal_t *j, bool receiver_was_reset,
                             const char **text);

/** Short name of a journal state ("prepared", ..., "needs-user"). */
const char *um98x_flash_journal_state_name(uint8_t state);

/* ---- byte capture (bench builds: CONFIG_UM98X_FLASH_BYTE_CAPTURE) ------------- */

/**
 * Every byte sent and received through this component, time-stamped (hex
 * and printable) in a PSRAM ring, and echoed to the console. Off: the
 * functions report nothing captured.
 */
bool um98x_flash_capture_available(void);
/** Start a new capture section with a title line (keeps older text until the ring wraps). */
void um98x_flash_capture_mark(const char *title);
/** Read the capture from *cursor on (0 = the oldest kept). @return bytes copied (0 = nothing more). */
size_t um98x_flash_capture_read(uint32_t *cursor, char *buf, size_t len);
/** Append a line of text (bench results). */
void um98x_flash_capture_line(const char *text);

#ifdef __cplusplus
}
#endif
