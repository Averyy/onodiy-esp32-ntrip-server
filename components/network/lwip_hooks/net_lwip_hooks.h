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

#include "lwip/err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct tcp_pcb;
struct tcp_hdr;

/**
 * LWIP_HOOK_TCP_INPACKET_PCB: lwIP calls it with the pcb an incoming segment
 * matched (listening, active or TIME_WAIT; tcp_in.c, tcp_input) and the
 * segment's header, its sequence number already in host order. For a
 * TIME_WAIT pcb, a reset that net_tw_reset_ends() accepts makes lwIP's next
 * slow-timer tick end that TIME_WAIT (components/network/net_tw.c). Always
 * ERR_OK: lwIP handles every segment as it would without the hook.
 */
err_t net_tw_inpacket(struct tcp_pcb *pcb, const struct tcp_hdr *hdr);

#define LWIP_HOOK_TCP_INPACKET_PCB(pcb, hdr, optlen, opt1len, opt2, p) net_tw_inpacket((pcb), (hdr))

#ifdef __cplusplus
}
#endif
