/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * ESP-IDF v6.1's socket option hook supplies a referenced socket on the TCP/IP
 * thread (or under its core lock). This private read-only option uses that
 * lifetime instead of looking up an unpinned socket/PCB by descriptor. The
 * project hook keeps Espressif's other options and releases its inner socket
 * reference exactly as the default hook does. The public getsockopt call owns
 * the outer reference until the copy has returned to the caller.
 */
#include "tcp_diag.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "lwip/api.h"
#include "lwip/priv/sockets_priv.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"

/* Private to this adapter; never sent over the network. A distinct level also
 * avoids claiming a standard TCP option that this lwIP release lacks. */
#define NET_TCP_SNAPSHOT_LEVEL 0x4e54
#define NET_TCP_SNAPSHOT_OPTION 1

_Static_assert(sizeof(net_tcp_snapshot_t) == 28, "TCP snapshot layout changed");
_Static_assert(sizeof(tcpwnd_size_t) <= sizeof(uint32_t), "TCP window exceeds snapshot field");
_Static_assert(sizeof(socklen_t) == sizeof(uint32_t), "lwIP hook optlen ABI changed");
#if LWIP_MPU_COMPATIBLE && !LWIP_TCPIP_CORE_LOCKING
_Static_assert(sizeof(net_tcp_snapshot_t) <= LWIP_SETGETSOCKOPT_MAXOPTLEN,
               "TCP snapshot exceeds lwIP's copied socket option capacity");
#endif

/* Called only by LWIP_HOOK_SOCKETS_GETSOCKOPT. Returning true means this
 * option was handled, including an error; the hook releases its reference. */
bool net_tcp_getsockopt(struct lwip_sock *sock, int level, int optname, void *optval,
                       uint32_t *optlen, int *err)
{
    if (level != NET_TCP_SNAPSHOT_LEVEL || optname != NET_TCP_SNAPSHOT_OPTION) {
        return false;
    }
    LWIP_ASSERT_CORE_LOCKED();
    if (*optlen < sizeof(net_tcp_snapshot_t)) {
        *err = EINVAL;
        return true;
    }
    if (sock->conn == NULL || NETCONNTYPE_GROUP(netconn_type(sock->conn)) != NETCONN_TCP) {
        *err = ENOPROTOOPT;
        return true;
    }
    const struct tcp_pcb *pcb = sock->conn->pcb.tcp;
    if (pcb == NULL || pcb->state != ESTABLISHED) {
        *err = ENOTCONN;
        return true;
    }

    /* ESTABLISHED excludes SYN/FIN sequence-space bytes. Do not label unsent
     * as never transmitted: retransmission moves previously sent segments
     * back there. Scalar reads keep work constant even when a peer stalls. */
    const net_tcp_snapshot_t snapshot = {
        .ack_seq = pcb->lastack,
        .outstanding_bytes = (uint32_t)(pcb->snd_lbb - pcb->lastack),
        .send_buffer_used_bytes = (uint32_t)(TCP_SND_BUF - pcb->snd_buf),
        .send_window_bytes = (uint32_t)pcb->snd_wnd,
        .congestion_window_bytes = (uint32_t)pcb->cwnd,
        .rto_ms = pcb->rto > 0 ? (uint32_t)pcb->rto * TCP_SLOW_INTERVAL : 0,
        .queued_pbufs = pcb->snd_queuelen,
        .retransmit_run = pcb->nrtx,
        .flags = (pcb->unsent != NULL ? NET_TCP_SNAPSHOT_UNSENT : 0) |
                 (pcb->unacked != NULL ? NET_TCP_SNAPSHOT_UNACKED : 0) |
                 ((pcb->flags & TF_RTO) != 0 ? NET_TCP_SNAPSHOT_RTO : 0),
    };
    memcpy(optval, &snapshot, sizeof(snapshot));
    *optlen = sizeof(snapshot);
    *err = 0;
    return true;
}

int net_tcp_snapshot(int fd, net_tcp_snapshot_t *out)
{
    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }
    net_tcp_snapshot_t snapshot;
    socklen_t size = sizeof(snapshot);
    if (getsockopt(fd, NET_TCP_SNAPSHOT_LEVEL, NET_TCP_SNAPSHOT_OPTION, &snapshot, &size) != 0) {
        return -1;
    }
    if (size != sizeof(snapshot)) {
        errno = EIO;
        return -1;
    }
    *out = snapshot;
    return 0;
}
