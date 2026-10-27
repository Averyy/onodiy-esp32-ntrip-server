/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * The station's lwIP hooks. ESP-IDF includes this file from its own hook
 * header (lwip_default_hooks.h, through ESP_IDF_LWIP_HOOK_FILENAME), which
 * the lwIP sources include after their own headers. Only the lwIP library is
 * built with it (components/network/CMakeLists.txt).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "lwip/err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tcp_pcb;
struct tcp_hdr;
struct lwip_sock;

/**
 * LWIP_HOOK_TCP_INPACKET_PCB: lwIP calls it with the pcb an incoming segment
 * matched (listening, active or TIME_WAIT; tcp_in.c, tcp_input) and the
 * segment's header, its sequence number already in host order, before it
 * handles the segment. While a socket that shut only its sending side holds
 * a connection in FIN_WAIT_1 or FIN_WAIT_2, the hook watches that pcb's
 * receive callback, so the socket lets go of the connection once the
 * client's FIN has reached it in CLOSING or TIME_WAIT (net_core.h,
 * net_tw_let_go). For a TIME_WAIT pcb no socket holds, a reset that
 * net_tw_reset_ends() accepts makes lwIP's next slow-timer tick end that
 * TIME_WAIT. Both in components/network/net_tw.c. Always ERR_OK: lwIP
 * handles every segment as it would without the hook.
 */
err_t net_tw_inpacket(struct tcp_pcb *pcb, const struct tcp_hdr *hdr);

#define LWIP_HOOK_TCP_INPACKET_PCB(pcb, hdr, optlen, opt1len, opt2, p) net_tw_inpacket((pcb), (hdr))

/* A private read-only TCP snapshot. Preserve Espressif's extended options and
 * the reference release performed by its default getsockopt hook. The socket
 * implementation has acquired a reference before invoking this hook. */
bool net_tcp_getsockopt(struct lwip_sock *sock, int level, int optname, void *optval,
                       uint32_t *optlen, int *err);

#undef LWIP_HOOK_SOCKETS_GETSOCKOPT
#define LWIP_HOOK_SOCKETS_GETSOCKOPT(s, sock, level, optname, optval, optlen, err)                 \
    ((net_tcp_getsockopt((sock), (level), (optname), (optval), (optlen), (err)) ||                \
      lwip_getsockopt_impl_ext((sock), (level), (optname), (optval), (optlen), (err)))             \
         ? (LWIP_DONE_SOCK(sock), true) : false)

#ifdef __cplusplus
}
#endif
