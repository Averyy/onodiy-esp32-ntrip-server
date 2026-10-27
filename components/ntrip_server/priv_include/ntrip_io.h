/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Transport adapters return positive byte counts or one of these values.
 * TLS can need reads while writing, and writes while reading. Preserve that
 * direction through the retry; EOF and errors never mean retry. */
enum {
    NTRIP_IO_CLOSED = -1,
    NTRIP_IO_ERROR = -2,
    NTRIP_IO_WANT_READ = -3,
    NTRIP_IO_WANT_WRITE = -4,
};

typedef enum {
    NTRIP_IO_WAIT_NONE = 0,
    NTRIP_IO_WAIT_READ,
    NTRIP_IO_WAIT_WRITE,
} ntrip_io_wait_t;

bool ntrip_io_would_block(int result);

/** Plaintext already buffered can satisfy an application read, but cannot
 * satisfy a write's TLS handshake read or any operation waiting to write. */
ntrip_io_wait_t ntrip_io_wait_for(int result, bool application_read, bool plaintext_available);

/** Pending application bytes. A WANT result leaves both arguments untouched,
 * as required when retrying a partial TLS record. Only positive acceptance
 * advances the span; the transport must never accept more than remaining. */
typedef struct {
    const uint8_t *data;
    size_t remaining;
} ntrip_io_write_span_t;

void ntrip_io_write_advance(ntrip_io_write_span_t *span, int result);
