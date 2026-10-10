/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include "captive_dns.h"

#include <stdbool.h>
#include <string.h>

#define HDR_LEN      12u
#define MAX_NAME_LEN 255u
#define MAX_LABEL    63u
#define TYPE_A       1u
#define TYPE_ANY     255u
#define CLASS_IN     1u
#define CLASS_ANY    255u
#define ANSWER_LEN   16u   /* pointer 2 + type 2 + class 2 + ttl 4 + rdlength 2 + address 4 */

#define FLAG_QR      0x80u /* first flags byte */
#define FLAG_AA      0x04u
#define FLAG_RD      0x01u
#define OPCODE_SHIFT 3u
#define OPCODE_MASK  0x0Fu

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

/* Header-only reply: ID echoed, no records. */
static size_t error_reply(const uint8_t *query, uint8_t opcode, uint8_t rcode, uint8_t *out, size_t out_size)
{
    if (out_size < HDR_LEN) {
        return 0;
    }
    memset(out, 0, HDR_LEN);
    out[0] = query[0];
    out[1] = query[1];
    out[2] = (uint8_t)(FLAG_QR | ((opcode & OPCODE_MASK) << OPCODE_SHIFT) | (query[2] & FLAG_RD));
    out[3] = rcode;
    return HDR_LEN;
}

/* Length of the encoded question name starting at off, 0 if malformed. */
static size_t name_len(const uint8_t *q, size_t len, size_t off)
{
    size_t start = off;
    for (;;) {
        if (off >= len) {
            return 0;
        }
        uint8_t l = q[off];
        if (l == 0) {
            off++;
            break;
        }
        if ((l & 0xC0u) != 0 || l > MAX_LABEL) {
            return 0;   /* compression pointer or reserved label type: not in a question we accept */
        }
        off += 1u + l;
        if (off - start > MAX_NAME_LEN) {
            return 0;
        }
    }
    if (off - start > MAX_NAME_LEN) {
        return 0;
    }
    return off - start;
}

size_t captive_dns_reply(const uint8_t *query, size_t query_len, const uint8_t addr[4], uint8_t *out,
                         size_t out_size)
{
    if (query == NULL || addr == NULL || out == NULL || query_len < HDR_LEN) {
        return 0;
    }
    if (query_len > CAPTIVE_DNS_MAX_QUERY) {
        query_len = CAPTIVE_DNS_MAX_QUERY;
    }
    if (query[2] & FLAG_QR) {
        return 0;   /* a response: never answer it */
    }
    uint8_t opcode = (uint8_t)((query[2] >> OPCODE_SHIFT) & OPCODE_MASK);
    if (opcode != 0) {
        return error_reply(query, opcode, CAPTIVE_DNS_RCODE_NOTIMP, out, out_size);
    }
    if (get16(query + 4) != 1) {
        return error_reply(query, 0, CAPTIVE_DNS_RCODE_FORMERR, out, out_size);
    }
    size_t nlen = name_len(query, query_len, HDR_LEN);
    if (nlen == 0 || HDR_LEN + nlen + 4u > query_len) {
        return error_reply(query, 0, CAPTIVE_DNS_RCODE_FORMERR, out, out_size);
    }
    size_t qend = HDR_LEN + nlen + 4u;
    uint16_t qtype = get16(query + HDR_LEN + nlen);
    uint16_t qclass = get16(query + HDR_LEN + nlen + 2u);
    bool answer = (qtype == TYPE_A || qtype == TYPE_ANY) && (qclass == CLASS_IN || qclass == CLASS_ANY);

    size_t total = qend + (answer ? ANSWER_LEN : 0u);
    if (out_size < total) {
        return 0;
    }
    memcpy(out, query, qend);   /* ID and question as received */
    out[2] = (uint8_t)(FLAG_QR | FLAG_AA | (query[2] & FLAG_RD));
    out[3] = CAPTIVE_DNS_RCODE_OK;
    put16(out + 4, 1);
    put16(out + 6, answer ? 1 : 0);
    put16(out + 8, 0);
    put16(out + 10, 0);
    if (answer) {
        uint8_t *a = out + qend;
        put16(a, (uint16_t)(0xC000u | HDR_LEN));   /* name: pointer to the question */
        put16(a + 2, TYPE_A);
        put16(a + 4, CLASS_IN);
        a[6] = (uint8_t)(CAPTIVE_DNS_TTL_S >> 24);
        a[7] = (uint8_t)(CAPTIVE_DNS_TTL_S >> 16);
        a[8] = (uint8_t)(CAPTIVE_DNS_TTL_S >> 8);
        a[9] = (uint8_t)CAPTIVE_DNS_TTL_S;
        put16(a + 10, 4);
        memcpy(a + 12, addr, 4);
    }
    return total;
}
