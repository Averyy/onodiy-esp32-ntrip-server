/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "captive_dns.h"
#include "unity.h"

static const uint8_t AP_ADDR[4] = { 192, 168, 4, 1 };

/* Build a query: header (id, flags, qdcount) + one encoded name + type/class. */
static size_t make_query(uint8_t *buf, uint16_t id, uint8_t flags_hi, uint16_t qdcount, const char *name,
                         uint16_t qtype, uint16_t qclass)
{
    memset(buf, 0, 12);
    buf[0] = (uint8_t)(id >> 8);
    buf[1] = (uint8_t)id;
    buf[2] = flags_hi;
    buf[4] = (uint8_t)(qdcount >> 8);
    buf[5] = (uint8_t)qdcount;
    size_t off = 12;
    const char *p = name;
    while (*p != '\0') {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        buf[off++] = (uint8_t)len;
        memcpy(buf + off, p, len);
        off += len;
        p += len;
        if (*p == '.') {
            p++;
        }
    }
    buf[off++] = 0;
    buf[off++] = (uint8_t)(qtype >> 8);
    buf[off++] = (uint8_t)qtype;
    buf[off++] = (uint8_t)(qclass >> 8);
    buf[off++] = (uint8_t)qclass;
    return off;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

TEST_CASE("captive DNS answers an A query with the AP address", "[captive_dns]")
{
    uint8_t q[CAPTIVE_DNS_MAX_QUERY];
    uint8_t r[CAPTIVE_DNS_MAX_REPLY];
    size_t qlen = make_query(q, 0xBEEF, 0x01 /* RD */, 1, "connectivitycheck.gstatic.com", 1, 1);
    size_t rlen = captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r));
    TEST_ASSERT_EQUAL_size_t(qlen + 16, rlen);
    TEST_ASSERT_EQUAL_HEX16(0xBEEF, rd16(r));
    TEST_ASSERT_EQUAL_HEX8(0x80 | 0x04 | 0x01, r[2]);   /* QR, AA, RD copied */
    TEST_ASSERT_EQUAL_HEX8(0x00, r[3]);                 /* RA clear, NOERROR */
    TEST_ASSERT_EQUAL_UINT16(1, rd16(r + 4));
    TEST_ASSERT_EQUAL_UINT16(1, rd16(r + 6));
    TEST_ASSERT_EQUAL_UINT16(0, rd16(r + 8));
    TEST_ASSERT_EQUAL_UINT16(0, rd16(r + 10));
    TEST_ASSERT_EQUAL_MEMORY(q + 12, r + 12, qlen - 12);  /* question echoed */
    const uint8_t *a = r + qlen;
    TEST_ASSERT_EQUAL_HEX16(0xC00C, rd16(a));
    TEST_ASSERT_EQUAL_UINT16(1, rd16(a + 2));
    TEST_ASSERT_EQUAL_UINT16(1, rd16(a + 4));
    TEST_ASSERT_EQUAL_UINT32(CAPTIVE_DNS_TTL_S, ((uint32_t)a[6] << 24) | ((uint32_t)a[7] << 16) | ((uint32_t)a[8] << 8) | a[9]);
    TEST_ASSERT_EQUAL_UINT16(4, rd16(a + 10));
    TEST_ASSERT_EQUAL_MEMORY(AP_ADDR, a + 12, 4);
}

TEST_CASE("captive DNS: ANY type and class get the address, other types an empty answer", "[captive_dns]")
{
    uint8_t q[CAPTIVE_DNS_MAX_QUERY];
    uint8_t r[CAPTIVE_DNS_MAX_REPLY];

    size_t qlen = make_query(q, 1, 0, 1, "captive.apple.com", 255, 255);
    TEST_ASSERT_EQUAL_size_t(qlen + 16, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_UINT16(1, rd16(r + 6));

    /* AAAA: NOERROR without answers, so the client falls back to IPv4. */
    qlen = make_query(q, 2, 0, 1, "captive.apple.com", 28, 1);
    TEST_ASSERT_EQUAL_size_t(qlen, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(0x84, r[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, r[3]);
    TEST_ASSERT_EQUAL_UINT16(1, rd16(r + 4));
    TEST_ASSERT_EQUAL_UINT16(0, rd16(r + 6));

    /* HTTPS record (65): the same. */
    qlen = make_query(q, 3, 0, 1, "www.msftconnecttest.com", 65, 1);
    TEST_ASSERT_EQUAL_size_t(qlen, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_UINT16(0, rd16(r + 6));

    /* Class CHAOS: no address. */
    qlen = make_query(q, 4, 0, 1, "version.bind", 1, 3);
    TEST_ASSERT_EQUAL_size_t(qlen, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_UINT16(0, rd16(r + 6));

    /* The root name is a valid question. */
    qlen = make_query(q, 5, 0, 1, "", 1, 1);
    TEST_ASSERT_EQUAL_size_t(12 + 1 + 4, qlen);
    TEST_ASSERT_EQUAL_size_t(qlen + 16, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
}

TEST_CASE("captive DNS never answers a response or a runt", "[captive_dns]")
{
    uint8_t q[CAPTIVE_DNS_MAX_QUERY];
    uint8_t r[CAPTIVE_DNS_MAX_REPLY];
    size_t qlen = make_query(q, 7, 0x80 /* QR */, 1, "example.com", 1, 1);
    TEST_ASSERT_EQUAL_size_t(0, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    qlen = make_query(q, 7, 0, 1, "example.com", 1, 1);
    TEST_ASSERT_EQUAL_size_t(0, captive_dns_reply(q, 11, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_size_t(0, captive_dns_reply(NULL, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_size_t(0, captive_dns_reply(q, qlen, AP_ADDR, NULL, sizeof(r)));
    /* Reply does not fit: nothing sent. */
    TEST_ASSERT_EQUAL_size_t(0, captive_dns_reply(q, qlen, AP_ADDR, r, qlen + 15));
    TEST_ASSERT_EQUAL_size_t(qlen + 16, captive_dns_reply(q, qlen, AP_ADDR, r, qlen + 16));
}

TEST_CASE("captive DNS: NOTIMP for other opcodes, FORMERR for malformed questions", "[captive_dns]")
{
    uint8_t q[CAPTIVE_DNS_MAX_QUERY];
    uint8_t r[CAPTIVE_DNS_MAX_REPLY];

    /* opcode 2 (STATUS) */
    size_t qlen = make_query(q, 0x1234, (uint8_t)(2u << 3) | 0x01, 1, "example.com", 1, 1);
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX16(0x1234, rd16(r));
    TEST_ASSERT_EQUAL_HEX8(0x80 | (2u << 3) | 0x01, r[2]);
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_NOTIMP, r[3]);
    TEST_ASSERT_EQUAL_UINT16(0, rd16(r + 4));

    /* two questions */
    qlen = make_query(q, 9, 0, 2, "example.com", 1, 1);
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_FORMERR, r[3]);

    /* no question */
    qlen = make_query(q, 9, 0, 0, "example.com", 1, 1);
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_FORMERR, r[3]);

    /* type and class missing */
    qlen = make_query(q, 9, 0, 1, "example.com", 1, 1);
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen - 4, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_FORMERR, r[3]);
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen - 1, AP_ADDR, r, sizeof(r)));

    /* name runs past the datagram */
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, 16, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_FORMERR, r[3]);

    /* compression pointer in the question */
    qlen = make_query(q, 9, 0, 1, "example.com", 1, 1);
    q[12] = 0xC0;
    q[13] = 0x0C;
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_FORMERR, r[3]);

    /* label longer than 63 */
    char label[80];
    memset(label, 'a', 64);
    label[64] = '\0';
    qlen = make_query(q, 9, 0, 1, label, 1, 1);
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_FORMERR, r[3]);
}

TEST_CASE("captive DNS: name length limit and longest valid name", "[captive_dns]")
{
    uint8_t q[CAPTIVE_DNS_MAX_QUERY];
    uint8_t r[CAPTIVE_DNS_MAX_REPLY];
    char name[300];

    /* 4 labels of 63 = 4 * 64 + 1 = 257 bytes encoded: over 255 */
    size_t n = 0;
    for (int l = 0; l < 4; l++) {
        memset(name + n, 'b', 63);
        n += 63;
        name[n++] = '.';
    }
    name[n - 1] = '\0';
    size_t qlen = make_query(q, 1, 0, 1, name, 1, 1);
    TEST_ASSERT_EQUAL_size_t(12, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
    TEST_ASSERT_EQUAL_HEX8(CAPTIVE_DNS_RCODE_FORMERR, r[3]);

    /* 3 x 63 + one of 61 = 3 * 64 + 62 + 1 = 255 bytes: the longest allowed */
    n = 0;
    for (int l = 0; l < 3; l++) {
        memset(name + n, 'c', 63);
        n += 63;
        name[n++] = '.';
    }
    memset(name + n, 'd', 61);
    n += 61;
    name[n] = '\0';
    qlen = make_query(q, 1, 0, 1, name, 1, 1);
    TEST_ASSERT_EQUAL_size_t(12 + 255 + 4, qlen);
    TEST_ASSERT_EQUAL_size_t(qlen + 16, captive_dns_reply(q, qlen, AP_ADDR, r, sizeof(r)));
}

TEST_CASE("captive DNS ignores EDNS and trailing records", "[captive_dns]")
{
    uint8_t q[CAPTIVE_DNS_MAX_QUERY];
    uint8_t r[CAPTIVE_DNS_MAX_REPLY];
    size_t qlen = make_query(q, 0x4242, 0x01, 1, "detectportal.firefox.com", 1, 1);
    /* ARCOUNT 1 + an OPT record (root name, type 41, payload 1232, ttl 0, rdlen 0) */
    q[11] = 1;
    const uint8_t opt[] = { 0x00, 0x00, 0x29, 0x04, 0xD0, 0, 0, 0, 0, 0x00, 0x00 };
    memcpy(q + qlen, opt, sizeof(opt));
    size_t rlen = captive_dns_reply(q, qlen + sizeof(opt), AP_ADDR, r, sizeof(r));
    TEST_ASSERT_EQUAL_size_t(qlen + 16, rlen);
    TEST_ASSERT_EQUAL_UINT16(0, rd16(r + 10));   /* ARCOUNT 0 */
    TEST_ASSERT_EQUAL_UINT16(1, rd16(r + 6));

    /* An oversized datagram is read up to the classic limit only. */
    uint8_t big[CAPTIVE_DNS_MAX_QUERY + 100];
    memset(big, 0, sizeof(big));
    memcpy(big, q, qlen);
    big[11] = 0;
    rlen = captive_dns_reply(big, sizeof(big), AP_ADDR, r, sizeof(r));
    TEST_ASSERT_EQUAL_size_t(qlen + 16, rlen);
}
