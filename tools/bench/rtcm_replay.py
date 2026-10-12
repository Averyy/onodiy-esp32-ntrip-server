#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Replay a recorded receiver stream to a TCP client (QEMU's UART1).

Serves a capture.py run (receiver-NNNN.bin parts timed by their .idx) or a
plain RTCM 3 file (grouped into epochs by the MSM epoch times) to one TCP
client at a time, paced as recorded: every .idx read, or every epoch, at
its time x 1/--speed. With --baud each burst also goes out at that line
rate (10 bits per byte) instead of all at once. Everything the client sends
back (the firmware's receiver queries) is logged.

Made for the firmware in QEMU: start the replay first, then
`tools/qemu/run.sh --uart1 connect:PORT`, so the firmware hears the stream
from its first boot. The replay keeps listening after a client leaves (QEMU
reconnects every second); each new connection starts the schedule from the
beginning. Local TCP only: no serial port is opened, nothing goes anywhere
else.

Writes to a new run directory (see --log-dir):
  from-firmware.bin   every byte the client sent, in order
  replay.log          timestamped: connections, each loop's counts, and
                      every read from the client (length and bytes)
  summary.txt         the totals at the end

Examples:
  uv run rtcm_replay.py --listen 127.0.0.1:5600 ../../test/fixtures/rtcm/centipede_LAX_30ep.rtcm3 --loop
  uv run rtcm_replay.py --listen 127.0.0.1:5600 bench-logs/<capture run> --speed 2
  ../qemu/run.sh --uart1 connect:5600            # in another terminal
"""

from __future__ import annotations

import argparse
import socket
import sys
import threading
import time
from pathlib import Path

from benchlib.common import StopFlag, add_log_args, hms, human_bytes, iso, make_run_dir, write_command_line
from benchlib.replay import Schedule, load

LINE_PIECE = 64  # bytes per send when pacing at a line rate


def parse_hostport(text: str) -> tuple[str, int]:
    host, sep, port = text.rpartition(":")
    if not sep or not port.isdigit() or not 0 <= int(port) <= 65535:
        raise ValueError(f"{text!r} is not HOST:PORT")
    return host or "127.0.0.1", int(port)


class ReplayServer:
    """Serves one schedule to one TCP client at a time, in a background thread.

    Also used in-process by tools/qemu/e2e.py. `say` gets one-line status
    messages. Counters are per server: totals over every connection."""

    def __init__(self, schedule: Schedule, host: str = "127.0.0.1", port: int = 0, loop: bool = False,
                 speed: float = 1.0, baud: int = 0, run_dir: Path | None = None, say=print):
        if speed <= 0:
            raise ValueError("speed must be > 0")
        if baud < 0:
            raise ValueError("baud must be >= 0")
        self.schedule = schedule
        self.loop = loop
        self.speed = speed
        self.baud = baud
        self.say = say
        self.stop_event = threading.Event()
        self.lock = threading.Lock()
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(1)
        self.sock.settimeout(0.2)
        self.host, self.port = self.sock.getsockname()[:2]
        self.run_dir = run_dir
        self.rx_file = open(run_dir / "from-firmware.bin", "ab") if run_dir else None
        self.log_file = open(run_dir / "replay.log", "a", encoding="utf-8") if run_dir else None
        self.connections = 0
        self.loops_done = 0
        self.bursts_sent = 0
        self.bytes_sent = 0
        self.frames_sent = 0
        self.bytes_received = 0
        self.bursts_paused = 0
        self.received = bytearray()  # everything clients sent (kept for in-process checks)
        self.connected = threading.Event()
        self.paused = threading.Event()
        self.thread = threading.Thread(target=self._serve, name="replay", daemon=True)

    # -- logging ----------------------------------------------------------------

    def _log(self, text: str, echo: bool = True) -> None:
        if self.log_file:
            with self.lock:
                self.log_file.write(f"{iso()} {text}\n")
                self.log_file.flush()
        if echo:
            self.say(f"{hms(time.time())} {text}")

    # -- control ----------------------------------------------------------------

    def start(self) -> ReplayServer:
        self.thread.start()
        return self

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=5)
        try:
            self.sock.close()
        except OSError:
            pass
        with self.lock:
            if self.rx_file:
                self.rx_file.close()
                self.rx_file = None
            if self.log_file:
                self.log_file.close()
                self.log_file = None

    def pause(self) -> None:
        """Send nothing until resume(), like a receiver whose output went quiet.
        The schedule's clock keeps running: bursts that fall due meanwhile are
        dropped (counted in bursts_paused), never sent late."""
        if not self.paused.is_set():
            self.paused.set()
            self._log("paused: bursts that fall due are dropped until resumed")

    def resume(self) -> None:
        if self.paused.is_set():
            self.paused.clear()
            with self.lock:
                n = self.bursts_paused
            self._log(f"resumed ({n} burst{'' if n == 1 else 's'} dropped while paused, in total)")

    def counts(self) -> dict:
        with self.lock:
            return {"connections": self.connections, "loops": self.loops_done, "bursts": self.bursts_sent,
                    "bytes_sent": self.bytes_sent, "frames_sent": self.frames_sent,
                    "bytes_received": self.bytes_received, "bursts_paused": self.bursts_paused}

    # -- serving ----------------------------------------------------------------

    def _serve(self) -> None:
        self._log(f"listening on {self.host}:{self.port}: {self.schedule.kind} {self.schedule.source}, "
                  f"{len(self.schedule.bursts)} bursts, {human_bytes(self.schedule.size)}, "
                  f"{self.schedule.frames} frames per loop, {self.schedule.duration:.3f} s per loop at speed 1"
                  + (f", speed {self.speed:g}" if self.speed != 1 else "")
                  + (f", {self.baud} baud" if self.baud else "") + (", looping" if self.loop else ""))
        while not self.stop_event.is_set():
            try:
                conn, peer = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with self.lock:
                self.connections += 1
                n = self.connections
            self._log(f"connection {n} from {peer[0]}:{peer[1]}")
            self.connected.set()
            try:
                self._session(conn, n)
            finally:
                try:
                    conn.close()
                except OSError:
                    pass
        self._log("stopped")

    def _session(self, conn: socket.socket, n: int) -> None:
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        gone = threading.Event()
        reader = threading.Thread(target=self._read, args=(conn, gone), name=f"replay-rx-{n}", daemon=True)
        reader.start()
        t0 = time.monotonic()
        loop_no = 0
        loop_counts = [0, 0, 0]  # bursts, bytes, frames
        loop_start = t0
        ended = "client closed"
        try:
            for k, _i, t, burst in self.schedule.iter_times(self.loop, self.speed):
                if k != loop_no:
                    self._loop_done(n, loop_no, loop_counts, time.monotonic() - loop_start)
                    loop_no, loop_counts, loop_start = k, [0, 0, 0], time.monotonic()
                if self._wait_until(t0 + t, gone):
                    break
                if self.paused.is_set():
                    with self.lock:
                        self.bursts_paused += 1
                    continue
                if not self._send(conn, burst.data, gone):
                    break
                with self.lock:
                    self.bursts_sent += 1
                    self.bytes_sent += len(burst.data)
                    self.frames_sent += len(burst.frames)
                loop_counts[0] += 1
                loop_counts[1] += len(burst.data)
                loop_counts[2] += len(burst.frames)
            else:
                self._loop_done(n, loop_no, loop_counts, time.monotonic() - loop_start)
                self._log(f"connection {n}: schedule done; still reading what the client sends")
                while not gone.wait(0.2) and not self.stop_event.is_set():
                    pass
            if self.stop_event.is_set():
                ended = "replay stopped"
        except OSError as e:
            ended = f"send failed: {e}"
        if loop_counts[0] and (gone.is_set() or self.stop_event.is_set()) and loop_counts[0] < len(self.schedule.bursts):
            self._log(f"connection {n}: loop {loop_no + 1} cut after {loop_counts[0]} of "
                      f"{len(self.schedule.bursts)} bursts ({human_bytes(loop_counts[1])})")
        try:
            conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        reader.join(timeout=2)
        self._log(f"connection {n} ended ({ended})")

    def _loop_done(self, n: int, loop_no: int, counts: list[int], took: float) -> None:
        with self.lock:
            self.loops_done += 1
            rx = self.bytes_received
        self._log(f"connection {n}: loop {loop_no + 1}: {counts[0]} bursts, {human_bytes(counts[1])}, "
                  f"{counts[2]} frames in {took:.1f} s; {rx} bytes from the client so far")

    def _wait_until(self, deadline: float, gone: threading.Event) -> bool:
        """Sleep until `deadline` (monotonic); True if the client left or the replay stops first."""
        while True:
            if gone.is_set() or self.stop_event.is_set():
                return True
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            time.sleep(min(left, 0.05))

    def _send(self, conn: socket.socket, data: bytes, gone: threading.Event) -> bool:
        if not self.baud:
            conn.sendall(data)
            return True
        # Line rate: 10 bits per byte (start, 8 data, stop), in small pieces.
        start = time.monotonic()
        sent = 0
        while sent < len(data):
            piece = data[sent : sent + LINE_PIECE]
            conn.sendall(piece)
            sent += len(piece)
            if self._wait_until(start + sent * 10.0 / self.baud, gone):
                return False
        return True

    def _read(self, conn: socket.socket, gone: threading.Event) -> None:
        while True:
            try:
                data = conn.recv(4096)
            except OSError:
                break
            if not data:
                break
            with self.lock:
                self.bytes_received += len(data)
                self.received += data
                if self.rx_file:
                    self.rx_file.write(data)
                    self.rx_file.flush()
            self._log(f"from client {len(data)} bytes: {data!r}", echo=False)
        gone.set()


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--listen", required=True, metavar="HOST:PORT", help="address to listen on, e.g. 127.0.0.1:5600")
    ap.add_argument("input", help="capture.py run directory, receiver-NNNN.bin (with its .idx), or a plain RTCM 3 file")
    ap.add_argument("--loop", action="store_true", help="start again after the last burst (continuous time base)")
    ap.add_argument("--speed", type=float, default=1.0, help="replay speed: 2 halves every gap (default 1)")
    ap.add_argument("--baud", type=int, default=0,
                    help="send each burst at this line rate (10 bits per byte) instead of all at once")
    add_log_args(ap)
    args = ap.parse_args()
    if args.speed <= 0:
        ap.error("--speed must be > 0")
    if args.baud < 0:
        ap.error("--baud must be > 0")
    try:
        host, port = parse_hostport(args.listen)
    except ValueError as e:
        ap.error(f"--listen: {e}")
    try:
        schedule = load(args.input)
    except (OSError, ValueError) as e:
        print(f"rtcm_replay: {e}", file=sys.stderr)
        return 2
    if args.baud and schedule.kind == "rtcm":
        # The line rate must keep up with the schedule, or bursts pile up.
        need = max(len(b.data) for b in schedule.bursts) * 10.0 / args.baud
        if need > schedule.period / args.speed:
            print(f"rtcm_replay: note: the largest epoch takes {need:.2f} s at {args.baud} baud, "
                  f"longer than the {schedule.period / args.speed:.2f} s between epochs; epochs will run late",
                  file=sys.stderr)

    run_dir = make_run_dir(args.log_dir, "replay", args.label)
    write_command_line(run_dir)
    stop = StopFlag()
    try:
        server = ReplayServer(schedule, host, port, loop=args.loop, speed=args.speed, baud=args.baud,
                              run_dir=run_dir, say=lambda m: print(m, flush=True))
    except OSError as e:
        print(f"rtcm_replay: cannot listen on {host}:{port}: {e}", file=sys.stderr)
        return 2
    print(f"[replay] logging to {run_dir}; Ctrl-C to stop", flush=True)
    server.start()
    try:
        while not stop.is_set():
            stop.wait(0.5)
    finally:
        server.stop()
        c = server.counts()
        text = (f"replay {run_dir.name}: {c['connections']} connection(s), {c['loops']} whole loop(s), "
                f"{c['bursts']} bursts, {human_bytes(c['bytes_sent'])} ({c['frames_sent']} frames) sent, "
                f"{c['bytes_received']} bytes received from the client(s)")
        (run_dir / "summary.txt").write_text(text + "\n", encoding="utf-8")
        print(text)
        print(f"log: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
