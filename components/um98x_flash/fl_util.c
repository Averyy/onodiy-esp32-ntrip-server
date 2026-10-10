/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Small helpers shared by the flasher's pure cores: checksums, NUL-safe
 * search, write and sleep wrappers.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "fl_crc.h"
#include "fl_io.h"

uint16_t fl_crc16_xmodem(const uint8_t *data, size_t len)
{
    uint16_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

uint32_t fl_adler32(uint32_t adler, const uint8_t *data, size_t len)
{
    const uint32_t mod = 65521u;
    uint32_t a = adler & 0xFFFFu;
    uint32_t b = adler >> 16;
    while (len > 0) {
        /* 5552 is the largest run that cannot overflow 32 bits before the modulo. */
        size_t n = len < 5552u ? len : 5552u;
        len -= n;
        while (n-- > 0) {
            a += *data++;
            b += a;
        }
        a %= mod;
        b %= mod;
    }
    return (b << 16) | a;
}

uint32_t fl_sum32_le(const uint8_t *data, size_t len)
{
    uint32_t s = 0;
    for (size_t i = 0; i + 4 <= len; i += 4) {
        s += fl_le32(data + i);
    }
    return s;
}

uint8_t fl_sum8(const uint8_t *data, size_t len)
{
    uint8_t s = 0;
    for (size_t i = 0; i < len; i++) {
        s = (uint8_t)(s + data[i]);
    }
    return s;
}

int fl_write_str(const fl_io_t *io, const char *s)
{
    return io->write(io->ctx, (const uint8_t *)s, strlen(s));
}

void fl_sleep(const fl_io_t *io, uint32_t ms)
{
    while (ms > 0) {
        uint32_t n = ms < FL_SLICE_MS ? ms : FL_SLICE_MS;
        io->sleep_ms(io->ctx, n);
        ms -= n;
    }
}

void fl_notef(const fl_io_t *io, const char *fmt, ...)
{
    if (io->note == NULL) {
        return;
    }
    char buf[161];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    io->note(io->ctx, buf);
}

const uint8_t *fl_memmem(const uint8_t *hay, size_t hay_len, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0 || hay_len < n) {
        return NULL;
    }
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (hay[i] == (uint8_t)needle[0] && memcmp(hay + i, needle, n) == 0) {
            return hay + i;
        }
    }
    return NULL;
}
