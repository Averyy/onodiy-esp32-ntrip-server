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
     API version 1, the station online over the emulated Ethernet;
  3. checks UART1 in both directions: the firmware's receiver detection
     queries (VERSIONA) arrive on the host side, and bytes the host sends
     (plain text no receiver would send: no '$', '#', RTCM preamble or loader
     prompt in it) reach the firmware, which then reports "bytes but nothing
     valid" instead of "no bytes at all" when detection gives up;
  4. keeps watching until --observe seconds after the start, then stops QEMU
     and fails on a panic (Guru Meditation, abort(), stack overflow, failed
     assert) or a reboot loop: a restart without the firmware's own
     "rebooting (reason N)" line before it is a crash, and more than
     --max-boots boots in the window is a loop.

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

ROOT = Path(__file__).resolve().parents[2]
RUN_SH = ROOT / "tools" / "qemu" / "run.sh"

FATAL = ["Guru Meditation", "abort()", "***ERROR*** A stack overflow", "assert failed:", "Backtrace:"]
BOOT_MARK = "ESP-ROM:"
OWN_REBOOT = re.compile(r"lifecycle: rebooting \(reason \d+\)")
DETECT_RESULT = re.compile(r"um98x_ctl: receiver not found \(([^)]*)\)|um98x_ctl: receiver found at")
# Sent to the firmware on UART1: plain text a receiver never sends (no '$' or
# '#' line start, no RTCM preamble, no loader prompt). The framer discards it,
# so the firmware sees bytes but no reply.
LINK_TEST = b"qemu uart1 link test\r\n"


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
         timeout: float = 10.0) -> tuple[int, dict, bytes]:
    conn = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    headers = {"Accept-Encoding": "gzip"}
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


class Smoke:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.out = Path(args.out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.console = self.out / "console.log"
        self.results: list[tuple[bool, str]] = []
        self.proc: subprocess.Popen | None = None
        self.tap: Uart1Tap | None = None
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
        cmd = [str(RUN_SH), "--no-build", "--headless", str(self.console), "--uart1", f"connect:{uart1_port}",
               "--http-port", str(self.http_port)]
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

    def observe(self) -> None:
        while self.elapsed() < self.args.observe and self.proc.poll() is None:
            time.sleep(1)
        alive = self.proc.poll() is None
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

    def run(self) -> int:
        try:
            self.start()
            if self.proc.poll() is None and self.wait_http():
                self.api_checks()
                self.uart1_checks()
            self.observe()
        finally:
            self.stop()
        failed = [t for ok, t in self.results if not ok]
        summary = "\n".join(("PASS " if ok else "FAIL ") + t for ok, t in self.results)
        (self.out / "summary.txt").write_text(summary + "\n", encoding="utf-8")
        print(f"\n{len(self.results) - len(failed)} passed, {len(failed)} failed; logs in {self.out}")
        return 1 if failed else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(ROOT / "build-qemu" / "smoke"), help="output directory")
    ap.add_argument("--timeout", type=int, default=150, help="seconds for the web server and the UART1 checks")
    ap.add_argument("--observe", type=int, default=75, help="seconds from start to watch for crashes")
    ap.add_argument("--max-boots", type=int, default=2,
                    help="boots allowed in the window (the firmware may restart once on its own)")
    ap.add_argument("--http-port", type=int, default=0, help="host port for the web server (default: a free one)")
    args = ap.parse_args()
    if not os.environ.get("IDF_PATH"):
        print("smoke.py: run . $IDF_PATH/export.sh first (tools/qemu/run.sh needs it)", file=sys.stderr)
        return 2
    return Smoke(args).run()


if __name__ == "__main__":
    sys.exit(main())
