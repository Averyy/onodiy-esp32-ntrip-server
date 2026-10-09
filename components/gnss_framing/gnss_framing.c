/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "gnss_framing.h"

#include <string.h>

enum {
    FRAMER_IDLE = 0,
};

void gnss_framer_init(gnss_framer_t *f, gnss_frame_cb_t cb, void *ctx)
{
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->ctx = ctx;
    f->state = FRAMER_IDLE;
}

void gnss_framer_reset(gnss_framer_t *f)
{
    f->stats.bytes_discarded += f->fill;
    f->fill = 0;
    f->expected = 0;
    f->state = FRAMER_IDLE;
}

void gnss_framer_feed(gnss_framer_t *f, const uint8_t *data, size_t len)
{
    (void)data;
    /* TODO: RTCM3 sync (preamble, reserved bits, length, CRC-24Q) + checked text lines.
     * Until then every byte is counted and discarded. */
    f->stats.bytes_in += len;
    f->stats.bytes_discarded += len;
}

void gnss_framer_get_stats(const gnss_framer_t *f, gnss_framing_stats_t *out)
{
    *out = f->stats;
}
