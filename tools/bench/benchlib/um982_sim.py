# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""A scripted UM982 on COM1, for the replay in QEMU (test tooling).

Um982Responder answers the commands a station's receiver session sends,
with the reply formats of benchlib.unicore (the public reply samples, the
host-test vectors and what UM982 build 25164 answered on the bench), so the
firmware in QEMU identifies the replayed "receiver", configures it and
switches its signal group:

  UNLOG COM1                  OK; the RTCM output stops
  VERSIONA                    OK and a UM982 version log (firmware chosen;
                              build 25164 by default)
  CONFIG                      OK and the dump: SIGNALGROUP, COM1, MMP,
                              RTCMB1CB2A, MASK, BASEANTENNAMODEL
  MODE                        OK and the base-mode log (what MODE BASE set)
  MODE BASE ..., RTCMxxxx COM1 n, CONFIG COM1 n, CONFIG RTCMB1CB2A ...,
  CONFIG BASEANTENNAMODEL ..., GPGSV COM1 n, UNLOG COM1 GPGSV
                              OK (RTCMxxxx turns the RTCM output on)
  SAVECONFIG                  OK; what runs is saved
  RESET                       OK, then a restart
  CONFIG SIGNALGROUP m [s]    OK for 4 5, 3 6, 5 0 and 7 0 (minus `reject`;
                              a missing s is 0, so "5" is 5 0), saved by
                              itself, then a restart when it changed; any
                              other group gets `PARSING FAILED GRAMMAR
                              ERROR,` and no restart, as on build 25164
  ANT1GROUPA, ANT2GROUPA      OK and the signal-group log: the body build
                              25164 gave for the running group (main and
                              second antenna input) in a made-up header
  anything else               `PARSING FAILED NO MATCHING FUNC  <word>`

A restart withholds the RTCM output for `restart_s`, answers every command
meanwhile with `The Board is resetting,do not response any command!*39`
(build 25164 answered a command 0.4 s into a restart so; later in a restart
it was not tried), then prints the restart banner and comes back with what
was saved (signal group, base mode, outputs). Build 25164 takes 8.0-9.2 s
to its banner; the default here is shorter, to keep the tests quick.
Nothing here touches a real receiver: the station's commands arrive over the
replay's local TCP connection.

The replayed RTCM is a recording of another receiver; the station messages
are this receiver's own, as configured: `RTCM1005|1006|1033|1230 COM1 n`
(n in seconds, one epoch per second) puts them at the start of every n-th
epoch (station_frames(); with_station_messages() swaps them for the
recording's own of the same types): 1005/1006 at the position `MODE BASE
<id> <X> <Y> <Z>` set (a fixed base only; none while surveying or as a
rover), 1033 with the BASEANTENNAMODEL descriptor and this receiver's type,
firmware and serial number, 1230 with no biases. All of them carry the
station ID of MODE BASE (0 before one).
"""

from __future__ import annotations

import math
import shlex
import threading
import time

from .rtcm import BitWriter, build_frame, llh_to_ecef, msg_type, payload_of
from .unicore import (
    GROUP_BODIES, GROUP_REFUSAL, ack_line, banner_line, config_line, groups_line, mode_line, refusal_text,
    restarting_line, version_line,
)

ACCEPTED_GROUPS = ((4, 5), (3, 6), (5, 0), (7, 0))
ACK_DELAY_S = 0.03
DATA_DELAY_S = 0.06
STATION_TYPES = (1005, 1006, 1033, 1230)
RECEIVER_SERIAL = "SYNTH00000001"   # the serial number in the version log (unicore.version_line)


# ---------------------------------------------------------------------------
# The receiver's own station messages (RTCM 3 test frames)


def _counted(w: BitWriter, text: str) -> None:
    raw = text.encode("latin-1")[:31]
    w.u(len(raw), 8)
    for b in raw:
        w.u(b, 8)


def arp_frame(mtype: int, station_id: int, xyz: tuple[float, float, float], height_m: float = 0.0) -> bytes:
    """A 1005 (or 1006 with the antenna height) for the ECEF position `xyz` in metres."""
    w = BitWriter().u(mtype, 12).u(station_id, 12).u(0, 6).u(1, 1).u(1, 1).u(1, 1).u(0, 1)
    w.s(round(xyz[0] * 1e4), 38).u(0, 1).u(0, 1).s(round(xyz[1] * 1e4), 38).u(0, 2).s(round(xyz[2] * 1e4), 38)
    if mtype == 1006:
        w.u(round(height_m * 1e4), 16)
    return build_frame(w.to_bytes())


def descriptor_frame(station_id: int, antenna: str, setup_id: int, antenna_serial: str, receiver_type: str,
                     firmware: str, receiver_serial: str) -> bytes:
    """A 1033: antenna descriptor, setup ID and serial, receiver type, firmware and serial."""
    w = BitWriter().u(1033, 12).u(station_id, 12)
    _counted(w, antenna)
    w.u(setup_id & 0xFF, 8)
    _counted(w, antenna_serial)
    _counted(w, receiver_type)
    _counted(w, firmware)
    _counted(w, receiver_serial)
    return build_frame(w.to_bytes())


def glo_bias_frame(station_id: int) -> bytes:
    """A 1230 with the code-phase bias indicator clear and no bias."""
    return build_frame(BitWriter().u(1230, 12).u(station_id, 12).u(0, 1).u(0, 3).u(0, 4).to_bytes())


def with_station_messages(frames: list[bytes], station: list[bytes], types) -> list[bytes]:
    """One epoch as the receiver sends it: its own station messages first,
    the recording's frames of those types left out."""
    return list(station) + [f for f in frames if msg_type(payload_of(f)) not in types]


class Um982Responder:
    """The receiver's side of the command channel. feed() takes what the
    station sent; due() hands back the reply bytes whose time has come;
    rtcm_on() says whether the receiver's RTCM output runs now. Thread-safe
    (the replay feeds from its reader thread and sends from its session)."""

    def __init__(self, group: tuple[int, int] = (4, 5), firmware: str = "R4.10Build25164",
                 reject: tuple[tuple[int, int], ...] | list = (), restart_s: float = 3.0, model: str = "UM982",
                 baud: int = 115200, clock=time.monotonic):
        self.clock = clock
        self.model = model
        self.firmware = firmware
        self.reject = {tuple(int(x) for x in g) for g in reject}
        self.restart_s = float(restart_s)
        self.baud = baud
        self.lock = threading.Lock()
        self.group = tuple(group)
        self.mode = "MODE ROVER"
        self.b1c = "ENABLE"
        self.antenna = '"" "" 0'
        self.output = True
        self.station_out: dict[int, int] = {}   # station message -> period in epochs
        self.epochs = 0
        self.saved = self._state()
        self.restart_until = 0.0
        self.banner_pending = False
        self.buf = bytearray()
        self.outbox: list[tuple[float, bytes]] = []
        self.log: list[tuple[float, str, str]] = []   # (time, command, answer summary)
        self.restarts = 0
        self.saves = 0

    # -- state --------------------------------------------------------------------

    def _state(self) -> dict:
        return {"mode": self.mode, "b1c": self.b1c, "antenna": self.antenna, "output": self.output,
                "station_out": dict(self.station_out)}

    def _restore(self) -> None:
        self.mode = self.saved["mode"]
        self.b1c = self.saved["b1c"]
        self.antenna = self.saved["antenna"]
        self.output = self.saved["output"]
        self.station_out = dict(self.saved["station_out"])

    def _restart(self, now: float) -> None:
        """Quiet for restart_s, then the banner and the saved configuration."""
        self.restarts += 1
        self.restart_until = now + self.restart_s
        self.banner_pending = True
        self.outbox = [x for x in self.outbox if x[0] <= now]   # what was not sent yet is lost
        self._restore()

    def _tick(self, now: float) -> None:
        if self.banner_pending and now >= self.restart_until:
            self.banner_pending = False
            self.outbox.append((now, (banner_line("COM1") + "\r\n").encode()))

    def restarting(self, now: float | None = None) -> bool:
        with self.lock:
            return (self.clock() if now is None else now) < self.restart_until

    def rtcm_on(self, now: float | None = None) -> bool:
        """The receiver sends RTCM now (output on, not restarting)."""
        with self.lock:
            now = self.clock() if now is None else now
            self._tick(now)
            return self.output and now >= self.restart_until

    def signal_group(self) -> tuple[int, int]:
        with self.lock:
            return self.group

    # -- the station messages ----------------------------------------------------------

    def _station_id(self) -> int:
        words = self.mode.split()
        if len(words) >= 3 and words[:2] == ["MODE", "BASE"] and words[2].isdigit():
            return int(words[2]) & 0xFFF
        return 0

    def _base_xyz(self) -> tuple[float, float, float] | None:
        """The fixed base position (ECEF metres) of `MODE BASE <id> <a> <b> <c>`, or None."""
        words = self.mode.split()
        if len(words) != 6 or words[:2] != ["MODE", "BASE"]:
            return None
        try:
            a, b, c = (float(x) for x in words[3:6])
        except ValueError:
            return None   # TIME ...: a survey
        if math.sqrt(a * a + b * b + c * c) > 6.0e6:
            return a, b, c
        return llh_to_ecef(a, b, c)   # latitude, longitude, height

    def _station_message(self, mtype: int) -> bytes | None:
        sid = self._station_id()
        if mtype in (1005, 1006):
            xyz = self._base_xyz()
            return arp_frame(mtype, sid, xyz) if xyz is not None else None
        if mtype == 1033:
            try:
                parts = shlex.split(self.antenna)
            except ValueError:
                parts = []
            model = parts[0] if parts else ""
            serial = parts[1] if len(parts) > 1 else ""
            setup = int(parts[2]) if len(parts) > 2 and parts[2].isdigit() else 0
            return descriptor_frame(sid, model, setup, serial, self.model, self.firmware, RECEIVER_SERIAL)
        if mtype == 1230:
            return glo_bias_frame(sid)
        return None

    def station_types(self) -> set[int]:
        """The station message types this receiver sends (configured and producible)."""
        with self.lock:
            return {t for t in self.station_out if self._station_message(t) is not None}

    def station_messages(self) -> list[bytes]:
        """Every station message as configured now, in type order (for checks)."""
        with self.lock:
            out = [self._station_message(t) for t in sorted(self.station_out)]
            return [f for f in out if f is not None]

    def station_frames(self, now: float | None = None) -> list[bytes]:
        """The station messages that go out with the epoch about to be sent (one call per epoch)."""
        with self.lock:
            now = self.clock() if now is None else now
            self._tick(now)
            if not self.output or now < self.restart_until:
                return []
            k = self.epochs
            self.epochs += 1
            out = [self._station_message(t) for t in sorted(self.station_out) if k % self.station_out[t] == 0]
            return [f for f in out if f is not None]

    # -- the command channel -----------------------------------------------------------

    def feed(self, data: bytes, now: float | None = None) -> None:
        """Bytes the station sent. Whole lines (CR LF or LF) are answered."""
        with self.lock:
            now = self.clock() if now is None else now
            self._tick(now)
            self.buf += data
            while True:
                i = self.buf.find(b"\n")
                if i < 0:
                    if len(self.buf) > 4096:
                        del self.buf[:-256]   # no line end in sight: keep the tail only
                    return
                raw = bytes(self.buf[:i])
                del self.buf[: i + 1]
                cmd = raw.decode("latin-1", "replace").strip("\r \t")
                if cmd:
                    self._command(cmd, now)

    def due(self, now: float | None = None) -> bytes:
        """The reply bytes due by now, in order."""
        with self.lock:
            now = self.clock() if now is None else now
            self._tick(now)
            ready = [x for x in self.outbox if x[0] <= now]
            self.outbox = [x for x in self.outbox if x[0] > now]
            return b"".join(b for _t, b in sorted(ready, key=lambda x: x[0]))

    def _say(self, now: float, delay: float, *lines: str) -> None:
        for k, line in enumerate(lines):
            self.outbox.append((now + delay + k * 0.002, (line + "\r\n").encode("latin-1")))

    def _ack(self, cmd: str, now: float, reply: str = "OK") -> None:
        self._say(now, ACK_DELAY_S, ack_line(cmd, reply))

    def _command(self, cmd: str, now: float) -> None:
        if now < self.restart_until:
            # Due before the banner at the latest, so it never follows it.
            self.outbox.append((min(now + ACK_DELAY_S, self.restart_until), (restarting_line() + "\r\n").encode()))
            self.log.append((now, cmd, "restarting"))
            return
        up = " ".join(cmd.upper().split())
        words = up.split()
        answer = "OK"
        if up == "VERSIONA":
            self._ack(cmd, now)
            self._say(now, DATA_DELAY_S, version_line(self.model, self.firmware))
        elif up == "UNLOG COM1":
            self.output = False
            self.station_out.clear()
            self._ack(cmd, now)
        elif up == "UNLOG COM1 GPGSV" or (len(words) == 3 and words[0] == "GPGSV" and words[1] == "COM1"):
            self._ack(cmd, now)
        elif up == "CONFIG":
            self._ack(cmd, now)
            m, s = self.group
            self._say(now, DATA_DELAY_S,
                      config_line("SIGNALGROUP", f"CONFIG SIGNALGROUP {m} {s}"),
                      config_line("COM1", f"CONFIG COM1 {self.baud}"),
                      config_line("MMP", "CONFIG MMP ENABLE"),
                      config_line("RTCMB1CB2A", f"CONFIG RTCMB1CB2A {self.b1c}"),
                      config_line("MASK", "CONFIG MASK 5.0"),
                      config_line("BASEANTENNAMODEL", f"CONFIG BASEANTENNAMODEL {self.antenna}"))
        elif up == "MODE":
            self._ack(cmd, now)
            self._say(now, DATA_DELAY_S, mode_line(self.mode))
        elif words[:2] == ["CONFIG", "SIGNALGROUP"] and len(words) >= 3:
            args = words[2:] + ["0"]   # a missing slave group is 0
            g = (int(args[0]), int(args[1])) if len(words) <= 4 and all(a.isdigit() for a in args[:2]) else None
            if g is None or g not in ACCEPTED_GROUPS or g in self.reject:
                answer = GROUP_REFUSAL
                self._ack(cmd, now, answer)
            else:
                self._ack(cmd, now)
                if g != self.group:
                    self.group = g   # saved by the receiver itself
                    self._restart(now + ACK_DELAY_S)
                    answer = "OK, restarting"
        elif up in ("ANT1GROUPA", "ANT2GROUPA"):
            ant = 1 if up == "ANT1GROUPA" else 2
            self._ack(cmd, now)
            bodies = GROUP_BODIES.get(self.group)
            if bodies is not None:
                self._say(now, DATA_DELAY_S, groups_line(bodies[ant - 1], ant))
        elif words[:2] == ["MODE", "BASE"]:
            self.mode = up
            self._ack(cmd, now)
        elif len(words) == 3 and words[0].startswith("RTCM") and words[0][4:].isdigit() and words[1] == "COM1":
            mtype = int(words[0][4:])
            try:
                period = float(words[2])
            except ValueError:
                period = 1.0
            if mtype in STATION_TYPES:
                if period > 0:
                    self.station_out[mtype] = max(1, round(period))
                else:
                    self.station_out.pop(mtype, None)
            self.output = True
            self._ack(cmd, now)
        elif len(words) == 3 and words[:2] == ["CONFIG", "COM1"] and words[2].isdigit():
            self.baud = int(words[2])
            self._ack(cmd, now)
        elif words[:2] == ["CONFIG", "RTCMB1CB2A"] and len(words) == 3:
            self.b1c = words[2]
            self._ack(cmd, now)
        elif words[:2] == ["CONFIG", "BASEANTENNAMODEL"]:
            rest = cmd.split(None, 2)[2] if len(cmd.split(None, 2)) == 3 else ""
            self.antenna = rest[:-5] if rest.upper().endswith(" USER") else rest
            self._ack(cmd, now)
        elif up == "SAVECONFIG":
            self.saved = self._state()
            self.saves += 1
            self._ack(cmd, now)
        elif up == "RESET":
            self._ack(cmd, now)
            self._restart(now + ACK_DELAY_S)
            answer = "OK, restarting"
        else:
            answer = refusal_text(cmd)
            self._ack(cmd, now, answer)
        self.log.append((now, cmd, answer))


def parse_group(text: str) -> tuple[int, int]:
    """"M,S" or "M S" -> (M, S), each 0-255."""
    parts = text.replace(",", " ").split()
    if len(parts) != 2 or not all(p.isdigit() for p in parts):
        raise ValueError(f"{text!r} is not a signal group like 3,6")
    g = (int(parts[0]), int(parts[1]))
    if not all(0 <= x <= 255 for x in g):
        raise ValueError(f"{text!r}: signal group numbers are 0-255")
    return g
