/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "ntrip_io.h"

bool ntrip_io_would_block(int result)
{
    return result == NTRIP_IO_WANT_READ || result == NTRIP_IO_WANT_WRITE;
}

ntrip_io_wait_t ntrip_io_wait_for(int result, bool application_read, bool plaintext_available)
{
    if (result == NTRIP_IO_WANT_READ) {
        return application_read && plaintext_available ? NTRIP_IO_WAIT_NONE : NTRIP_IO_WAIT_READ;
    }
    return result == NTRIP_IO_WANT_WRITE ? NTRIP_IO_WAIT_WRITE : NTRIP_IO_WAIT_NONE;
}

void ntrip_io_write_advance(ntrip_io_write_span_t *span, int result)
{
    if (result > 0) {
        span->data += (size_t)result;
        span->remaining -= (size_t)result;
    }
}
