#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Decode an RTCM 3 capture (or a live port, read-only) into a station report.

Reports:
  - per message type: frames, MSM epochs, bytes, rate and interval spread
  - MSM content per constellation and band: satellites, signals, mean C/N0
  - 1005/1006 position (ECEF and WGS84 lat/lon/height) and how much it moved
  - 1033 descriptor strings, 1230 GLONASS biases
  - stream health: CRC failures, text lines, junk bytes, MMB per epoch
  - Onocoy checks: MSM7 at 1 Hz for GPS/GLONASS/Galileo/BeiDou, 1005/1033/1230
    present, at least one constellation with 4 bands (L1/L2/L5/L6)

Inputs: capture run directories (receiver-*.bin, timed by receiver-*.idx),
any raw .bin files (concatenated in the order given), or --port for a live
read. Live mode only listens; it also saves the bytes it read.

Examples:
  uv run rtcm_inspect.py bench-logs/20261010-141500-capture
  uv run rtcm_inspect.py some-stream.bin --frames
  uv run rtcm_inspect.py --port /dev/cu.usbserial-1120 --seconds 60
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import serial

from benchlib.common import StopFlag, add_log_args, iso, make_run_dir, write_command_line
from benchlib.rtcm import summarize_frame
from benchlib.rtcmstats import RtcmStats
from benchlib.serialio import find_port, open_serial
from benchlib.stream import StreamSplitter


def expand_inputs(paths: list[str]) -> list[tuple[Path, Path | None]]:
    """[(bin, idx or None)] in stream order."""
    out = []
    for p in map(Path, paths):
        if p.is_dir():
            bins = sorted(p.glob("receiver-*.bin"))
            if not bins:
                raise SystemExit(f"{p}: no receiver-*.bin in this directory")
            for b in bins:
                idx = b.with_suffix(".idx")
                out.append((b, idx if idx.exists() else None))
        elif p.is_file():
            idx = p.with_suffix(".idx")
            out.append((p, idx if idx.exists() else None))
        else:
            raise SystemExit(f"{p}: no such file or directory")
    return out


def feed_file(bin_path: Path, idx_path: Path | None, handle) -> int:
    data = bin_path.read_bytes()
    if idx_path is not None:
        entries = []
        for line in idx_path.read_text(encoding="utf-8").splitlines():
            try:
                t, off, n = line.split(",")
                entries.append((float(t), int(n)))
            except ValueError:
                continue
        if sum(n for _, n in entries) == len(data):
            pos = 0
            for t, n in entries:
                handle(data[pos : pos + n], t)
                pos += n
            return len(data)
        print(f"note: {idx_path.name} does not match {bin_path.name}; rates use stream time only", file=sys.stderr)
    for pos in range(0, len(data), 65536):
        handle(data[pos : pos + 65536], None)
    return len(data)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("inputs", nargs="*", help="capture run directories or .bin files")
    ap.add_argument("--port", help="read a live port instead (listen only), or 'auto' for the UM982 COM2 bridge")
    ap.add_argument("--baud", type=int, default=115200, help="live mode baud (default 115200)")
    ap.add_argument("--seconds", type=float, default=30.0, help="live mode duration (default 30)")
    ap.add_argument("--frames", action="store_true", help="also print one line per frame/line/junk run")
    ap.add_argument("--json", action="store_true", help="print the report as JSON")
    add_log_args(ap)
    args = ap.parse_args()
    if bool(args.inputs) == bool(args.port):
        ap.error("give capture inputs or --port (not both)")

    stats = RtcmStats()
    split = StreamSplitter()
    frames_out: list[str] = []

    def handle_items(items) -> None:
        for it in items:
            stats.add_item(it)
            if args.frames:
                stamp = iso(it.t) + " " if it.t is not None else ""
                if it.kind == "rtcm":
                    line = summarize_frame(it.data)
                elif it.kind == "text":
                    line = f"TXT[{it.check}] {it.text}"
                elif it.kind == "rtcm_bad":
                    line = f"RTCM-BAD {it.note}"
                else:
                    line = f"{it.kind.upper()} {len(it.data)} bytes @{it.offset}"
                frames_out.append(stamp + line)
                if not args.json:
                    print(stamp + line)

    def handle(chunk: bytes, t: float | None) -> None:
        handle_items(split.feed(chunk, t))

    run_dir = make_run_dir(args.log_dir, "inspect", args.label)
    write_command_line(run_dir)
    total = 0
    if args.port:
        device = args.port
        if device == "auto":
            device = find_port("um982-com2")
            if not device:
                ap.error("--port auto: no single UM982 COM2 bridge by VID/PID; run `uv run ports.py`")
        stop = StopFlag()
        raw = open(run_dir / "receiver-0000.bin", "wb")
        idx = open(run_dir / "receiver-0000.idx", "w", encoding="utf-8")
        print(f"listening on {device} @ {args.baud} for {args.seconds:g} s (read-only) ...", file=sys.stderr)
        ser = open_serial(device, args.baud, timeout=0.05)
        try:
            end = time.monotonic() + args.seconds
            while time.monotonic() < end and not stop.is_set():
                waiting = ser.in_waiting
                data = ser.read(waiting) if waiting else ser.read(1)
                if data:
                    t = time.time()
                    raw.write(data)
                    idx.write(f"{t:.6f},{total},{len(data)}\n")
                    total += len(data)
                    handle(data, t)
        except serial.SerialException as e:
            print(f"port error: {e}", file=sys.stderr)
        finally:
            ser.close()
            raw.close()
            idx.close()
    else:
        for bin_path, idx_path in expand_inputs(args.inputs):
            total += feed_file(bin_path, idx_path, handle)
    handle_items(split.flush())
    stats.finish()

    report = stats.report()
    head = f"{total} bytes from {args.port or ', '.join(args.inputs)}"
    (run_dir / "report.txt").write_text(head + "\n\n" + report + "\n", encoding="utf-8")
    (run_dir / "report.json").write_text(json.dumps(stats.to_dict(), indent=2, default=str) + "\n", encoding="utf-8")
    if frames_out:
        (run_dir / "frames.log").write_text("\n".join(frames_out) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps(stats.to_dict(), indent=2, default=str))
    else:
        print(head)
        print()
        print(report)
        print(f"\nlog: {run_dir}")
    checks = stats.checks()
    return 1 if any(c["status"] == "FAIL" for c in checks) else 0


if __name__ == "__main__":
    sys.exit(main())
