/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Bench console (bench builds only, CONFIG_BENCH_CONSOLE): an esp_console
 * command line on the UART0 console, beside the log, that drives the
 * receiver flasher's bench tests one at a time. Type `help` for the list.
 *
 * Rules every command follows:
 *  - a command that writes to the receiver needs its confirmation word typed
 *    at the end (shown by `help`); without it nothing happens;
 *  - the receiver link is taken through the station (its normal RAW lease,
 *    casters paused), never around it; refused in receiver maintenance, while
 *    a receiver update journal is open, and while any other receiver session
 *    or job runs;
 *  - every byte sent and received is printed with its time (hex and text)
 *    and kept in a capture ring the web UI's diagnostics can download (admin);
 *  - each ends with one PASS or FAIL line with the values the bench test asks
 *    to record.
 * Nothing runs at boot except the stop key from boot, and only for the one
 * boot after `race_arm` armed it (a one-shot flag, cleared before it runs).
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * From app_main right after the settings and the boot accounting, before
 * anything else uses the receiver's UART: the armed one-shot stop key from
 * boot. Does nothing unless armed (and in builds without the console).
 */
esp_err_t bench_boot_early(void);

/** After the start steps: the console's command line. Does nothing in builds without it. */
esp_err_t bench_start(void);

#ifdef __cplusplus
}
#endif
