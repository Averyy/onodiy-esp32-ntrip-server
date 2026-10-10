/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * Captive-portal DNS reply builder (pure logic, no IDF includes,
 * host-testable).
 *
 * While the setup AP is up, a small DNS responder answers every A query with
 * the AP's own address, so a phone that joins the AP finds the setup page
 * whatever name it looks up (and its OS shows the "sign in to network"
 * prompt). This file builds the reply for one query datagram; the socket
 * loop lives in network.c.
 *
 * Reply rules (RFC 1035 message format):
 *  - Responses (QR set) and datagrams shorter than a header are dropped (no
 *    reply), so the responder never answers another server's answer.
 *  - Opcodes other than QUERY get NOTIMP; anything but exactly one
 *    well-formed question (labels of at most 63 bytes, name of at most 255,
 *    no compression pointers, type and class present) gets FORMERR. Both are
 *    a bare header that echoes the ID.
 *  - A question of type A or ANY, class IN or ANY, gets one A record: the
 *    name as a pointer to the question, CAPTIVE_DNS_TTL_S, the given address.
 *    Every other type (AAAA, HTTPS, ...) gets NOERROR without answers, so
 *    clients fall back to IPv4 instead of waiting for a timeout.
 *  - The question is echoed as received (case kept); EDNS or other records
 *    after it are ignored, and the reply carries none (ARCOUNT 0).
 *  - Flags: QR, AA, RD copied from the query; RA clear.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Largest query handled (classic DNS over UDP; longer datagrams are cut by the reader). */
#define CAPTIVE_DNS_MAX_QUERY 512

/** Reply buffer size that is always enough for a query of CAPTIVE_DNS_MAX_QUERY bytes. */
#define CAPTIVE_DNS_MAX_REPLY (CAPTIVE_DNS_MAX_QUERY + 16)

/** TTL of the A record: short, so nothing caches the AP address after setup. */
#define CAPTIVE_DNS_TTL_S 10u

/** Response codes used in replies. */
#define CAPTIVE_DNS_RCODE_OK      0
#define CAPTIVE_DNS_RCODE_FORMERR 1
#define CAPTIVE_DNS_RCODE_NOTIMP  4

/**
 * Build the reply to one DNS query.
 *
 * @param query      datagram as received
 * @param query_len  its length (bytes past CAPTIVE_DNS_MAX_QUERY are ignored)
 * @param addr       IPv4 address to answer with, network byte order (a[0] = first octet)
 * @param out        reply buffer
 * @param out_size   its size (CAPTIVE_DNS_MAX_REPLY is always enough)
 * @return reply length, or 0 when nothing should be sent (not a query, too
 *         short, or the reply does not fit in out)
 */
size_t captive_dns_reply(const uint8_t *query, size_t query_len, const uint8_t addr[4], uint8_t *out,
                         size_t out_size);

#ifdef __cplusplus
}
#endif
