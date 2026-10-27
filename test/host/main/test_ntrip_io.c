/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "fixture_util.h"
#include "ntrip_io.h"
#include "ntrip_proto.h"
#include "unity.h"

TEST_CASE("NTRIP I/O: TLS read/write requirements override the application operation", "[ntrip_io]")
{
    /* Buffered application bytes cannot satisfy a write's handshake read. */
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_READ, ntrip_io_wait_for(NTRIP_IO_WANT_READ, false, false));
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_READ, ntrip_io_wait_for(NTRIP_IO_WANT_READ, false, true));
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_WRITE, ntrip_io_wait_for(NTRIP_IO_WANT_WRITE, false, false));
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_WRITE, ntrip_io_wait_for(NTRIP_IO_WANT_WRITE, false, true));
    /* A read that must flush handshake output waits for write readiness,
     * even when application bytes are already buffered in TLS. */
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_WRITE, ntrip_io_wait_for(NTRIP_IO_WANT_WRITE, true, false));
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_WRITE, ntrip_io_wait_for(NTRIP_IO_WANT_WRITE, true, true));
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_READ, ntrip_io_wait_for(NTRIP_IO_WANT_READ, true, false));
    TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_NONE, ntrip_io_wait_for(NTRIP_IO_WANT_READ, true, true));
    TEST_ASSERT_TRUE(ntrip_io_would_block(NTRIP_IO_WANT_READ));
    TEST_ASSERT_TRUE(ntrip_io_would_block(NTRIP_IO_WANT_WRITE));
    static const int finished[] = {NTRIP_IO_CLOSED, NTRIP_IO_ERROR, 1, 4096};
    for (size_t i = 0; i < sizeof(finished) / sizeof(finished[0]); i++) {
        TEST_ASSERT_FALSE(ntrip_io_would_block(finished[i]));
        TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_NONE, ntrip_io_wait_for(finished[i], true, true));
        TEST_ASSERT_EQUAL(NTRIP_IO_WAIT_NONE, ntrip_io_wait_for(finished[i], false, false));
    }
}

TEST_CASE("NTRIP I/O: partial TLS acceptance retries identical arguments across 4096-byte record boundary", "[ntrip_io]")
{
    size_t fixture_len = 0;
    uint8_t *fixture = fx_read("rtcm/centipede_ROBU1_35ep.rtcm3", &fixture_len);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(NTRIP_CHUNK_MAX_PAYLOAD, fixture_len);
    size_t capacity = NTRIP_CHUNK_HEAD_RESERVE + NTRIP_CHUNK_MAX_PAYLOAD + NTRIP_CHUNK_TAIL;
    uint8_t *tx = calloc(1, capacity);
    uint8_t *accepted = calloc(1, capacity);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_NOT_NULL(accepted);
    memcpy(tx + NTRIP_CHUNK_HEAD_RESERVE, fixture, NTRIP_CHUNK_MAX_PAYLOAD);
    size_t start = 0;
    size_t bytes = ntrip_chunk_wrap(tx, NTRIP_CHUNK_MAX_PAYLOAD, &start);
    TEST_ASSERT_EQUAL_UINT32(4104, bytes);
    ntrip_io_write_span_t span = {.data = tx + start, .remaining = bytes};
    const int results[] = {
        NTRIP_IO_WANT_WRITE, 4096, NTRIP_IO_WANT_READ, NTRIP_IO_WANT_READ,
        NTRIP_IO_WANT_WRITE, 4, NTRIP_IO_WANT_READ, 4,
    };
    size_t used = 0;
    for (size_t i = 0; i < sizeof(results) / sizeof(results[0]); i++) {
        const uint8_t *before = span.data;
        size_t remaining = span.remaining;
        int result = results[i];
        if (result > 0) {
            TEST_ASSERT_LESS_OR_EQUAL_UINT32(remaining, (size_t)result);
            memcpy(accepted + used, span.data, (size_t)result);
            used += (size_t)result;
        }
        ntrip_io_write_advance(&span, result);
        if (ntrip_io_would_block(result)) {
            TEST_ASSERT_EQUAL_PTR(before, span.data);
            TEST_ASSERT_EQUAL_UINT32(remaining, span.remaining);
            TEST_ASSERT_NOT_EQUAL(NTRIP_IO_WAIT_NONE, ntrip_io_wait_for(result, false, true));
        }
        TEST_ASSERT_EQUAL_PTR(tx + start + used, span.data);
        TEST_ASSERT_EQUAL_UINT32(bytes - used, span.remaining);
    }
    TEST_ASSERT_EQUAL_UINT32(bytes, used);
    TEST_ASSERT_EQUAL_UINT32(0, span.remaining);
    TEST_ASSERT_EQUAL_MEMORY(tx + start, accepted, bytes);
    TEST_ASSERT_EQUAL_MEMORY(fixture, tx + NTRIP_CHUNK_HEAD_RESERVE, NTRIP_CHUNK_MAX_PAYLOAD);
    free(accepted);
    free(tx);
    free(fixture);
}

TEST_CASE("NTRIP I/O: EOF and errors preserve a partly accepted write for the existing close path", "[ntrip_io]")
{
    size_t len = 0;
    uint8_t *frame = fx_read("rtcm/go-gnss_1077_frame.bin", &len);
    ntrip_io_write_span_t span = {.data = frame, .remaining = len};
    ntrip_io_write_advance(&span, 3);
    ntrip_io_write_advance(&span, NTRIP_IO_ERROR);
    ntrip_io_write_advance(&span, NTRIP_IO_CLOSED);
    TEST_ASSERT_EQUAL_PTR(frame + 3, span.data);
    TEST_ASSERT_EQUAL_UINT32(len - 3, span.remaining);
    TEST_ASSERT_FALSE(ntrip_io_would_block(NTRIP_IO_ERROR));
    TEST_ASSERT_FALSE(ntrip_io_would_block(NTRIP_IO_CLOSED));
    free(frame);
}
