/*
 * SPDX-FileCopyrightText: 2026 Avery L.
 * SPDX-License-Identifier: MIT
 *
 * TIME_WAIT: a half-closed socket lets go of its connection there, and a
 * client's reset ends a TIME_WAIT the station holds.
 *
 * The web server ends some connections first (Connection: close, HTTP/1.0,
 * refusals with a body, evicted idle connections), and lwIP then holds the
 * client's port in TIME_WAIT for 2 x MSL (sdkconfig.defaults:
 * CONFIG_LWIP_TCP_MSL).
 *
 * A socket lets go at TIME_WAIT. The web server's lingering close shuts
 * only its sending side (shutdown(SHUT_WR)) and reads on, so the client's
 * FIN puts the connection in TIME_WAIT (or first in CLOSING, when the two
 * FINs crossed) with the socket still on its pcb until the socket's
 * close() (api_msg.c, lwip_netconn_do_close_internal: tcp_arg and tcp_err
 * to NULL). lwIP frees a TIME_WAIT pcb without telling its socket: tcp.c,
 * tcp_kill_timewait (a new connection needs a pcb and all
 * CONFIG_LWIP_MAX_ACTIVE_TCP are taken; the oldest TIME_WAIT goes, and a
 * new one is the oldest when it is the only one) and tcp_slowtmr after
 * 2 x MSL both free one with no error callback, unlike a pcb in any other
 * state (tcp_abandon). The socket would then work on freed memory at its
 * next read or its close(), or on another connection's pcb allocated in
 * the same place: the one that needed it. The window is short (the
 * lingering close sees the FIN and closes), but a client's FIN on one
 * connection and its SYN on the next can arrive together, and a full pool
 * is when it matters. So the socket lets go when the client's FIN reaches it
 * in CLOSING or TIME_WAIT, inside lwIP's own handling of that segment:
 * while a socket holds a connection in FIN_WAIT_1 or FIN_WAIT_2, the hook
 * below puts tw_recv in front of the socket layer's receive callback, and
 * tw_recv, once the FIN went through it, clears the pcb's callbacks and
 * calls the socket's error callback with ERR_CLSD, as lwIP's tcp_input
 * does when a connection whose sending side alone was shut ends in
 * LAST_ACK (TF_CLOSED). The socket layer then forgets the pcb (api_msg.c,
 * err_tcp: conn->pcb.tcp = NULL): the lingering close reads what is queued
 * and then the end, and its close() touches no pcb. The pcb goes on as one
 * a socket closed whole. The rules are in net_core.h (net_tw_let_go).
 *
 * A client's reset ends a TIME_WAIT. A client back on a port the station
 * still holds within that time gets the old connection's ACK to its SYN, and
 * answers it with a reset that carries exactly the station's RCV.NXT. lwIP
 * ignores a reset in TIME_WAIT (tcp_in.c, tcp_timewait_input: "RFC 1337: in
 * TIME_WAIT, ignore RST"), so the client's next SYNs are turned away the
 * same way until TIME_WAIT ends. RFC 9293 section 3.10.7.4 ends TIME-WAIT on
 * that reset instead, as Linux does by default; this hook does the same for
 * a reset that passes RFC 5961's exact-match check (net_core.h,
 * net_tw_reset_ends), so the client's next SYN reaches the listener. Linux
 * (5.10 and later) sends that SYN a few milliseconds after its reset, then
 * again 1 s after its first. Only a TIME_WAIT that no socket holds: with
 * the socket's letting go above there is none, and the check stays as a
 * guard (a QEMU smoke run of an earlier version of this hook, before either,
 * aborted in tcp_recved: a refused 16 KB upload, the client's FIN, then its
 * reset 1.5 ms later, while the lingering close still read).
 *
 * lwIP calls the hook from tcp_input before it handles the segment, on the
 * TCP/IP thread, and its contract (lwip/opt.h, LWIP_HOOK_TCP_INPACKET_PCB)
 * is that the hook calls no TCP function that changes a pcb's state or a
 * pcb list. Setting a pcb's callbacks changes neither, nor does a socket's
 * error callback, which only marks its netconn. For the reset, the hook
 * only moves the pcb's TIME_WAIT start back, which makes that TIME_WAIT
 * over for lwIP's own slow timer (tcp.c, tcp_slowtmr, every 500 ms), and
 * posts tw_sweep to the TCP/IP thread's queue. The queue is first in, first
 * out (this firmware sets no core locking, so every received segment goes
 * through it too), so tw_sweep runs after this tcp_input and before any
 * segment that arrives later, such as the client's next SYN. It frees every
 * TIME_WAIT that is over and that no socket holds, as that timer would at
 * its next tick, with tcp_abort, which for a TIME_WAIT pcb only takes it
 * off tcp_tw_pcbs and frees it, sending nothing (tcp.c, tcp_abandon; lwIP's
 * tcp_kill_timewait frees one the same way). Should the queue be full, the
 * timer frees it within 500 ms. The hook returns ERR_OK, so lwIP still
 * handles every segment itself (a reset in TIME_WAIT: ignored).
 *
 * Every TCP pcb in this firmware belongs to a socket: the socket layer
 * (api_msg.c) is the only caller of tcp_arg and tcp_recv in the image.
 */
#include "net_lwip_hooks.h"

#include "lwip/api.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/tcpip.h"
#include "net_core.h"

#define TW_TICKS ((uint32_t)(2 * TCP_MSL / TCP_SLOW_INTERVAL))

/* The socket layer's receive callback (api_msg.c, recv_tcp), learnt from
 * the first pcb tw_recv goes in front of. TCP/IP thread only. */
static tcp_recv_fn s_sock_recv;

static net_tcp_state_t tw_state(const struct tcp_pcb *pcb)
{
    switch (pcb->state) {
    case FIN_WAIT_1:
    case FIN_WAIT_2:
        return NET_TCP_FIN_WAIT;
    case CLOSING:
        return NET_TCP_CLOSING;
    case TIME_WAIT:
        return NET_TCP_TIME_WAIT;
    default:
        return NET_TCP_OTHER;
    }
}

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

/* The pcb's argument is the netconn on it (what recv_tcp itself asserts). */
static bool tw_netconn_on(const struct tcp_pcb *pcb)
{
    const struct netconn *conn = (const struct netconn *)pcb->callback_arg;
    return conn != NULL && conn->pcb.tcp == pcb;
}

/* The socket lets go of the pcb (see the top of this file). */
static void tw_let_go(struct tcp_pcb *pcb)
{
#if LWIP_CALLBACK_API
    tcp_err_fn errf = pcb->errf;
#endif
    void *arg = pcb->callback_arg;
    tcp_arg(pcb, NULL);
    tcp_recv(pcb, NULL);
    tcp_sent(pcb, NULL);
    tcp_poll(pcb, NULL, 0);
    tcp_err(pcb, NULL);
#if LWIP_CALLBACK_API
    if (errf != NULL && arg != NULL) {
        errf(arg, ERR_CLSD);
    }
#else
    (void)arg;
#endif
}

/* In front of the socket layer's receive callback while a socket holds a
 * connection whose sending side alone it shut. After letting go, a FIN
 * the socket's queue had no room for counts as delivered: the socket has
 * the end of the stream from its error callback instead. */
static err_t tw_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err)
{
    bool fin = p == NULL;
    err_t r = s_sock_recv(arg, pcb, p, err);
    if (r == ERR_ABRT) {
        return r;   /* the pcb is gone (recv_tcp never says so) */
    }
    net_tcp_state_t st = tw_state(pcb);
    bool taken = r == ERR_OK;
    if (net_tw_let_go(st, fin, taken)) {
        bool drop = net_tw_drop_refused(st, fin, taken);
        if (drop) {
            pbuf_free(p);
        }
        tw_let_go(pcb);
        if (fin || drop) {
            r = ERR_OK;
        }
    }
    return r;
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
     * state is a common field, and nothing else of it is read (a listening
     * pcb maps to NET_TCP_OTHER). */
    net_tcp_state_t st = tw_state(pcb);
    if (st == NET_TCP_OTHER) {
        return ERR_OK;
    }
    bool owned = tw_owned(pcb);
    if (net_tw_let_go_now(st, owned)) {
        tw_let_go(pcb);
        owned = false;
    } else if (net_tw_watch(st, owned) && pcb->recv != NULL && pcb->recv != tw_recv && tw_netconn_on(pcb)) {
        if (s_sock_recv == NULL) {
            s_sock_recv = pcb->recv;
        }
        if (pcb->recv == s_sock_recv) {
            tcp_recv(pcb, tw_recv);
        }
    }
    if (st == NET_TCP_TIME_WAIT &&
        net_tw_reset_ends(TCPH_FLAGS(hdr), hdr->seqno, pcb->rcv_nxt, owned)) {
        pcb->tmr = net_tw_over_start(tcp_ticks, TW_TICKS);
        (void)tcpip_try_callback(tw_sweep, NULL);
    }
    return ERR_OK;
}
