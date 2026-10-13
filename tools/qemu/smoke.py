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
     nothing pending);
  3. checks UART1 in both directions: the firmware's receiver detection
     queries (VERSIONA) arrive on the host side, and bytes the host sends
     (plain text no receiver would send: no '$', '#', RTCM preamble or loader
     prompt in it) reach the firmware, which then reports "bytes but nothing
     valid" instead of "no bytes at all" when detection gives up; in the
     no-receiver episode that follows, the GNSS ladder's ESP32 restart is
     held for first setup (health.reboot_hold has "setup": no caster is
     enabled; the status names holds only during a ladder episode); with no
     receiver (GET /api/gnss state "no_receiver") a signal-group comparison
     is refused with 409 no_receiver and GET /api/gnss/signal-test stays
     idle;
  4. keeps watching until --observe seconds after the start (at least 45),
     checks that the web server still answers (GET /, and GET /api/status
     with the session), then stops QEMU and fails on a panic (Guru
     Meditation, abort(), stack overflow, failed assert, a task watchdog
     report) or a reboot loop: a restart without the firmware's own
     "rebooting (reason N)" line before it is a crash, and more than
     --max-boots boots in the window is a loop. It also fails on a GNSS
     ladder restart ("rebooting (reason 6)": held while first setup is not
     finished), on a console note about dropped lines ("[diag:"), and unless
     the console writer task runs and /api/health counts no console drop.
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
stops it on the way out, and run.sh gets --seconds as a backstop (observe +
timeout + a margin) in case this script is killed outright.

Everything lands in --out: the console log, the UART1 capture (raw bytes and
a timestamped log of both directions), the HTTP answers and summary.txt.
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
LADDER_REBOOT = "rebooting (reason 6)"   # the GNSS ladder's ESP32 restart (LIFECYCLE_REBOOT_GNSS_LADDER)
CONSOLE_NOTE = "[diag:"                    # the console's note about lines it could not send
MIN_OBSERVE_S = 45                         # longer than the 27 s at which a no-receiver restart came before
DETECT_RESULT = re.compile(r"um98x_ctl: receiver not found \(([^)]*)\)|um98x_ctl: receiver found at")
# Sent to the firmware on UART1: plain text a receiver never sends (no '$' or
# '#' line start, no RTCM preamble, no loader prompt). The framer discards it,
# so the firmware sees bytes but no reply.
LINK_TEST = b"qemu uart1 link test\r\n"
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


def request(port: int, method: str, path: str, body: dict | None = None, cookie: str | None = None,
         timeout: float = 10.0, host: str | None = None) -> tuple[int, dict, bytes]:
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
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as c:
        c.sendall(data)
        c.settimeout(0.2)
        while time.monotonic() - t0 < timeout:
            try:
                chunk = c.recv(65536)
            except socket.timeout:
                continue
            except OSError:
                return bytes(got), time.monotonic() - last
            if not chunk:
                return bytes(got), time.monotonic() - last
            got += chunk
            last = time.monotonic()
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
    try:
        conn.request(method, path, body=body, headers=headers or {})
        resp = conn.getresponse()
        raw = resp.read()
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
        backstop = max(self.args.observe, self.args.timeout) + BACKSTOP_MARGIN_S
        cmd = [str(RUN_SH), "--no-build", "--headless", str(self.console), "--uart1", f"connect:{uart1_port}",
               "--http-port", str(self.http_port), "--seconds", str(backstop)]
        print("starting: " + " ".join(cmd), flush=True)
        self.console.unlink(missing_ok=True)
        self.t0 = time.monotonic()
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
                status, hdrs, body = request(self.http_port, "GET", "/")
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
        self.http10_stream_checks()
        self.client_log_checks()
        self.log_level_checks()
        self.wifi_checks()

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
        with socket.create_connection(("127.0.0.1", port), timeout=10) as c:
            rd = AnswerReader(c)
            c.sendall(f"GET /api/session HTTP/1.1\r\n{host}\r\n".encode())
            first = rd.read()
            open_after = rd.wait_close(1.0) is None and not rd.buf
            c.sendall(f"GET /api/session HTTP/1.1\r\n{host}Connection: close\r\n\r\n".encode())
            second = rd.read()
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
            rd = AnswerReader(c)
            for path in ("/api/session", "/api/status"):
                c.sendall(f"GET {path} HTTP/1.1\r\n{host}\r\n".encode())
                answers.append((path, rd.read()))
            c.sendall(f"GET /api/session HTTP/1.1\r\n{host}Connection: close\r\n\r\n".encode())
            last = rd.read()
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
        with socket.create_connection(("127.0.0.1", self.http_port), timeout=10) as c:
            c.sendall(req.encode())
            a = AnswerReader(c).read()
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
        names = [t.get("name") for t in json.loads(body).get("tasks", [])] if status == 200 else []
        self.check("console" in names, f"the console writer task runs (HTTP {status}, {len(names)} tasks)")
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
                if request(self.http_port, "GET", "/", timeout=5)[0] == 200:
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
        boots = text.split(BOOT_MARK)
        n = len(boots) - 1
        crashes = sum(1 for before in boots[1:-1] if not OWN_REBOOT.search(before))
        self.check(n >= 1, f"{n} boot(s) in {self.args.observe} s")
        self.check(crashes == 0, f"{crashes} restart(s) without the firmware's own reboot line")
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
            self.observe()
        finally:
            self.stop()
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
