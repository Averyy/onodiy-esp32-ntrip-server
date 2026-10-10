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
 * Validation (um98x_flash_prepare): the staged package is loaded into one
 * PSRAM buffer (the session is refused if the allocation fails) and every
 * pre-flight rule runs on that RAM copy, which is then the only copy sent:
 * block structure and header checks, payload sums and trailers, the
 * compressed sections decoded and checked, the loader baud and a known
 * partition table, the model string ("UM982" exactly, equal to the live
 * version query of this session), the UM982 product tags, a length that is
 * not a multiple of 1024, size and SHA-256 equal to the signed manifest entry
 * or to what the user confirmed for an upload, and a build not lower than
 * the receiver's (a package with a different partition layout than the
 * receiver's current build is refused until bench-tested, then needs its own
 * confirmation). The rule checks are a pure core with host tests.
 *
 * Journal (gnss/flash_job): a versioned record with the fields of
 * um98x_flash_journal_t, written before each action that starts a new state.
 * It exists only while a session is open. The station reads it at boot (right
 * after the settings, before anything can transmit) and decides recovery; an
 * unreadable journal, or one from a newer schema, counts as open and waiting
 * for the user: nothing is sent to the receiver. The journal closes once the
 * receiver's application is confirmed running (any build) and the station
 * profile has been re-applied and checked, or after the abort path before
 * the point of no return; closing it releases the staged package.
 *
 * Loader entry runs at UM98X_FLASH_BAUD. With
 * CONFIG_UM98X_FLASH_FALLBACK_115200, an entry attempt that follows a failed
 * attempt which never reached the loader prompt may run at 115200 instead; it
 * is one of the session's three entry attempts under the same journal, never
 * a second session.
 *
 * Interrupting a transfer can leave the receiver without valid firmware, so:
 *  - um98x_flash_cancel() is honoured only before the transfer starts;
 *  - the station opens the journal and pins the staged package before the
 *    first action, and holds a reboot inhibit until the session ends, the
 *    journal reaches "needs user", or the post-flash re-apply has been
 *    checked;
 *  - um98x_flash_run() must be safe to call again after a failed run or an
 *    interrupted one (reboot, power cut): the station's journal-driven
 *    recovery calls it with recovery = true, and such a run takes the model
 *    and the package SHA-256 from the journal, never from the request or the
 *    stored gnss/model. A journal allows at most 3 transfers in total (the
 *    user's first plus up to 2 automatic retries, counted across reboots);
 *    more need a user confirmation each.
 *
 * Watchdog: every wait (block acknowledgements up to 60 s, the handshake,
 * the up to 600 s wait after the transfer, and the slices of package
 * validation) blocks in slices of at most 1 s and feeds the calling task's
 * watchdog; not only between blocks.
 *
 * The XMODEM-1K sender is a pure core behind read/write callbacks with an
 * injected clock, host-tested against a simulated receiver.
 *
 * Status: PROVISIONAL, to be confirmed on hardware. Loader entry, baud
 * switching, framing details, retry and timeout values, completion check and
 * recovery must be bench-tested before field use. The boot-time recovery
 * actions are report-only unless CONFIG_UM98X_FLASH_RECOVERY_ACTIONS is set.
 */
#pragma once

#include <stdbool.h>
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

/** Progress callback, called from the flashing task; keep it short. */
typedef void (*um98x_flash_progress_cb_t)(const um98x_flash_progress_t *p, void *ctx);

/** Parameters for one flash run. */
typedef struct {
    const char *expected_model;   /**< model from this session's live version query (ignored for recovery runs) */
    bool recovery;                /**< set only by the station's journal-driven recovery: model and SHA-256 come from the journal */
    um98x_flash_progress_cb_t progress_cb;  /**< optional */
    void *progress_ctx;
} um98x_flash_request_t;

/** What the user confirmed and what the receiver reports, for um98x_flash_prepare(). */
typedef struct {
    uint8_t sha256[32];           /**< from the signed manifest, or confirmed by the user for an upload */
    uint32_t size;
    char expected_fw[48];         /**< firmware string the update should produce */
    uint32_t expected_build;
    um98x_info_t receiver;        /**< live identity from this session's version query */
    bool cross_layout_confirmed;  /**< the user confirmed a package with a different partition layout */
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
} um98x_flash_pkg_facts_t;

/** Prepare internal state. */
esp_err_t um98x_flash_init(void);

/**
 * Load the staged package into one PSRAM buffer and run every pre-flight
 * rule on that copy (see above). The buffer stays allocated for the run
 * that follows; um98x_flash_discard() frees it.
 *
 * @return ESP_ERR_NOT_FOUND (nothing staged), ESP_ERR_NO_MEM (no PSRAM
 *         buffer: never streamed from flash instead), ESP_ERR_INVALID_CRC
 *         (SHA-256 or size differ from what was confirmed),
 *         ESP_ERR_INVALID_VERSION (wrong model, a downgrade, or a cross-layout
 *         package not confirmed or not yet allowed), ESP_ERR_INVALID_RESPONSE
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
 * Flash the prepared copy. Blocks until done or failed (minutes); feeds the
 * calling task's watchdog in every wait (at most 1 s slices), not only between
 * blocks. Writes the journal before each new state.
 *
 * @param raw_lease RAW lease on the GNSS link, held by the caller throughout.
 */
esp_err_t um98x_flash_run(gnss_link_lease_t raw_lease, const um98x_flash_request_t *req);

/**
 * Boot-time race into the loader after a power cut during a session: stream
 * the stop key at UM98X_FLASH_BAUD for up to max_ms, looking for the loader
 * prompt. Called by the station's early boot step before anything else uses
 * the UART. Only with CONFIG_UM98X_FLASH_RECOVERY_ACTIONS; PROVISIONAL, to be
 * confirmed on hardware.
 * @return ESP_OK when the prompt was seen, ESP_ERR_TIMEOUT otherwise.
 */
esp_err_t um98x_flash_race(gnss_link_lease_t raw_lease, uint32_t max_ms);

/** Request cancellation; only effective before the transfer phase. */
void um98x_flash_cancel(void);

/** Latest progress (safe from any task). */
void um98x_flash_get_progress(um98x_flash_progress_t *out);

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
 * later), including "needs user" with such a phase. An ESP32-only reset
 * never races: the receiver did not reset.
 */
bool um98x_flash_journal_needs_race(const um98x_flash_journal_t *j, bool receiver_was_reset);

#ifdef __cplusplus
}
#endif
