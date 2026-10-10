/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Inside the flasher component: the io over the RAW lease, shared by the
 * session wrapper (um98x_flash.c) and the bench primitives (um98x_bench.c).
 */
#pragma once

#include <stdbool.h>

#include "fl_io.h"
#include "gnss_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Point the io at this lease (every byte also goes to the text ring and the capture). Returns the io. */
const fl_io_t *um98x_flash_io_setup(gnss_link_lease_t lease);

/** Forget the receiver text kept for the UI. */
void um98x_flash_text_clear(void);

/** One line into the byte capture (and the console when asked); bench builds only. */
void um98x_flash_cap_printf(bool console, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
