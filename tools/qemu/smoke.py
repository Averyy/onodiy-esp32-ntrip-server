#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""Boot the QEMU development build headless and check that it works.

    uv run tools/qemu/smoke.py [--out DIR] [--observe S] [--timeout S]

Needs the ESP-IDF environment (for tools/qemu/run.sh) and a QEMU build in
build-qemu/ (tools/qemu/run.sh --build-only). Standard library only.

What it does:
  1. listens on a free local port for UART1 and starts tools/qemu/run.sh
     --headless with --uart1 connect:<port>, so every byte the firmware sends
     on its receiver link from the first boot is captured;
  2. waits for HTTP 200 on / (the UI page), sets the first admin password
     (POST /api/setup/admin; the QEMU build treats requests as coming over
     the setup AP) and reads GET /api/status with that session: valid JSON,
     API version 1, the station online over the emulated Ethernet; GET
     /api/session names the station "onocoydiy-qemu" (the QEMU build's own
     host name, so it never answers a real station's name on the LAN) and
     its mDNS name "onocoydiy-qemu", and a request with
     "Host: onocoydiy-qemu.local" (also fully qualified,
     "onocoydiy-qemu.local.") passes the Host check;
     HTTP connection handling: a request with a body that no route takes
     gets its real answer (405, 404, the captive redirect) with
     "Connection: close" instead of 413, a body on a route that takes none
     gets 400, "Connection: close" and HTTP/1.0 end the connection after the
     answer (HTTP/1.0 with "Connection: keep-alive" too, answered with
     "Connection: close"), a chunked body is never read (one answer: 411 on
     a route that takes a body, the route's own answer elsewhere, with
     "Connection: close"; the chunks never come back as a second answer),
     and two pipelined HTTP/1.1 requests on one socket are both answered;
     a refused body several TCP windows long is drained after the answer
     (the station's packets in the pcap acknowledge every byte of it and
     the client's FIN);
     both ways a JSON answer goes out, on one kept HTTP/1.1 connection: one
     that fits the web server's response buffer (its size read from
     components/web/web.c) with its Content-Length, a larger one streamed
     (chunked), each expectation following the answer's measured size, both
     ways taken, and the connection answers again after the streamed one;
     streamed answers to HTTP/1.0 carry no chunked framing: GET
     /api/logs?format=text and GET /api/status come without
     Transfer-Encoding (the status, larger than the buffer, also without a
     Content-Length), their body as it is (a log line first, JSON that
     parses) and the connection ends after them. Every answer is read to
     the end its framing gives, waiting up to 30 s for it: QEMU's emulated
     Ethernet drops a frame when the firmware has no free receive buffer,
     and TCP resends it only seconds later, so a late answer is never taken
     for a missing or different one; a "closed" time counts from the end of
     the answer; client addresses in the log (a wrong admin password, a
     refused WebSocket handshake: "from 10.0.2.2"); log levels: "*" at info
     keeps the HTTP server's own tags at error (no "W httpd_uri" line for a
     refused handshake), a level the build cannot show is refused,
     /api/logs says level_max "info", and a cursor that is not a number is
     refused; PUT /api/wifi stores the credentials only in this build
     (applied and trial false) and GET /api/wifi agrees at once (no trial,
     nothing pending) and has no access point to name (bssid and
     tx_power_dbm present, null: the emulated link), no moves between
     access points (roams 0, and no ap_steering at all: roaming is off
     without a radio, so the build never reads it); DELETE /api/journal clears the event journal: GET
     /api/journal from cursor 0 then holds only records numbered after the
     newest one before it (the numbering goes on), GET /api/connections is
     empty and complete;
  3. checks UART1 in both directions: the firmware's receiver detection
     queries (VERSIONA) arrive on the host side, and bytes the host sends
     (plain text no receiver would send: no '$', '#', RTCM preamble or loader
     prompt in it) reach the firmware, which then reports "bytes but nothing
     valid" instead of "no bytes at all" when detection gives up; in the
     no-receiver episode that follows, the GNSS ladder's ESP32 restart is
     held for first setup (health.reboot_hold has "setup": no caster is
     enabled; the status names holds only during a ladder episode); with no
     receiver (GET /api/gnss state "no_receiver") GET /api/gnss and GET
     /api/status both say it does not answer (responding false), a
     signal-group comparison is refused with 409 no_receiver and GET
     /api/gnss/signal-test stays idle; then 400 refused requests (POST
     /api/nope with a body) whose client ends its sending side right after
     the body: every one answered
     404 with "Connection: close", no crash, no restart (the drain must leave
     the client's FIN unread: the one before the fix took it, which let lwIP
     free the connection behind the socket), and 100 more refused by a route
     itself (GET /api/session with a 1 KB body: 400 bad_request); and a
     client back on its local port 15 s after the station ended the
     connection first (the station's TIME_WAIT is 10 s) is answered less
     than 5 s after its connect, and the station's packets show its first
     SYN taken (no bare ACK or reset before the SYN-ACK), so a TIME_WAIT
     over about 15 s fails; and a client back on its port 1 s after such a
     close, inside that TIME_WAIT, gets one bare ACK, answers it with a
     reset carrying the station's RCV.NXT, and its next SYN (libslirp's
     first resend, 5.5 to 6 s later: libslirp's own retransmission
     timer) is taken: the reset ended the TIME_WAIT
     (components/network/net_tw.c); the pcap takes the request's time
     apart, and the station's own part (its bare ACK, its SYN-ACK, the
     request to its FIN) must be under 5 s, as any request's;
  4. keeps watching until --observe seconds after the start (at least 45),
     checks that the web server still answers (GET /, and GET /api/status
     with the session), then stops QEMU and fails on a panic (Guru
     Meditation, abort(), stack overflow, failed assert, a task watchdog
     report) or a reboot loop: a restart without the firmware's own
     "rebooting (reason N)" line before it is a crash, and more than
     --max-boots boots in the window is a loop. It also fails on a GNSS
     ladder restart ("rebooting (reason 6)": held while first setup is not
     finished), on a console note about dropped lines ("[diag:"), and unless
     the console writer task runs and /api/health counts no console drop,
     and the lifecycle task (planned restarts) is pinned to core 0, and
     every "rebooting (reason N)" line names a task pinned to core 0: on the
     APP CPU a restart in this QEMU is cut short in the bootloader (no CPU
     stall), which the boot check names when it sees one.
     QEMU's UART sends every byte at once, so a burst cannot back the
     console up here: the writer's ordering and drop rules are host-tested
     (test/host test_diag_console.c), and this checks that the writer is
     there. The emulated watchdogs are live (run.sh leaves them on), so a
     task that starves IDLE shows up here as on a board. The boot log has
     the mDNS line, the mDNS task's stack in PSRAM (the console writer's
     is too, unless the log says otherwise) and the station's IPv6
     link-local address, and in the 30 s after that line no "is not
     link-local" warning (QEMU's network advertises fec0::/64 to the guest:
     an address from it would mean stateless autoconfiguration is on). Last, POST
     /api/system/reboot: the mDNS goodbye ("mDNS: goodbye sent") is on the
     console before the next boot, with no shutdown-budget warning.

QEMU always stops: on a normal end, Ctrl-C, SIGTERM or SIGHUP the script
stops it on the way out, and run.sh gets --seconds as a backstop (observe
or timeout, whichever is longer, plus 180 s for the half-close, drain and
port reuse checks and a margin) in case this script is killed outright.

Every request but the polls that wait for the station to come up (QEMU's
network holds those connections until the guest listens) is watched, the
checks' own socket exchanges included: one still unanswered after 5 s gets
probes on other connections, the QEMU threads' CPU time and the console
lines meanwhile written to slow-requests.log (no retry, no longer timeout:
the next one shows why), each line naming the request's client port, and
every Ethernet frame goes to qemu-net.pcap (run.sh --pcap), where QEMU's
network keeps that port: its packets are the ones to and from it. A check
that takes such a request's time apart in its packets (the port reuse
check inside TIME_WAIT: about 6 s, all but milliseconds of it libslirp's
SYN resend) says so in the log, and the run's last lines count it apart
from the slow requests nothing explains.

Everything lands in --out: the console log, the UART1 capture (raw bytes and
a timestamped log of both directions), the HTTP answers, qemu-net.pcap,
slow-requests.log (when a request was slow) and summary.txt.
Exit status 0 when every check passed, 1 otherwise.
"""

from __future__ import annotations

import argparse
import gzip
import http.client
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parents[2]
RUN_SH = ROOT / "tools" / "qemu" / "run.sh"

FATAL = ["Guru Meditation", "abort()", "***ERROR*** A stack overflow", "assert failed:", "Backtrace:", "task_wdt:"]
BACKSTOP_MARGIN_S = 60  # run.sh stops QEMU this long after the latest point the checks need it
BOOT_MARK = "ESP-ROM:"
HOSTNAME = "onocoydiy-qemu"                # sdkconfig.qemu CONFIG_LWIP_LOCAL_HOSTNAME
QEMU_PEER = "10.0.2.2"                     # how the guest sees this computer (QEMU user-mode network)
IP6_LINE = re.compile(r"network: IPv6 link-local (fe80:[0-9a-f:]+) on station")
IP6_OTHER = "is not link-local"            # network.c: an IPv6 address other than link-local (never expected)
IP6_QUIET_S = 30                           # seconds after the link-local line that must show no such warning
LOG_STAMP = re.compile(r"^[EWIDV] \((\d+)\) ")  # ESP log line: its milliseconds since boot
GOODBYE = "network: mDNS: goodbye sent"
BUDGET_WARNINGS = ("shutdown budget used up", "shutdown hooks overran")
OWN_REBOOT = re.compile(r"lifecycle: rebooting \(reason \d+\)")
# The same line names the task that runs the restart and its core pin (lifecycle.c do_reboot).
REBOOT_RUNNER = re.compile(r"lifecycle: rebooting \(reason (\d+)\) from (\S+), pinned to (core \d|no core)")
LADDER_REBOOT = "rebooting (reason 6)"   # the GNSS ladder's ESP32 restart (LIFECYCLE_REBOOT_GNSS_LADDER)
CONSOLE_NOTE = "[diag:"                    # the console's note about lines it could not send
MIN_OBSERVE_S = 45                         # longer than the 27 s at which a no-receiver restart came before
DETECT_RESULT = re.compile(r"um98x_ctl: receiver not found \(([^)]*)\)|um98x_ctl: receiver found at")
# Sent to the firmware on UART1: plain text a receiver never sends (no '$' or
# '#' line start, no RTCM preamble, no loader prompt). The framer discards it,
# so the firmware sees bytes but no reply.
LINK_TEST = b"qemu uart1 link test\r\n"
APP_START = "cpu_start: "                 # the app's startup: a boot that got past the 2nd-stage bootloader
BOOTLOADER = "2nd stage bootloader"
SW_CPU_RESET = "rst:0xc (RTC_SW_CPU_RST)"
# Refused requests whose client ends its sending side right after its body (half-close checks): on the
# catch-all, and on a route that refuses the body itself.
HALFCLOSE_REQUESTS = 400
HALFCLOSE_ROUTE_REQUESTS = 100
# The route's refused body: more than esp_http_server reads along with the head (128-byte blocks,
# PARSER_BLOCK_SIZE), so most of it is still on the socket for the drain.
HALFCLOSE_ROUTE_BODY = 1024
# Refused bodies whose client closes right after the answer and the station's FIN, then connects again at once
# (linger_fin_check): the client's FIN reaches the station while its lingering close still reads.
LINGER_FIN_ROUNDS = 20
LINGER_FIN_BODY = 4096
HALFCLOSE_BUDGET_S = 200   # what they, the drain checks and the port reuse checks may add (run.sh's backstop)
# A client reuses its local port this long after the station ended the connection first: the station keeps
# it in TIME_WAIT for 2 x CONFIG_LWIP_TCP_MSL (sdkconfig.defaults: 10 s), so by then it must take the first SYN.
REUSE_AFTER_S = 15.0
# port_reuse_reset_check comes back this soon instead, inside that TIME_WAIT: the station turns the first SYN
# away, libslirp's reset to that ends the TIME_WAIT (components/network/net_tw.c), and libslirp's first resend
# of the SYN is taken. That resend waits for libslirp's retransmission timer, not for the station: a new
# connection's first timeout is 6 s (tcp_newtcpcb: ((TCPTV_SRTTBASE >> 2) + (TCPTV_SRTTDFLT << 2)) >> 1 = 12
# ticks of PR_SLOWHZ, 2 a second), counted down in 500 ms ticks, so 5.5 to 6 s after the first SYN, and the
# reset libslirp sends to the station's bare ACK does not cut it short (tcp_input, SYN_SENT: an unacceptable
# ACK goes to dropwithreset, the SYN stays on that timer). The check takes the request's time apart in the
# pcap: the resend must come RESEND_MIN_S to RESEND_MAX_S after the first SYN (QEMU's main loop may run the
# tick late), and the station's own part (its bare ACK, its SYN-ACK, the request to its FIN) must stay under
# SLOW_S, as any request's must.
REUSE_EARLY_S = 1.0
RESEND_MIN_S = 5.4
RESEND_MAX_S = 6.5
# A request still unanswered after this long is watched: probes on other connections, the QEMU threads'
# CPU time and the console lines meanwhile go to slow-requests.log in the output directory. port_reuse_check
# also wants its second exchange over within it: a SYN the station turns away costs 5.5 s or more here.
SLOW_S = 5.0
# How long an HTTP answer may take to arrive. QEMU's emulated Ethernet MAC drops a received frame when the
# firmware has no free receive buffer ("RX frame dropped" on the console), and TCP sends it again only seconds
# later (an answer 5.5 s late in CI): a late answer is waited for, never read as a missing or a different one.
ANSWER_TIMEOUT_S = 30.0


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Uart1Tap:
    """The host end of UART1: records everything, sends on request."""

    def __init__(self, conn: socket.socket, out: Path, t0: float):
        self.conn = conn
        self.t0 = t0
        self.rx = bytearray()
        self.lock = threading.Lock()
        self.raw = open(out / "uart1-from-firmware.bin", "wb")
        self.log = open(out / "uart1.log", "w", encoding="utf-8")
        self.closed = threading.Event()
        self.thread = threading.Thread(target=self._read, name="uart1", daemon=True)
        self.thread.start()

    def _note(self, direction: str, data: bytes) -> None:
        with self.lock:
            self.log.write(f"{time.monotonic() - self.t0:9.3f} {direction} {len(data):4d} {data!r}\n")
            self.log.flush()

    def _read(self) -> None:
        while True:
            try:
                data = self.conn.recv(4096)
            except OSError:
                break
            if not data:
                break
            self._note("firmware->host", data)
            with self.lock:
                self.rx += data
                self.raw.write(data)
                self.raw.flush()
        self.closed.set()

    def send(self, data: bytes) -> None:
        self._note("host->firmware", data)
        self.conn.sendall(data)

    def received(self) -> bytes:
        with self.lock:
            return bytes(self.rx)

    def close(self) -> None:
        try:
            self.conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.conn.close()
        self.thread.join(timeout=2)
        self.raw.close()
        self.log.close()


class BootReport(NamedTuple):
    boots: int
    unplanned: list[int]   # boots (1-based) that ended in a restart without the firmware's reboot line
    app_cpu: list[int]     # ... of those: a planned restart run on the APP CPU (QEMU artefact, see boot_report)
    off_pro: list[str] = []   # planned restarts whose task is not pinned to core 0: "reason N from TASK, ..."


def boot_report(text: str) -> BootReport:
    """Boots on a console and how each one ended.

    A boot that ends in the next ROM banner without the firmware's own "rebooting (reason N)" line is a
    restart nobody asked for. One kind has a known cause in this QEMU: a planned restart run on the APP CPU.
    esp_restart_noos() there resets the PRO CPU and has the RTC controller stall it, but Espressif's
    esp32s3 machine leaves the stall handler empty (hw/xtensa/esp32s3.c, esp32s3_cpu_stall), so the PRO CPU
    runs the ROM and the 2nd-stage bootloader while the APP CPU goes on, and the APP CPU's own reset of the
    PRO CPU then starts it over: a boot that never got past the bootloader, between two rst:0xc banners,
    right after a planned restart. When it happens it is reported as that, and it still fails.

    Whether it can happen does not hang on scheduling luck here: the firmware runs every restart it plans on
    a task pinned to the PRO CPU (lifecycle.c: a caller pinned there runs its own, any other caller's goes
    to the lifecycle task), and its reboot line names that task and its pin. A planned restart whose task
    is not pinned to core 0 is listed in `off_pro`, even when the restart happened to run on the PRO CPU."""
    parts = text.split(BOOT_MARK)
    n = len(parts) - 1
    unplanned: list[int] = []
    app_cpu: list[int] = []
    for i in range(1, n):   # every boot but the last ends at the next banner
        seg = parts[i]
        if OWN_REBOOT.search(seg):
            continue
        unplanned.append(i)
        if (i >= 2 and OWN_REBOOT.search(parts[i - 1]) and SW_CPU_RESET in seg and BOOTLOADER in seg
                and APP_START not in seg and SW_CPU_RESET in parts[i + 1]):
            app_cpu.append(i)
    off_pro = [f"reason {m.group(1)} from {m.group(2)}, pinned to {m.group(3)}"
               for m in REBOOT_RUNNER.finditer(text) if m.group(3) != "core 0"]
    off_pro += [m.group(0) for m in OWN_REBOOT.finditer(text)
                if not REBOOT_RUNNER.match(text, m.start())]   # a reboot line without its runner
    return BootReport(n, unplanned, app_cpu, off_pro)


def runner_text(r: BootReport, planned: int) -> str:
    """The check's text for where the planned restarts ran."""
    if r.off_pro:
        return (f"{len(r.off_pro)} planned restart(s) not on a task pinned to the PRO CPU: {'; '.join(r.off_pro[:3])}"
                f" (in this QEMU one on the APP CPU boots twice; lifecycle.c hands such a restart to its task)")
    return f"every planned restart ({planned}) ran on a task pinned to core 0"


def boot_text(r: BootReport) -> str:
    """The check's text for restarts without the firmware's reboot line."""
    text = f"{len(r.unplanned)} restart(s) without the firmware's own reboot line"
    if r.app_cpu:
        text += (f" ({len(r.app_cpu)} of them right after a planned restart, cut short inside the 2nd-stage "
                 f"bootloader by a second rst:0xc: the restart ran on the APP CPU, and QEMU has no CPU stall; "
                 f"tools/qemu/README.md)")
    return text


GUEST_IP = "10.0.2.15"                     # the station's address on QEMU's user-mode network
TCP_FIN, TCP_SYN, TCP_RST, TCP_ACK = 0x01, 0x02, 0x04, 0x10


class Segment(NamedTuple):
    flags: int
    seq: int
    ack: int


class Packet(NamedTuple):
    """One TCP segment of a connection in the pcap, with when QEMU passed it (the pcap's clock, seconds)."""
    t: float
    from_guest: bool
    seg: Segment
    payload: int    # bytes of data in it


def tcp_packets(pcap: Path, peer_port: int) -> list[Packet]:
    """Every TCP segment between the station's port 80 and `peer_port` in a run.sh --pcap file (Ethernet frames,
    IPv4), both ways, in the order QEMU passed them. QEMU's user-mode network forwards a host connection with
    the client's own port, so `peer_port` is the host socket's local port."""
    try:
        d = pcap.read_bytes()
    except OSError:
        return []
    if len(d) < 24 or d[:4] != b"\xd4\xc3\xb2\xa1":
        return []
    out = []
    off = 24
    guest = bytes(int(x) for x in GUEST_IP.split("."))
    while off + 16 <= len(d):
        t = int.from_bytes(d[off:off + 4], "little") + int.from_bytes(d[off + 4:off + 8], "little") / 1e6
        incl = int.from_bytes(d[off + 8:off + 12], "little")
        pkt = d[off + 16:off + 16 + incl]
        off += 16 + incl
        if len(pkt) < 14 + 20 or pkt[12:14] != b"\x08\x00":
            continue
        ip = pkt[14:]
        ihl = (ip[0] & 0x0F) * 4
        if ip[9] != 6 or len(ip) < ihl + 14:
            continue
        tcp = ip[ihl:]
        ports = (int.from_bytes(tcp[0:2], "big"), int.from_bytes(tcp[2:4], "big"))
        if ip[12:16] == guest and ports == (80, peer_port):
            from_guest = True
        elif ip[16:20] == guest and ports == (peer_port, 80):
            from_guest = False
        else:
            continue
        payload = int.from_bytes(ip[2:4], "big") - ihl - (tcp[12] >> 4) * 4
        out.append(Packet(t, from_guest, Segment(tcp[13], int.from_bytes(tcp[4:8], "big"),
                                                 int.from_bytes(tcp[8:12], "big")), max(0, payload)))
    return out


def tcp_segments(pcap: Path, peer_port: int) -> list[tuple[bool, Segment]]:
    """tcp_packets without the times: (True when the station sent it, the segment)."""
    return [(p.from_guest, p.seg) for p in tcp_packets(pcap, peer_port)]


def tcp_from_guest(pcap: Path, peer_port: int) -> list[Segment]:
    """Every TCP segment the station sent from its port 80 to `peer_port` (tcp_segments)."""
    return [s for from_guest, s in tcp_segments(pcap, peer_port) if from_guest]


def refused_syns(segments: list[tuple[bool, Segment]]) -> list[Segment]:
    """The station's answers to a client's SYN other than a SYN-ACK: every segment it sent while a SYN from the
    client (tcp_segments) was not yet acknowledged by its SYN-ACK. On a port the station still holds in
    TIME_WAIT, lwIP answers a SYN with a bare ACK, or with a reset when the SYN falls inside the old window
    (tcp_in.c, tcp_timewait_input), and takes no connection: any segment here means a SYN was turned away."""
    waiting: set[int] = set()   # the client's SYNs (by sequence number) without the station's SYN-ACK yet
    taken: set[int] = set()
    out: list[Segment] = []
    for from_guest, s in segments:
        if not from_guest:
            if s.flags & (TCP_SYN | TCP_ACK) == TCP_SYN and s.seq not in taken:
                waiting.add(s.seq)
        elif s.flags & TCP_SYN:
            isn = (s.ack - 1) % (1 << 32)
            waiting.discard(isn)
            taken.add(isn)
        elif waiting:
            out.append(s)
    return out


def client_resets(segments: list[tuple[bool, Segment]]) -> list[Segment]:
    """Every reset the client sent (tcp_segments). libslirp answers the station's bare ACK to its SYN with a
    reset whose sequence number is that ACK's acknowledgment number (RFC 9293 section 3.10.7.3), which is the
    station's RCV.NXT of the connection it holds."""
    return [s for from_guest, s in segments if not from_guest and s.flags & TCP_RST]


def syns_until_taken(segments: list[tuple[bool, Segment]]) -> int | None:
    """How many SYNs the client sent (the first and every resend) up to the station's first SYN-ACK, which
    answers the last of them; None without a SYN-ACK."""
    n = 0
    for from_guest, s in segments:
        if from_guest:
            if s.flags & TCP_SYN:
                return n
        elif s.flags & (TCP_SYN | TCP_ACK) == TCP_SYN:
            n += 1
    return None


class ReconnectTime(NamedTuple):
    """Where the time of a connection whose first SYN the station turned away went, from its packets
    (reconnect_time). resend: from the client's first SYN to the SYN the station took (the client's own timer:
    libslirp's here); station: the station's own part, its answer to the turned-away SYN, its SYN-ACK to the
    SYN it took and the request to the end of its answer (its FIN)."""
    resend: float
    station: float
    turned: float    # the station's answer to the first SYN
    synack: float    # its SYN-ACK to the SYN it took
    answer: float    # from the request to the end of its answer


def reconnect_time(packets: list[Packet]) -> ReconnectTime | None:
    """ReconnectTime for one connection's packets (tcp_packets after the connection before it), None without
    a turned-away SYN, a SYN-ACK, a request or the station's FIN after it."""
    syns = [p for p in packets if not p.from_guest and p.seg.flags & (TCP_SYN | TCP_ACK) == TCP_SYN]
    synack = next((p for p in packets if p.from_guest and p.seg.flags & TCP_SYN), None)
    if not syns or synack is None or syns[0].t >= synack.t:
        return None
    turned = next((p for p in packets if p.from_guest and syns[0].t <= p.t < synack.t), None)
    taken = [p for p in syns if p.t <= synack.t][-1]
    request = next((p for p in packets if not p.from_guest and p.payload > 0 and p.t >= synack.t), None)
    fin = next((p for p in packets if p.from_guest and p.seg.flags & TCP_FIN and request and p.t >= request.t),
               None)
    if turned is None or taken is syns[0] or request is None or fin is None:
        return None
    parts = (turned.t - syns[0].t, synack.t - taken.t, fin.t - request.t)
    return ReconnectTime(taken.t - syns[0].t, sum(parts), *parts)


class ReusePair(NamedTuple):
    """Smoke.reuse_pair: two connections from one local port, the station closing each first."""
    first: Answer
    closed: float | None                # when the station's FIN of the first came (None: not seen)
    first_close: bool | None            # guest_closed_first for the first connection
    before: list[int]                   # the first connection's SYN-ACK ISNs
    again: Answer
    took: float                         # seconds from the second connect to the end of its answer
    later: list[int]                    # the second connection's SYN-ACK ISNs
    resent: int                         # SYN-ACKs sent again (same ISN)
    second: list[tuple[bool, Segment]]  # every segment both ways after the first connection's
    packets: list[Packet] = []          # the same with their times
    watched: Watched | None = None      # the second exchange's request, as the watch on slow requests saw it


def synack_isns(segments: list[Segment]) -> list[int]:
    """The initial sequence numbers of the station's SYN-ACKs, in the order first seen: one per connection it
    accepted. lwIP sends a SYN-ACK again when the client's ACK does not reach it (QEMU's MAC drops a received
    frame when the firmware has no free buffer), with the same ISN, so a resent one never counts as another
    connection."""
    out: list[int] = []
    for x in segments:
        if x.flags & TCP_SYN and x.seq not in out:
            out.append(x.seq)
    return out


def guest_closed_first(segments: list[Segment], sent: int) -> bool | None:
    """Whether the station sent its FIN before the client's FIN reached it: its FIN acknowledges the client's
    `sent` bytes and no FIN (the station then holds the connection in TIME_WAIT). None without a SYN-ACK or
    a FIN from the station."""
    syn = next((x for x in segments if x.flags & TCP_SYN), None)
    fin = next((x for x in segments if x.flags & TCP_FIN), None)
    if syn is None or fin is None:
        return None
    return (fin.ack - syn.ack) % (1 << 32) == sent


def acked_by_guest(segments: list[Segment]) -> int | None:
    """Bytes of the client's stream the station acknowledged (its FIN counts one), from its SYN-ACK on; None
    without a SYN-ACK."""
    syn = next((x for x in segments if x.flags & TCP_SYN), None)
    if syn is None:
        return None
    return max((x.ack - syn.ack) % (1 << 32) for x in segments if x.flags & TCP_ACK)


def qemu_pid_from(run_log: Path) -> int | None:
    """The QEMU process run.sh started (its "QEMU pid N" line)."""
    try:
        m = re.search(r"^QEMU pid (\d+)$", run_log.read_text(encoding="utf-8", errors="replace"), re.M)
    except OSError:
        return None
    return int(m.group(1)) if m else None


def qemu_threads(pid: int) -> str:
    """CPU time of each QEMU thread: Linux /proc (tid:name:state:utime+stime ticks), else ps -M."""
    task = Path(f"/proc/{pid}/task")
    if task.is_dir():
        out = []
        for t in sorted(task.iterdir(), key=lambda p: int(p.name)):
            try:
                st = (t / "stat").read_text()
            except OSError:
                continue
            name = st[st.index("(") + 1:st.rindex(")")]
            f = st[st.rindex(")") + 2:].split()
            out.append(f"{t.name}:{name}:{f[0]}:{int(f[11]) + int(f[12])}")
        return " ".join(out)
    try:
        r = subprocess.run(["ps", "-M", "-p", str(pid)], capture_output=True, text=True, timeout=5)
        return " | ".join(" ".join(line.split()) for line in r.stdout.splitlines()[1:])
    except (OSError, subprocess.SubprocessError) as e:
        return f"ps failed: {e}"


class SlowWatch:
    """Evidence for an HTTP request that takes long, instead of a retry or a longer timeout.

    Every request is reported here (Watched: request() and the checks that use a socket themselves). Once one
    has waited SLOW_S, a thread writes to slow-requests.log, every second until it ends: a probe on another
    connection (GET /, does the server answer meanwhile?), the QEMU threads' CPU time (is the emulator running,
    or starved by the host?) and every console line printed since the request began (what the firmware did
    meanwhile), all on the host's clock. Each line names the request's client port: the pcap (run.sh --pcap)
    shows its own packets."""

    def __init__(self, out: Path, console: Path, run_log: Path, port: int, t0: float):
        self.path = out / "slow-requests.log"
        self.console = console
        self.run_log = run_log
        self.port = port
        self.t0 = t0
        self.lock = threading.Lock()
        self.count = 0
        self.explained: list[str] = []

    def _write(self, text: str) -> None:
        with self.lock, open(self.path, "a", encoding="utf-8") as f:
            f.write(f"{time.monotonic() - self.t0:9.3f} {text}\n")

    def _console_size(self) -> int:
        try:
            return self.console.stat().st_size
        except OSError:
            return 0

    def _probe(self) -> str:
        t = time.monotonic()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=4)
            try:
                conn.request("GET", "/")
                resp = conn.getresponse()
                resp.read()
                return f"probe GET / on another connection: HTTP {resp.status} in {time.monotonic() - t:.2f} s"
            finally:
                conn.close()
        except (OSError, http.client.HTTPException) as e:
            return f"probe GET / on another connection: {type(e).__name__} {e} after {time.monotonic() - t:.2f} s"

    def begin(self, what: str) -> dict:
        w = {"what": what, "t": time.monotonic(), "pos": self._console_size(), "done": threading.Event(),
             "watching": False, "outcome": ""}
        w["thread"] = threading.Thread(target=self._watch, args=(w,), name="slow-watch", daemon=True)
        w["thread"].start()
        return w

    def _console_lines(self, w: dict) -> None:
        try:
            with open(self.console, "rb") as f:
                f.seek(w["pos"])
                data = f.read()
        except OSError:
            return
        if b"\n" not in data:
            return
        data = data[:data.rindex(b"\n") + 1]
        w["pos"] += len(data)
        for line in data.decode("utf-8", "replace").splitlines():
            self._write(f"{w['what']}: console: {line}")

    def _watch(self, w: dict) -> None:
        if w["done"].wait(SLOW_S):
            return
        w["watching"] = True
        with self.lock:
            self.count += 1
        self._write(f"{w['what']}: no answer after {SLOW_S:.0f} s, watching")
        pid = qemu_pid_from(self.run_log)
        while True:
            self._console_lines(w)
            if w["done"].is_set():
                break
            self._write(f"{w['what']}: {self._probe()}")
            self._write(f"{w['what']}: QEMU threads (pid {pid}): {qemu_threads(pid) if pid else 'pid unknown'}")
            w["done"].wait(1.0)
        self._write(f"{w['what']}: {w['outcome']} after {time.monotonic() - w['t']:.2f} s")

    def end(self, w: dict, outcome: str) -> None:
        w["outcome"] = outcome
        w["done"].set()
        if w["watching"]:
            w["thread"].join(timeout=10)

    def explain(self, what: str, why: str) -> None:
        """A watched request whose time its own packets account for (a check measured them): the log says how,
        and the run's last lines count it apart from the ones nothing explains."""
        with self.lock:
            self.explained.append(f"{what}: {why}")
        self._write(f"{what}: {why}")


_slow_watch: SlowWatch | None = None


def watch_requests(out: Path, console: Path, run_log: Path, port: int, t0: float) -> None:
    """Watch every request from now on (slow-requests.log in `out`)."""
    global _slow_watch
    _slow_watch = SlowWatch(out, console, run_log, port, t0)


def slow_requests() -> int:
    """Requests that took SLOW_S or longer so far (each one in slow-requests.log)."""
    return _slow_watch.count if _slow_watch is not None else 0


def slow_explained() -> list[str]:
    """Those of them a check explained from their packets (SlowWatch.explain), each with its reason."""
    return list(_slow_watch.explained) if _slow_watch is not None else []


class Watched:
    """One request reported to the watch on slow requests (slow-requests.log), by request() and by the checks
    that use a socket themselves. port() adds the client's local port to its name once connected: QEMU's
    network gives the station that same port, so the request's packets are the ones to and from it in
    qemu-net.pcap (a SYN that got a bare ACK there met a port the station still held in TIME_WAIT). `outcome`
    is what its last line in the log says; an exception ends it with the exception's text. `on` False: not
    watched (the polls that wait for the station to come up)."""

    def __init__(self, what: str, on: bool = True):
        self.w = _slow_watch.begin(what) if _slow_watch is not None and on else None
        self.outcome = "ended"

    def port(self, sock: socket.socket | None) -> None:
        if self.w is not None and sock is not None:
            try:
                self.w["what"] += f" (client port {sock.getsockname()[1]})"
            except OSError:
                pass

    @property
    def slow(self) -> bool:
        """It went SLOW_S without its answer and was watched (a line in slow-requests.log)."""
        return self.w is not None and self.w["watching"]

    def explain(self, why: str) -> None:
        """Its time, accounted for by its packets: SlowWatch.explain (only when it was watched as slow)."""
        if self.slow and _slow_watch is not None:
            _slow_watch.explain(self.w["what"], why)

    def __enter__(self) -> Watched:
        return self

    def __exit__(self, kind, err, tb) -> None:
        if self.w is not None and _slow_watch is not None:
            _slow_watch.end(self.w, f"{kind.__name__}: {err}" if kind is not None else self.outcome)


def answer_text(a: Answer) -> str:
    """An answer read off a socket, for the watch's last line."""
    return f"HTTP {a.status or 'no answer'}{'' if a.complete else ' (incomplete)'}"


def request(port: int, method: str, path: str, body: dict | None = None, cookie: str | None = None,
            timeout: float = 10.0, host: str | None = None, watch: bool = True) -> tuple[int, dict, bytes]:
    """One HTTP request (a new connection), its answer read whole. `watch`: False for the polls that wait for
    the station to come up (QEMU's network holds a connection until the guest listens: slow by design)."""
    with Watched(f"{method} {path}", on=watch) as w:
        r = _request(port, method, path, body, cookie, timeout, host, w)
        w.outcome = f"HTTP {r[0]}"
        return r


def _request(port: int, method: str, path: str, body: dict | None, cookie: str | None, timeout: float,
             host: str | None, w: Watched) -> tuple[int, dict, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    headers = {"Accept-Encoding": "gzip"}
    if host:
        headers["Host"] = host
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    if cookie:
        headers["Cookie"] = cookie
    try:
        conn.connect()
        w.port(conn.sock)
        conn.request(method, path, body=data, headers=headers)
        resp = conn.getresponse()
        raw = resp.read()
        hdrs = {k.lower(): v for k, v in resp.getheaders()}
        if hdrs.get("content-encoding") == "gzip":
            raw = gzip.decompress(raw)
        return resp.status, hdrs, raw
    finally:
        conn.close()


def raw_exchange(port: int, data: bytes, timeout: float = ANSWER_TIMEOUT_S) -> tuple[bytes, float | None]:
    """Send raw bytes, read until the server closes or `timeout`: (bytes, seconds from the last byte received to
    the end of the connection, or None when it did not end). Measured from the answer, not from the request: an
    answer that a lost frame delayed (see ANSWER_TIMEOUT_S) still shows whether the server closed right after it."""
    t0 = time.monotonic()
    last = t0
    got = bytearray()
    with Watched(data.split(b"\r\n", 1)[0].decode("latin-1")) as w, \
            socket.create_connection(("127.0.0.1", port), timeout=timeout) as c:
        w.port(c)
        c.sendall(data)
        c.settimeout(0.2)
        while time.monotonic() - t0 < timeout:
            try:
                chunk = c.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                chunk = b""
            if not chunk:
                w.outcome = f"{len(got)} bytes, then the end of the connection"
                return bytes(got), time.monotonic() - last
            got += chunk
            last = time.monotonic()
        w.outcome = f"{len(got)} bytes, the connection still open"
    return bytes(got), None


def secs(t: float | None) -> str:
    """A time for a check's text (None: it did not happen within the timeout)."""
    return f"(not within {ANSWER_TIMEOUT_S:.0f} s)" if t is None else f"{t:.2f} s"


class Answer(NamedTuple):
    """One HTTP answer as read off its socket."""
    status: int                  # 0: no status line
    head: bytes                  # status line and header lines
    headers: dict[str, str]      # lower-case names
    body: bytes                  # without chunked framing
    framing: str                 # "chunked", "length" (Content-Length), "close" (to the end), "" (none: incomplete)
    complete: bool               # the answer ended where its framing says
    seconds: float               # from the read's start to its end


class AnswerReader:
    """HTTP/1.x answers read off one socket in turn (keep-alive, pipelining), each until its framing says it
    ended: Content-Length, the last chunk, or the end of the connection. Never a guess from a quiet socket."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = bytearray()
        self.eof = False

    def _fill(self, deadline: float) -> bool:
        """More bytes; False once the connection ended or the deadline passed."""
        while not self.eof:
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            self.sock.settimeout(min(left, 0.5))
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                self.eof = True
                return False
            if not chunk:
                self.eof = True
                return False
            self.buf += chunk
            return True
        return False

    def _until(self, need, deadline: float) -> bool:
        while not need():
            if not self._fill(deadline):
                return need()
        return True

    def read(self, timeout: float = ANSWER_TIMEOUT_S) -> Answer:
        t0 = time.monotonic()
        deadline = t0 + timeout

        def done(status=0, head=b"", headers=None, body=b"", framing="", complete=False) -> Answer:
            return Answer(status, head, headers or {}, bytes(body), framing, complete, time.monotonic() - t0)

        if not self._until(lambda: b"\r\n\r\n" in self.buf, deadline):
            return done(head=bytes(self.buf))
        head, _, rest = bytes(self.buf).partition(b"\r\n\r\n")
        self.buf = bytearray(rest)
        lines = head.split(b"\r\n")
        m = re.match(rb"HTTP/1\.[01] (\d{3})", lines[0])
        status = int(m.group(1)) if m else 0
        headers = {}
        for line in lines[1:]:
            name, sep, value = line.partition(b":")
            if sep:
                headers[name.strip().lower().decode("latin-1")] = value.strip().decode("latin-1")
        if 100 <= status < 200 or status in (204, 304):
            return done(status, head, headers, b"", "length", True)   # no body (a WebSocket's 101 included)
        if "chunked" in headers.get("transfer-encoding", "").lower():
            body = bytearray()
            while True:
                if not self._until(lambda: b"\r\n" in self.buf, deadline):
                    return done(status, head, headers, body, "chunked")
                size_line, _, rest = bytes(self.buf).partition(b"\r\n")
                try:
                    size = int(size_line.split(b";")[0].strip(), 16)
                except ValueError:
                    return done(status, head, headers, body, "chunked")
                if size == 0:
                    # The last chunk, then trailer lines up to an empty one (none here: "0\r\n\r\n").
                    self.buf = bytearray(rest)
                    if not self._until(lambda: self.buf.startswith(b"\r\n") or b"\r\n\r\n" in self.buf, deadline):
                        return done(status, head, headers, body, "chunked")
                    end = 2 if self.buf.startswith(b"\r\n") else self.buf.index(b"\r\n\r\n") + 4
                    del self.buf[:end]
                    return done(status, head, headers, body, "chunked", True)
                need = len(size_line) + 2 + size + 2
                if not self._until(lambda: len(self.buf) >= need, deadline):
                    return done(status, head, headers, body, "chunked")
                data = bytes(self.buf[len(size_line) + 2:need])
                if not data.endswith(b"\r\n"):
                    return done(status, head, headers, body, "chunked")
                body += data[:-2]
                del self.buf[:need]
        if "content-length" in headers:
            try:
                n = int(headers["content-length"])
            except ValueError:
                return done(status, head, headers, b"", "length")
            ok = self._until(lambda: len(self.buf) >= n, deadline)
            body = bytes(self.buf[:n])
            del self.buf[:n]
            return done(status, head, headers, body, "length", ok)
        self._until(lambda: False, deadline)   # to the end of the connection
        body = bytes(self.buf)
        self.buf.clear()
        return done(status, head, headers, body, "close", self.eof)

    def wait_close(self, seconds: float) -> float | None:
        """Seconds until the server ended the connection, or None (bytes that still come are kept in buf)."""
        t0 = time.monotonic()
        while self._fill(t0 + seconds):
            pass
        return time.monotonic() - t0 if self.eof else None


def response_buffer_size() -> int | None:
    """The web server's response buffer (components/web/web.c, s_http_buf): a JSON answer larger than it is
    streamed (chunked to HTTP/1.1), one that fits goes out with its Content-Length."""
    m = re.search(r"\bs_http_buf\[(\d+)\];", (ROOT / "components" / "web" / "web.c").read_text(encoding="utf-8"))
    return int(m.group(1)) if m else None


def raw_request(port: int, method: str, path: str, body: bytes, headers: dict[str, str] | None = None,
                timeout: float = 30.0) -> tuple[int, dict, bytes]:
    """A request with any method and a raw body (http.client, Content-Length set)."""
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    with Watched(f"{method} {path} ({len(body)}-byte body)") as w:
        try:
            conn.connect()
            w.port(conn.sock)
            conn.request(method, path, body=body, headers=headers or {})
            resp = conn.getresponse()
            raw = resp.read()
            w.outcome = f"HTTP {resp.status}"
            return resp.status, {k.lower(): v for k, v in resp.getheaders()}, raw
        finally:
            conn.close()


def error_code(body: bytes) -> tuple[str | None, str | None]:
    try:
        err = json.loads(body).get("error") or {}
        return err.get("code"), err.get("field")
    except (ValueError, AttributeError):
        return None, None


class Smoke:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.out = Path(args.out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.console = self.out / "console.log"
        self.results: list[tuple[bool, str]] = []
        self.proc: subprocess.Popen | None = None
        self.tap: Uart1Tap | None = None
        self.cookie: str | None = None
        self.t0 = time.monotonic()

    def check(self, ok: bool, text: str) -> bool:
        self.results.append((ok, text))
        print(("PASS " if ok else "FAIL ") + text, flush=True)
        return ok

    def log_text(self) -> str:
        try:
            return self.console.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def elapsed(self) -> float:
        return time.monotonic() - self.t0

    def start(self) -> None:
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        uart1_port = listener.getsockname()[1]
        self.http_port = self.args.http_port or free_port()
        backstop = max(self.args.observe, self.args.timeout) + HALFCLOSE_BUDGET_S + BACKSTOP_MARGIN_S
        pcap = self.out / "qemu-net.pcap"
        cmd = [str(RUN_SH), "--no-build", "--headless", str(self.console), "--uart1", f"connect:{uart1_port}",
               "--http-port", str(self.http_port), "--seconds", str(backstop), "--pcap", str(pcap)]
        print("starting: " + " ".join(cmd), flush=True)
        self.console.unlink(missing_ok=True)
        pcap.unlink(missing_ok=True)
        (self.out / "slow-requests.log").unlink(missing_ok=True)
        self.t0 = time.monotonic()
        watch_requests(self.out, self.console, self.out / "run.log", self.http_port, self.t0)
        self.proc = subprocess.Popen(cmd, stdout=open(self.out / "run.log", "w"), stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        listener.settimeout(1)
        conn = None
        try:
            while conn is None and self.elapsed() < 60 and self.proc.poll() is None:
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    pass
        finally:
            listener.close()
        if self.check(conn is not None, "QEMU connected UART1 to the host"):
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            self.tap = Uart1Tap(conn, self.out, self.t0)

    def stop(self) -> None:
        if self.proc is not None and self.proc.poll() is None:
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
        if self.tap is not None:
            self.tap.close()

    def wait_http(self) -> bool:
        deadline = self.t0 + self.args.timeout
        last = "no answer"
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                last = f"QEMU exited ({self.proc.returncode})"
                break
            try:
                status, hdrs, body = request(self.http_port, "GET", "/", watch=False)
                last = f"HTTP {status}"
                if status == 200:
                    (self.out / "index.html").write_bytes(body)
                    return self.check(b"<html" in body.lower() and hdrs.get("content-type", "").startswith("text/html"),
                                      f"GET / answered 200 with the UI page after {self.elapsed():.1f} s")
            except (OSError, http.client.HTTPException) as e:
                last = f"{type(e).__name__}: {e}"
            time.sleep(1)
        return self.check(False, f"GET / did not answer 200 within {self.args.timeout} s ({last})")

    def api_checks(self) -> None:
        try:
            self._api_checks()
        except (OSError, http.client.HTTPException) as e:
            self.check(False, f"web API: {type(e).__name__}: {e}")

    def _api_checks(self) -> None:
        password = "smoke-" + secrets.token_hex(8)
        status, hdrs, body = request(self.http_port, "POST", "/api/setup/admin", {"password": password}, timeout=60)
        (self.out / "setup-admin.json").write_bytes(body)
        cookie = hdrs.get("set-cookie", "").split(";", 1)[0]
        if not self.check(status == 200 and cookie.startswith("sid=") and len(cookie) > 4,
                          f"POST /api/setup/admin set the first password and a session (HTTP {status})"):
            return
        self.cookie = cookie
        status, _, body = request(self.http_port, "GET", "/api/status", cookie=cookie, timeout=30)
        (self.out / "api-status.json").write_bytes(body)
        try:
            st = json.loads(body)
        except ValueError as e:
            self.check(False, f"GET /api/status: not JSON ({e})")
            return
        net = st.get("network", {}) if isinstance(st, dict) else {}
        self.check(status == 200 and st.get("api") == 1 and st.get("public") is False,
                   f"GET /api/status: HTTP {status}, valid JSON, API {st.get('api')}, admin form")
        self.check(net.get("online") is True and net.get("ip"),
                   f"station online over the emulated Ethernet (ip {net.get('ip')}, \"{net.get('ssid')}\")")
        self.name_checks()
        self.connection_checks()
        self.drain_check()
        self.linger_fin_check()
        self.http10_stream_checks()
        self.client_log_checks()
        self.log_level_checks()
        self.wifi_checks()
        self.journal_clear_checks()

    def hold_checks(self) -> None:
        """After detection gave up (a ladder episode): the restart is held for first setup."""
        if not self.cookie:
            return
        hold = None
        deadline = time.monotonic() + 20
        try:
            while time.monotonic() < deadline and self.proc.poll() is None:
                status, _, body = request(self.http_port, "GET", "/api/status", cookie=self.cookie, timeout=30)
                if status == 200:
                    (self.out / "api-status-no-receiver.json").write_bytes(body)
                    hold = (json.loads(body).get("health") or {}).get("reboot_hold")
                    if isinstance(hold, list) and hold:
                        break
                time.sleep(1)
        except (OSError, http.client.HTTPException, ValueError) as e:
            self.check(False, f"GET /api/status in the no-receiver episode: {type(e).__name__}: {e}")
            return
        self.check(isinstance(hold, list) and "setup" in hold,
                   f"GNSS ladder restart held while first setup is not finished (health.reboot_hold {hold})")

    def name_checks(self) -> None:
        status, _, body = request(self.http_port, "GET", "/api/session", cookie=self.cookie, timeout=30)
        (self.out / "api-session.json").write_bytes(body)
        try:
            sess = json.loads(body)
        except ValueError as e:
            self.check(False, f"GET /api/session: not JSON ({e})")
            return
        self.check(status == 200 and sess.get("hostname") == HOSTNAME and sess.get("mdns_host") == HOSTNAME,
                   f"GET /api/session: hostname {sess.get('hostname')!r}, mdns_host {sess.get('mdns_host')!r}, "
                   f"ap_ssid {sess.get('ap_ssid')!r}, setup_step {sess.get('setup_step')!r}")
        status, _, body = request(self.http_port, "GET", "/api/session", timeout=30, host=f"{HOSTNAME}.local")
        self.check(status == 200, f"Host: {HOSTNAME}.local passes the Host check (HTTP {status})")
        status, _, body = request(self.http_port, "GET", "/api/session", timeout=30, host=f"{HOSTNAME}.local.")
        self.check(status == 200, f"Host: {HOSTNAME}.local. (fully qualified) passes the Host check (HTTP {status})")
        status, _, _ = request(self.http_port, "GET", "/api/session", timeout=30, host="evil.local")
        self.check(status != 200, f"Host: evil.local is refused (HTTP {status})")

    def connection_checks(self) -> None:
        """Bodies nobody reads, Connection: close, HTTP/1.0 and pipelining."""
        port = self.http_port
        js = {"Content-Type": "application/json"}
        status, hdrs, body = raw_request(port, "POST", "/api/status", b"{}", js)
        self.check(status == 405 and error_code(body)[0] == "method_not_allowed" and hdrs.get("connection") == "close",
                   f"POST /api/status with {{}}: HTTP {status} {error_code(body)[0]}, Connection: {hdrs.get('connection')}")
        status, hdrs, body = raw_request(port, "POST", "/api/nope", b"{}", js)
        self.check(status == 404 and error_code(body)[0] == "not_found" and hdrs.get("connection") == "close",
                   f"POST /api/nope with a 2-byte body: HTTP {status} {error_code(body)[0]}, "
                   f"Connection: {hdrs.get('connection')}")
        # Any other path: 404 on the station interface; this build counts as the setup AP, where the
        # answer is the captive redirect (the 404 row is host-tested, test_web_core.c).
        status, hdrs, body = raw_request(port, "GET", "/nope", b"xyz")
        self.check(status == 302 and hdrs.get("location", "").startswith("http://192.168.4.1")
                   and hdrs.get("connection") == "close",
                   f"GET /nope with a body: HTTP {status} (the setup AP's captive redirect, not 413), "
                   f"Connection: {hdrs.get('connection')}")
        status, hdrs, body = raw_request(port, "GET", "/api/session", b"{}", js)
        self.check(status == 400 and error_code(body)[0] == "bad_request" and hdrs.get("connection") == "close",
                   f"GET /api/session with a body: HTTP {status} {error_code(body)[0]}, Connection: {hdrs.get('connection')}")
        host = f"Host: 127.0.0.1:{port}\r\n"
        data, eof = raw_exchange(port, f"GET /api/session HTTP/1.1\r\n{host}Connection: close\r\n\r\n".encode())
        self.check(data.startswith(b"HTTP/1.1 200") and b"connection: close" in data.lower() and eof is not None
                   and eof < 2.0,
                   f"Connection: close: answered and closed {secs(eof)} after the answer")
        data, eof = raw_exchange(port, f"GET / HTTP/1.0\r\n{host}\r\n".encode())
        self.check(data.startswith(b"HTTP/1.1 200") and eof is not None and eof < 2.0,
                   f"GET / HTTP/1.0: answered ({data[:15]!r}) and closed "
                   f"{secs(eof)} after the answer")
        # HTTP/1.0 with keep-alive: never kept (the client would wait for the end of the connection
        # unless the token were echoed), so the answer says close and the connection ends.
        data, eof = raw_exchange(port, f"GET /api/session HTTP/1.0\r\n{host}Connection: keep-alive\r\n\r\n".encode())
        head = data.split(b"\r\n\r\n", 1)[0].lower()
        self.check(data.startswith(b"HTTP/1.1 200") and b"connection: close" in head and b"keep-alive" not in head
                   and eof is not None and eof < 2.0,
                   f"GET /api/session HTTP/1.0 with Connection: keep-alive: answered with Connection: close and "
                   f"closed {secs(eof)} after the answer")
        # A chunked body is never read: one answer (411 on a route that takes a body, the route's own answer
        # elsewhere) with Connection: close, and the chunks never come back as a second answer.
        chunks = b"5\r\nhello\r\n0\r\n\r\n"
        for method, path, want, code in (("POST", "/api/nope", 404, "not_found"),
                                         ("POST", "/api/status", 405, "method_not_allowed"),
                                         ("POST", "/api/session", 411, "length_required"),
                                         ("GET", "/nope", 302, None)):
            req = (f"{method} {path} HTTP/1.1\r\n{host}Content-Type: application/json\r\n"
                   f"Transfer-Encoding: chunked\r\n\r\n").encode() + chunks
            data, eof = raw_exchange(port, req)
            head, _, rest = data.partition(b"\r\n\r\n")
            status_ok = data.startswith(f"HTTP/1.1 {want} ".encode())
            code_ok = code is None or f'"code":"{code}"'.encode() in rest.replace(b" ", b"")
            self.check(status_ok and code_ok and b"connection: close" in head.lower()
                       and data.count(b"HTTP/1.1 ") == 1 and eof is not None and eof < 3.0,
                       f"{method} {path} with a chunked body: {data[:12]!r}, one answer "
                       f"({data.count(b'HTTP/1.1 ')}), Connection: close, closed "
                       f"{secs(eof)} after the answer")
        two = (f"GET /api/session HTTP/1.1\r\n{host}\r\n"
               f"GET /api/session HTTP/1.1\r\n{host}Connection: close\r\n\r\n").encode()
        data, eof = raw_exchange(port, two)
        self.check(data.count(b"HTTP/1.1 200") == 2 and eof is not None,
                   f"two pipelined HTTP/1.1 requests on one socket: {data.count(b'HTTP/1.1 200')} answered, "
                   f"closed after the second")
        # Keep-alive: an HTTP/1.1 request without "close" leaves the connection open (its answer read whole by its
        # Content-Length, then the connection still there 1 s later), and the second request is answered on it.
        with Watched("GET /api/session twice (keep-alive)") as w, \
                socket.create_connection(("127.0.0.1", port), timeout=10) as c:
            w.port(c)
            rd = AnswerReader(c)
            c.sendall(f"GET /api/session HTTP/1.1\r\n{host}\r\n".encode())
            first = rd.read()
            open_after = rd.wait_close(1.0) is None and not rd.buf
            c.sendall(f"GET /api/session HTTP/1.1\r\n{host}Connection: close\r\n\r\n".encode())
            second = rd.read()
            w.outcome = f"{answer_text(first)}, then {answer_text(second)}"
        self.check(first.status == 200 and first.complete and open_after and second.status == 200
                   and second.complete,
                   f"HTTP/1.1 keep-alive: the first answer ({first.status or 'none'}, {first.framing or 'incomplete'}, "
                   f"after {first.seconds:.2f} s) leaves the connection open "
                   f"({'yes' if open_after else 'NO'}), and a second request on it is answered "
                   f"({second.status or 'none'}, after {second.seconds:.2f} s)")

    def http10_stream_checks(self) -> None:
        """Streamed answers to HTTP/1.0: no chunked framing, the body as it is, the connection ends after it."""
        if not self.cookie:
            return
        port = self.http_port
        host = f"Host: 127.0.0.1:{port}\r\nCookie: {self.cookie}\r\n"
        # A download (the worker): the log as text.
        data, eof = raw_exchange(port, f"GET /api/logs?format=text HTTP/1.0\r\n{host}\r\n".encode())
        head, _, body = data.partition(b"\r\n\r\n")
        first = body.split(b"\n", 1)[0].strip()
        self.check(data.startswith(b"HTTP/1.1 200") and b"transfer-encoding" not in head.lower()
                   and b"connection: close" in head.lower() and first and not re.fullmatch(rb"[0-9a-fA-F]+", first)
                   and b" " in first and eof is not None and eof < 2.0,
                   f"GET /api/logs?format=text HTTP/1.0: no Transfer-Encoding, first body line {first[:40]!r}, "
                   f"closed {secs(eof)} after the answer")
        # Both ways a JSON answer goes out, on one HTTP/1.1 connection kept alive throughout: an answer that fits
        # the response buffer with its Content-Length, a larger one streamed (chunked). Which way an answer must
        # take follows from its size, measured here, against the buffer the firmware has (read from web.c), so the
        # expectation holds wherever the answers' sizes differ; that both ways were taken is checked on its own.
        cap = response_buffer_size()
        if not self.check(cap is not None, f"the web server's response buffer read from web.c ({cap} bytes)"):
            return
        answers: list[tuple[str, Answer]] = []
        with socket.create_connection(("127.0.0.1", port), timeout=10) as c:
            with Watched("GET /api/session, /api/status, /api/session (one connection)") as w:
                w.port(c)
                rd = AnswerReader(c)
                for path in ("/api/session", "/api/status"):
                    c.sendall(f"GET {path} HTTP/1.1\r\n{host}\r\n".encode())
                    answers.append((path, rd.read()))
                c.sendall(f"GET /api/session HTTP/1.1\r\n{host}Connection: close\r\n\r\n".encode())
                last = rd.read()
                w.outcome = ", ".join(answer_text(a) for a in [a for _, a in answers] + [last])
            closed = rd.wait_close(5.0)
        for path, a in answers:
            want = "chunked" if len(a.body) > cap else "length"
            try:
                parses = isinstance(json.loads(a.body), dict)
            except ValueError:
                parses = False
            self.check(a.status == 200 and a.complete and a.framing == want and parses,
                       f"GET {path} HTTP/1.1 on the kept connection: {len(a.body)} bytes of JSON "
                       f"({'parses' if parses else 'does not parse'}), "
                       + (f"larger than the {cap}-byte response buffer, so streamed (chunked)" if want == "chunked"
                          else f"within the {cap}-byte response buffer, so with its Content-Length")
                       + f": {a.framing or 'no answer'}{'' if a.complete else ' (incomplete)'}, "
                       f"HTTP {a.status or '-'}, after {a.seconds:.2f} s")
        sizes = [len(a.body) for _, a in answers]
        self.check(any(n > cap for n in sizes) and any(0 < n <= cap for n in sizes),
                   f"both ways taken: answers of {' and '.join(map(str, sizes))} bytes against the {cap}-byte buffer "
                   f"(one larger, one within)")
        self.check(last.status == 200 and last.complete and closed is not None and closed < 2.0,
                   f"after the streamed answer the same connection answers again (HTTP {last.status or '-'}, after "
                   f"{last.seconds:.2f} s) and ends {secs(closed)} after that Connection: close answer")
        # To HTTP/1.0 nothing is chunked: the streamed answer comes without Transfer-Encoding and without a
        # Content-Length (its end is the connection's), a buffered one with its Content-Length.
        path = "/api/status"
        data, eof = raw_exchange(port, f"GET {path} HTTP/1.0\r\n{host}\r\n".encode())
        head, _, body = data.partition(b"\r\n\r\n")
        try:
            parsed = json.loads(body)
        except ValueError:
            parsed = None
        streamed = len(body) > cap
        length = re.search(rb"(?im)^content-length:\s*(\d+)\s*$", head)
        length_ok = length is None if streamed else (length is not None and int(length.group(1)) == len(body))
        self.check(data.startswith(b"HTTP/1.1 200") and b"transfer-encoding" not in head.lower() and length_ok
                   and b"connection: close" in head.lower() and b"cache-control: no-store" in head.lower()
                   and isinstance(parsed, dict) and parsed.get("api") == 1 and eof is not None and eof < 2.0,
                   f"GET {path} HTTP/1.0: {len(body)} bytes of JSON "
                   f"({'parses' if isinstance(parsed, dict) else 'does not parse'}) without Transfer-Encoding, "
                   + ("streamed: no Content-Length" if streamed else "buffered: its Content-Length")
                   + f" ({'as expected' if length_ok else 'NOT as expected'}), closed {secs(eof)} after the answer")

    def halfclose_checks(self) -> None:
        """Refused requests whose client ends its sending side right after the body (its FIN queued before the
        answer): the server answers, ends its side and drains what the client still sends (web_linger_close).
        Every one must be answered, with no crash and no restart. The drain reads only what FIONREAD counts and
        leaves that FIN unread; the drain before the fix read until recv() returned 0, took the FIN while our
        FIN was out (LAST_ACK) and so shut lwIP's receive side too, after which lwIP freed the connection on
        the client's ACK without telling the socket (lwIP bug #62141) and the close() after it worked on freed
        memory. Reading the FIN was our defect; test/host/main/test_web_conn.c holds the drain to never reading
        it. Here that image lost one request in a few hundred or crashed, so a few hundred are sent. That rate is
        QEMU's (on a test board it answered thousands without a fault): a guard for the path, not a measure of
        how often a station would fail.

        Both kinds of refusal: the catch-all's (POST /api/nope, 404) and a route's own (GET /api/session with a
        body, 400: api_handle). Each must end its request with ESP_FAIL after the drain, so that the server
        deletes the session without reading; ended with ESP_OK, the server would read away the rest of the
        announced body, find the drained socket empty and take the FIN (web_internal.h, web_linger_close). The
        route's body is larger than what the server reads along with the head, so the drain has bytes to take
        there too. Like the rest of this check, a guard for the path: the race a FIN read opens is rare, so a
        run that passes does not prove the FIN was left unread (the host tests do that for the drain)."""
        self.halfclose_run("POST", "/api/nope", b"{}", 404, "not_found", HALFCLOSE_REQUESTS)
        self.halfclose_run("GET", "/api/session", b"x" * HALFCLOSE_ROUTE_BODY, 400, "bad_request",
                           HALFCLOSE_ROUTE_REQUESTS)

    def halfclose_run(self, method: str, path: str, body: bytes, want: int, code: str, n: int) -> None:
        """`n` requests with `body`, each client half-closing after it: every one answered `want` (error `code`)
        with Connection: close, and no crash or restart on the console meanwhile. An answer that took SLOW_S or
        longer passes but is named with its client port (and watched in slow-requests.log): on a Linux host one
        can meet a port the station still holds in TIME_WAIT, which its packets in the pcap show (a bare ACK to
        the SYN, libslirp's reset that ends the TIME_WAIT, the SYN-ACK to its resend 5.5 to 6 s later), see
        port_reuse_reset_check."""
        port = self.http_port
        req = (f"{method} {path} HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\n"
               f"Content-Length: {len(body)}\r\n\r\n").encode() + body
        start = len(self.log_text())
        answered = 0
        first_bad = ""
        late: list[str] = []
        t0 = time.monotonic()
        for i in range(n):
            local = None
            try:
                with Watched(f"{method} {path} (half-closed, {i + 1} of {n})") as w, \
                        socket.create_connection(("127.0.0.1", port), timeout=10) as c:
                    w.port(c)
                    local = c.getsockname()[1]
                    c.sendall(req)
                    c.shutdown(socket.SHUT_WR)
                    a = AnswerReader(c).read()
                    w.outcome = answer_text(a)
                ok = (a.status == want and a.complete and a.headers.get("connection") == "close"
                      and error_code(a.body)[0] == code)
                what = ((f"{a.status} {error_code(a.body)[0]}" if a.status else "no answer")
                        + f"{'' if a.complete else ' (incomplete)'} after {a.seconds:.1f} s")
                if ok and a.seconds >= SLOW_S:
                    late.append(f"request {i + 1} after {a.seconds:.1f} s (client port {local})")
            except OSError as e:
                ok = False
                what = f"{type(e).__name__}: {e}"
            text = self.log_text()[start:]
            fatal = next((line.strip()[:160] for line in text.splitlines() if any(m in line for m in FATAL)), "")
            if ok and not fatal and BOOT_MARK not in text:
                answered += 1
                continue
            first_bad = (f"request {i + 1}" + (f" (client port {local})" if local else "") + f": {what}"
                         + (f"; console: {fatal}" if fatal else "")
                         + ("; the station restarted" if BOOT_MARK in text else ""))
            break
        self.check(answered == n,
                   f"{n} refused requests ({method} {path} with a {len(body)}-byte body) whose client half-closed "
                   f"after it: {answered} answered {want} {code} with Connection: close, no crash "
                   f"({time.monotonic() - t0:.0f} s" + (f"; {first_bad}" if first_bad else "")
                   + (f"; answered late: {', '.join(late)}" if late else "") + ")")

    def drain_check(self) -> None:
        """A refused body several TCP windows long, sent whole before the client reads: the server answers, ends
        its side and reads what the client still sends (web_linger_close), so the client's upload completes and
        the client reads the answer. Seen in the station's own packets (run.sh --pcap): it acknowledges every
        byte of the request and the client's FIN. A server that read nothing after its answer would leave its
        receive window shut (lwIP opens it only as the socket is read): the client's upload stalls with the
        answer behind it, and the acknowledgements stop a window or so into the body. libslirp has also sent a
        reset with exactly the station's RCV.NXT right after the client's FIN (every run on the Mac before the
        socket let go at TIME_WAIT), while the lingering close still read. The client's FIN puts the connection
        in TIME_WAIT, and the socket lets go of it right there, inside lwIP's handling of that FIN
        (components/network/net_tw.c), so the firmware's TIME_WAIT hook may end that TIME_WAIT on the reset at
        once. A version of the hook from before the letting go ended it with the socket still on the pcb, and
        the station aborted (the crash checks at the end catch that); the line says whether such a reset
        came."""
        port = self.http_port
        body = b"x" * (16 * 1024)
        head = (f"POST /api/nope HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\n"
                f"Content-Length: {len(body)}\r\n\r\n").encode()
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=10) as c:
                local = c.getsockname()[1]
                with Watched(f"POST /api/nope ({len(body)}-byte body)") as w:
                    w.port(c)
                    c.sendall(head + body)
                    rd = AnswerReader(c)
                    a = rd.read()
                    w.outcome = answer_text(a)
                closed = rd.wait_close(5.0)
        except OSError as e:
            self.check(False, f"POST /api/nope with a {len(body)}-byte body: {type(e).__name__}: {e}")
            return
        time.sleep(2.0)   # the station's last segments of it come after the client's close
        both = tcp_segments(self.out / "qemu-net.pcap", local)
        segs = [s for from_guest, s in both if from_guest]
        acked = acked_by_guest(segs)
        want = len(head) + len(body) + 1
        rst = any(x.flags & TCP_RST for x in segs)
        synack = next((x for x in segs if x.flags & TCP_SYN), None)
        late_rst = synack is not None and any(x.seq == (synack.ack + want) % (1 << 32) for x in client_resets(both))
        self.check(a.status == 404 and a.complete and closed is not None and acked == want and not rst,
                   f"POST /api/nope with a {len(body)}-byte body sent whole: HTTP {a.status or '-'}"
                   f"{'' if a.complete else ' (incomplete)'}, the server's FIN {secs(closed)} after the answer; "
                   f"the station acknowledged {acked if acked is not None else 'nothing (no SYN-ACK in the pcap)'} "
                   f"of {want} (the request and the client's FIN){', and reset the connection' if rst else ''}"
                   + ("; the client's reset with RCV.NXT after its FIN reached it too" if late_rst else ""))

    def linger_fin_check(self) -> None:
        """LINGER_FIN_ROUNDS refused bodies (POST /api/nope, LINGER_FIN_BODY bytes sent whole), each client
        reading the answer and the station's FIN and then closing, and the next one connecting at once. The
        client's FIN reaches the station while its lingering close still reads (web_linger_close) and puts the
        connection in TIME_WAIT there, with the next connection's SYN right behind it: the socket lets go of
        that connection inside lwIP's handling of the FIN (components/network/net_tw.c), so lwIP may free the
        TIME_WAIT at any time after it without the socket ever touching it again. Every round must be answered
        404 with Connection: close and end with the station's FIN, with no crash or restart on the console. A
        round that took SLOW_S or more passes but is named: a drain that missed the end of the connection
        would keep the server task for its full 2 s (LINGER_DRAIN_MS) before the next answer."""
        port = self.http_port
        body = b"x" * LINGER_FIN_BODY
        req = (f"POST /api/nope HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nContent-Type: application/json\r\n"
               f"Content-Length: {len(body)}\r\n\r\n").encode() + body
        start = len(self.log_text())
        answered = 0
        first_bad = ""
        late: list[str] = []
        t0 = time.monotonic()
        for i in range(LINGER_FIN_ROUNDS):
            local = None
            try:
                with Watched(f"POST /api/nope ({len(body)}-byte body, closed after the station's FIN, "
                             f"{i + 1} of {LINGER_FIN_ROUNDS})") as w, \
                        socket.create_connection(("127.0.0.1", port), timeout=10) as c:
                    w.port(c)
                    local = c.getsockname()[1]
                    t1 = time.monotonic()
                    c.sendall(req)
                    rd = AnswerReader(c)
                    a = rd.read()
                    w.outcome = answer_text(a)
                    closed = rd.wait_close(5.0) if a.complete else None
                    took = time.monotonic() - t1
                ok = (a.status == 404 and a.complete and a.headers.get("connection") == "close"
                      and closed is not None)
                what = ((f"{a.status} {error_code(a.body)[0]}" if a.status else "no answer")
                        + f"{'' if a.complete else ' (incomplete)'}"
                        + ("" if closed is not None else ", no FIN from the station within 5 s"))
                if ok and took >= SLOW_S:
                    late.append(f"round {i + 1} after {took:.1f} s (client port {local})")
            except OSError as e:
                ok = False
                what = f"{type(e).__name__}: {e}"
            text = self.log_text()[start:]
            fatal = next((line.strip()[:160] for line in text.splitlines() if any(m in line for m in FATAL)), "")
            if ok and not fatal and BOOT_MARK not in text:
                answered += 1
                continue
            first_bad = (f"round {i + 1}" + (f" (client port {local})" if local else "") + f": {what}"
                         + (f"; console: {fatal}" if fatal else "")
                         + ("; the station restarted" if BOOT_MARK in text else ""))
            break
        self.check(answered == LINGER_FIN_ROUNDS,
                   f"{LINGER_FIN_ROUNDS} refused {len(body)}-byte bodies, each client closing after the answer and "
                   f"the station's FIN and the next connecting at once: {answered} answered 404 with Connection: "
                   f"close and the station's FIN, no crash ({time.monotonic() - t0:.0f} s"
                   + (f"; {first_bad}" if first_bad else "")
                   + (f"; slow: {', '.join(late)}" if late else "") + ")")

    def reuse_pair(self, after_s: float) -> ReusePair:
        """Two connections from one local port, each a GET /api/session with Connection: close, so the station
        ends both first. The second connects `after_s` after the first ended on the client's side (the
        station's TIME_WAIT starts when the client's FIN reaches it, at once here). The first binds port 0
        and the second binds the port the first got, on purpose. Raises OSError."""
        port = self.http_port
        req = f"GET /api/session HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\nConnection: close\r\n\r\n".encode()

        def exchange(local: int, timeout: float) -> tuple[int, Answer, float | None, float, Watched]:
            """(local port, answer, when the station's FIN came, seconds from the connect to the answer's end,
            the request as watched)"""
            with socket.socket() as c:
                c.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                c.bind(("127.0.0.1", local))
                c.settimeout(10)
                with Watched("GET /api/session (Connection: close, from a bound port)") as w:
                    t0 = time.monotonic()
                    c.connect(("127.0.0.1", port))
                    w.port(c)
                    bound = c.getsockname()[1]
                    c.sendall(req)
                    rd = AnswerReader(c)
                    a = rd.read(timeout)
                    took = time.monotonic() - t0
                    w.outcome = answer_text(a)
                return bound, a, rd.wait_close(5.0) if a.complete else None, took, w
        local, first, closed, _, _ = exchange(0, ANSWER_TIMEOUT_S)
        settle = min(1.0, after_s)
        time.sleep(settle)   # the station's last segments of it
        both = tcp_segments(self.out / "qemu-net.pcap", local)
        seen = len(both)     # the first connection's packets; the second's come after them
        segs = [s for from_guest, s in both if from_guest]
        first_close = guest_closed_first(segs, len(req))
        before = synack_isns(segs)   # the first connection's (every SYN-ACK of it is in by now)
        time.sleep(max(0.0, after_s - settle))
        _, again, _, took, watched = exchange(local, ANSWER_TIMEOUT_S)
        time.sleep(1.0)
        packets = tcp_packets(self.out / "qemu-net.pcap", local)
        both = [(x.from_guest, x.seg) for x in packets]
        segs = [s for from_guest, s in both if from_guest]
        isns = synack_isns(segs)
        return ReusePair(first=first, closed=closed, first_close=first_close, before=before, again=again,
                         took=took, later=[i for i in isns if i not in before],
                         resent=sum(1 for x in segs if x.flags & TCP_SYN) - len(isns), second=both[seen:],
                         packets=packets[seen:], watched=watched)

    @staticmethod
    def reuse_text(r: ReusePair, after_s: float) -> str:
        """The start of a port reuse check's line: the first connection, and the second's answer."""
        return (f"a client back on its local port {after_s:.0f} s after the station ended its connection first "
                f"(HTTP {r.first.status or '-'}, the station's FIN "
                f"{'first' if r.first_close else 'NOT first' if r.first_close is False else 'not in the pcap'}): "
                f"HTTP {r.again.status or 'no answer'}{'' if r.again.complete else ' (incomplete)'} "
                f"{r.took:.2f} s after its connect")

    @staticmethod
    def reuse_synacks(r: ReusePair) -> str:
        return (f"SYN-ACKs from the station: {len(r.before)} for the first connection, {len(r.later)} for the "
                f"second (by initial sequence number" + (f"; {r.resent} sent again" if r.resent else "") + ")")

    def port_reuse_check(self) -> None:
        """A client that comes back from the same local port after the station's TIME_WAIT for that connection
        is over. The station holds a connection it ended first in TIME_WAIT for 2 x MSL and takes no new
        connection on it until then (tcp_in.c, tcp_timewait_input): a SYN gets a bare ACK (a reset in the rare
        case that it falls inside the old window). Not reopening from TIME_WAIT is upstream lwIP's choice, and
        optional (RFC 9293 section 3.6.1). Linux (since 2022) takes the local ports of its connections to one
        server from a starting point drawn anew every 10 s, so a Linux client now and then comes back on a port
        the station still holds; QEMU's network passes the host client's port through, so the runs on Linux
        meet that too, at random. The fault was the firmware's: it ran with lwIP's 120 s, and a Linux client
        then waited up to two minutes, or its connect() failed. This check does not wait for chance: it binds
        the port on purpose, to hold the station to the TIME_WAIT it is configured for (a client's reset can
        end one sooner: port_reuse_reset_check). The firmware sets MSL to 5 s (sdkconfig.defaults), so
        REUSE_AFTER_S later the station must take the second connection's first SYN: its packets (pcap) show
        no bare ACK or reset before its SYN-ACK to that SYN (refused_syns) and one SYN-ACK for each of the two
        connections (told apart by their initial sequence numbers, so a SYN-ACK sent again is not a third), and
        the answer ends less than SLOW_S after the connect. A SYN turned away costs more than that here: libslirp
        sends the guest SYN again only 5.5 to 6 s after the first (6 s, counted in 500 ms ticks). A TIME_WAIT longer
        than about REUSE_AFTER_S (MSL over about 7.5 s) fails the check. So would a frame of the exchange that
        QEMU's MAC drops (an answer seconds late with no SYN turned away; slow-requests.log has the console)."""
        try:
            r = self.reuse_pair(REUSE_AFTER_S)
        except OSError as e:
            self.check(False, f"a client back on its port after the station closed first: {type(e).__name__}: {e}")
            return
        refused = refused_syns(r.second)
        resets = sum(1 for x in refused if x.flags & TCP_RST)
        turned_away = f"{len(refused) - resets} bare ACK(s), {resets} reset(s)" if refused else "none"
        held = refused or (r.before and not r.later)
        self.check(r.first.status == 200 and r.closed is not None and r.first_close is True
                   and r.again.status == 200 and r.again.complete and len(r.before) == 1 and len(r.later) == 1
                   and not refused and r.took < SLOW_S,
                   self.reuse_text(r, REUSE_AFTER_S) + f" (must be under {SLOW_S:.0f} s); the station's answers to "
                   f"its SYN before a SYN-ACK: {turned_away}; " + self.reuse_synacks(r)
                   + (" (the station still held the first connection in TIME_WAIT)" if held else ""))

    def port_reuse_reset_check(self) -> None:
        """A client that comes back from the same local port REUSE_EARLY_S after the station ended its
        connection first, inside the station's TIME_WAIT. lwIP answers its SYN with a bare ACK that carries the
        old connection's numbers, and libslirp answers that with a reset whose sequence number is the station's
        RCV.NXT (RFC 9293 section 3.10.7.3), as a Linux client does. lwIP alone ignores that reset (tcp_in.c,
        tcp_timewait_input: RFC 1337's fix, an Informational RFC), so every SYN until the TIME_WAIT is over is
        turned away the same way; RFC 9293 section 3.10.7.4 ends TIME-WAIT on it, and so does Linux by default.
        The firmware's lwIP hook (components/network/net_tw.c) ends the TIME_WAIT on that reset when its
        sequence number is exactly RCV.NXT (RFC 5961's check) and no socket holds the connection any more (the
        first connection here is closed whole), before the station reads its next segment. So here: one
        bare ACK and nothing else before the SYN-ACK (refused_syns), libslirp's reset with exactly the bare
        ACK's acknowledgment number, the SYN-ACK to the client's second SYN (libslirp's first resend:
        syns_until_taken), one SYN-ACK for each of the two connections, and the request's time taken apart in
        the pcap (reconnect_time): libslirp's resend RESEND_MIN_S to RESEND_MAX_S after the first SYN, which is
        its own retransmission timer (6 s in 500 ms ticks; the reset it sent does not cut it short), and the
        station's own part under SLOW_S, as for any request. So the answer comes about 6 s after the connect, all
        but a few milliseconds of it libslirp's wait: the watch on slow requests counts that request as
        explained, with the split. Without the hook the second SYN still falls inside the 10 s TIME_WAIT and
        gets a bare ACK too, and the third, about 18 s after the connect, is taken. A Linux client (5.10 and
        later) sends its next SYN a few milliseconds after its reset, not 5.5 s. The station could take the
        first SYN itself (RFC 9293 section 3.6.1 lets a TIME-WAIT reopen for a SYN above RCV.NXT, as Linux
        does); upstream lwIP does not, and a segment that matches a TIME_WAIT pcb never reaches the listener
        (tcp_in.c: the hook or tcp_timewait_input, then the segment is freed)."""
        try:
            r = self.reuse_pair(REUSE_EARLY_S)
        except OSError as e:
            self.check(False, f"a client back on its port inside the station's TIME_WAIT: {type(e).__name__}: {e}")
            return
        refused = refused_syns(r.second)
        bare = [x for x in refused if not x.flags & TCP_RST]
        resets = client_resets(r.second)
        rcv_nxt = bare[0].ack if bare else None
        exact = rcv_nxt is not None and any(x.seq == rcv_nxt for x in resets)
        syns = syns_until_taken(r.second)
        if not resets:
            reset_text = "no reset from the client"
        elif rcv_nxt is None:
            reset_text = f"{len(resets)} reset(s) from the client, no bare ACK to compare"
        else:
            seqs = "" if exact else "; its sequence numbers " + ", ".join(str(x.seq) for x in resets[:3])
            reset_text = f"the client's reset {'with' if exact else 'NOT with'} the station's RCV.NXT ({rcv_nxt}{seqs})"
        t = reconnect_time(r.packets)
        timed = (t is not None and RESEND_MIN_S <= t.resend <= RESEND_MAX_S and t.station < SLOW_S
                 and r.took < t.resend + SLOW_S)
        if t is None:
            time_text = "its time not in the pcap (no turned-away SYN, SYN-ACK, request or FIN of the station)"
        else:
            time_text = (f"{t.resend:.2f} s of it libslirp's resend of the SYN (must be {RESEND_MIN_S} to "
                         f"{RESEND_MAX_S} s: its retransmission timer), the station's own {t.station:.3f} s (must be "
                         f"under {SLOW_S:.0f} s: the bare ACK {t.turned * 1000:.1f} ms after the first SYN, the "
                         f"SYN-ACK {t.synack * 1000:.1f} ms after the resend, the request to its FIN "
                         f"{t.answer * 1000:.0f} ms)")
        if timed and r.watched is not None:
            # Only a split that holds: a station that took its time stays a slow request nothing explains.
            r.watched.explain(f"{r.took:.2f} s, {t.resend:.2f} s of it libslirp's SYN resend after the station "
                              f"turned the first SYN away in TIME_WAIT (port_reuse_reset_check); the station's own "
                              f"time {t.station:.3f} s")
        self.check(r.first.status == 200 and r.closed is not None and r.first_close is True
                   and r.again.status == 200 and r.again.complete and len(r.before) == 1 and len(r.later) == 1
                   and len(refused) == 1 and len(bare) == 1 and exact and syns == 2 and timed,
                   self.reuse_text(r, REUSE_EARLY_S) + f": {time_text}; the "
                   f"station's answers to its SYN before a SYN-ACK: {len(bare)} bare ACK(s), "
                   f"{len(refused) - len(bare)} reset(s) (must be 1 bare ACK: it held the first connection in "
                   f"TIME_WAIT); {reset_text}; the SYN-ACK answered the client's SYN number "
                   f"{syns if syns is not None else '- (none)'} (must be 2: the reset ended that TIME_WAIT); "
                   + self.reuse_synacks(r))

    def signal_test_checks(self) -> None:
        """No receiver attached: a signal-group comparison is refused with no_receiver, and none runs."""
        if not self.cookie:
            return
        state = None
        deadline = time.monotonic() + 30
        try:
            while time.monotonic() < deadline and self.proc.poll() is None:
                status, _, body = request(self.http_port, "GET", "/api/gnss", cookie=self.cookie, timeout=30)
                if status == 200:
                    state = json.loads(body).get("state")
                    if state == "no_receiver":
                        break
                time.sleep(1)
            if not self.check(state == "no_receiver", f"GET /api/gnss: state {state!r} (no receiver attached)"):
                return
            # One rule for both: no receiver found, so neither says it answers.
            gnss_resp = json.loads(body).get("responding")
            status, _, body = request(self.http_port, "GET", "/api/status", cookie=self.cookie, timeout=30)
            st_resp = (json.loads(body).get("receiver") or {}).get("responding") if status == 200 else None
            self.check(gnss_resp is False and st_resp is False,
                       f"no receiver: responding {gnss_resp!r} in GET /api/gnss, {st_resp!r} in GET /api/status")
            body_in = {"groups": [{"master": 3, "slave": 6}, {"master": 7, "slave": 0}], "phase_s": 1800}
            status, _, body = request(self.http_port, "POST", "/api/gnss/signal-test", body_in, cookie=self.cookie,
                                      timeout=30)
            (self.out / "signal-test-refused.json").write_bytes(body)
            try:
                err = json.loads(body).get("error") or {}
            except ValueError:
                err = {}
            self.check(status == 409 and err.get("code") == "no_receiver"
                       and err.get("message") == "No receiver is connected",
                       f"POST /api/gnss/signal-test without a receiver: HTTP {status} {err.get('code')} "
                       f"({err.get('message')!r})")
            status, _, body = request(self.http_port, "GET", "/api/gnss/signal-test", cookie=self.cookie, timeout=30)
            st = json.loads(body).get("state") if status == 200 else None
            self.check(status == 200 and st == "idle", f"GET /api/gnss/signal-test after it: state {st!r}")
        except (OSError, http.client.HTTPException, ValueError) as e:
            self.check(False, f"signal-group comparison without a receiver: {type(e).__name__}: {e}")

    def wifi_checks(self) -> None:
        """Wi-Fi credentials in this build are stored only (the emulated Ethernet stays the link): the 202 and the
        next GET agree at once (no trial, nothing pending)."""
        status, _, body = request(self.http_port, "PUT", "/api/wifi", {"ssid": "SmokeNet", "password": "fake-pass-123"},
                                  cookie=self.cookie, timeout=30)
        try:
            put = json.loads(body)
        except ValueError:
            put = {}
        self.check(status == 202 and put.get("applied") is False and put.get("trial") is False,
                   f"PUT /api/wifi: HTTP {status}, applied {put.get('applied')}, trial {put.get('trial')} (store only)")
        status, _, body = request(self.http_port, "GET", "/api/wifi", cookie=self.cookie, timeout=30)
        try:
            w = json.loads(body)
        except ValueError:
            w = {}
        self.check(status == 200 and w.get("cred_trial") is False and w.get("cred_pending") is False
                   and w.get("pending_ssid") is None,
                   f"GET /api/wifi right after: cred_trial {w.get('cred_trial')}, cred_pending {w.get('cred_pending')}, "
                   f"pending_ssid {w.get('pending_ssid')!r}")
        # The access point and the transmit power: in the answer, null without an access point (emulated Ethernet).
        self.check(status == 200 and "bssid" in w and "tx_power_dbm" in w and w["bssid"] is None
                   and w["tx_power_dbm"] is None,
                   f"GET /api/wifi: bssid {w.get('bssid', 'missing')!r}, "
                   f"tx_power_dbm {w.get('tx_power_dbm', 'missing')!r} (null in this build)")
        # Roaming: off in this build (no radio), so no moves, and the access point's 802.11v support is never read:
        # left out (null would say "not read yet").
        self.check(status == 200 and w.get("roams") == 0 and "ap_steering" not in w,
                   f"GET /api/wifi: roams {w.get('roams', 'missing')!r}, "
                   f"ap_steering {w.get('ap_steering', 'missing')!r} (0 and missing in this build)")

    def journal_clear_checks(self) -> None:
        """DELETE /api/journal: the records are gone at once and the numbering goes on (the station may journal
        something of its own right after the clear, such as a ladder step: only numbers after the old newest)."""
        def get(path: str) -> tuple[int, dict]:
            status, _, body = request(self.http_port, "GET", path, cookie=self.cookie, timeout=30)
            try:
                return status, json.loads(body)
            except ValueError:
                return status, {}

        _, hl = get("/api/health")
        before = (hl.get("journal") or {}).get("newest_seq") or 0
        status, _, body = request(self.http_port, "DELETE", "/api/journal", cookie=self.cookie, timeout=60)
        try:
            ok = json.loads(body).get("ok") is True
        except (ValueError, AttributeError):
            ok = False
        self.check(status == 200 and ok, f"DELETE /api/journal: HTTP {status} {body[:40]!r}")
        status, j = get("/api/journal?cursor=0")
        seqs = [e.get("seq", 0) for e in j.get("entries") or []]
        self.check(status == 200 and before > 0 and all(s > before for s in seqs),
                   f"GET /api/journal after the clear: {len(seqs)} record(s), numbers {seqs[:3]} after {before}")
        status, c = get("/api/connections")
        self.check(status == 200 and c.get("entries") == [] and c.get("complete") is True,
                   f"GET /api/connections after the clear: {len(c.get('entries') or [])} record(s), "
                   f"complete {c.get('complete')}")

    def console_lines(self, start: int, needle: str) -> list[str]:
        return [line for line in self.log_text()[start:].splitlines() if needle in line]

    def wait_console(self, start: int, needle: str, seconds: float = 5.0) -> list[str]:
        deadline = time.monotonic() + seconds
        while True:
            found = self.console_lines(start, needle)
            if found or time.monotonic() >= deadline:
                return found
            time.sleep(0.25)

    def ws_handshake(self, cookie: str | None = None) -> bytes:
        key = "dGhlIHNhbXBsZSBub25jZQ=="
        req = (f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{self.http_port}\r\nUpgrade: websocket\r\n"
               f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
               + (f"Cookie: {cookie}\r\n" if cookie else "") + "\r\n")
        with Watched("GET /ws (handshake)") as w, \
                socket.create_connection(("127.0.0.1", self.http_port), timeout=10) as c:
            w.port(c)
            c.sendall(req.encode())
            a = AnswerReader(c).read()
            w.outcome = answer_text(a)
        return a.head + (b"\r\n\r\n" if a.head else b"") + a.body

    def client_log_checks(self) -> None:
        """A wrong admin password and a refused WebSocket handshake name the client in the log."""
        start = len(self.log_text())
        status, _, body = request(self.http_port, "POST", "/api/session", {"password": "not-the-password"}, timeout=60)
        lines = self.wait_console(start, "wrong admin password from")
        self.check(status == 403 and len(lines) == 1 and f"from {QEMU_PEER}" in lines[0],
                   f"a wrong admin password (HTTP {status}): {len(lines)} console line(s): "
                   f"{lines[0].strip()[:120] if lines else '-'}")
        start = len(self.log_text())
        data = self.ws_handshake()
        lines = self.wait_console(start, f"WebSocket refused for {QEMU_PEER}")
        self.check(data.startswith(b"HTTP/1.1 403") and len(lines) == 1,
                   f"a WebSocket handshake without a session (public status off): {data[:12]!r}, "
                   f"{len(lines)} console line(s): {lines[0].strip()[:120] if lines else '-'}")

    def log_level_checks(self) -> None:
        start = len(self.log_text())
        status, _, body = request(self.http_port, "PUT", "/api/diag/log-level", {"tag": "*", "level": "info"},
                                  cookie=self.cookie, timeout=30)
        self.check(status == 200, f"PUT /api/diag/log-level * info: HTTP {status}")
        data = self.ws_handshake()
        time.sleep(1.0)
        noisy = [line for line in self.log_text()[start:].splitlines() if re.search(r"^W \([^)]*\) httpd_uri:", line)]
        self.check(data.startswith(b"HTTP/1.1 403") and not noisy,
                   f"after \"*\" at info, a refused handshake prints no W httpd_uri line "
                   f"({noisy[0].strip()[:100] if noisy else 'none'})")
        status, _, body = request(self.http_port, "PUT", "/api/diag/log-level", {"tag": "station", "level": "debug"},
                                  cookie=self.cookie, timeout=30)
        code, field = error_code(body)
        self.check(status == 400 and code == "invalid_field" and field == "level",
                   f"log level debug (above this build's maximum): HTTP {status} {code} {field}")
        status, _, body = request(self.http_port, "GET", "/api/logs?cursor=0", cookie=self.cookie, timeout=30)
        try:
            level_max = json.loads(body).get("level_max")
        except ValueError:
            level_max = None
        self.check(status == 200 and level_max == "info", f"GET /api/logs: level_max {level_max!r}")
        for path in ("/api/logs?cursor=abc", "/api/journal?cursor=abc"):
            status, _, body = request(self.http_port, "GET", path, cookie=self.cookie, timeout=30)
            code, field = error_code(body)
            self.check(status == 400 and code == "invalid_field" and field == "cursor",
                       f"GET {path}: HTTP {status} {code} {field}")

    def uart1_checks(self) -> None:
        if self.tap is None:
            return
        deadline = self.t0 + self.args.timeout
        result = None
        while time.monotonic() < deadline and self.proc.poll() is None:
            m = DETECT_RESULT.search(self.log_text())
            if m:
                result = m
                break
            try:
                self.tap.send(LINK_TEST)
            except OSError:
                break
            time.sleep(1)
        rx = self.tap.received()
        self.check(b"VERSIONA" in rx,
                   f"firmware -> host: {len(rx)} bytes on UART1, {rx.count(b'VERSIONA')} receiver detection queries")
        if result is None:
            self.check(False, "host -> firmware: no detection result in the console log")
        else:
            self.check(result.group(1) == "bytes but nothing valid",
                       f"host -> firmware: detection ended \"{result.group(0).split(': ', 1)[1]}\"")

    def still_serving(self) -> None:
        """At the end of the window: the web server still answers (a hang after the first checks shows here)."""
        try:
            status, _, _ = request(self.http_port, "GET", "/", timeout=30)
            self.check(status == 200, f"GET / still answers 200 after {self.elapsed():.0f} s (HTTP {status})")
            if self.cookie:
                status, _, body = request(self.http_port, "GET", "/api/status", cookie=self.cookie, timeout=30)
                (self.out / "api-status-end.json").write_bytes(body)
                ok = status == 200 and json.loads(body).get("api") == 1
                self.check(ok, f"GET /api/status still answers after {self.elapsed():.0f} s (HTTP {status})")
                self.console_checks()
                self.reboot_checks()
        except (OSError, http.client.HTTPException, ValueError) as e:
            self.check(False, f"web server at the end of the window: {type(e).__name__}: {e}")

    def console_checks(self) -> None:
        """The console writer task runs, and no console line was lost."""
        status, _, body = request(self.http_port, "GET", "/api/diag/tasks", cookie=self.cookie, timeout=30)
        (self.out / "api-tasks.json").write_bytes(body)
        tasks = json.loads(body).get("tasks", []) if status == 200 else []
        names = [t.get("name") for t in tasks]
        self.check("console" in names, f"the console writer task runs (HTTP {status}, {len(names)} tasks)")
        # Every planned restart runs on a task pinned to the PRO CPU (see boot_report: on the APP CPU this QEMU
        # restarts the PRO CPU twice); the lifecycle task runs every one whose caller is not pinned there.
        core = next((t.get("core", "absent") for t in tasks if t.get("name") == "lifecycle"), "no task")
        self.check(core == 0, f"the lifecycle task (planned restarts) is pinned to core 0 (core {core})")
        status, _, body = request(self.http_port, "GET", "/api/health", cookie=self.cookie, timeout=30)
        (self.out / "api-health-end.json").write_bytes(body)
        log = (json.loads(body).get("log") or {}) if status == 200 else {}
        self.check(status == 200 and log.get("console_dropped") == 0,
                   f"/api/health log.console_dropped {log.get('console_dropped')} of {log.get('lines')} lines")

    def reboot_checks(self) -> None:
        """A planned restart says goodbye on mDNS while the network is still up, within the hook budget."""
        start = len(self.log_text())
        status, _, body = request(self.http_port, "POST", "/api/system/reboot", {}, cookie=self.cookie, timeout=30)
        if not self.check(status == 202, f"POST /api/system/reboot: HTTP {status}"):
            return
        deadline = time.monotonic() + 30
        text = ""
        while time.monotonic() < deadline and self.proc.poll() is None:
            text = self.log_text()[start:]
            if BOOT_MARK in text:
                break
            time.sleep(0.5)
        before = text.split(BOOT_MARK, 1)[0]
        self.check(BOOT_MARK in text and GOODBYE in before,
                   "the mDNS goodbye is on the console before the restart" if GOODBYE in before
                   else "no mDNS goodbye before the restart")
        warned = [w for w in BUDGET_WARNINGS if w in before]
        self.check(not warned, f"no shutdown-budget warning ({warned[0] if warned else 'none'})")
        # Back up again before the window ends (the boot checks count this restart as the firmware's own).
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline and self.proc.poll() is None:
            try:
                if request(self.http_port, "GET", "/", timeout=5, watch=False)[0] == 200:
                    break
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(1)

    def observe(self) -> None:
        while self.elapsed() < self.args.observe and self.proc.poll() is None:
            time.sleep(1)
        alive = self.proc.poll() is None
        if alive:
            self.still_serving()
        self.stop()
        self.check(alive, f"QEMU still running after {self.args.observe} s")
        text = self.log_text()
        for marker in FATAL:
            for line in text.splitlines():
                if marker in line:
                    self.check(False, f"console: {line.strip()[:160]}")
                    break
        r = boot_report(text)
        n = r.boots
        self.check(n >= 1, f"{n} boot(s) in {self.args.observe} s")
        self.check(not r.unplanned, boot_text(r))
        self.check(not r.off_pro, runner_text(r, len(OWN_REBOOT.findall(text))))
        self.check(n <= self.args.max_boots, f"at most {self.args.max_boots} boots (no reboot loop)")
        self.check(LADDER_REBOOT not in text, "no GNSS ladder restart (\"rebooting (reason 6)\") during the run")
        notes = [line.strip()[:160] for line in text.splitlines() if CONSOLE_NOTE in line]
        self.check(not notes, f"no dropped console lines ({notes[0] if notes else 'no [diag: note'})")
        self.check(f"network: mDNS: answering as {HOSTNAME}.local (web page as service {HOSTNAME})" in text,
                   "the boot log has the mDNS line")
        self.check("mdns_mem: mDNS task will be created from SPIRAM" in text
                   and "console task stack in internal RAM" not in text,
                   "the mDNS and console task stacks are in PSRAM (boot log)")
        m = IP6_LINE.search(text)
        self.check(m is not None, f"the boot log has the station's IPv6 link-local address ({m.group(1) if m else '-'})")
        self.ip6_only_link_local(text)

    def ip6_only_link_local(self, text: str) -> None:
        """No IPv6 address but the link-local one: no "is not link-local" warning in the 30 s after the station's
        link-local line (first boot; QEMU's network advertises a prefix, so stateless autoconfiguration would
        show here)."""
        boot = text.split(BOOT_MARK)[1] if BOOT_MARK in text else text
        lines = boot.splitlines()
        at = next((i for i, line in enumerate(lines) if IP6_LINE.search(line)), None)
        if at is None:
            self.check(False, "no IPv6 link-local line to check the 30 s after it")
            return
        stamp = LOG_STAMP.match(lines[at].strip())
        t0 = int(stamp.group(1)) if stamp else None
        last = None
        warned = []
        for line in lines[at:]:
            s = LOG_STAMP.match(line.strip())
            if s:
                last = int(s.group(1))
            if IP6_OTHER in line:
                warned.append(line.strip()[:160])
        covered = t0 is not None and last is not None and last - t0 >= IP6_QUIET_S * 1000
        self.check(covered and not warned,
                   f"no IPv6 address but the link-local one in the {IP6_QUIET_S} s after it "
                   f"(log covers {((last - t0) / 1000) if covered or (t0 is not None and last is not None) else 0:.0f} s; "
                   f"{warned[0] if warned else 'no is-not-link-local warning'})")

    def run(self) -> int:
        try:
            self.start()
            if self.proc.poll() is None and self.wait_http():
                self.api_checks()
                self.uart1_checks()
                self.hold_checks()
                self.signal_test_checks()
                self.halfclose_checks()
                self.port_reuse_check()
                self.port_reuse_reset_check()
            self.observe()
        finally:
            self.stop()
        if slow_requests():
            told = slow_explained()
            print(f"     {slow_requests()} request(s) took {SLOW_S:.0f} s or more, {slow_requests() - len(told)} "
                  f"of them not explained: watched in {self.out / 'slow-requests.log'} (packets in qemu-net.pcap)",
                  flush=True)
            for why in told:
                print(f"     explained by its packets: {why}", flush=True)
        failed = [t for ok, t in self.results if not ok]
        summary = "\n".join(("PASS " if ok else "FAIL ") + t for ok, t in self.results)
        (self.out / "summary.txt").write_text(summary + "\n", encoding="utf-8")
        print(f"\n{len(self.results) - len(failed)} passed, {len(failed)} failed; logs in {self.out}")
        return 1 if failed else 0


def _exit_on_signal(signum, frame) -> None:
    # SystemExit unwinds through Smoke.run's `finally`, which stops QEMU: run.sh
    # and QEMU run in their own session, so the signal never reaches them.
    raise SystemExit(128 + signum)


def main() -> int:
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, _exit_on_signal)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(ROOT / "build-qemu" / "smoke"), help="output directory")
    ap.add_argument("--timeout", type=int, default=150, help="seconds for the web server and the UART1 checks")
    ap.add_argument("--observe", type=int, default=75, help="seconds from start to watch for crashes")
    ap.add_argument("--max-boots", type=int, default=2,
                    help="boots allowed in the window (the firmware may restart once on its own)")
    ap.add_argument("--http-port", type=int, default=0, help="host port for the web server (default: a free one)")
    args = ap.parse_args()
    if args.observe < MIN_OBSERVE_S:
        ap.error(f"--observe must be at least {MIN_OBSERVE_S} s (a no-receiver restart came at 27 s before it was held)")
    if not os.environ.get("IDF_PATH"):
        print("smoke.py: run . $IDF_PATH/export.sh first (tools/qemu/run.sh needs it)", file=sys.stderr)
        return 2
    return Smoke(args).run()


if __name__ == "__main__":
    sys.exit(main())
