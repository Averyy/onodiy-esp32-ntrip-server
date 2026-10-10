#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Capture the ESP32 console and/or the UM982 COM2 port with host timestamps.

Read-only: nothing is ever written to either port. The modem lines stay as
the OS sets them on open, so attaching does not reset the ESP32; only
--reset-esp32 pulses EN (a line toggle, still no bytes) to capture a boot
from its first line.

Writes to a new run directory (see --log-dir):
  console-NNNN.log    console lines, "<UTC time> <line>" (ANSI colours removed)
  console-NNNN.bin    raw console bytes (only with --console-raw)
  events.log          panics, resets, watchdogs and decoded backtraces seen on the console
  receiver-NNNN.bin   the receiver byte stream exactly as received
  receiver-NNNN.idx   "<unix time>,<stream offset>,<bytes>" per read, to time the .bin
  receiver-NNNN.log   one line per item: RTCM frames (type, length, CRC, decoded summary),
                      NMEA / $command / $CONFIG / # log lines verbatim with checksum verdict,
                      CRC failures and junk bytes
  summary.txt         counts and the RTCM inspection report at the end
Files rotate at --rotate-mb; parts sort in stream order. Ctrl-C stops
cleanly (twice to force).

Examples:
  uv run capture.py --auto
  uv run capture.py --console /dev/cu.usbmodem1101 --reset-esp32 --elf ../../build/onocoydiy-esp-um982.elf
  uv run capture.py --receiver /dev/cu.usbserial-1120 --duration 600 --label survey
  uv run rtcm_inspect.py bench-logs/<run dir>     # inspect a capture afterwards
"""

from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

import serial

from benchlib.common import (
    RotatingFile, StopFlag, add_log_args, hms, human_bytes, iso, make_run_dir, write_command_line,
)
from benchlib.rtcm import summarize_frame
from benchlib.rtcmstats import RtcmStats
from benchlib.serialio import find_port, open_serial, pulse_esp32_reset
from benchlib.stream import StreamSplitter

ANSI = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
EVENT_PATTERNS = [
    ("panic", re.compile(r"Guru Meditation Error|abort\(\) was called|assert failed|panic'ed")),
    ("backtrace", re.compile(r"^Backtrace:")),
    ("reset", re.compile(r"^rst:0x[0-9a-f]+|^ESP-ROM:")),
    ("rebooting", re.compile(r"^Rebooting\.\.\.")),
    ("watchdog", re.compile(r"task_wdt|Interrupt wdt timeout|TG\d+WDT")),
    ("brownout", re.compile(r"Brownout detector")),
    ("stack", re.compile(r"stack overflow", re.I)),
    ("error-log", re.compile(r"^E \(\d+\) ")),
]
PC_PAIR = re.compile(r"0x([0-9a-fA-F]{8}):0x[0-9a-fA-F]{8}")

print_lock = threading.Lock()


def say(msg: str) -> None:
    with print_lock:
        print(msg, flush=True)


def find_addr2line() -> str | None:
    for name in ("xtensa-esp32s3-elf-addr2line", "xtensa-esp-elf-addr2line"):
        p = shutil.which(name)
        if p:
            return p
    hits = sorted(glob.glob(os.path.expanduser("~/.espressif/tools/xtensa-esp-elf/*/xtensa-esp-elf/bin/xtensa-esp32s3-elf-addr2line")))
    return hits[-1] if hits else None


class PortReader(threading.Thread):
    """Reads one port until stopped; reopens it if it disappears (unplug, re-enumeration)."""

    def __init__(self, name: str, device: str, baud: int, stop: StopFlag, on_data, on_status, reset_first=False):
        super().__init__(name=name, daemon=True)
        self.device, self.baud, self.stop, self.on_data, self.on_status = device, baud, stop, on_data, on_status
        self.reset_first = reset_first
        self.bytes = 0
        self.reopens = 0

    def run(self) -> None:
        ser = None
        opened_once = False
        reported_failure = False
        while not self.stop.is_set():
            if ser is None:
                try:
                    ser = open_serial(self.device, self.baud, timeout=0.05)
                except (serial.SerialException, OSError) as e:
                    if not reported_failure:
                        self.on_status(f"cannot open {self.device}: {e}; retrying every second")
                        reported_failure = True
                    self.stop.wait(1.0)
                    continue
                reported_failure = False
                self.on_status(f"opened {self.device} @ {self.baud}" + (" (reopened)" if opened_once else ""))
                if opened_once:
                    self.reopens += 1
                elif self.reset_first:
                    try:
                        pulse_esp32_reset(ser)
                        self.on_status("pulsed ESP32 EN (reset) via RTS; no bytes sent")
                    except (serial.SerialException, OSError) as e:
                        self.on_status(f"could not pulse the reset line ({e}); press the board's RST button instead")
                opened_once = True
            try:
                # Read what is waiting, or block up to the timeout for one byte,
                # so each chunk's host time is within a few ms of its arrival.
                waiting = ser.in_waiting
                data = ser.read(waiting) if waiting else ser.read(1)
            except (serial.SerialException, OSError) as e:
                self.on_status(f"lost {self.device}: {e}; retrying every second")
                try:
                    ser.close()
                except Exception:
                    pass
                ser = None
                reported_failure = True
                continue
            if data:
                self.bytes += len(data)
                try:
                    self.on_data(data, time.time())
                except Exception as e:  # a decoding bug must not end the capture
                    self.on_status(f"internal error while logging {len(data)} bytes: {e!r} (capture continues)")
        if ser is not None:
            try:
                ser.close()
            except Exception:
                pass


class ConsoleSink:
    def __init__(self, run_dir: Path, rotate: int, echo: bool, raw: bool, elf: str | None):
        self.log = RotatingFile(run_dir, "console", ".log", rotate, binary=False)
        self.raw = RotatingFile(run_dir, "console", ".bin", rotate, binary=True) if raw else None
        self.events = open(run_dir / "events.log", "a", encoding="utf-8")
        self.echo = echo
        self.elf = elf
        self.addr2line = find_addr2line() if elf else None
        self.partial = bytearray()
        self.partial_t = 0.0
        self.lines = 0
        self.event_counts: dict[str, int] = {}
        self.lock = threading.Lock()
        if elf and not self.addr2line:
            say("[capture] --elf given but no xtensa addr2line found (source the ESP-IDF export.sh); backtraces stay raw")

    def data(self, chunk: bytes, t: float) -> None:
        with self.lock:
            if self.raw:
                self.raw.write(chunk)
            self.partial += chunk
            self.partial_t = t
            while True:
                nl = self.partial.find(b"\n")
                if nl < 0:
                    break
                line = bytes(self.partial[:nl])
                del self.partial[: nl + 1]
                self._line(line, t)

    def tick(self, now: float) -> None:
        """Flush a partial line after 0.5 s of silence (prompts, ROM text without newline)."""
        with self.lock:
            if self.partial and now - self.partial_t > 0.5:
                line = bytes(self.partial)
                self.partial.clear()
                self._line(line, self.partial_t)

    def _line(self, raw: bytes, t: float) -> None:
        text = ANSI.sub("", raw.decode("utf-8", errors="replace")).rstrip("\r")
        self.lines += 1
        self.log.write(f"{iso(t)} {text}\n")
        hit = None
        for name, pat in EVENT_PATTERNS:
            if pat.search(text):
                hit = name
                break
        if hit:
            self.event_counts[hit] = self.event_counts.get(hit, 0) + 1
            if hit != "error-log":
                self.events.write(f"{iso(t)} [{hit}] {text}\n")
                self.events.flush()
        if self.echo:
            mark = f" <<< {hit.upper()}" if hit and hit != "error-log" else ""
            say(f"{hms(t)} C| {text}{mark}")
        if hit == "backtrace" and self.addr2line:
            self._decode_backtrace(text, t)

    def _decode_backtrace(self, text: str, t: float) -> None:
        pcs = ["0x" + m.group(1) for m in PC_PAIR.finditer(text)]
        if not pcs:
            return
        try:
            res = subprocess.run([self.addr2line, "-pfiaC", "-e", self.elf, *pcs],
                                 capture_output=True, text=True, timeout=10)
            decoded = res.stdout.strip() or res.stderr.strip() or f"addr2line gave no output (exit {res.returncode})"
        except (OSError, subprocess.TimeoutExpired) as e:
            decoded = f"addr2line failed: {e}"
        for line in decoded.splitlines():
            self.events.write(f"{iso(t)} [decoded] {line}\n")
            self.log.write(f"{iso(t)} [decoded] {line}\n")
            if self.echo:
                say(f"{hms(t)} C| [decoded] {line}")
        self.events.flush()

    def close(self) -> None:
        with self.lock:
            if self.partial:
                self._line(bytes(self.partial), self.partial_t)
                self.partial.clear()
            self.log.close()
            if self.raw:
                self.raw.close()
            self.events.close()


class ReceiverSink:
    def __init__(self, run_dir: Path, rotate: int, echo: str):
        self.run_dir = run_dir
        self.rotate = rotate
        self.bin = RotatingFile(run_dir, "receiver", ".bin", rotate, binary=True)
        self.idx = open(run_dir / f"receiver-{self.bin.part:04d}.idx", "a", encoding="utf-8")
        self.idx_part = self.bin.part
        self.log = RotatingFile(run_dir, "receiver", ".log", rotate, binary=False)
        self.split = StreamSplitter()
        self.stats = RtcmStats()
        self.echo = echo
        self.offset = 0
        self.lock = threading.Lock()
        self.window_types: dict[int, int] = {}
        self.window_bytes = 0

    def data(self, chunk: bytes, t: float) -> None:
        with self.lock:
            self.bin.write(chunk)
            if self.bin.part != self.idx_part:
                self.idx.close()
                self.idx_part = self.bin.part
                self.idx = open(self.run_dir / f"receiver-{self.idx_part:04d}.idx", "a", encoding="utf-8")
            self.idx.write(f"{t:.6f},{self.offset},{len(chunk)}\n")
            self.offset += len(chunk)
            self.window_bytes += len(chunk)
            for item in self.split.feed(chunk, t):
                self._item(item)

    def _item(self, item) -> None:
        self.stats.add_item(item)
        stamp = iso(item.t)
        if item.kind == "rtcm":
            line = summarize_frame(item.data) + " crc=ok"
            mt = item.rtcm_type
            self.window_types[mt] = self.window_types.get(mt, 0) + 1
            if self.echo == "all":
                say(f"{hms(item.t)} R| {line}")
        elif item.kind == "rtcm_bad":
            line = f"RTCM-BAD len={len(item.data) - 6} {item.note} @{item.offset}"
            if self.echo != "none":
                say(f"{hms(item.t)} R| {line}")
        elif item.kind == "text":
            note = f" ({item.note})" if item.note else ""
            line = f"TXT[{item.check}]{note} {item.text}"
            if self.echo != "none":
                say(f"{hms(item.t)} R| {line}")
        elif item.kind == "binary":
            line = f"BIN {item.note} ({len(item.data)} bytes)"
        else:
            head = item.data[:32].hex(" ")
            line = f"JUNK {len(item.data)} bytes @{item.offset}: {head}{' ...' if len(item.data) > 32 else ''}"
        self.log.write(f"{stamp} {line}\n")

    def window(self) -> str:
        with self.lock:
            types = " ".join(f"{k}x{v}" for k, v in sorted(self.window_types.items()))
            b = self.window_bytes
            self.window_types = {}
            self.window_bytes = 0
        return f"{human_bytes(b)} [{types or 'no RTCM'}]"

    def close(self) -> None:
        with self.lock:
            for item in self.split.flush():
                self._item(item)
            self.stats.finish()
            self.bin.close()
            self.idx.close()
            self.log.close()


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--console", metavar="PORT", help="ESP32 console port, or 'auto' (CH343P by VID/PID)")
    ap.add_argument("--receiver", metavar="PORT", help="UM982 COM2 port, or 'auto' (CH340 by VID/PID)")
    ap.add_argument("--auto", action="store_true", help="same as --console auto --receiver auto")
    ap.add_argument("--console-baud", type=int, default=115200, help="default 115200 (the firmware's console rate)")
    ap.add_argument("--receiver-baud", type=int, default=115200, help="default 115200 (UM982 COM2 factory rate)")
    ap.add_argument("--duration", type=float, help="stop after this many seconds (default: until Ctrl-C)")
    ap.add_argument("--rotate-mb", type=float, default=100, help="start a new file part at this size (default 100)")
    ap.add_argument("--reset-esp32", action="store_true",
                    help="pulse the ESP32 EN line through RTS after opening the console (no bytes sent) to capture a boot")
    ap.add_argument("--elf", help="app ELF (e.g. build/onocoydiy-esp-um982.elf) to decode console backtraces with addr2line")
    ap.add_argument("--console-raw", action="store_true", help="also keep the raw console bytes (console-NNNN.bin)")
    ap.add_argument("--quiet", action="store_true", help="do not echo console lines")
    ap.add_argument("--show", choices=("none", "text", "all"), default="text",
                    help="receiver items to echo: none, text lines and CRC failures (default), or every frame too")
    ap.add_argument("--status-every", type=float, default=10.0, help="seconds between status lines (default 10, 0 = off)")
    add_log_args(ap)
    args = ap.parse_args()

    if args.auto:
        args.console = args.console or "auto"
        args.receiver = args.receiver or "auto"
    if not args.console and not args.receiver:
        ap.error("give --console and/or --receiver (or --auto); `uv run ports.py` lists the ports")
    for attr, role in (("console", "esp32-console"), ("receiver", "um982-com2")):
        if getattr(args, attr) == "auto":
            dev = find_port(role)
            if not dev:
                ap.error(f"--{attr} auto: no single port matches {role} by VID/PID; run `uv run ports.py` and pass the device")
            setattr(args, attr, dev)
    if args.elf and not Path(args.elf).is_file():
        ap.error(f"--elf {args.elf}: no such file")

    run_dir = make_run_dir(args.log_dir, "capture", args.label)
    write_command_line(run_dir)
    stop = StopFlag()
    rotate = int(args.rotate_mb * 1024 * 1024)
    say(f"[capture] logging to {run_dir}")
    say("[capture] read-only: nothing is sent to any port. Ctrl-C to stop.")

    readers: list[PortReader] = []
    console = receiver = None
    if args.console:
        console = ConsoleSink(run_dir, rotate, echo=not args.quiet, raw=args.console_raw, elf=args.elf)
        readers.append(PortReader("console", args.console, args.console_baud, stop, console.data,
                                  lambda m: say(f"[console] {m}"), reset_first=args.reset_esp32))
    if args.receiver:
        receiver = ReceiverSink(run_dir, rotate, echo=args.show)
        readers.append(PortReader("receiver", args.receiver, args.receiver_baud, stop, receiver.data,
                                  lambda m: say(f"[receiver] {m}")))
    for r in readers:
        r.start()

    start = time.time()
    next_status = start + args.status_every if args.status_every else None
    try:
        while not stop.is_set():
            stop.wait(0.2)
            now = time.time()
            if console:
                console.tick(now)
            if args.duration and now - start >= args.duration:
                break
            if next_status and now >= next_status:
                parts = [f"{int(now - start)} s"]
                if console:
                    ev = ", ".join(f"{k} {v}" for k, v in sorted(console.event_counts.items())) or "no events"
                    parts.append(f"console {console.lines} lines ({ev})")
                if receiver:
                    parts.append(f"receiver last {args.status_every:g} s: {receiver.window()}")
                say("[status] " + " | ".join(parts))
                next_status = now + args.status_every
    finally:
        stop.event.set()
        for r in readers:
            r.join(timeout=2)
        summary = [f"capture {run_dir.name}: {time.time() - start:.0f} s"]
        if console:
            console.close()
            ev = ", ".join(f"{k} {v}" for k, v in sorted(console.event_counts.items())) or "none"
            summary.append(f"console {args.console}: {console.lines} lines, events: {ev}")
        if receiver:
            receiver.close()
            summary.append(f"receiver {args.receiver}: {human_bytes(receiver.offset)} captured")
            summary.append("")
            summary.append(receiver.stats.report())
        text = "\n".join(summary)
        (run_dir / "summary.txt").write_text(text + "\n", encoding="utf-8")
        say("")
        say(text)
        say(f"\nlog: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
