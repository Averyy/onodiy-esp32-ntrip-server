/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * LZO1X stream format, as decoded here:
 *  - First byte > 17: copy (byte - 17) literals; the state is that count
 *    (4 for four or more).
 *  - Then instructions. "State" is how many literals the previous
 *    instruction copied (0, 1..3, or 4 for a run of four or more):
 *      0000LLLL  state 0: a literal run of L + 3 bytes (L == 0: 18 plus 255
 *                per zero byte plus the next non-zero byte); state 4.
 *                state 1..3: copy 2 bytes from distance 1 + (L >> 2) + (H << 2)
 *                state 4:    copy 3 bytes from distance 2049 + (L >> 2) + (H << 2)
 *                (H: the next byte; the low two bits of the opcode give the
 *                trailing literal count)
 *      0001HLLL  copy L + 2 bytes (L == 0: 9 plus extension) from distance
 *                16384 + (H << 14) + (D >> 2), D = the next two bytes (LE);
 *                distance 16384 exactly is the end of the stream
 *      001LLLLL  copy L + 2 bytes (L == 0: 33 plus extension) from
 *                distance 1 + (D >> 2), D = the next two bytes (LE)
 *      01LDDDSS / 1LLDDDSS  copy (opcode >> 5) + 1 bytes from distance
 *                1 + DDD + (H << 3), H = the next byte
 *    After a match, the low two bits (of the opcode, or of D for the
 *    two-byte forms) are a count of 0..3 literals that follow at once.
 */
#include "lzo1x.h"

#include <string.h>

/* Length extension: zero bytes add 255 each, the first non-zero byte ends it. */
static lzo1x_result_t extend(const uint8_t *in, size_t in_len, size_t *ip, size_t *n)
{
    for (;;) {
        if (*ip >= in_len) {
            return LZO1X_INPUT_OVERRUN;
        }
        uint8_t b = in[(*ip)++];
        if (b == 0) {
            if (*n > (size_t)-1 / 2) {
                return LZO1X_OUTPUT_OVERRUN;
            }
            *n += 255;
            continue;
        }
        *n += b;
        return LZO1X_OK;
    }
}

static lzo1x_result_t literals(const uint8_t *in, size_t in_len, size_t *ip, uint8_t *out, size_t out_cap,
                               size_t *op, size_t n)
{
    if (n > in_len - *ip) {
        return LZO1X_INPUT_OVERRUN;
    }
    if (n > out_cap - *op) {
        return LZO1X_OUTPUT_OVERRUN;
    }
    memcpy(out + *op, in + *ip, n);
    *ip += n;
    *op += n;
    return LZO1X_OK;
}

lzo1x_result_t lzo1x_decompress(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap, size_t *out_len)
{
    size_t ip = 0;
    size_t op = 0;
    unsigned state = 0;
    lzo1x_result_t r;
    *out_len = 0;
    if (in_len == 0) {
        return LZO1X_NO_END;
    }
    if (in[0] > 17) {
        size_t t = (size_t)in[0] - 17u;
        ip = 1;
        r = literals(in, in_len, &ip, out, out_cap, &op, t);
        if (r != LZO1X_OK) {
            return r;
        }
        state = t < 4 ? (unsigned)t : 4u;
    }
    for (;;) {
        if (ip >= in_len) {
            *out_len = op;
            return LZO1X_NO_END;
        }
        unsigned t = in[ip++];
        size_t len;
        size_t dist;
        unsigned trail;
        if (t < 16) {
            if (state == 0) {
                size_t n = t;
                if (n == 0) {
                    n = 15;
                    r = extend(in, in_len, &ip, &n);
                    if (r != LZO1X_OK) {
                        return r;
                    }
                }
                r = literals(in, in_len, &ip, out, out_cap, &op, n + 3u);
                if (r != LZO1X_OK) {
                    return r;
                }
                state = 4;
                continue;
            }
            if (ip >= in_len) {
                return LZO1X_INPUT_OVERRUN;
            }
            size_t h = in[ip++];
            if (state < 4) {
                dist = 1u + (t >> 2) + (h << 2);
                len = 2;
            } else {
                dist = 2049u + (t >> 2) + (h << 2);
                len = 3;
            }
            trail = t & 3u;
        } else if (t < 32) {
            size_t n = t & 7u;
            if (n == 0) {
                n = 7;
                r = extend(in, in_len, &ip, &n);
                if (r != LZO1X_OK) {
                    return r;
                }
            }
            len = n + 2u;
            if (in_len - ip < 2) {
                return LZO1X_INPUT_OVERRUN;
            }
            unsigned v = (unsigned)in[ip] | ((unsigned)in[ip + 1] << 8);
            ip += 2;
            trail = v & 3u;
            dist = ((size_t)(t & 8u) << 11) + (v >> 2);
            if (dist == 0) {
                *out_len = op;
                return ip == in_len ? LZO1X_OK : LZO1X_INPUT_LEFT;
            }
            dist += 16384u;
        } else if (t < 64) {
            size_t n = t & 31u;
            if (n == 0) {
                n = 31;
                r = extend(in, in_len, &ip, &n);
                if (r != LZO1X_OK) {
                    return r;
                }
            }
            len = n + 2u;
            if (in_len - ip < 2) {
                return LZO1X_INPUT_OVERRUN;
            }
            unsigned v = (unsigned)in[ip] | ((unsigned)in[ip + 1] << 8);
            ip += 2;
            trail = v & 3u;
            dist = 1u + (v >> 2);
        } else {
            if (ip >= in_len) {
                return LZO1X_INPUT_OVERRUN;
            }
            dist = 1u + ((t >> 2) & 7u) + ((size_t)in[ip++] << 3);
            len = (t >> 5) + 1u;
            trail = t & 3u;
        }
        if (dist > op) {
            return LZO1X_LOOKBEHIND;
        }
        if (len > out_cap - op) {
            return LZO1X_OUTPUT_OVERRUN;
        }
        for (size_t i = 0; i < len; i++, op++) {
            out[op] = out[op - dist];   /* byte by byte: overlapping copies repeat */
        }
        if (trail > 0) {
            r = literals(in, in_len, &ip, out, out_cap, &op, trail);
            if (r != LZO1X_OK) {
                return r;
            }
        }
        state = trail;
    }
}
