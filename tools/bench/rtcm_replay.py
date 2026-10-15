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

In process (tools/qemu/e2e.py), set_last_frame_delay() sends each burst's
last frame a set time after the rest of it: an epoch whose last MSM comes
late, as a stalled line or reader makes it look to the station; a pause
that begins while such a frame waits drops it too. wait_burst_head() waits
for the next burst to start going out (to time a request inside an epoch).

Made for the firmware in QEMU: start the replay first, then
`tools/qemu/run.sh --uart1 connect:PORT`, so the firmware hears the stream
from its first boot. The replay keeps listening after a client leaves (QEMU
reconnects every second); each new connection starts the schedule from the
beginning. Local TCP only: no serial port is opened, nothing goes anywhere
else.

With --respond a scripted UM982 (benchlib.um982_sim) answers the station's
receiver commands: its identity, configuration dump and base mode, the
configuration it sends, and signal-group changes (accepted for 4 5, 3 6,
5 0 and 7 0, less any --respond-reject; a change or RESET withholds the RTCM
for --respond-restart seconds, 3 by default, a real UM982 build 25164 about
9, answering any command meanwhile with the receiver's "The Board is
resetting" line, then the restart banner). The RTCM output follows it: off
after UNLOG COM1 until an RTCMxxxx command, off while it restarts. Bursts
that fall due meanwhile are dropped, never sent late. The station messages
it was told to send (1005/1006 of a fixed base, 1033, 1230) are its own: they
open each epoch, the recording's own of those types left out.

Writes to a new run directory (see --log-dir):
  from-firmware.bin   every byte the client sent, in order
  replay.log          timestamped: connections, each loop's counts, and
                      every read from the client (length and bytes)
  summary.txt         the totals at the end

Examples:
  uv run rtcm_replay.py --listen 127.0.0.1:5600 ../../test/fixtures/rtcm/centipede_LAX_30ep.rtcm3 --loop
  uv run rtcm_replay.py --listen 127.0.0.1:5600 bench-logs/<capture run> --speed 2
  uv run rtcm_replay.py --listen 127.0.0.1:5600 ../../test/fixtures/rtcm/centipede_LAX_30ep.rtcm3 --loop \
      --respond --respond-group 4,5 --respond-reject 7,0
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
from benchlib.replay import Burst, Schedule, load
from benchlib.um982_sim import Um982Responder, parse_group, with_station_messages

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
                 speed: float = 1.0, baud: int = 0, run_dir: Path | None = None, say=print,
                 responder: Um982Responder | None = None):
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
        self.bursts_withheld = 0     # due while the responder's receiver sent no RTCM (UNLOG, restart)
        self.replies_sent = 0        # responder bytes sent
        self.last_frame_delay_s = 0.0  # set_last_frame_delay: each burst's last frame held back this long
        self.bursts_late = 0         # bursts sent with their last frame late
        self.late_paused = 0         # late last frames dropped by a pause that began while they waited
        self.heads = threading.Condition()   # wait_burst_head: the bursts started so far and the last one's time
        self.heads_sent = 0
        self.head_t = 0.0
        self.responder = responder
        self.send_lock = threading.Lock()   # bursts and replies never interleave inside one write
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

    def set_last_frame_delay(self, seconds: float) -> None:
        """Send each burst's last frame `seconds` after the rest of it (0: at once again): an epoch whose last
        MSM comes late, as a line or a reader that stalls inside an epoch makes it look to the station. Only
        bursts of whole frames (a capture's reads go out as recorded)."""
        with self.lock:
            self.last_frame_delay_s = max(0.0, float(seconds))
        self._log(f"last frame of each burst {f'{seconds:.3f} s late' if seconds > 0 else 'at once again'}")

    def wait_burst_head(self, timeout: float) -> float | None:
        """Wait until the next burst starts going out (its first byte sent, after this call); its monotonic
        time, or None after `timeout`."""
        with self.heads:
            n = self.heads_sent
            if not self.heads.wait_for(lambda: self.heads_sent > n, timeout):
                return None
            return self.head_t

    def _head_sent(self) -> None:
        with self.heads:
            self.heads_sent += 1
            self.head_t = time.monotonic()
            self.heads.notify_all()

    def set_responder(self, responder: Um982Responder | None) -> None:
        """Answer the client's receiver commands from now on (None: answer nothing again)."""
        with self.lock:
            self.responder = responder
        self._log("responder " + ("on: a scripted UM982 answers the receiver commands" if responder else "off"))

    def counts(self) -> dict:
        with self.lock:
            return {"connections": self.connections, "loops": self.loops_done, "bursts": self.bursts_sent,
                    "bytes_sent": self.bytes_sent, "frames_sent": self.frames_sent,
                    "bytes_received": self.bytes_received, "bursts_paused": self.bursts_paused,
                    "bursts_withheld": self.bursts_withheld, "replies_sent": self.replies_sent,
                    "bursts_late": self.bursts_late, "late_paused": self.late_paused}

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
                if self._wait_until(t0 + t, gone, conn):
                    break
                if self.paused.is_set():
                    with self.lock:
                        self.bursts_paused += 1
                    continue
                resp = self.responder
                if resp is not None and not resp.rtcm_on():
                    with self.lock:
                        self.bursts_withheld += 1
                    continue
                data, frames = burst.data, list(burst.frames)
                if resp is not None:
                    data, frames = self._with_station(resp, burst)
                nframes = len(frames)
                with self.lock:
                    late = self.last_frame_delay_s
                if late > 0 and nframes > 1 and b"".join(frames) == data:
                    self._head_sent()
                    if not self._send(conn, b"".join(frames[:-1]), gone):
                        break
                    if self._wait_until(time.monotonic() + late, gone, conn):
                        break
                    if self.paused.is_set():
                        head = len(data) - len(frames[-1])
                        with self.lock:   # it fell due while paused: dropped, like a burst
                            self.late_paused += 1
                            self.bytes_sent += head
                            self.frames_sent += nframes - 1
                        loop_counts[1] += head
                        loop_counts[2] += nframes - 1
                        continue
                    if not self._send(conn, frames[-1], gone):
                        break
                    with self.lock:
                        self.bursts_late += 1
                else:
                    self._head_sent()
                    if not self._send(conn, data, gone):
                        break
                with self.lock:
                    self.bursts_sent += 1
                    self.bytes_sent += len(data)
                    self.frames_sent += nframes
                loop_counts[0] += 1
                loop_counts[1] += len(data)
                loop_counts[2] += nframes
            else:
                self._loop_done(n, loop_no, loop_counts, time.monotonic() - loop_start)
                self._log(f"connection {n}: schedule done; still reading what the client sends")
                while not gone.wait(0.05) and not self.stop_event.is_set():
                    self._flush_replies(conn)
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

    def _wait_until(self, deadline: float, gone: threading.Event, conn: socket.socket | None = None) -> bool:
        """Sleep until `deadline` (monotonic), sending the responder's replies as they fall due;
        True if the client left or the replay stops first."""
        while True:
            if gone.is_set() or self.stop_event.is_set():
                return True
            if conn is not None:
                self._flush_replies(conn)
            left = deadline - time.monotonic()
            if left <= 0:
                return False
            time.sleep(min(left, 0.02 if self.responder is not None else 0.05))

    def _flush_replies(self, conn: socket.socket) -> None:
        resp = self.responder
        if resp is None:
            return
        data = resp.due()
        if not data:
            return
        if self.paused.is_set():
            self._log(f"paused: {len(data)} reply bytes dropped", echo=False)   # a quiet wire, as for the RTCM
            return
        with self.send_lock:
            conn.sendall(data)
        with self.lock:
            self.replies_sent += len(data)
        self._log(f"to client {len(data)} reply bytes: {data!r}", echo=False)

    @staticmethod
    def _with_station(resp: Um982Responder, burst: Burst) -> tuple[bytes, list[bytes]]:
        """The burst as the responder's receiver sends it, and its frames: its own station messages first
        (a burst of whole frames only; a capture's reads go out as recorded)."""
        station = resp.station_frames()
        types = resp.station_types()
        if not types or b"".join(burst.frames) != burst.data:
            return burst.data, list(burst.frames)
        frames = with_station_messages(burst.frames, station, types)
        return b"".join(frames), frames

    def _send(self, conn: socket.socket, data: bytes, gone: threading.Event) -> bool:
        if not self.baud:
            with self.send_lock:
                conn.sendall(data)
            return True
        # Line rate: 10 bits per byte (start, 8 data, stop), in small pieces.
        start = time.monotonic()
        sent = 0
        while sent < len(data):
            piece = data[sent : sent + LINE_PIECE]
            with self.send_lock:
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
                resp = self.responder
            self._log(f"from client {len(data)} bytes: {data!r}", echo=False)
            if resp is not None:
                resp.feed(data)
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
    ap.add_argument("--respond", action="store_true",
                    help="answer the station's receiver commands as a scripted UM982 (benchlib.um982_sim)")
    ap.add_argument("--respond-group", default="4,5", metavar="M,S",
                    help="the responder's signal group at the start (default 4,5, the factory one)")
    ap.add_argument("--respond-reject", action="append", default=[], metavar="M,S",
                    help="a signal group the responder refuses (repeatable)")
    ap.add_argument("--respond-restart", type=float, default=None, metavar="S",
                    help="seconds the responder takes to restart after a signal-group change or RESET (default 3; "
                         "UM982 build 25164 takes 8.0-9.2 s to its banner)")
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
    responder = None
    if args.respond:
        restart_s = 3.0 if args.respond_restart is None else args.respond_restart
        if not 0 < restart_s <= 60:
            ap.error("--respond-restart must be more than 0 and at most 60 s")
        try:
            responder = Um982Responder(group=parse_group(args.respond_group),
                                       reject=[parse_group(g) for g in args.respond_reject], restart_s=restart_s)
        except ValueError as e:
            ap.error(f"--respond-group/--respond-reject: {e}")
    elif args.respond_reject or args.respond_group != "4,5" or args.respond_restart is not None:
        ap.error("--respond-group, --respond-reject and --respond-restart need --respond")
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
                              run_dir=run_dir, say=lambda m: print(m, flush=True), responder=responder)
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
        if responder is not None:
            text += (f"; responder: {c['replies_sent']} reply bytes, {c['bursts_withheld']} bursts withheld, "
                     f"{responder.restarts} restart(s), signal group now {responder.signal_group()}")
        (run_dir / "summary.txt").write_text(text + "\n", encoding="utf-8")
        print(text)
        print(f"log: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
