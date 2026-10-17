# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Turn a recorded receiver stream into a replay schedule (pure, stdlib only).

Two inputs:

  capture   a capture.py run directory (every receiver-NNNN.bin part in
            order, each timed by the receiver-NNNN.idx beside it) or one
            receiver-NNNN.bin with its .idx. Every .idx line is
            "<unix time>,<stream offset>,<bytes>": one read on the host. Each
            read becomes one burst at its time relative to the first read,
            so the replay reproduces the gaps and the read sizes.
  rtcm      a plain RTCM 3 file (.rtcm3, or a .bin without an .idx). It is
            split into frames (benchlib.stream) and grouped into epochs: a
            new epoch starts at the first frame after an MSM with the
            multiple-message bit clear, or at an MSM whose epoch time differs
            from the one its constellation already has in the current epoch
            (an epoch whose closing MSM never came). Epoch times are compared
            per constellation, on the constellation's own time scale, so an
            encoder that puts GPS time into its BeiDou MSMs groups correctly
            too. One epoch = one burst: every byte of the file from its first
            frame up to the next epoch's (text or junk in between stays with
            it, so the bursts put together are the file, byte for byte). The
            burst times come from the MSM epoch times: each epoch is as far
            after the previous one as a constellation they share advanced.

A schedule loops on a continuous time base: loop k starts k x duration
after loop 0, where duration is the last burst's time plus one typical gap
(the median gap between bursts), so the replayed clock never steps back
and the gap at the seam is a normal one.
"""

from __future__ import annotations

import statistics
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterator

from . import rtcm
from .stream import StreamSplitter

GPS_WEEK_S = 604800.0
BDS_TO_GPS_S = 14.0
GLO_TO_GPS_S = -3 * 3600 + 18.0  # Moscow time -> GPS time (GPS-UTC 18 s)


@dataclass
class Burst:
    """Bytes sent at once, `t` seconds after the first burst of a loop."""

    t: float
    data: bytes
    frames: list[bytes] = field(default_factory=list)  # CRC-valid RTCM frames in it, in order
    epoch_s: dict[str, float] = field(default_factory=dict)  # rtcm input: constellation -> MSM epoch time (own scale)


@dataclass
class Schedule:
    kind: str  # "capture" or "rtcm"
    source: str
    bursts: list[Burst]
    period: float  # gap between the last burst of a loop and the first of the next

    @property
    def duration(self) -> float:
        """Length of one loop on the replay clock (unscaled)."""
        return (self.bursts[-1].t if self.bursts else 0.0) + self.period

    @property
    def size(self) -> int:
        return sum(len(b.data) for b in self.bursts)

    @property
    def frames(self) -> int:
        return sum(len(b.frames) for b in self.bursts)

    def data(self) -> bytes:
        """One loop's bytes, exactly as sent."""
        return b"".join(b.data for b in self.bursts)

    def iter_times(self, loop: bool, speed: float = 1.0) -> Iterator[tuple[int, int, float, Burst]]:
        """(loop number, burst index, seconds after the start, burst) in replay order.

        `speed` 2 halves every gap. Endless with loop=True."""
        if speed <= 0:
            raise ValueError("speed must be > 0")
        if not self.bursts:
            return
        k = 0
        while True:
            base = k * self.duration
            for i, b in enumerate(self.bursts):
                yield k, i, (base + b.t) / speed, b
            if not loop:
                return
            k += 1


def _typical_gap(times: list[float], fallback: float = 1.0) -> float:
    gaps = [b - a for a, b in zip(times, times[1:]) if b > a]
    return statistics.median(gaps) if gaps else fallback


# ---------------------------------------------------------------------------
# capture.py run directories


def read_idx(path: Path) -> list[tuple[float, int, int]]:
    """[(unix time, stream offset, bytes)] of a capture .idx file."""
    out = []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            t, off, size = line.split(",")
            out.append((float(t), int(off), int(size)))
        except ValueError as e:
            raise ValueError(f"{path.name} line {n}: not '<unix time>,<stream offset>,<bytes>': {line!r}") from e
    return out


def capture_parts(path: Path) -> list[tuple[Path, Path]]:
    """[(bin, idx)] of a run directory (receiver-*.bin in name order) or of one .bin file."""
    path = Path(path)
    if path.is_dir():
        bins = sorted(path.glob("receiver-*.bin"))
        if not bins:
            raise ValueError(f"{path}: no receiver-*.bin in this directory")
    elif path.is_file():
        bins = [path]
    else:
        raise ValueError(f"{path}: no such file or directory")
    parts = []
    for b in bins:
        idx = b.with_suffix(".idx")
        if not idx.is_file():
            raise ValueError(f"{b}: no {idx.name} beside it (a capture part needs its read times)")
        parts.append((b, idx))
    return parts


def load_capture(path: str | Path) -> Schedule:
    """One burst per recorded read, timed by the .idx files (parts in order)."""
    reads: list[tuple[float, bytes]] = []
    expect = None  # stream offset the next part must start at
    for bin_path, idx_path in capture_parts(Path(path)):
        data = bin_path.read_bytes()
        entries = read_idx(idx_path)
        if sum(n for _, _, n in entries) != len(data):
            raise ValueError(f"{idx_path.name} accounts for {sum(n for _, _, n in entries)} bytes, "
                             f"{bin_path.name} has {len(data)}")
        if entries and expect is not None and entries[0][1] != expect:
            raise ValueError(f"{idx_path.name} starts at stream offset {entries[0][1]}, expected {expect} "
                             f"(a part is missing or out of order)")
        pos = 0
        for t, off, n in entries:
            if expect is not None and off != expect:
                raise ValueError(f"{idx_path.name}: offset {off} where {expect} was expected")
            reads.append((t, data[pos : pos + n]))
            pos += n
            expect = off + n
    if not reads:
        raise ValueError(f"{path}: the capture holds no bytes")
    t0 = reads[0][0]
    times = [t - t0 for t, _ in reads]
    for a, b in zip(times, times[1:]):
        if b < a:
            raise ValueError("the .idx times go backwards")
    bursts = []
    split = StreamSplitter()
    for (t, chunk), rel in zip(reads, times):
        frames = [it.data for it in split.feed(chunk) if it.kind == "rtcm"]
        bursts.append(Burst(rel, chunk, frames))
    tail = [it.data for it in split.flush() if it.kind == "rtcm"]
    if tail:
        bursts[-1].frames.extend(tail)
    return Schedule("capture", str(path), bursts, _typical_gap(times))


# ---------------------------------------------------------------------------
# plain RTCM files


def gps_seconds(m: rtcm.Msm) -> float | None:
    """The MSM epoch time on the GPS time-of-week scale (None: GLONASS without a day)."""
    sec, period = m.epoch_seconds()
    if m.system == "BeiDou":
        return (sec + BDS_TO_GPS_S) % GPS_WEEK_S
    if m.system == "GLONASS":
        return (sec + GLO_TO_GPS_S) % GPS_WEEK_S if period == GPS_WEEK_S else None
    return sec % GPS_WEEK_S


def _wrapped(delta: float, period: float) -> float:
    return delta % period


def msm_of(frame: bytes) -> rtcm.Msm | None:
    """The decoded MSM header of a frame, or None (not an MSM, SBAS numbers, malformed)."""
    payload = rtcm.payload_of(frame)
    mtype = rtcm.msg_type(payload)
    if mtype is None or not rtcm.msm_system(mtype) or rtcm.is_suspect_sbas(mtype):
        return None
    try:
        return rtcm.decode_msm(payload)
    except ValueError:
        return None


class EpochGrouper:
    """Groups RTCM frames into epochs (the rule in the module docstring).

    feed(frame) returns True when the frame starts a new epoch."""

    def __init__(self) -> None:
        self.closed = False  # the previous frame was an MSM with MMB 0
        self.started = False
        self.current: dict[str, tuple[float, float, rtcm.Msm]] = {}  # system -> (seconds, period, first msm)

    def feed(self, frame: bytes) -> tuple[bool, rtcm.Msm | None]:
        m = msm_of(frame)
        new = not self.started or self.closed
        if m is not None and not new and m.system in self.current:
            sec, _period = m.epoch_seconds()
            if abs(sec - self.current[m.system][0]) > 0.0005:
                new = True
        if new:
            self.current = {}
            self.started = True
        if m is not None:
            if m.system not in self.current:
                sec, period = m.epoch_seconds()
                self.current[m.system] = (sec, period, m)
            self.closed = not m.mmb
        else:
            self.closed = False
        return new, m


def load_rtcm(path: str | Path) -> Schedule:
    """One burst per epoch, timed by the MSM epoch times."""
    path = Path(path)
    data = path.read_bytes()
    if not data:
        raise ValueError(f"{path}: empty file")
    split = StreamSplitter()
    items = split.feed(data) + split.flush()
    grouper = EpochGrouper()
    starts: list[int] = []  # stream offset where each epoch starts
    frames: list[list[bytes]] = []
    epochs: list[dict[str, tuple[float, float]]] = []
    gps_t: list[float | None] = []
    for it in items:
        if it.kind != "rtcm":
            continue
        new, m = grouper.feed(it.data)
        if new:
            starts.append(it.offset)
            frames.append([])
            epochs.append({})
            gps_t.append(None)
        frames[-1].append(it.data)
        if m is not None and m.system not in epochs[-1]:
            epochs[-1][m.system] = m.epoch_seconds()
            if gps_t[-1] is None:
                gps_t[-1] = gps_seconds(m)
    if not starts:
        raise ValueError(f"{path}: no RTCM 3 frame in this file")
    starts[0] = 0  # bytes in front of the first frame go with the first epoch
    steps = [_epoch_step(epochs[i - 1], epochs[i], gps_t[i - 1], gps_t[i]) for i in range(1, len(starts))]
    good = [s for s in steps if s is not None and 0 < s <= MAX_STEP_S]
    typical = statistics.median(good) if good else 1.0
    times = [0.0]
    for s in steps:
        # A step back (wrapped to almost a period) or a jump over an hour is a
        # discontinuity (files put together): one typical gap instead.
        times.append(times[-1] + (typical if s is None or s > MAX_STEP_S else s))
    bursts = []
    for i, start in enumerate(starts):
        end = starts[i + 1] if i + 1 < len(starts) else len(data)
        bursts.append(Burst(times[i], data[start:end], frames[i],
                            {s: sec for s, (sec, _p) in epochs[i].items()}))
    return Schedule("rtcm", str(path), bursts, typical)


MAX_STEP_S = 3600.0


def _epoch_step(prev: dict[str, tuple[float, float]], cur: dict[str, tuple[float, float]],
                prev_gps: float | None, cur_gps: float | None) -> float | None:
    """Seconds from one epoch to the next: by a constellation both have (its own
    time scale, wrapped at its period), else by GPS time. 0 for an epoch
    without MSMs (only the last one can be: it is sent right after the
    previous one); None when nothing relates the two."""
    if not cur:
        return 0.0
    for system in ("GPS", "Galileo", "BeiDou", "GLONASS", "QZSS", "NavIC"):
        if system in prev and system in cur:
            (a, period), b = prev[system], cur[system][0]
            return _wrapped(b - a, period)
    if prev_gps is not None and cur_gps is not None:
        return _wrapped(cur_gps - prev_gps, GPS_WEEK_S)
    return None


NO_RTCM = "none"   # load()'s name for a receiver that sends no RTCM at all


def no_rtcm() -> Schedule:
    """No RTCM at all: one empty burst (the receiver without an antenna; its answers come from a responder)."""
    return Schedule("capture", "no RTCM", [Burst(0.0, b"", [])], 1.0)


def load(path: str | Path) -> Schedule:
    """A capture (directory, or a .bin with its .idx beside it), a plain RTCM file, or NO_RTCM ("none")."""
    if str(path) == NO_RTCM:
        return no_rtcm()
    p = Path(path)
    if p.is_dir():
        return load_capture(p)
    if not p.is_file():
        raise ValueError(f"{p}: no such file or directory")
    if p.suffix == ".bin" and p.with_suffix(".idx").is_file():
        return load_capture(p)
    return load_rtcm(p)
