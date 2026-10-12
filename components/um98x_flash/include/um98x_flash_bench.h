/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Bench primitives of the receiver flasher, for the bench console only
 * (bench builds: CONFIG_UM98X_FLASH_BYTE_CAPTURE, selected by the console).
 * Each runs under a RAW lease the station took through its normal job path
 * (casters paused, refused in maintenance and while any other session runs)
 * and records every byte in the capture (um98x_flash.h). None sends a data
 * frame; the full flash and the deliberate abort go through the station's
 * flash session instead.
 *
 * Operations (what each records for its PASS/FAIL line):
 *  LISTEN    listen only at a baud for N s: bytes, CRC-valid RTCM frames,
 *            checksum-valid sentences, "$devicename", a loader prompt. PASS:
 *            RTCM or sentences decoded (record_only: always PASS, values only)
 *  VERSION   the version query at a baud: model, firmware, build, the
 *            header's field count. PASS: a CRC-valid reply
 *  ENTRY     version, quiesce, loader baud (verified), the soft reset and
 *            the stop key at the loader baud, the prompt confirmed with
 *            "\r\n" (banner, boot configuration print and menu captured
 *            verbatim), "6", the application and its version again. PASS:
 *            prompt reached and the application back
 *  MENU2     ENTRY up to the prompt, then menu item 2: every text until the
 *            ready line or the firmware submenu, the start bytes for N s
 *            (nothing answered, no frame), CAN CAN (or "q"), back at the
 *            prompt, "6", the version. PASS: application back at the same build
 *  IDLE      ENTRY up to the prompt, then nothing sent for N s (what the
 *            loader does by itself), then "6" (still at the loader) and the
 *            version. PASS: recorded and the application back
 *
 * PROVISIONAL, to be confirmed on hardware: these are the instruments that
 * confirm the rest.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "gnss_link.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UM98X_BENCH_LISTEN = 0,
    UM98X_BENCH_VERSION,
    UM98X_BENCH_ENTRY,
    UM98X_BENCH_MENU2,
    UM98X_BENCH_IDLE,
} um98x_bench_op_t;

typedef struct {
    uint8_t op;               /**< um98x_bench_op_t */
    uint32_t baud;            /**< LISTEN / VERSION: host baud; the others: loader baud */
    uint32_t seconds;         /**< LISTEN, MENU2 (start bytes), IDLE */
    uint32_t app_baud;        /**< the receiver application's baud */
    bool record_only;         /**< LISTEN: values only (a PC flashes over the other port) */
} um98x_bench_req_t;

typedef struct {
    bool pass;
    char line[200];           /**< "PASS ..." / "FAIL ..." with the recorded values */
    uint32_t bytes;
    uint32_t rtcm;
    uint32_t sentences;
    bool devicename;
    bool prompt;
    uint32_t build_before;
    uint32_t build_after;
    uint8_t header_fields;
    uint32_t prompt_ms;
    uint32_t key_bytes;
    uint32_t c_count;
    uint32_t nak_count;
    uint32_t first_start_ms;
    uint32_t start_interval_ms;
    bool ready_line;
    bool submenu;
    bool left_loader;
    uint32_t race_first_key_ms;   /**< 6a: ESP32 reset to the first stop-key byte */
} um98x_bench_res_t;

/** Run one bench operation (see above). */
esp_err_t um98x_flash_bench(gnss_link_lease_t raw_lease, const um98x_bench_req_t *req, um98x_bench_res_t *out);

/**
 * Bench check "stop key from boot": the stop key at the loader baud from
 * now (called from the earliest point of the boot) for max_ms; at the
 * prompt "6" and the application's version at app_baud. Records the time
 * from the ESP32 reset to the first stop-key byte and whether the prompt
 * was reached.
 */
esp_err_t um98x_flash_bench_race(gnss_link_lease_t raw_lease, uint32_t max_ms, uint32_t app_baud,
                                 um98x_bench_res_t *out);

/** Bench: restart the ESP32 right after the journal is written in this state (0 = never). */
void um98x_flash_bench_reset_at(uint8_t state);

#ifdef __cplusplus
}
#endif
