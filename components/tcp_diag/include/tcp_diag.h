/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    NET_TCP_SNAPSHOT_UNSENT = 1u << 0,
    NET_TCP_SNAPSHOT_UNACKED = 1u << 1,
    NET_TCP_SNAPSHOT_RTO = 1u << 2,
};

/** A read-only view of one established TCP connection, not remote RTCM age.
 *
 * All byte counts describe TCP sequence space/buffers, including HTTP framing
 * and, for a TLS socket, encrypted records. They exclude bytes still held by
 * the application or TLS library and cannot show remote application progress.
 */
typedef struct {
    uint32_t ack_seq;                 /**< Cumulative ACK position; wraps modulo 2^32. */
    uint32_t outstanding_bytes;       /**< Buffered sequence bytes not cumulatively ACKed. */
    uint32_t send_buffer_used_bytes;  /**< Occupied lwIP send capacity; not a delivery age. */
    uint32_t send_window_bytes;       /**< Peer's current advertised window. */
    uint32_t congestion_window_bytes;
    uint32_t rto_ms;                  /**< Current retransmission timeout, coarse timer units. */
    uint16_t queued_pbufs;            /**< Buffer count, not byte or packet count. */
    uint8_t retransmit_run;           /**< Retry count reset by ACK progress, not a total. */
    uint8_t flags;                    /**< NET_TCP_SNAPSHOT_*; unsent may include retries. */
} net_tcp_snapshot_t;

/** Read a synchronous snapshot using the socket API's reference and TCP/IP
 * serialization. Returns 0 on success, -1 with errno on failure; leaves *out
 * unchanged on failure. Requires an established TCP socket (IPv4 or IPv6).
 *
 * Call from the task owning the connection, at a bounded diagnostic cadence,
 * never from the TCP/IP thread or while holding the TCP/IP core lock. Socket
 * references protect this call, not the caller's fd before or after it: reset
 * all ACK deltas and histories whenever that connection closes or is replaced.
 * A failed sample is unavailable evidence, not a zero backlog. No PCB pointer
 * escapes, and this function changes no socket or transport policy.
 */
int net_tcp_snapshot(int fd, net_tcp_snapshot_t *out);

#ifdef __cplusplus
}
#endif
