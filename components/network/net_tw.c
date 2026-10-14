/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * A client's reset ends a TIME_WAIT the station holds.
 *
 * The web server ends some connections first (Connection: close, HTTP/1.0,
 * refusals with a body, evicted idle connections), and lwIP then holds the
 * client's port in TIME_WAIT for 2 x MSL (sdkconfig.defaults:
 * CONFIG_LWIP_TCP_MSL). A client back on that port within that time gets
 * the old connection's ACK to its SYN, and answers it with a reset that
 * carries exactly the station's RCV.NXT. lwIP ignores a reset in TIME_WAIT
 * (tcp_in.c, tcp_timewait_input: "RFC 1337: in TIME_WAIT, ignore RST"),
 * so the client's next SYNs are turned away the same way until TIME_WAIT
 * ends. RFC 9293 section 3.10.7.4 ends TIME-WAIT on that reset instead, as
 * Linux does by default; this hook does the same for a reset that passes
 * RFC 5961's exact-match check (net_core.h, net_tw_reset_ends), so the
 * client's next SYN reaches the listener. Linux (5.10 and later) sends that
 * SYN a few milliseconds after its reset, then again 1 s after its first.
 *
 * Only a TIME_WAIT that no socket holds any more. The web server's
 * lingering close shuts only its sending side (shutdown(SHUT_WR)) and reads
 * on, so a client FIN in that time puts the connection in TIME_WAIT with
 * the socket still on its pcb; the socket lets go of it at its close()
 * (api_msg.c, lwip_netconn_do_close_internal: tcp_arg and tcp_err to NULL).
 * lwIP frees a TIME_WAIT pcb without telling its socket, so ending that
 * TIME_WAIT early would leave the socket on freed memory: a QEMU smoke run
 * of an earlier version of this hook, without this check, aborted in
 * tcp_recved on exactly that (a refused 16 KB upload, the client's FIN,
 * then its reset 1.5 ms later).
 *
 * lwIP calls the hook from tcp_input before tcp_timewait_input, on the
 * TCP/IP thread, and its contract (lwip/opt.h, LWIP_HOOK_TCP_INPACKET_PCB)
 * is that the hook calls no TCP function that changes a pcb's state or a
 * pcb list. So the hook only moves the pcb's TIME_WAIT start back, which
 * makes that TIME_WAIT over for lwIP's own slow timer (tcp.c, tcp_slowtmr,
 * every 500 ms), and posts tw_sweep to the TCP/IP thread's queue. The queue
 * is first in, first out (this firmware sets no core locking, so every
 * received segment goes through it too), so tw_sweep runs after this
 * tcp_input and before any segment that arrives later, such as the
 * client's next SYN. It frees every TIME_WAIT that is over and that no
 * socket holds, as that timer would at its next tick, with tcp_abort, which
 * for a TIME_WAIT pcb only takes it off tcp_tw_pcbs and frees it, sending
 * nothing (tcp.c, tcp_abandon; lwIP's tcp_kill_timewait frees one the same
 * way). Should the queue be full, the timer frees it within 500 ms. The
 * hook returns ERR_OK, so lwIP still handles the reset itself, which in
 * TIME_WAIT is to ignore it.
 */
#include "net_lwip_hooks.h"

#include "lwip/priv/tcp_priv.h"
#include "lwip/tcpip.h"
#include "net_core.h"

#define TW_TICKS ((uint32_t)(2 * TCP_MSL / TCP_SLOW_INTERVAL))

/* A socket still holds the pcb: the netconn layer sets its argument and
 * error callback, and clears both when the socket is closed. */
static bool tw_owned(const struct tcp_pcb *pcb)
{
#if LWIP_CALLBACK_API
    return pcb->callback_arg != NULL || pcb->errf != NULL;
#else
    return pcb->callback_arg != NULL;
#endif
}

static void tw_sweep(void *arg)
{
    (void)arg;
    struct tcp_pcb *pcb = tcp_tw_pcbs;
    while (pcb != NULL) {
        struct tcp_pcb *next = pcb->next;
        if (net_tw_free_now(tcp_ticks, pcb->tmr, TW_TICKS, tw_owned(pcb))) {
            tcp_abort(pcb);
        }
        pcb = next;
    }
}

err_t net_tw_inpacket(struct tcp_pcb *pcb, const struct tcp_hdr *hdr)
{
    /* lwIP passes a listening pcb here too, as a struct tcp_pcb_listen: its
     * state is a common field, and nothing else of it is read. */
    if (pcb->state == TIME_WAIT &&
        net_tw_reset_ends(TCPH_FLAGS(hdr), hdr->seqno, pcb->rcv_nxt, tw_owned(pcb))) {
        pcb->tmr = net_tw_over_start(tcp_ticks, TW_TICKS);
        (void)tcpip_try_callback(tw_sweep, NULL);
    }
    return ERR_OK;
}
