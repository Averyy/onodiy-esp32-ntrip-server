#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Capture an NTRIP mountpoint with host timestamps and report its latency.

  capture  connect as an NTRIP client (GET) to a caster and save the stream
           as stream-NNNN.bin + stream-NNNN.idx (capture.py's format: one
           "<unix time>,<stream offset>,<bytes>" line per read; NTRIP 2.0
           chunking removed) and the caster's reply header
  report   for every epoch's first MSM frame: its arrival time (the .idx
           time of the read that completed the frame) minus its MSM epoch
           time, per constellation and overall (p50, p95, max), then
           "PASS p95 < 1000 ms" or "FAIL". Also takes a capture.py run
           (receiver-NNNN.bin/.idx): for a COM2 mirror of the receiver's
           RTCM that is the receiver's own output delay.

Epoch times: GPS, Galileo, QZSS and NavIC time of week, BeiDou + 14 s,
GLONASS Moscow day time (UTC + 3 h); GPS - UTC = 18 s. The latency is only
as good as this computer's clock: keep it synchronised (NTP).

There is no default host: say which caster to read. The password is taken
from an environment variable (--password-env) or a prompt
(--password-prompt), never from the command line, and is never printed or
written to the run directory.

Examples:
  uv run ntrip_timing.py capture --host 127.0.0.1 --port 2101 --mount BENCH --duration 120
  export TIMING_PASSWORD=...   # the client password of that mountpoint
  uv run ntrip_timing.py capture --host <caster> --port 2101 --mount <mountpoint> \\
      --user <user> --password-env TIMING_PASSWORD --ntrip-version 2 --duration 600 --label loopback
  uv run ntrip_timing.py report bench-logs/<timing run>
  uv run ntrip_timing.py report bench-logs/<capture.py run> --json
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import socket
import ssl
import sys
import time
from pathlib import Path

from benchlib import rtcm
from benchlib.common import (
    FLUSH_EVERY_S, RotatingFile, StopFlag, add_log_args, human_bytes, make_run_dir, write_command_line,
)
from benchlib.ntrip import Dechunker, basic_auth
from benchlib.replay import EpochGrouper, read_idx
from benchlib.stream import StreamSplitter

AGENT = "NTRIP onocoydiy-ntrip-timing/1.0"
GPS_EPOCH_UNIX = 315964800  # 1980-01-06 00:00:00 UTC
GPS_UTC_S = 18  # leap seconds, as the station's ledger uses
GPS_WEEK_S = 604800.0
BDS_TO_GPS_S = 14.0
MOSCOW_UTC_S = 3 * 3600.0
THRESHOLD_MS = 1000.0
REPLY_TIMEOUT_S = 15.0
SYSTEMS = ("GPS", "GLONASS", "Galileo", "BeiDou", "QZSS", "NavIC")


# ---------------------------------------------------------------------------
# latency


def latency_s(m: rtcm.Msm, arrival_unix: float) -> float:
    """Arrival time minus the MSM epoch time, in seconds (wrapped to the
    nearest period: a day for GLONASS, a week otherwise)."""
    if m.system == "GLONASS":
        tod = (m.epoch & ((1 << 27) - 1)) / 1000.0
        epoch_utc = (tod - MOSCOW_UTC_S) % 86400.0
        d = (arrival_unix % 86400.0) - epoch_utc
        return (d + 43200.0) % 86400.0 - 43200.0
    tow = m.epoch / 1000.0
    if m.system == "BeiDou":
        tow += BDS_TO_GPS_S
    arrival_tow = (arrival_unix - GPS_EPOCH_UNIX + GPS_UTC_S) % GPS_WEEK_S
    d = arrival_tow - tow
    return (d + GPS_WEEK_S / 2) % GPS_WEEK_S - GPS_WEEK_S / 2


def percentile(values: list[float], p: float) -> float:
    """Linear interpolation between the closest ranks (p in 0..100)."""
    v = sorted(values)
    if not v:
        raise ValueError("no values")
    k = (len(v) - 1) * p / 100.0
    lo = int(k)
    hi = min(lo + 1, len(v) - 1)
    return v[lo] + (v[hi] - v[lo]) * (k - lo)


def stats_ms(values_s: list[float]) -> dict:
    ms = [x * 1000.0 for x in values_s]
    if not ms:
        return {"epochs": 0, "p50_ms": None, "p95_ms": None, "max_ms": None, "min_ms": None}
    return {"epochs": len(ms), "p50_ms": round(percentile(ms, 50), 1), "p95_ms": round(percentile(ms, 95), 1),
            "max_ms": round(max(ms), 1), "min_ms": round(min(ms), 1)}


def stream_parts(run_dir: Path) -> list[tuple[Path, Path]]:
    run_dir = Path(run_dir)
    if not run_dir.is_dir():
        raise ValueError(f"{run_dir}: not a directory")
    bins = sorted(run_dir.glob("stream-*.bin")) or sorted(run_dir.glob("receiver-*.bin"))
    if not bins:
        raise ValueError(f"{run_dir}: no stream-*.bin or receiver-*.bin")
    out = []
    for b in bins:
        idx = b.with_suffix(".idx")
        if not idx.is_file():
            raise ValueError(f"{b.name}: no {idx.name} beside it (the read times are needed)")
        out.append((b, idx))
    return out


def analyse(run_dir: Path) -> dict:
    """Latency per constellation and overall from a run directory."""
    split = StreamSplitter()
    grouper = EpochGrouper()
    per_sys: dict[str, list[float]] = {}
    overall: list[float] = []
    samples = []
    state = {"epoch_sample": False, "seen": set()}

    def items(found) -> None:
        for it in found:
            if it.kind != "rtcm":
                continue
            new, m = grouper.feed(it.data)
            if new:
                state["epoch_sample"] = False
                state["seen"] = set()
            if m is None or it.t is None:
                continue
            lat = latency_s(m, it.t)
            if m.system not in state["seen"]:
                state["seen"].add(m.system)
                per_sys.setdefault(m.system, []).append(lat)
            if not state["epoch_sample"]:
                state["epoch_sample"] = True
                overall.append(lat)
                samples.append({"arrival_unix": round(it.t, 6), "type": m.mtype, "system": m.system,
                                "epoch": m.epoch, "latency_ms": round(lat * 1000.0, 1)})

    total = 0
    for bin_path, idx_path in stream_parts(run_dir):
        data = bin_path.read_bytes()
        entries = read_idx(idx_path)
        if sum(n for _, _, n in entries) != len(data):
            raise ValueError(f"{idx_path.name} does not account for the bytes of {bin_path.name}")
        pos = 0
        for t, _off, n in entries:
            items(split.feed(data[pos : pos + n], t))
            pos += n
        total += len(data)
    items(split.flush())
    out = {
        "run_dir": str(run_dir), "bytes": total, "threshold_ms": THRESHOLD_MS,
        "systems": {s: stats_ms(per_sys[s]) for s in SYSTEMS if s in per_sys},
        "overall": stats_ms(overall),
        "samples": samples,
    }
    p95 = out["overall"]["p95_ms"]
    out["verdict"] = "PASS" if p95 is not None and p95 < THRESHOLD_MS else "FAIL"
    return out


def report_text(d: dict) -> str:
    lines = [f"latency = arrival (host clock, the read that completed the frame) - MSM epoch time; "
             f"{human_bytes(d['bytes'])} in {d['run_dir']}",
             f"{'':<10} {'epochs':>7} {'p50 ms':>9} {'p95 ms':>9} {'max ms':>9} {'min ms':>9}"]

    def row(name: str, s: dict) -> str:
        f = (lambda v: "-" if v is None else f"{v:.1f}")
        return f"{name:<10} {s['epochs']:>7} {f(s['p50_ms']):>9} {f(s['p95_ms']):>9} {f(s['max_ms']):>9} {f(s['min_ms']):>9}"

    for name, s in d["systems"].items():
        lines.append(row(name, s))
    lines.append(row("overall", d["overall"]))
    p95 = d["overall"]["p95_ms"]
    if d["verdict"] == "PASS":
        lines.append(f"PASS p95 < {THRESHOLD_MS:.0f} ms")
    elif p95 is None:
        lines.append("FAIL (no MSM epoch in the stream)")
    else:
        lines.append(f"FAIL p95 {p95:.1f} ms >= {THRESHOLD_MS:.0f} ms")
    return "\n".join(lines)


# ---------------------------------------------------------------------------
# capture


def build_request(host: str, port: int, mount: str, user: str | None, password: str | None, version: int) -> bytes:
    lines = [f"GET /{mount} HTTP/{'1.1' if version == 2 else '1.0'}"]
    if version == 2:
        lines.append(f"Host: {host}:{port}")
        lines.append("Ntrip-Version: Ntrip/2.0")
    lines.append(f"User-Agent: {AGENT}")
    if user is not None:
        lines.append(f"Authorization: {basic_auth(user, password or '')}")
    if version == 2:
        lines.append("Connection: close")
    return ("\r\n".join(lines) + "\r\n\r\n").encode()


def read_reply(sock: socket.socket) -> tuple[bytes, bytes, bool]:
    """(reply header as received, body bytes that came with it, chunked)."""
    buf = bytearray()
    deadline = time.monotonic() + REPLY_TIMEOUT_S
    while True:
        nl = buf.find(b"\r\n")
        if nl >= 0:
            first = bytes(buf[:nl])
            if not first.startswith(b"HTTP/"):
                # NTRIP 1.0: "ICY 200 OK" and the data right after it (some
                # casters add an empty line first).
                rest = bytes(buf[nl + 2 :])
                if rest.startswith(b"\r\n"):
                    rest = rest[2:]
                return first + b"\r\n", rest, False
            end = buf.find(b"\r\n\r\n")
            if end >= 0:
                head = bytes(buf[: end + 4])
                chunked = b"transfer-encoding: chunked" in head.lower()
                return head, bytes(buf[end + 4 :]), chunked
        if len(buf) > 16384:
            raise ValueError("reply header longer than 16 KB")
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError(f"no complete reply within {REPLY_TIMEOUT_S:.0f} s (got {bytes(buf[:80])!r})")
        sock.settimeout(left)
        data = sock.recv(4096)
        if not data:
            raise ConnectionError(f"the caster closed before replying (got {bytes(buf[:200])!r})")
        buf += data


def capture(args) -> int:
    password = None
    if args.password_env:
        password = os.environ.get(args.password_env)
        if password is None:
            print(f"ntrip_timing: ${args.password_env} is not set", file=sys.stderr)
            return 2
    elif args.password_prompt:
        password = getpass.getpass(f"password for {args.user or 'the mountpoint'}: ")
    run_dir = make_run_dir(args.log_dir, "timing", args.label)
    write_command_line(run_dir)
    stop = StopFlag()
    print(f"[timing] {args.host}:{args.port}/{args.mount} NTRIP {args.ntrip_version}"
          f"{' TLS' if args.tls else ''}; logging to {run_dir}; Ctrl-C to stop", flush=True)
    req = build_request(args.host, args.port, args.mount, args.user, password, args.ntrip_version)
    password = None
    try:
        sock = socket.create_connection((args.host, args.port), timeout=REPLY_TIMEOUT_S)
    except OSError as e:
        print(f"ntrip_timing: cannot connect to {args.host}:{args.port}: {e}", file=sys.stderr)
        return 1
    try:
        if args.tls:
            sock = ssl.create_default_context().wrap_socket(sock, server_hostname=args.host)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        sock.sendall(req)
        req = b""
        head, body, chunked = read_reply(sock)
    except (OSError, ValueError, TimeoutError, ssl.SSLError) as e:
        print(f"ntrip_timing: {e}", file=sys.stderr)
        sock.close()
        return 1
    (run_dir / "reply-header.txt").write_bytes(head)
    first = head.split(b"\r\n", 1)[0].decode("latin-1")
    print(f"[timing] reply: {first}" + (" (chunked)" if chunked else ""), flush=True)
    if not (first == "ICY 200 OK" or (first.startswith("HTTP/") and first.split(" ")[1:2] == ["200"])):
        print(f"ntrip_timing: the caster refused: {first!r} (the whole header is in reply-header.txt)",
              file=sys.stderr)
        sock.close()
        return 1
    rot = RotatingFile(run_dir, "stream", ".bin", 100 * 1024 * 1024, binary=True)
    idx_part = rot.part
    idx = open(run_dir / f"stream-{idx_part:04d}.idx", "a", encoding="utf-8")
    dechunk = Dechunker() if chunked else None
    offset = 0
    start = time.monotonic()
    next_status = start + 10
    next_flush = start + FLUSH_EVERY_S
    split = StreamSplitter()
    frames = 0
    end_reason = "duration reached"

    def take(raw: bytes, t: float) -> None:
        nonlocal offset, idx_part, idx, frames
        data = dechunk.feed(raw) if dechunk else raw
        if not data:
            return
        rot.write(data)
        if rot.part != idx_part:
            idx.close()
            idx_part = rot.part
            idx = open(run_dir / f"stream-{idx_part:04d}.idx", "a", encoding="utf-8")
        idx.write(f"{t:.6f},{offset},{len(data)}\n")
        offset += len(data)
        frames += sum(1 for it in split.feed(data) if it.kind == "rtcm")

    try:
        if body:
            take(body, time.time())
        while not stop.is_set():
            now = time.monotonic()
            if args.duration and now - start >= args.duration:
                break
            if now >= next_flush or now >= next_status:
                rot.flush()   # tail -f works, and a killed run loses at most about a second
                idx.flush()
                next_flush = now + FLUSH_EVERY_S
            if now >= next_status:
                print(f"[timing] {now - start:.0f} s: {human_bytes(offset)}, {frames} frames", flush=True)
                next_status = now + 10
            sock.settimeout(0.2)
            try:
                data = sock.recv(65536)
            except (socket.timeout, ssl.SSLWantReadError):
                continue
            if not data:
                end_reason = "the caster closed the stream"
                break
            take(data, time.time())
            if dechunk and dechunk.done:
                end_reason = "the caster ended the chunked body"
                break
        else:
            end_reason = "stopped"
    except OSError as e:
        end_reason = f"read failed: {e}"
    finally:
        sock.close()
        rot.close()
        idx.close()
    print(f"[timing] {end_reason}: {human_bytes(offset)}, {frames} frames in {time.monotonic() - start:.0f} s",
          flush=True)
    if dechunk and dechunk.error:
        print(f"[timing] chunk framing error: {dechunk.error}", flush=True)
    if offset == 0:
        print("ntrip_timing: no data received", file=sys.stderr)
        return 1
    return report_cmd(run_dir, as_json=False, save=True)


def report_cmd(run_dir: Path, as_json: bool, save: bool = False) -> int:
    try:
        d = analyse(Path(run_dir))
    except (OSError, ValueError) as e:
        print(f"ntrip_timing: {e}", file=sys.stderr)
        return 2
    text = report_text(d)
    if save:
        (Path(run_dir) / "report.txt").write_text(text + "\n", encoding="utf-8")
        (Path(run_dir) / "report.json").write_text(json.dumps(d, indent=1) + "\n", encoding="utf-8")
    print(json.dumps(d, indent=1) if as_json else text)
    return 0 if d["verdict"] == "PASS" else 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], epilog=__doc__.split("\n\n", 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    cp = sub.add_parser("capture", help="read a mountpoint as an NTRIP client and save it with read times")
    cp.add_argument("--host", required=True, help="caster host name or address (no default)")
    cp.add_argument("--port", type=int, default=2101, help="caster port (default 2101)")
    cp.add_argument("--mount", required=True, help="mountpoint to read")
    cp.add_argument("--user", help="user name for Basic authentication (none: no Authorization header)")
    g = cp.add_mutually_exclusive_group()
    g.add_argument("--password-env", metavar="VAR", help="take the password from this environment variable")
    g.add_argument("--password-prompt", action="store_true", help="ask for the password")
    cp.add_argument("--ntrip-version", type=int, choices=(1, 2), default=1, help="1 (default) or 2")
    cp.add_argument("--tls", action="store_true", help="connect with TLS (the caster's certificate is checked)")
    cp.add_argument("--duration", type=float, default=60.0, help="seconds to read (default 60, 0 = until Ctrl-C)")
    add_log_args(cp)
    rp = sub.add_parser("report", help="latency report of a timing run or a capture.py run")
    rp.add_argument("run_dir")
    rp.add_argument("--json", action="store_true", help="print the report as JSON")
    args = ap.parse_args()
    if args.cmd == "report":
        return report_cmd(Path(args.run_dir), args.json)
    if not 1 <= args.port <= 65535:
        ap.error("--port must be 1-65535")
    return capture(args)


if __name__ == "__main__":
    sys.exit(main())
