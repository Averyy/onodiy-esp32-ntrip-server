#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""The 8 MB board's install in QEMU: the same build on partitions-8mb.csv finds its board, runs without a package
area, and keeps a crash's core dump.

    uv run tools/qemu/board_8mb.py [--out DIR] [--timeout S]

Needs the ESP-IDF environment and the bench overlay's QEMU build in
build-qemu-bench/ (tools/qemu/run.sh --overlay sdkconfig.bench
--build-only): its console's `crash` command makes the crash.
Standard library only.

The GNS NTRIP-X gets the station's app and bootloader with another partition
table (partitions-8mb.csv: 2.5 MB app slots, no receiver package partition,
a 128 KB core dump partition and the board.um980-ntrx marker) and --flash-size
8MB. This runs that install:

  1. starts tools/qemu/run.sh --no-build --overlay sdkconfig.bench --table 8mb
     --headless (an 8 MB flash image), the console on --console
     connect:<port> and UART1 on --uart1 connect:<port>, so it sees every
     byte of both from the first boot;
  2. the console: the board line ("board: GNS NTRIP-X (marked in the
     partition table)"), the receiver link on the board's pins ("GNS
     NTRIP-X: GNSS on UART1 (RX GPIO16, TX GPIO15) to the receiver's COM1"),
     the package store's note that the board has no package partition, and
     "startup complete" without a failed step;
  3. UART1: the receiver detection arrives (the link opened on the board);
  4. the web API, with the first admin password: GET /api/system names the
     board (um980-ntrx, "GNS NTRIP-X", source marker), 8 MB flash, the board's
     wiring as the default (RX GPIO16, TX GPIO15, COM1) and no pin of the
     board's own among the link's choices (1, 2, 4-8, 11, 40, 41); GET
     /api/update: app_max_bytes 2621440 (the 2.5 MB slot), pkg_max_bytes
     null; GET /api/status: gnss_flash_reason "no_package_area";
  5. a crash: `crash CRASH-ESP32` aborts; the panic handler saves the core
     dump (every task, as for any crash) to the 128 KB partition ("Core dump has been saved to flash.", never "Not
     enough space"), the station starts again, and GET /api/health has the
     dump (coredump.present, its size within 128 KB) and its summary (the
     crashed task, its PC, a backtrace);
  6. throughout: no panic but that one, two boots.

Everything lands in --out: the console log (console.log), what was typed
(typed.log), the UART1 bytes (uart1.bin), the HTTP answers and summary.txt.
Exit status 0 when every check passed, 1 otherwise.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ next to the scripts
sys.path.insert(0, str(Path(__file__).resolve().parent))

import bench_console  # noqa: E402  (by path, above: its console tap and waits)
import smoke  # noqa: E402

BUILD = bench_console.BUILD
BOARD_LINE = "board: GNS NTRIP-X (marked in the partition table)"
LINK_LINE = "GNS NTRIP-X: GNSS on UART1 (RX GPIO16, TX GPIO15) to the receiver's COM1"
NO_PKG_LINE = "no partition 'gnss_pkg' on this board"
SAVED = "Core dump has been saved to flash."
NO_ROOM = "Not enough space to save core dump"
COREDUMP_BYTES = 0x20000     # partitions-8mb.csv
APP_SLOT_BYTES = 0x280000
BOARD_PINS = {1, 2, 4, 5, 6, 7, 8, 11, 40, 41}


class Uart1Tap:
    """The host end of UART1: QEMU connects to it (retrying every second) and every byte the firmware sends lands
    here."""

    def __init__(self, out: Path):
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.port = self.listener.getsockname()[1]
        self.rx = bytearray()
        self.lock = threading.Lock()
        self.file = open(out / "uart1.bin", "wb")
        self.thread = threading.Thread(target=self._run, name="uart1", daemon=True)
        self.thread.start()

    def _run(self) -> None:
        self.listener.settimeout(1)
        while True:
            try:
                conn, _ = self.listener.accept()
            except socket.timeout:
                continue
            except OSError:
                return
            while True:
                try:
                    data = conn.recv(4096)
                except OSError:
                    break
                if not data:
                    break
                with self.lock:
                    self.rx += data
                    self.file.write(data)
                    self.file.flush()
            conn.close()

    def size(self) -> int:
        with self.lock:
            return len(self.rx)

    def close(self) -> None:
        self.listener.close()
        self.file.close()


class Board8mb(bench_console.BenchConsole):
    def __init__(self, args: argparse.Namespace):
        super().__init__(args)
        self.uart1: Uart1Tap | None = None

    def start(self) -> bool:
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        self.uart1 = Uart1Tap(self.out)
        self.http_port = smoke.free_port()
        backstop = 5 * self.args.timeout + smoke.BACKSTOP_MARGIN_S
        cmd = [str(smoke.RUN_SH), "--no-build", "--overlay", "sdkconfig.bench", "--table", "8mb", "--headless",
               str(self.console), "--console", f"connect:{port}", "--uart1", f"connect:{self.uart1.port}",
               "--http-port", str(self.http_port), "--seconds", str(backstop)]
        print("starting: " + " ".join(cmd), flush=True)
        self.console.unlink(missing_ok=True)
        self.t0 = time.monotonic()
        self.proc = subprocess.Popen(cmd, stdout=open(self.out / "run.log", "w"), stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        listener.settimeout(1)
        conn = None
        try:
            while conn is None and self.elapsed() < 60 and self.alive():
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    pass
        finally:
            listener.close()
        if not self.check(conn is not None, "QEMU connected the console (UART0) to the host"):
            return False
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.tap = bench_console.ConsoleTap(conn, self.out, self.t0)
        return True

    def stop(self) -> None:
        super().stop()
        if self.uart1 is not None:
            self.uart1.close()
            self.uart1 = None

    def console_checks(self) -> None:
        text = self.wait_text(0, lambda t: "startup complete" in t, self.args.timeout)
        for want in (BOARD_LINE, LINK_LINE, NO_PKG_LINE):
            self.check(want in text, f"the console says \"{want}\"")
        line = next((x.strip() for x in text.splitlines() if "startup complete" in x), None)
        self.check(line is not None and "failed" not in line, f"startup: {line or 'no startup line'}")
        failed = [x.strip()[:160] for x in text.splitlines() if " failed: " in x and ("init " in x or "start " in x)]
        self.check(not failed, f"no failed start step ({failed[0] if failed else 'none'})")
        deadline = time.monotonic() + self.args.timeout
        while time.monotonic() < deadline and self.alive() and self.uart1.size() == 0:
            time.sleep(0.2)
        n = self.uart1.size()
        self.check(n > 0, f"the firmware sends its receiver detection on UART1 ({n} bytes)")

    def get(self, path: str) -> dict | None:
        status, _, body = smoke.request(self.http_port, "GET", path, cookie=self.cookie, timeout=30)
        (self.out / (path.strip("/").replace("/", "-") + ".json")).write_bytes(body)
        if not self.check(status == 200, f"GET {path}: HTTP {status}"):
            return None
        return json.loads(body)

    def api_checks(self) -> None:
        if self.cookie is None:
            return
        try:
            sysj = self.get("/api/system")
            if sysj is not None:
                board = sysj.get("board") or {}
                self.check(board == {"id": "um980-ntrx", "name": "GNS NTRIP-X", "source": "marker"},
                           f"GET /api/system board: {board}")
                flash = (sysj.get("chip") or {}).get("flash_mb")
                self.check(flash == 8, f"GET /api/system chip.flash_mb: {flash}")
                w = sysj.get("wiring") or {}
                default = w.get("default") or {}
                self.check(default == {"rx_gpio": 16, "tx_gpio": 15, "port": 1},
                           f"the board's wiring is the default: {default}")
                self.check((w.get("rx_gpio"), w.get("tx_gpio"), w.get("port")) == (16, 15, 1),
                           f"this start uses it: RX GPIO{w.get('rx_gpio')}, TX GPIO{w.get('tx_gpio')}, "
                           f"COM{w.get('port')}")
                taken = sorted(BOARD_PINS & set(w.get("pins") or []))
                self.check(bool(w.get("pins")) and not taken,
                           f"none of the board's own pins is a choice for the link ({taken or 'none'})")
            upd = self.get("/api/update")
            if upd is not None:
                self.check(upd.get("app_max_bytes") == APP_SLOT_BYTES,
                           f"GET /api/update app_max_bytes {upd.get('app_max_bytes')} (the 2.5 MB slot)")
                self.check("pkg_max_bytes" in upd and upd["pkg_max_bytes"] is None,
                           f"GET /api/update pkg_max_bytes {upd.get('pkg_max_bytes', 'missing')} (no package area)")
            st = self.get("/api/status")
            if st is not None:
                why = (st.get("update") or {}).get("gnss_flash_reason")
                self.check(why == "no_package_area", f"GET /api/status update.gnss_flash_reason {why}")
        except (OSError, http.client.HTTPException, ValueError) as e:
            self.check(False, f"web API: {type(e).__name__}: {e}")

    def crash_checks(self) -> None:
        if self.cookie is None:
            return
        start = self.tap.size()
        self.tap.type("crash CRASH-ESP32")
        text = self.wait_text(start, lambda t: SAVED in t or NO_ROOM in t, self.args.timeout)
        line = next((x.strip()[:160] for x in text.splitlines() if "BENCH" in x), "no answer")
        self.check("BENCH crash requested" in line, f"crash: {line}")
        self.check(SAVED in text and NO_ROOM not in text,
                   f"the panic saved the core dump in the 128 KB partition"
                   + ("" if SAVED in text else f" (\"{NO_ROOM}\" on the console)" if NO_ROOM in text else
                      " (no \"Core dump has been saved\" line)"))
        text = self.wait_text(start, lambda t: smoke.BOOT_MARK in t, self.args.timeout)
        if not self.check(self.web_up(), "the station starts again after the crash, the session is back"):
            return
        try:
            health = self.get("/api/health")
            dump = (health or {}).get("coredump") or {}
            size = dump.get("size") or 0
            self.check(dump.get("present") is True and 0 < size <= COREDUMP_BYTES,
                       f"GET /api/health coredump present {dump.get('present')}, {size} bytes of "
                       f"{COREDUMP_BYTES}")
            # The dump's summary: the crashed task, its PC and a backtrace.
            self.check(bool(dump.get("task")) and dump.get("pc") not in (None, "0x00000000")
                       and bool(dump.get("backtrace")),
                       f"the dump names the task {dump.get('task')!r}, PC {dump.get('pc')}, "
                       f"{len(dump.get('backtrace') or [])} backtrace address(es)")
        except (OSError, http.client.HTTPException, ValueError) as e:
            self.check(False, f"web API after the crash: {type(e).__name__}: {e}")

    def final_checks(self) -> None:
        alive = self.alive()
        text = self.tap.text() if self.tap is not None else ""
        self.stop()
        self.check(alive, f"QEMU still running after {self.elapsed():.0f} s")
        boots = text.count(smoke.BOOT_MARK)
        self.check(boots == 2, f"{boots} boot(s): the first and the one after the asked-for crash")
        # The asked-for crash prints its abort and a backtrace, once; anything after the restart is another crash.
        first = text.find(smoke.BOOT_MARK, text.find(smoke.BOOT_MARK) + 1)
        later = text[first:] if first >= 0 else ""
        fatal = next((x.strip()[:160] for x in later.splitlines() if any(f in x for f in smoke.FATAL)), None)
        self.check(fatal is None, f"no panic after the restart ({fatal or 'none'})")

    def run(self) -> int:
        try:
            if self.start():
                self.console_checks()
                started = self.wait_start()
                self.web_checks()
                self.api_checks()
                if started:
                    self.crash_checks()
            self.final_checks()
        finally:
            self.stop()
        failed = [t for ok, t in self.results if not ok]
        summary = "\n".join(("PASS " if ok else "FAIL ") + t for ok, t in self.results)
        (self.out / "summary.txt").write_text(summary + "\n", encoding="utf-8")
        print(f"\n{len(self.results) - len(failed)} passed, {len(failed)} failed; logs in {self.out}")
        return 1 if failed else 0


def _exit_on_signal(signum, frame) -> None:
    raise SystemExit(128 + signum)


def main() -> int:
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, _exit_on_signal)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(BUILD / "board-8mb"), help="output directory")
    ap.add_argument("--timeout", type=int, default=150, help="seconds for each wait")
    args = ap.parse_args()
    if not os.environ.get("IDF_PATH"):
        print("board_8mb.py: run . $IDF_PATH/export.sh first (tools/qemu/run.sh needs it)", file=sys.stderr)
        return 2
    if not bench_console.sdkconfig_on(BUILD, "CONFIG_BENCH_CONSOLE"):
        print(f"board_8mb.py: {BUILD.name}/ holds no bench build: "
              "tools/qemu/run.sh --overlay sdkconfig.bench --build-only", file=sys.stderr)
        return 2
    return Board8mb(args).run()


if __name__ == "__main__":
    sys.exit(main())
