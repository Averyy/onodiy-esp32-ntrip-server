#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""QEMU end to end: recorded RTCM through the firmware to a caster on this machine.

    uv run tools/qemu/e2e.py [--out DIR] [--scenario stream|onocoy-guard|signal-test|all] [--long]
                             [--caster fake|docker] [--fixture PATH] [--observe S]

Needs the ESP-IDF environment and a QEMU build (tools/qemu/run.sh
--build-only), like smoke.py. Standard library only: the bench tools it
uses (tools/bench rtcm_replay.py, fake_caster.py and benchlib) are imported
by path and need no third-party package. Everything stays on this machine:
the replay and the casters listen on 127.0.0.1, which the emulated station
reaches as 10.0.2.2.

What it does:
  1. replays the fixture (default: test/fixtures/rtcm/centipede_LAX_30ep.rtcm3,
     looped at its own pace) into UART1 from the first boot
     (run.sh --uart1 connect:PORT): the station hears a streaming receiver;
  2. starts a local fake caster (or, with --caster docker, the BKG caster
     image from tools/bench/docker/bkg-caster plus a GET client that
     records what it relays), boots the image headless, sets the admin
     password and, as soon as the station streams, configures slot 1:
     {name "Bench", host 10.0.2.2, the caster's port, NTRIP 1.0, mountpoint
     BENCH, a made-up password, standard profile, enabled};
  scenario "stream":
  3. slot 1 streaming within 60 s ("ICY 200 OK"; "OK" from the BKG caster)
     and never `paused` (the replay answers no query, and the station never
     takes the receiver line for its identity query while RTCM flows: no
     "COMMAND lease taken by identify" on the console);
     GET /api/gnss/signals lists the fixture's constellations with their
     bands (computed from the fixture with benchlib's band table, which
     equals the firmware's; SBAS never listed); /api/health hub.frames_in
     grows by at least 5 x the fixture's frames per epoch in 10 s and the
     slot's epochs_sent rises;
  4. the caster's record holds at least 60 s of frames (--observe): every
     one CRC-valid and byte-identical to a fixture frame; after the sticky
     copies it starts at a fixture epoch's first frame, then follows the
     fixture in order (modulo the loop; fixture frames the station never
     forwards, such as the SBAS numbers 1101-1107, are skipped), and any gap
     falls on an epoch boundary (an epoch's last frame, then sticky copies
     and an epoch's first frame); nothing between frames, a partial frame
     only at the very end; receiver maintenance answers its start and
     extension with the time it set, and GET /api/gnss agrees; a restart
     during it (the replay paused across it) resumes it with its time left
     (never more, at most 5 s less) and the station only listens;
  scenario "signal-test" (the replay answers as a scripted UM982,
  benchlib.um982_sim; with --scenario all it runs last, under 10 min):
  5. a fixed profile (the stream's own position) through PUT: the station
     identifies the receiver, switches it from 4 5 to 3 6 and locks;
     a comparison is refused with automatic configuration off and in
     receiver maintenance; 7 0 refused by the receiver ends a comparison as
     rejected, back on 3 6 without config_failed; 3 6 against 7 0 with
     phase_s 60 and a restart in the middle of measuring (the replay paused
     across the reboot) resumes with its measured time, runs through the
     states in order within their time bounds, shows both results and keep 1
     leaves the profile on 7 0; 12 applies of the stored profile in a row,
     each after the slot streamed two more epochs and a random wait of up
     to 1 s (each a pause for receiver work, so the station decides to
     pause at any moment of the second), then 12 more with each epoch's
     last MSM sent 0.7 s after the rest of it (about 0.4 s after the MSM
     before it reaches the station: longer than the hub's 250 ms gap rule,
     so a pause decided in that time must still wait for it); still with
     the late last MSM, 6 saves of slot 1's settings (each a planned close
     and a reconnect; every other one inside an epoch) with at least one
     close that waited for its epoch's end and none that gave up, and a
     restart timed to come inside an epoch while the replay streams (paused
     once that session has ended) whose session finished its epoch first;
     every caster record of the scenario passes the strict epoch check
     above, and a session that ended (closed during a pause, for new
     settings, or by a restart) ends on a whole epoch, as does the part of
     the first session before the configuration's pause; one check per
     span of the scenario (since its start, since the restart in the
     comparison, since each save, since the restart inside an epoch) over
     its sessions' records, so the number of checks is the same in every
     run (the first session and the one after the last restart stream two
     epochs before they are judged);
  scenario "onocoy-guard" (firmware with test-Onocoy slots; SKIP otherwise;
  with --scenario all it runs before "stream"):
  5. slot 0 as a test-Onocoy slot (test_onocoy) against a second fake caster
     scripted with Onocoy's termination reasons: a credential in use on
     credentials that never worked stops the slot at once (and so do 409,
     "Mount Point Taken" and "Already Connected"; three 503 replies keep the
     fast transient row; a refused login whose line is closed before its
     line end counts as one); the guard's
     waits after rejections, login blocks and in-use replies (each delay in
     [entry, 1.2 x entry] + 2 s for QEMU, from next_retry_s and from the
     connection times); sessions the caster ends quickly (and a "Device
     reconnected" line, which scores 2 at once) stop the slot after a score
     of 3, waiting 5 s at a score of 1 and 10 s at 2; duplicate and banned
     reports stop it at once; a stop survives a reboot (the replay pauses
     across it, see tools/qemu/README.md) and POST /api/casters/0/retry
     clears it. While the guard waits after a rejected login, a login block
     or an in-use reply, /api/health network.ladder.triggers has no
     `protected` (a network action cannot fix a caster's reply); with the
     slot pointed at a closed port (connect refused) it has.
     --long adds the 5-minute wait after a rejected login and the whole
     in-use retry ladder;
  6. throughout: no panic, abort, watchdog or reboot loop on the console
     (smoke.py's checks).

Everything lands in --out: the console log, replay/ (what the firmware sent
on UART1), caster/ and guard/ (connections.jsonl, the records), the HTTP
answers, qemu-net.pcap and slow-requests.log (as smoke.py writes them) and
summary.txt. Exit status 0 when every check passed (a SKIP
passes), 1 otherwise.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import random
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BENCH = ROOT / "tools" / "bench"
sys.dont_write_bytecode = True  # no __pycache__ next to smoke.py and the bench tools
sys.path.insert(0, str(BENCH))
sys.path.insert(0, str(Path(__file__).resolve().parent))

# Imported by path (above): smoke.py for its console checks and HTTP helper,
# the bench tools for the replay and the casters.
import smoke
from benchlib import rtcm
from benchlib.replay import Burst, EpochGrouper, Schedule, load, msm_of
from benchlib.stream import StreamSplitter
from benchlib.um982_sim import Um982Responder, with_station_messages
from fake_caster import FakeCaster
from rtcm_replay import ReplayServer

DEFAULT_FIXTURE = ROOT / "test" / "fixtures" / "rtcm" / "centipede_LAX_30ep.rtcm3"
HOST_FROM_GUEST = "10.0.2.2"  # QEMU user networking: the host's 127.0.0.1
STREAM_MOUNT = "BENCH"
STREAM_PASSWORD = "bench-pass-1"
DOCKER_SOURCE_PASSWORD = "bench-src"  # tools/bench/docker/bkg-caster/ntripcaster.conf
DOCKER_IMAGE = os.environ.get("BKG_CASTER_IMAGE", "onocoydiy-bench-bkg-caster")
GUARD_MOUNT = "TESTMOUNT"
GUARD_USER = "testuser01"
GUARD_PASSWORD = "fakepass123"
MIN_RECORD_S = 60.0
QEMU_TOLERANCE_S = 2.0
IDENTIFY_LEASE_LINE = "COMMAND lease taken by identify"
SGT_PHASE_S = 60
SGT_PAUSES = 12              # applies in a row for the pause check, twice (signal-test scenario, step 5)
# The second time each epoch's last MSM is sent this long after the rest of it. The rest takes about 0.3 s to
# reach the station (the emulated UART paces it), so the last MSM comes about 0.4 s after the one before it:
# longer than the hub's 250 ms gap rule, well within the station's 1 s wait for an epoch in flight.
LATE_LAST_S = 0.7
SGT_EDITS = 6                # saves of slot 1's settings, each a planned close (signal-test scenario, step 5)
RESUMED_LINE = "ntrip: resumed"
# A planned close (new settings, a restart) that waited for the end of the epoch the session was sending, and
# one that gave up on it (ntrip_server.c drain_step).
DRAINED_LINE = "ending the session after the epoch it was sending"
# The restart inside an epoch (restart_in_epoch): the request goes out this long after a burst began, so the
# shutdown (1 s after the request, REBOOT_DELAY_MS) comes this long plus the request's own time into the next
# burst: past its station messages and the MSMs the emulated UART paces in about 0.3 s, before the late last
# MSM (LATE_LAST_S). Earlier (0.1 s) it could come after the station messages alone: no MSM run open yet, the
# session closed at once as between epochs, and the rest of the burst still arriving across the restart hung
# the next boot (tools/qemu/README.md, UART1 section).
RESTART_AFTER_HEAD_S = 0.25
# ... and the replay holds (no new burst; the one going out finishes, its late MSM too) from this long after
# that burst began: once the next burst, the one the shutdown comes in, is out, and before the one after it,
# so nothing arrives across the restart however late the shutdown comes.
RESTART_HOLD_AFTER_HEAD_S = 1.5
CUT_CLOSE_LINE = "closing inside an epoch"
STOP_REASONS = ["none", "rejected", "login_blocked", "in_use", "kicked", "duplicate", "banned"]


class SkipScenario(Exception):
    pass


# ---------------------------------------------------------------------------
# fixture expectations and record checks


def expected_signals(schedule: Schedule) -> dict[str, list[str]]:
    """What /api/gnss/signals lists for the fixture: every constellation with
    an MSM the station decodes, with the bands of the signals that carry
    cells, by benchlib's band table (the firmware's own). SBAS is never
    listed (its message numbers never reach the signal view)."""
    out: dict[str, set[str]] = {}
    for b in schedule.bursts:
        for f in b.frames:
            m = msm_of(f)
            if m is None or m.system == "SBAS":
                continue
            bands = out.setdefault(m.system.lower(), set())
            for c in m.cells:
                band = rtcm.band_of(m.system, c.sig)
                if band:
                    bands.add(band.lower())
    return {k: sorted(v) for k, v in sorted(out.items())}


# The station metadata the hub keeps as sticky copies and replays at the
# start of every caster session (components/rtcm_hub).
STICKY_TYPES = frozenset({1005, 1006, 1033, 1230})


def frame_type(f: bytes) -> int | None:
    return rtcm.msg_type(rtcm.payload_of(f))


def frames_per_epoch(schedule: Schedule) -> float:
    """Frames per epoch of one loop, epochs as benchlib.replay groups them
    (a capture's bursts are reads, not epochs)."""
    g = EpochGrouper()
    frames = [f for b in schedule.bursts for f in b.frames]
    epochs = sum(1 for f in frames if g.feed(f)[0])
    return len(frames) / epochs if epochs else 0.0


def with_station(schedule: Schedule, station: list[bytes], types) -> Schedule:
    """The fixture as the scripted receiver sends it: its own station messages
    open every epoch, the fixture's own of those types left out (as the
    replay sends it with a responder; bursts of whole frames only)."""
    bursts = []
    for b in schedule.bursts:
        frames = with_station_messages(b.frames, station, types) if b"".join(b.frames) == b.data else list(b.frames)
        bursts.append(Burst(b.t, b"".join(frames), frames, dict(b.epoch_s)))
    return Schedule(schedule.kind, schedule.source, bursts, schedule.period)


def carries_epoch(data: bytes) -> bool:
    """The record (or the part of one) holds a frame that is no sticky type:
    it carried an epoch, not only a session start's sticky copies."""
    split = StreamSplitter()
    return any(it.kind == "rtcm" and frame_type(it.data) not in STICKY_TYPES
               for it in split.feed(data) + split.flush())


def split_at_station(data: bytes, station: list[bytes]) -> tuple[bytes, bytes]:
    """A caster record cut where the configured receiver's own station
    messages first appear (a session that ran through the configuration):
    the part before it, sent as recorded, and the rest."""
    split = StreamSplitter()
    own = set(station)
    for it in split.feed(data) + split.flush():
        if it.kind == "rtcm" and it.data in own:
            return data[:it.offset], data[it.offset:]
    return data, b""


def check_record(data: bytes, schedule: Schedule, leading_junk_ok: bool = False,
                 whole_tail: bool = False) -> tuple[bool, str, float]:
    """The record against the fixture, strictly. Returns (ok, detail, seconds
    of the fixture's time the record covers).

    After the sticky copies (1005/1006, 1033, 1230: replayed at every session
    start and resume, up to 30 s old) the record starts at a fixture epoch's
    first frame, never inside an epoch. Then every frame is the fixture's
    next one (modulo the loop; fixture types the record never shows, the
    SBAS numbers the station drops, are skipped). Any other step is a gap
    (a caster pause: a receiver switch, a lease): it must fall on an epoch
    boundary, the frame before it an epoch's last and the frames after it
    sticky copies, then an epoch's first frame.

    Only the record's very first epoch, and only when no sticky copies come
    before it (the hub had none to replay), may lack its leading station
    messages (sticky types), never its MSMs. After a pause the epoch comes
    whole, from its first frame: frames carry their arrival time from the
    UART interrupt, so a reader stalled by flash writes no longer makes the
    hub's gap rule start an epoch late. A station message missing anywhere
    else is a lost frame.

    whole_tail: the record ended before the data did (a session the station
    closed while the casters were paused, or the part of a session before a
    pause): its last frame that is no sticky type must be an epoch's last,
    with no partial frame after it. Station messages after it are allowed
    (the next epoch's, sent before the pause: they are never part of an MSM
    run). Without it the record's last epoch may be cut, as a record still
    growing when it is read is."""
    split = StreamSplitter()
    items = split.feed(data) + split.flush()
    fx: list[bytes] = []
    burst_of: list[int] = []
    for i, b in enumerate(schedule.bursts):
        fx += b.frames
        burst_of += [i] * len(b.frames)
    if not fx:
        return False, "the fixture has no frame", 0.0
    pos: dict[bytes, list[int]] = {}
    for i, f in enumerate(fx):
        pos.setdefault(f, []).append(i)
    frames = [it for it in items if it.kind == "rtcm"]
    if not frames:
        return False, "no frame recorded", 0.0
    first_off = frames[0].offset
    # Bytes outside frames: none, except a partial frame at the very end (and,
    # for a relay that starts a client inside older data, bytes before the first frame).
    tail_start = frames[-1].offset + len(frames[-1].data)
    if whole_tail and tail_start < len(data):
        return False, (f"{len(data) - tail_start} bytes after the last whole frame of a record that ended at a pause "
                       f"(a frame cut at the end)"), 0.0
    for it in items:
        if it.kind == "rtcm":
            continue
        if it.offset >= tail_start and data[tail_start:tail_start + 1] == b"\xd3":
            continue  # the cut frame at the end
        if leading_junk_ok and it.offset < first_off:
            continue
        return False, (f"{len(it.data)} bytes outside frames at offset {it.offset} ({it.kind}"
                       f"{', CRC failure' if it.kind == 'rtcm_bad' else ''})"), 0.0
    unknown = [k for k, it in enumerate(frames) if it.data not in pos]
    if unknown:
        return False, f"{len(unknown)} recorded frames are no fixture frame (first: #{unknown[0]})", 0.0
    seen_types = {frame_type(it.data) for it in frames}
    sticky = [frame_type(it.data) in STICKY_TYPES for it in frames]

    def skippable(q: int) -> bool:
        return frame_type(fx[q]) not in seen_types

    def first_of_burst(q: int) -> bool:
        """q is its burst's first frame that is no sticky type (and that the record could show)."""
        k = q - 1
        while k >= 0 and burst_of[k] == burst_of[q]:
            if frame_type(fx[k]) not in STICKY_TYPES and not skippable(k):
                return False
            k -= 1
        return True

    def last_of_burst(q: int) -> bool:
        """q is its burst's last frame the record could show."""
        k = (q + 1) % len(fx)
        while burst_of[k] == burst_of[q] and k != q:
            if not skippable(k):
                return False
            k = (k + 1) % len(fx)
        return True

    def opener(k: int) -> tuple[int, int, bool] | None:
        """From record frame k on: sticky copies, then an epoch from its first frame (its own leading
        station messages among them: the hub sends an epoch from its first frame, after the copies).
        Returns (record index of the epoch's first non-sticky frame, its fixture index, whether some of
        its leading station messages are missing), or None."""
        k_run = k
        while k < len(frames) and sticky[k]:
            k += 1
        if k == len(frames):
            return None
        q = pos[frames[k].data][0]
        if len(pos[frames[k].data]) != 1 or not first_of_burst(q):
            return None
        run = {frames[i].data for i in range(k_run, k)}
        lead = q - 1
        lead_missing = False
        while lead >= 0 and burst_of[lead] == burst_of[q]:
            if not skippable(lead) and fx[lead] not in run:
                if frame_type(fx[lead]) not in STICKY_TYPES:
                    return None   # the epoch's own first frame is missing
                lead_missing = True
            lead -= 1
        return k, q, lead_missing

    start = opener(0)
    if start is None:
        k = next((k for k, st in enumerate(sticky) if not st), None)
        what = "only sticky copies" if k is None else f"record frame #{k} (type {frame_type(frames[k].data)})"
        return False, f"the record does not start at an epoch's first frame: {what}", 0.0
    k0, p, lead = start
    if lead and k0 > 0:
        return False, (f"record frame #{k0} (type {frame_type(frames[k0].data)}) opens the record after the hub's "
                       f"sticky copies, but that epoch's own station messages are missing"), 0.0
    skipped_types: dict[int, int] = {}
    gaps = 0
    epochs = 1
    seconds = 0.0
    times = [b.t for b in schedule.bursts]
    anchor_k, anchor_p = k0, p   # the last frame that is no sticky type, and its fixture index
    k = k0 + 1
    while k < len(frames):
        q = (p + 1) % len(fx)
        hops = 0
        while fx[q] != frames[k].data and skippable(q) and hops <= len(fx):
            t = frame_type(fx[q])
            skipped_types[t] = skipped_types.get(t, 0) + 1
            q = (q + 1) % len(fx)
            hops += 1
        if fx[q] == frames[k].data:
            if burst_of[q] != burst_of[p] or q <= p:
                dt = times[burst_of[q]] - times[burst_of[p]]
                seconds += dt if q > p else dt + schedule.duration  # across the loop's seam
                epochs += 1
            p = q
            if not sticky[k]:
                anchor_k, anchor_p = k, p
            k += 1
            continue
        # A gap: only between an epoch's last frame and sticky copies plus an epoch's first frame. Station
        # messages that matched the fixture after the last other frame may be the copies of a resume (a
        # fixed base sends them unchanged): judge the gap from that frame.
        if anchor_k < k - 1 and all(sticky[anchor_k + 1:k]):
            k, p = anchor_k + 1, anchor_p
        if not last_of_burst(p):
            return False, (f"record frame #{k} (type {frame_type(frames[k].data)}) follows fixture frame {p} "
                           f"(type {frame_type(fx[p])}) inside its epoch: fixture frame {q} (type "
                           f"{frame_type(fx[q])}) is missing"), 0.0
        nxt = opener(k)
        if nxt is None:
            if all(sticky[k:]):
                break   # the record ends with sticky copies of the next session start
            return False, (f"after the gap at record frame #{k} the record does not resume at an epoch's first "
                           f"frame: fixture frame {q} (type {frame_type(fx[q])}) or that epoch's start is missing"), 0.0
        if nxt[2]:
            what = ("follows the epoch before it" if burst_of[nxt[1]] == (burst_of[p] + 1) % len(schedule.bursts)
                    else "resumes after a pause")
            return False, (f"record frame #{nxt[0]} (type {frame_type(frames[nxt[0]].data)}) {what}, but that "
                           f"epoch's station messages are missing"), 0.0
        k, p = nxt[0] + 1, nxt[1]
        anchor_k, anchor_p = nxt[0], nxt[1]
        gaps += 1
        epochs += 1
    if whole_tail and not last_of_burst(anchor_p):
        q = (anchor_p + 1) % len(fx)
        while skippable(q) and q != anchor_p:
            q = (q + 1) % len(fx)
        return False, (f"the record ends inside an epoch: record frame #{anchor_k} (type "
                       f"{frame_type(frames[anchor_k].data)}, fixture frame {anchor_p}) is not its epoch's last; "
                       f"fixture frame {q} (type {frame_type(fx[q])}) never came"), 0.0
    unit = "epochs" if schedule.kind == "rtcm" else "recorded reads"
    detail = (f"{len(frames)} frames, {len(frames) - k0} from the first epoch's first frame over {epochs} {unit}, "
              f"{gaps} gap{'' if gaps == 1 else 's'} on epoch boundaries; skipped fixture types never forwarded: "
              f"{', '.join(f'{t} x{n}' for t, n in sorted(skipped_types.items())) or 'none'}")
    return True, detail, seconds + schedule.period


# ---------------------------------------------------------------------------
# the BKG caster in Docker (optional stream target)


class DockerCaster:
    """The BKG caster container plus a GET client recording what it relays."""

    def __init__(self, out: Path, say):
        self.out = out
        self.say = say
        self.name = f"onocoydiy-e2e-bkg-{uuid.uuid4().hex[:8]}"
        self.port = smoke.free_port()
        self.stop_event = threading.Event()
        self.data = bytearray()
        self.lock = threading.Lock()
        self.first_t: float | None = None
        self.sessions = 0
        self.thread = threading.Thread(target=self._client, name="bkg-client", daemon=True)

    def start(self) -> None:
        if subprocess.run(["docker", "image", "inspect", DOCKER_IMAGE], capture_output=True).returncode != 0:
            self.say(f"building {DOCKER_IMAGE} (tools/bench/docker/bkg-caster/build.sh)")
            r = subprocess.run([str(BENCH / "docker" / "bkg-caster" / "build.sh"), "-q"], capture_output=True,
                               text=True)
            if r.returncode != 0:
                raise RuntimeError(f"docker build failed: {r.stderr.strip()[-400:]}")
        r = subprocess.run(["docker", "run", "-d", "--rm", "--name", self.name, "-p", f"127.0.0.1:{self.port}:2101",
                            DOCKER_IMAGE], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"docker run failed: {r.stderr.strip()[-400:]}")
        self.thread.start()

    def _client(self) -> None:
        rec = open(self.out / "record-docker.rtcm3", "wb")
        while not self.stop_event.is_set():
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=5) as s:
                    s.sendall(f"GET /{STREAM_MOUNT} HTTP/1.0\r\nUser-Agent: NTRIP onocoydiy-e2e/1.0\r\n\r\n".encode())
                    head = b""
                    while b"\r\n" not in head and len(head) < 4096:
                        d = s.recv(1)
                        if not d:
                            break
                        head += d
                    if head != b"ICY 200 OK\r\n":
                        raise OSError(f"reply {head[:40]!r}")
                    self.sessions += 1
                    self.say(f"docker caster: relay client in ({head.strip().decode('latin-1')})")
                    s.settimeout(0.5)
                    while not self.stop_event.is_set():
                        try:
                            d = s.recv(65536)
                        except socket.timeout:
                            continue
                        if not d:
                            break
                        with self.lock:
                            if self.first_t is None:
                                self.first_t = time.monotonic()
                            if self.sessions == 1:
                                self.data += d
                                rec.write(d)
                                rec.flush()
            except OSError:
                self.stop_event.wait(1.0)
        rec.close()

    def record(self) -> bytes:
        with self.lock:
            return bytes(self.data)

    def stop(self) -> None:
        self.stop_event.set()
        self.thread.join(timeout=5)
        r = subprocess.run(["docker", "logs", self.name], capture_output=True, text=True)
        (self.out / "docker-caster.log").write_text(r.stdout + r.stderr, encoding="utf-8")
        subprocess.run(["docker", "rm", "-f", self.name], capture_output=True)


# ---------------------------------------------------------------------------


class E2E:
    def __init__(self, args: argparse.Namespace, schedule: Schedule):
        self.args = args
        self.out = Path(args.out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        for sub in ("replay", "caster", "guard"):
            (self.out / sub).mkdir(exist_ok=True)
        self.console = self.out / "console.log"
        self.results: list[tuple[str, str]] = []
        self.schedule = schedule
        self.proc: subprocess.Popen | None = None
        self.replay: ReplayServer | None = None
        self.caster: FakeCaster | None = None
        self.docker: DockerCaster | None = None
        self.guard: FakeCaster | None = None
        self.responder: Um982Responder | None = None
        self.cookie: str | None = None
        self.password = "e2e-" + secrets.token_hex(8)
        self.http_port = smoke.free_port()
        self.planned_reboots = 0
        self.slot1_body: dict | None = None   # slot 1's settings as PUT (saved again for planned closes)
        # The signal-test scenario's caster sessions by what opened them: (first connection, what), in order.
        self.sgt_spans: list[tuple[int, str]] = []
        self.t0 = time.monotonic()
        self.events = open(self.out / "e2e.log", "w", encoding="utf-8")

    # -- reporting --------------------------------------------------------------

    def say(self, text: str) -> None:
        line = f"{time.monotonic() - self.t0:8.1f} {text}"
        self.events.write(line + "\n")
        self.events.flush()

    def check(self, ok: bool, text: str) -> bool:
        self.results.append(("PASS" if ok else "FAIL", text))
        print(("PASS " if ok else "FAIL ") + text, flush=True)
        self.say(("PASS " if ok else "FAIL ") + text)
        return ok

    def skip(self, text: str) -> None:
        self.results.append(("SKIP", text))
        print("SKIP " + text, flush=True)
        self.say("SKIP " + text)

    def note(self, text: str) -> None:
        print("     " + text, flush=True)
        self.say(text)

    # -- HTTP -------------------------------------------------------------------

    def api(self, method: str, path: str, body: dict | None = None, timeout: float = 30) -> tuple[int, object]:
        status, _h, raw = smoke.request(self.http_port, method, path, body, cookie=self.cookie, timeout=timeout)
        try:
            data = json.loads(raw) if raw else None
        except ValueError:
            data = raw.decode("latin-1", "replace")
        return status, data

    def save(self, name: str, data) -> None:
        (self.out / name).write_text(json.dumps(data, indent=1) if not isinstance(data, str) else data,
                                     encoding="utf-8")

    def wait_http(self, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.proc.poll() is None:
            try:
                status, _h, _b = smoke.request(self.http_port, "GET", "/", timeout=10, watch=False)
                if status == 200:
                    return True
            except Exception:  # noqa: BLE001 - not up yet (refused, reset, timed out, half an answer)
                pass
            time.sleep(1)
        return False

    def login(self, first: bool) -> bool:
        path = "/api/setup/admin" if first else "/api/session"
        status, hdrs, body = smoke.request(self.http_port, "POST", path, {"password": self.password}, timeout=60)
        cookie = hdrs.get("set-cookie", "").split(";", 1)[0]
        if status == 200 and cookie.startswith("sid="):
            self.cookie = cookie
            return True
        self.note(f"{path}: HTTP {status} {body[:200]!r}")
        return False

    def poll(self, fn, timeout: float, every: float = 1.0):
        """fn() until it returns something truthy, or None after `timeout`."""
        deadline = time.monotonic() + timeout
        while True:
            try:
                v = fn()
            except (OSError, http.client.HTTPException, ValueError, KeyError, TypeError) as e:  # try again
                self.say(f"poll: {type(e).__name__}: {e}")
                v = None
            if v:
                return v
            if time.monotonic() >= deadline or (self.proc is not None and self.proc.poll() is not None):
                return None
            time.sleep(every)

    def slot(self, n: int) -> dict:
        status, data = self.api("GET", f"/api/casters/{n}")
        if status != 200 or not isinstance(data, dict):
            raise ValueError(f"GET /api/casters/{n}: HTTP {status}")
        return data

    def health(self) -> dict:
        status, data = self.api("GET", "/api/health")
        if status != 200 or not isinstance(data, dict):
            raise ValueError(f"GET /api/health: HTTP {status}")
        return data

    # -- processes --------------------------------------------------------------

    def start(self) -> None:
        self.replay = ReplayServer(self.schedule, loop=True, run_dir=self.out / "replay", say=self.say).start()
        backstop = self.budget_s() + smoke.BACKSTOP_MARGIN_S
        pcap = self.out / "qemu-net.pcap"
        cmd = [str(smoke.RUN_SH), "--no-build", "--headless", str(self.console), "--uart1",
               f"connect:{self.replay.port}", "--http-port", str(self.http_port), "--seconds", str(int(backstop)),
               "--pcap", str(pcap)]
        print("starting: " + " ".join(cmd), flush=True)
        self.console.unlink(missing_ok=True)
        pcap.unlink(missing_ok=True)
        (self.out / "slow-requests.log").unlink(missing_ok=True)
        smoke.watch_requests(self.out, self.console, self.out / "run.log", self.http_port, self.t0)
        self.proc = subprocess.Popen(cmd, stdout=open(self.out / "run.log", "w"), stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)

    def budget_s(self) -> float:
        """Upper bound on the whole run (run.sh's backstop)."""
        s = 240 + self.args.observe
        if self.args.scenario in ("onocoy-guard", "all"):
            s += 900 + (900 if self.args.long else 0)
        if self.args.scenario in ("signal-test", "all"):
            s += 720 + 2 * SGT_PAUSES * 30
        return s

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
        for x in (self.replay, self.caster, self.guard, self.docker):
            if x is not None:
                try:
                    x.stop()
                except Exception as e:  # noqa: BLE001 - clean-up goes on
                    self.say(f"stop {type(x).__name__}: {e}")

    # -- the station ------------------------------------------------------------

    def boot(self) -> bool:
        if not self.check(self.replay.connected.wait(60), "QEMU connected UART1 to the replay"):
            return False
        if not self.check(self.wait_http(self.args.timeout), "the web server answers GET /"):
            return False
        if not self.check(self.login(first=True), "POST /api/setup/admin set the admin password"):
            return False
        return self.settle()

    def settle(self) -> bool:
        """Streaming. The identity query that gets no answer while RTCM flows never takes the
        receiver line (no lease, the casters are never paused for it), so nothing else to wait for."""
        def ready():
            h = self.health()
            return h if h.get("station", {}).get("state") == "streaming" else None
        h = self.poll(ready, 90)
        if h is None:
            st = self.health().get("station", {})
            self.check(False, f"station streaming the replay within 90 s (state {st.get('state')})")
            return False
        self.check(True, f"station streaming the replay (hub {h['hub']['frames_in']} frames in)")
        return True

    # -- scenario: stream -------------------------------------------------------

    def console_text(self) -> str:
        return self.console.read_text(encoding="utf-8", errors="replace") if self.console.exists() else ""

    def stream(self) -> None:
        console_at = len(self.console_text())
        states: set[str] = set()
        watching = threading.Event()

        def watch_slot() -> None:
            """Every state slot 1 reports while the scenario runs (it must never be `paused`)."""
            while not watching.wait(1.0):
                try:
                    states.add(self.slot(1)["status"].get("state"))
                except (OSError, http.client.HTTPException, ValueError, KeyError, TypeError):
                    pass
        watcher = threading.Thread(target=watch_slot, name="slot1-watch", daemon=True)
        if self.args.caster == "docker":
            self.docker = DockerCaster(self.out / "caster", self.say)
            self.docker.start()
            port, password, want_line = self.docker.port, DOCKER_SOURCE_PASSWORD, "OK"
            self.note(f"stream target: BKG caster container {self.docker.name} on 127.0.0.1:{port}")
        else:
            self.caster = FakeCaster(0, mount=STREAM_MOUNT, password=STREAM_PASSWORD, record=True,
                                     run_dir=self.out / "caster", say=self.say).start()
            port, password, want_line = self.caster.port, STREAM_PASSWORD, "ICY 200 OK"
        body = {"name": "Bench", "host": HOST_FROM_GUEST, "port": port, "ntrip_version": 1,
                "mountpoint": STREAM_MOUNT, "password": password, "backoff_profile": "standard", "enabled": True}
        self.slot1_body = body
        status, data = self.api("PUT", "/api/casters/1", body)
        self.save("put-caster-1.json", data)
        if not self.check(status == 200, f"PUT /api/casters/1 (slot 1 'Bench' -> {HOST_FROM_GUEST}:{port}): "
                                         f"HTTP {status}"):
            return
        t_put = time.monotonic()
        watcher.start()

        def streaming():
            s = self.slot(1)["status"]
            return s if s.get("state") == "streaming" and s.get("last_status_line") == want_line else None
        s = self.poll(streaming, 60)
        if not self.check(s is not None, f"slot 1 streaming within 60 s with last_status_line {want_line!r}"
                          + (f" after {time.monotonic() - t_put:.0f} s" if s else
                             f" (now {self.slot(1)['status'].get('state')!r}, "
                             f"{self.slot(1)['status'].get('last_status_line')!r})")):
            return

        want = expected_signals(self.schedule)
        status, sig = self.api("GET", "/api/gnss/signals")
        self.save("api-gnss-signals.json", sig)
        got = {x["sys"]: sorted(x["bands"]) for x in (sig or {}).get("systems", [])} if status == 200 else {}
        self.check(got == want, f"/api/gnss/signals systems and bands equal the fixture's: {got}"
                   + ("" if got == want else f" (fixture: {want})"))

        per_epoch = frames_per_epoch(self.schedule)
        h0, e0 = self.health()["hub"]["frames_in"], self.slot(1)["status"]["epochs_sent"]
        time.sleep(10)
        h1, e1 = self.health()["hub"]["frames_in"], self.slot(1)["status"]["epochs_sent"]
        self.check(h1 - h0 >= 5 * per_epoch, f"/api/health hub.frames_in grew by {h1 - h0} in 10 s "
                                            f"(at least 5 x {per_epoch:.1f} frames per epoch)")
        self.check(e1 > e0, f"slot 1 epochs_sent rising: {e0} -> {e1} in 10 s")

        record, leading_ok = self.wait_record()
        ok, detail, seconds = check_record(record, self.schedule, leading_junk_ok=leading_ok)
        self.check(ok and seconds >= MIN_RECORD_S,
                   f"caster record: {seconds:.0f} s of frames (at least {MIN_RECORD_S:.0f}); {detail}")
        self.save("api-caster-1-end.json", self.slot(1))
        watching.set()
        watcher.join(timeout=5)
        self.check("paused" not in states, f"slot 1 never paused during the stream scenario (states seen: "
                                           f"{sorted(x for x in states if x)})")
        lease_lines = [x for x in self.console_text()[console_at:].splitlines() if IDENTIFY_LEASE_LINE in x]
        self.check(not lease_lines, f"no {IDENTIFY_LEASE_LINE!r} on the console while RTCM flows"
                   + (f": {lease_lines[0].strip()[:120]}" if lease_lines else ""))
        self.maintenance_check()

    def maintenance_check(self) -> None:
        """Receiver maintenance answers with the state the start or extension set ("effective at once"),
        not with what the station task last published."""
        def post(what: str) -> tuple[int, dict]:
            for _ in range(6):  # a receiver job may be running: offered again after it (409 busy)
                status, m = self.api("POST", "/api/gnss/maintenance", {"duration_s": 900})
                if status != 409 or not isinstance(m, dict) or m.get("error", {}).get("code") != "busy":
                    break
                time.sleep(5)
            self.save(f"maintenance-{what}.json", m)
            return status, m if isinstance(m, dict) else {}
        status, m = post("start")
        if not self.check(status == 200 and m.get("on") is True and m.get("chosen_s") == 900
                          and 895 <= (m.get("left_s") or 0) <= 900,
                          f"POST /api/gnss/maintenance (900 s, off before): HTTP {status} {m}"):
            return
        status, m = post("extend")
        self.check(status == 200 and m.get("on") is True and m.get("chosen_s") == 1800   # with the extension
                   and 1790 <= (m.get("left_s") or 0) <= 1800,
                   f"POST /api/gnss/maintenance again (extended by 900 s): HTTP {status} {m}")
        status, g = self.api("GET", "/api/gnss")
        gm = (g or {}).get("maintenance", {}) if isinstance(g, dict) else {}
        self.check(gm.get("on") is True and abs((gm.get("left_s") or 0) - (m.get("left_s") or 0)) <= 5,
                   f"GET /api/gnss maintenance agrees: {gm.get('on')}, left_s {gm.get('left_s')}")
        self.maintenance_restart_check()
        status, d = self.api("DELETE", "/api/gnss/maintenance")
        self.check(status == 202, f"DELETE /api/gnss/maintenance: HTTP {status} {d}")

    def maintenance_restart_check(self) -> None:
        """A restart during receiver maintenance (an ESP32 software reset) resumes it from its RTC record with
        the time left: never more than was left, and nothing lost but the restart's own time."""
        def maint() -> dict:
            status, g = self.api("GET", "/api/gnss")
            return (g or {}).get("maintenance", {}) if status == 200 and isinstance(g, dict) else {}
        before = maint()
        t_ref = time.monotonic()
        if not self.check(before.get("on") is True and (before.get("left_s") or 0) > 60,
                          f"maintenance on before the restart: {before}"):
            return
        self.replay.pause()   # quiet across the restart (reboot_check says why)
        time.sleep(2)
        status, data = self.api("POST", "/api/system/reboot", {})
        if not self.check(status == 202, f"POST /api/system/reboot during maintenance: HTTP {status} {data}"):
            self.replay.resume()
            return
        self.planned_reboots += 1
        time.sleep(5)
        self.cookie = None
        back = self.wait_http(self.args.timeout)
        self.replay.resume()
        if not self.check(back and self.login(first=False), "back after the restart during maintenance"):
            return
        after = maint()
        want = before["left_s"] - (time.monotonic() - t_ref)
        self.save("maintenance-after-restart.json", after)
        self.check(after.get("on") is True and after.get("chosen_s") == before.get("chosen_s")
                   and want - 5 <= (after.get("left_s") or 0) <= want + 1,
                   f"maintenance resumed after the restart: {after.get('left_s')} s left of {after.get('chosen_s')} "
                   f"(want {want:.0f}: never more, at most 5 s less)")
        lines = [x for x in self.console_text().splitlines() if "maintenance continues after the reset" in x]
        self.check(bool(lines) and "RTC record" in lines[-1],
                   "console: " + (lines[-1].strip()[-150:] if lines else "no 'maintenance continues' line"))
        status, st = self.api("GET", "/api/status")
        rx = (st or {}).get("receiver", {}) if isinstance(st, dict) else {}
        self.check(rx.get("rx_maintenance") is True and rx.get("state") == "idle",
                   f"/api/status after the restart: rx_maintenance {rx.get('rx_maintenance')}, state {rx.get('state')!r}")

    def wait_record(self) -> tuple[bytes, bool]:
        """The first session's bytes once they span --observe seconds (or the time is up)."""
        need = max(self.args.observe, MIN_RECORD_S) + 2 * self.schedule.period
        deadline = time.monotonic() + need + 60
        if self.docker is not None:
            while time.monotonic() < deadline:
                if self.docker.first_t and time.monotonic() - self.docker.first_t >= need:
                    break
                time.sleep(1)
            return self.docker.record(), True
        n = None
        while time.monotonic() < deadline:
            if n is None:
                n = next((c["n"] for c in self.caster.all_connections()
                          if c["role"] == "source" and c["row"] == "icy_ok"), None)
            rec = self.caster.record_of(n) if n is not None else None
            if rec and rec.first_t and time.time() - rec.first_t >= need:
                break
            time.sleep(1)
        if n is None:
            return b"", False
        sessions = [c for c in self.caster.all_connections() if c["role"] == "source"]
        if len(sessions) > 1:
            self.note(f"slot 1 logged in {len(sessions)} times; checking the first session (connection {n})")
        path = self.out / "caster" / f"record-{n:03d}.rtcm3"
        return (path.read_bytes() if path.exists() else b""), False

    # -- scenario: onocoy-guard ---------------------------------------------------

    def guard_conns(self) -> list[dict]:
        return self.guard.all_connections()

    def wait_conn_end(self, n: int, timeout: float) -> dict | None:
        """Connection number n (1-based), once it ended."""
        def ended():
            c = [x for x in self.guard.all_connections() if x["n"] == n]
            return c[0] if c and c[0]["end_mono"] is not None else None
        return self.poll(ended, timeout, every=0.2)

    def wait_conn_start(self, n: int, timeout: float) -> dict | None:
        """Guard connection n once the fake caster has parsed its request
        (or the connection ended without one): the record exists from the
        accept on, with an empty request line until the request is read."""
        def started():
            c = [x for x in self.guard.all_connections() if x["n"] == n]
            if not c:
                return None
            x = c[0]
            return x if x["request_line"] or x["request_complete"] or x["closed_by"] else None
        return self.poll(started, timeout, every=0.2)

    def quiet(self, seconds: float, what: str) -> bool:
        n = len(self.guard_conns())
        time.sleep(seconds)
        m = len(self.guard_conns())
        return self.check(m == n, f"{what}: no further login in {seconds:.0f} s" + (f" ({m - n} came)" if m != n else ""))

    def wait_stopped(self, reason: str, timeout: float = 20) -> dict | None:
        def stopped():
            s = self.slot(0)["status"]
            return s if s.get("state") == "stopped" else None
        s = self.poll(stopped, timeout, every=0.5)
        ok = s is not None and s.get("stop_reason") == reason
        self.check(ok, f"slot 0 stopped with stop_reason {reason!r}"
                   + ("" if ok else f" (state {self.slot(0)['status'].get('state')!r}, "
                                    f"stop_reason {self.slot(0)['status'].get('stop_reason')!r})"))
        if ok:
            status, st = self.api("GET", "/api/status")
            entry = next((c for c in (st or {}).get("casters", []) if c.get("slot") == 0), {})
            self.check(entry.get("stop_reason") == reason, f"/api/status casters[0].stop_reason {entry.get('stop_reason')!r}")
        return s if ok else None

    def retry(self, sequence: str) -> int:
        """Script the guard caster, then POST /api/casters/0/retry. Returns the
        number the next guard connection will have."""
        self.guard.set_sequence(sequence)
        n = len(self.guard_conns()) + 1
        # Body {}: every POST carries Content-Type: application/json (API.md, CSRF).
        status, data = self.api("POST", "/api/casters/0/retry", {})
        self.check(status == 202 and isinstance(data, dict) and data.get("accepted") is True,
                   f"POST /api/casters/0/retry ({sequence}): HTTP {status} {data}")
        return n

    def within(self, value: float | None, entry: float, what: str, slack: float = 0.5) -> bool:
        """[entry, 1.2 x entry] plus QEMU's tolerance; `slack` below for how the value was measured."""
        lo, hi = entry - slack, 1.2 * entry + QEMU_TOLERANCE_S
        ok = value is not None and lo <= value <= hi
        return self.check(ok, f"{what}: {'-' if value is None else f'{value:.1f}'} s "
                              f"(expected {entry:.0f} s, window {lo:.1f}-{hi:.1f} s)")

    def next_retry_after(self, n: int, entry: float, what: str) -> bool:
        """After connection n ended: the slot's next_retry_s, plus the time gone since the close."""
        c = self.wait_conn_end(n, 30)
        if c is None:
            return self.check(False, f"{what}: connection {n} did not end")

        def waiting():
            st = self.slot(0)["status"]
            return (st, time.monotonic()) if st.get("state") not in ("connecting", "streaming") else None
        got = self.poll(waiting, 10, every=0.3)
        if got is None:
            return self.check(False, f"{what}: slot 0 not waiting after the refusal")
        s, t = got
        # next_retry_s is in whole seconds: 1 s of slack below.
        return self.within(s.get("next_retry_s", -1) + (t - c["end_mono"]), entry,
                           f"{what}: next_retry_s + time since the close (state {s.get('state')!r})", slack=1.0)

    def gap(self, n: int, entry: float, what: str, timeout: float | None = None) -> bool:
        """Connection n+1 starts `entry` s after connection n ended."""
        a = self.wait_conn_end(n, 30)
        b = self.wait_conn_start(n + 1, timeout if timeout is not None else 1.2 * entry + 30)
        if a is None or b is None:
            return self.check(False, f"{what}: connection {n + 1} never came")
        if self.lease_overlap(a["end_mono"]):
            self.note(f"{what}: a receiver query paused the casters inside this wait; not judged")
            return True
        return self.within(b["start_mono"] - a["end_mono"], entry, what)

    def lease_overlap(self, since_mono: float) -> bool:
        """A receiver lease (casters paused) since then?"""
        st = self.health().get("station", {})
        rel = st.get("since_lease_release_ms")
        return bool(st.get("lease_held")) or (rel is not None and time.monotonic() - rel / 1000.0 > since_mono)

    def onocoy_guard(self) -> None:
        cfg = self.slot(0).get("config") or {}
        if "test_onocoy" not in cfg:
            raise SkipScenario("onocoy-guard: this firmware has no test-Onocoy slots (no config.test_onocoy)")
        self.guard = FakeCaster(0, run_dir=self.out / "guard", say=self.say).start()
        G = self.guard

        # 1. A credential in use, on credentials that never worked: stopped at once.
        G.set_sequence("onocoy_in_use")
        body = {"enabled": True, "name": "Onocoy test", "host": HOST_FROM_GUEST, "port": G.port, "ntrip_version": 1,
                "tls": False, "mountpoint": GUARD_MOUNT, "username": GUARD_USER, "password": GUARD_PASSWORD,
                "test_onocoy": True}
        status, data = self.api("PUT", "/api/casters/0", body)
        self.save("put-caster-0.json", data)
        if status == 400 and isinstance(data, dict) and data.get("field") == "test_onocoy":
            raise SkipScenario("onocoy-guard: PUT answered 400 invalid_field test_onocoy (firmware without "
                               "test-Onocoy slots)")
        if not self.check(status == 200, f"PUT /api/casters/0 as a test-Onocoy slot: HTTP {status}"):
            return
        c = data.get("config", {})
        self.check(c.get("test_onocoy") is True and c.get("protected") is True and c.get("backoff_profile") == "fast",
                   f"slot 0 counts as Onocoy: test_onocoy {c.get('test_onocoy')}, protected {c.get('protected')}, "
                   f"profile {c.get('backoff_profile')!r}")
        first = self.wait_conn_start(1, 90)
        if not self.check(first is not None, "slot 0 logged in to the guard caster"):
            return
        self.check(first["request_line"] == f"SOURCE *** /{GUARD_USER}",
                   f"Onocoy NTRIP 1.0 wire layout: {first['request_line']!r}")
        fb = self.slot(0)["status"].get("failures_by_class", {})
        # Every class but "none": the eleven of every slot, the guard's three and "local".
        self.check(len(fb) == 15 and {"login_blocked", "in_use", "replaced", "local"} <= set(fb),
                   f"failures_by_class has {len(fb)} classes")
        if self.wait_stopped("in_use"):
            self.quiet(15, "stopped (in use)")

        # 1b. The generic in-use replies are the same thing on an Onocoy slot. The slot is
        #     stopped for the same reason already: only once the retry's login reached the
        #     caster (the retry cleared that stop first) is a stop the new one.
        for seq in ("http_409", "mount_taken", "already_connected"):
            n = self.retry(seq)
            if self.check(self.wait_conn_start(n, 30) is not None, f"{seq}: slot 0 logged in again") and \
                    self.wait_stopped("in_use"):
                s = self.slot(0)["status"]
                self.check(s.get("login_rejects") == 0, f"{seq}: no refused login counted ({s.get('login_rejects')})")
                self.quiet(5, f"stopped (in use: {seq})")

        # 1c. 503 is the caster's own trouble: the fast transient row, never a refused login
        #     (three in a row within seconds), then an in-use reply stops the slot again.
        n = self.retry("http_503,http_503,http_503,onocoy_in_use")
        if self.check(self.wait_conn_start(n + 3, 40) is not None, "503 x3: the fourth login came within seconds") and \
                self.wait_stopped("in_use"):
            s = self.slot(0)["status"]
            self.check(s.get("login_rejects") == 0, f"503 x3: no refused login counted ({s.get('login_rejects')})")

        # 1d. Onocoy's reason in a reply line the caster closes before its line end (the wire
        #     form is not known) is a refused login all the same, not a silent close.
        self.no_line_end_check()

        # 2. A rejected login on credentials that never worked: 5 minutes.
        n = self.retry("onocoy_auth_failed")
        self.next_retry_after(n, 300, "rejected login, credentials never worked")
        self.no_protected_trigger("waiting after a rejected login")
        if self.args.long:
            self.gap(n, 300, "rejected login: the wait before the second attempt", timeout=420)
            self.next_retry_after(n + 1, 1800, "second rejected login")
        self.quiet(10, "waiting after a rejected login")

        # 3. Too many unsuccessful logins: 60 minutes.
        n = self.retry("onocoy_login_blocked")
        self.next_retry_after(n, 3600, "login blocked")
        self.no_protected_trigger("waiting after a login block")
        self.quiet(10, "waiting after a login block")

        # 4. Sessions the caster ends within 120 s: 5 s, then 10 s, stopped at a score of 3.
        n = self.retry("icy_ok:kick=3")
        if self.wait_conn_start(n, 30):
            self.gap(n, 5, "reconnect after the first short session")
            self.gap(n + 1, 10, "reconnect after the second short session")
            if self.wait_conn_end(n + 2, 30) and self.wait_stopped("kicked"):
                k = self.slot(0)["status"].get("kicked_sessions")
                self.check(isinstance(k, int) and k >= 3, f"kicked_sessions {k}")
                self.quiet(15, "stopped (kicked)")

        # 5. "Device reconnected" on the live session scores 2: stopped after two sessions.
        #    The reconnect wait follows the score (5 s at 1, 10 s at 2), so 10 s here.
        n = self.retry("icy_ok:kick=3:text=onocoy_reconnected")
        if self.wait_conn_start(n, 30):
            self.gap(n, 10, "reconnect after a 'Device reconnected' session (score 2)")
            if self.wait_conn_end(n + 1, 30) and self.wait_stopped("kicked"):
                text = self.slot(0)["status"].get("last_session_text") or ""
                self.check("Device reconnected" in text, f"last_session_text {text!r}")
                self.quiet(15, "stopped (device reconnected)")

        # 6. Inside the 300 s window after a session, on credentials that worked:
        #    a rejected login waits 30 s, the second one 30 min.
        n = self.retry("icy_ok:kick=3,onocoy_auth_failed")
        if self.wait_conn_start(n, 30):
            self.gap(n + 1, 30, "rejected login inside the window after a session: the wait", timeout=60)
            self.next_retry_after(n + 2, 1800, "second rejected login")

        # 7. Credential in use inside the window: 15, 30 (60, 120 with --long) s, then stopped.
        n = self.retry("icy_ok:kick=3,onocoy_in_use")
        if self.wait_conn_start(n, 30):
            self.gap(n + 1, 15, "in use inside the window: first wait")
            self.no_protected_trigger("waiting after an in-use reply")
            self.gap(n + 2, 30, "in use inside the window: second wait")
            if self.args.long:
                self.gap(n + 3, 60, "in use inside the window: third wait")
                self.gap(n + 4, 120, "in use inside the window: fourth wait")
                if self.wait_conn_end(n + 5, 30):
                    self.wait_stopped("in_use")

        # 8. A banned credential stops the slot at once; a save clears the
        #    stop, and a duplicate-stream report stops it at once again.
        self.retry("onocoy_credential_banned")
        if self.wait_stopped("banned", timeout=40):
            self.quiet(10, "stopped (banned)")
        self.guard.set_sequence("onocoy_duplicate")
        n = len(self.guard_conns()) + 1
        status, _ = self.api("PUT", "/api/casters/0", {"enabled": True})
        self.check(status == 200, f"PUT /api/casters/0 (a save of the stopped slot): HTTP {status}")
        self.check(self.wait_conn_start(n, 40) is not None, "the save cleared the stop: slot 0 logged in again")
        if not self.wait_stopped("duplicate", timeout=40):
            return

        # 9. A stop survives a reboot; retry clears it.
        self.reboot_check()
        # 10. A slot that cannot reach its caster (a refused connect) does hold the trigger.
        self.closed_port_trigger()
        self.journal_check()

    def protected_trigger(self) -> bool:
        return "protected" in (self.health().get("network", {}).get("ladder", {}).get("triggers") or [])

    def no_protected_trigger(self, what: str) -> None:
        """A wait on the caster's own reply: a network action cannot help, so no `protected` trigger.

        Health recomputes the triggers once a second, and during the connect
        attempt before the reply (no failure recorded yet) the trigger holds
        by design, so the first samples wait out the tick after the reply."""
        time.sleep(2.0)
        seen = []
        for _ in range(5):
            seen.append(self.protected_trigger())
            time.sleep(1)
        st = self.slot(0)["status"]
        self.check(not any(seen), f"{what}: /api/health network.ladder.triggers without 'protected' "
                                  f"(slot 0 {st.get('state')!r}, last failure {st.get('reason')!r})")

    def closed_port_trigger(self) -> None:
        closed = smoke.free_port()   # nothing listens there: the connect is refused
        status, _ = self.api("PUT", "/api/casters/0", {"port": closed})
        if not self.check(status == 200, f"PUT /api/casters/0 (a closed port): HTTP {status}"):
            return
        got = self.poll(lambda: self.protected_trigger(), 30, every=1.0)
        st = self.slot(0)["status"]
        self.check(bool(got), f"connect refused: /api/health network.ladder.triggers has 'protected' "
                              f"(slot 0 {st.get('state')!r}, last failure {st.get('reason')!r})")
        self.guard.set_sequence("icy_ok")
        status, _ = self.api("PUT", "/api/casters/0", {"port": self.guard.port})
        s = self.poll(lambda: (lambda x: x if x.get("state") == "streaming" else None)(self.slot(0)["status"]), 60)
        self.check(status == 200 and s is not None, f"slot 0 back on the guard caster and streaming (PUT: HTTP {status})")

    def no_line_end_check(self) -> None:
        main = self.guard
        (self.out / "guard-no-line-end").mkdir(exist_ok=True)
        g2 = FakeCaster(0, onocoy_form="no_line_end", run_dir=self.out / "guard-no-line-end", say=self.say).start()
        g2.set_sequence("onocoy_auth_failed")
        self.guard = g2
        try:
            status, _ = self.api("PUT", "/api/casters/0", {"port": g2.port})
            if self.check(status == 200, f"PUT /api/casters/0 (port of a caster that ends its lines early): HTTP {status}"):
                self.next_retry_after(1, 300, "rejected login, its line closed without CR LF")
                s = self.slot(0)["status"]
                self.check(s.get("login_rejects") == 1 and s.get("state") == "auth_failed",
                           f"counted as a refused login: login_rejects {s.get('login_rejects')}, state {s.get('state')!r}")
        finally:
            self.guard = main
            main.set_sequence("onocoy_in_use")
            n = len(self.guard_conns()) + 1
            status, _ = self.api("PUT", "/api/casters/0", {"port": main.port})
            self.check(status == 200 and self.wait_conn_start(n, 30) is not None and self.wait_stopped("in_use")
                       is not None, f"slot 0 back on the guard caster (PUT: HTTP {status}), stopped in use")
            g2.stop()

    def reboot_check(self) -> None:
        n_before = len(self.guard_conns())
        # The replayed receiver goes quiet across the restart (a QEMU limit,
        # tools/qemu/README.md, UART1 section): this QEMU keeps UART1's
        # registers and a raised CPU interrupt line through the firmware's
        # restart, so RTCM pending at that moment storms the new boot's UART
        # interrupt. On a board the next boot resets UART1 at its first use.
        self.replay.pause()
        time.sleep(2)
        status, data = self.api("POST", "/api/system/reboot", {})
        if not self.check(status == 202, f"POST /api/system/reboot: HTTP {status} {data}"):
            self.replay.resume()
            return
        self.planned_reboots += 1
        time.sleep(5)
        self.cookie = None
        back = self.wait_http(self.args.timeout)
        self.replay.resume()
        if not self.check(back and self.login(first=False), "back after the reboot"):
            return
        t_back = time.monotonic()
        s = self.slot(0)["status"]
        self.check(s.get("state") == "stopped" and s.get("stop_reason") == "duplicate",
                   f"after the reboot slot 0 is still stopped ({s.get('state')!r}, {s.get('stop_reason')!r})")
        self.settle()
        left = 30 - (time.monotonic() - t_back)
        if left > 0:
            time.sleep(left)
        self.check(len(self.guard_conns()) == n_before,
                   f"no login after the reboot ({len(self.guard_conns()) - n_before} came)")
        self.retry("icy_ok")
        s = self.poll(lambda: (lambda st: st if st.get("state") == "streaming" else None)(self.slot(0)["status"]), 60)
        self.check(s is not None, "slot 0 streams after the retry")

    def journal_check(self) -> None:
        cursor, entries = 0, []
        for _ in range(200):
            status, data = self.api("GET", f"/api/journal?cursor={cursor}")
            if status != 200 or not isinstance(data, dict):
                break
            entries += data.get("entries", [])
            cursor = data.get("cursor", cursor)
            if not data.get("more"):
                break
        self.save("api-journal.json", entries)
        stops = [STOP_REASONS[(e["a"] >> 8) & 0xFF] if (e["a"] >> 8) & 0xFF < len(STOP_REASONS) else "?"
                 for e in entries if e.get("kind") == "caster_stop" and e["a"] & 0xFF == 0]
        want = {"in_use", "kicked", "banned", "duplicate"}
        self.check(want <= set(stops), f"journal caster_stop records for slot 0: {stops}")

    # -- scenario: signal-test ---------------------------------------------------

    def gnss(self) -> dict:
        status, data = self.api("GET", "/api/gnss")
        if status != 200 or not isinstance(data, dict):
            raise ValueError(f"GET /api/gnss: HTTP {status}")
        return data

    def sgt(self) -> dict:
        status, data = self.api("GET", "/api/gnss/signal-test")
        if status != 200 or not isinstance(data, dict):
            raise ValueError(f"GET /api/gnss/signal-test: HTTP {status}")
        return data

    def stream_slot(self) -> None:
        """Slot 1 streaming to the local fake caster (the stream scenario's, or set up here)."""
        if self.caster is None and self.docker is None:
            self.caster = FakeCaster(0, mount=STREAM_MOUNT, password=STREAM_PASSWORD, record=True,
                                     run_dir=self.out / "caster", say=self.say).start()
            body = {"name": "Bench", "host": HOST_FROM_GUEST, "port": self.caster.port, "ntrip_version": 1,
                    "mountpoint": STREAM_MOUNT, "password": STREAM_PASSWORD, "backoff_profile": "standard",
                    "enabled": True}
            self.slot1_body = body
            status, _ = self.api("PUT", "/api/casters/1", body)
            self.check(status == 200, f"PUT /api/casters/1 (slot 1 for the signal-test records): HTTP {status}")
        s = self.poll(lambda: (lambda x: x if x.get("state") == "streaming" else None)(self.slot(1)["status"]), 90)
        # This session runs through the first configuration, and step 6 judges its part before the receiver's own
        # station messages (the fixture as recorded). It may have only just begun (after the stream scenario's
        # maintenance): two epochs of it first, so that part is there to judge in every run.
        self.check(s is not None and self.two_more_epochs("before the comparison"),
                   "slot 1 streaming before the comparison, two more epochs sent")

    def receiver_ready(self, group: tuple[int, int], timeout: float) -> dict | None:
        """Configured as the profile says, identified, locked, streaming, on `group`."""
        def ready():
            g = self.gnss()
            rx = g.get("rx_signal_group") or {}
            ok = (g.get("state") == "streaming" and g.get("profile_applied") and g.get("identity_confirmed")
                  and not g.get("config_failed") and (g.get("position") or {}).get("locked")
                  and (rx.get("master"), rx.get("slave")) == group)
            return g if ok else None
        return self.poll(ready, timeout, every=2.0)

    def sgt_post(self, groups: list[tuple[int, int]], phase_s: int) -> tuple[int, object]:
        body = {"groups": [{"master": m, "slave": s} for m, s in groups], "phase_s": phase_s}
        return self.api("POST", "/api/gnss/signal-test", body)

    def pause_storm(self, n: int, late_last_s: float = 0.0) -> None:
        """`n` applies of the stored profile, each a pause for receiver work (the casters paused, the receiver
        configured, the casters resumed). Before each one the slot has streamed two more epochs (so the pause
        meets a live stream and the session is not idle), then a random wait of up to 1 s: the moment the
        station decides to pause lands anywhere in the receiver's second, at the end of an epoch, between an
        epoch's station messages and its first MSM, inside its MSMs. With `late_last_s` each epoch's last MSM
        comes that long after the rest (the replay holds it back), longer than the hub's 250 ms gap rule: a
        pause decided in that time must wait for it, not take the quiet line for the epoch's end. The records
        are judged afterwards."""
        if not self.check(self.receiver_ready((7, 0), 120) is not None, "streaming on 7 0 before the pauses"):
            return
        seed = secrets.randbits(32)
        rng = random.Random(seed)
        done = 0
        t0 = time.monotonic()
        self.replay.set_last_frame_delay(late_last_s)
        for i in range(n):
            e0 = self.slot(1)["status"].get("epochs_sent", 0)
            if self.poll(lambda: self.slot(1)["status"].get("epochs_sent", 0) >= e0 + 2, 60, every=0.5) is None:
                self.note(f"pause {i + 1}: slot 1 sent no two epochs within 60 s")
                break
            time.sleep(rng.uniform(0.0, 1.0))
            before = self.console_text().count(RESUMED_LINE)
            status, data = self.api("POST", "/api/gnss/apply", {})
            if status != 202:
                self.note(f"pause {i + 1}: POST /api/gnss/apply: HTTP {status} {data}")
                break
            # One pause per apply: "paused for receiver work" ... "resumed".
            if self.poll(lambda: self.console_text().count(RESUMED_LINE) > before, 90, every=0.5) is None:
                self.note(f"pause {i + 1}: the casters did not resume within 90 s")
                break
            done += 1
        self.replay.set_last_frame_delay(0)
        late = f", each epoch's last MSM {late_last_s:.1f} s late" if late_last_s > 0 else ""
        self.check(done == n, f"{done} of {n} applies paused and resumed the casters, each after two epochs "
                              f"streamed{late} ({time.monotonic() - t0:.0f} s, waits seeded {seed})")

    def sources(self) -> list[dict]:
        return [c for c in self.caster.all_connections() if c["role"] == "source"]

    def streams_again(self, after_n: int, timeout: float) -> bool:
        """Slot 1 streams on a caster session newer than connection `after_n`."""
        def again():
            new = [c for c in self.sources() if c["n"] > after_n and c["row"] == "icy_ok"]
            return new and self.slot(1)["status"].get("state") == "streaming"
        return self.poll(again, timeout, every=0.5) is not None

    def two_more_epochs(self, what: str) -> bool:
        e0 = self.slot(1)["status"].get("epochs_sent", 0)
        if self.poll(lambda: self.slot(1)["status"].get("epochs_sent", 0) >= e0 + 2, 60, every=0.5) is None:
            self.note(f"{what}: slot 1 sent no two epochs within 60 s")
            return False
        return True

    def planned_closes(self, n: int, late_last_s: float) -> None:
        """`n` saves of slot 1's settings, unchanged: each closes the session for the new settings and
        reconnects. Each after the slot streamed two more epochs, with each epoch's last MSM `late_last_s` late;
        every other one 0.1 s after a burst began going out (inside its epoch: the session sent its MSMs but
        not the last), the others at a random moment of the second. A session inside an epoch sends the rest
        of it before it closes (ntrip_server.c drain_step): the records are judged in step 6 (whole tails);
        here at least one close waited for its epoch, none gave up on it, and every save streamed again."""
        if self.slot1_body is None:
            return
        seed = secrets.randbits(32)
        rng = random.Random(seed)
        at = len(self.console_text())
        self.replay.set_last_frame_delay(late_last_s)
        done = 0
        for i in range(n):
            if not self.two_more_epochs(f"save {i + 1}"):
                break
            if i % 2 == 0:
                h = self.replay.wait_burst_head(5.0)
                time.sleep(max(0.0, (h or time.monotonic()) + 0.1 - time.monotonic()))
            else:
                time.sleep(rng.uniform(0.0, 1.0))
            last = max((c["n"] for c in self.sources()), default=0)
            status, data = self.api("PUT", "/api/casters/1", self.slot1_body)
            if status != 200:
                self.note(f"save {i + 1}: PUT /api/casters/1: HTTP {status} {data}")
                break
            self.sgt_spans.append((last + 1, f"since save {i + 1}"))
            if not self.streams_again(last, 60):
                self.note(f"save {i + 1}: slot 1 did not stream again within 60 s")
                break
            done += 1
        self.replay.set_last_frame_delay(0)
        text = self.console_text()[at:]
        drained = text.count(DRAINED_LINE)
        cut = [x.strip()[:160] for x in text.splitlines() if CUT_CLOSE_LINE in x]
        self.check(done == n and drained >= 1 and not cut,
                   f"{done} of {n} saves of slot 1 closed the session and streamed again; {drained} close(s) "
                   f"waited for the epoch being sent, {len(cut)} gave up on it" + (f" ({cut[0]})" if cut else "")
                   + f" (each epoch's last MSM {late_last_s:.1f} s late, waits seeded {seed})")

    def restart_in_epoch(self, late_last_s: float, attempts: int = 3) -> None:
        """A restart that comes inside an epoch while the receiver streams: the session it ends finishes that
        epoch first (its record is judged in step 6). The web server restarts 1 s after the request
        (REBOOT_DELAY_MS), so the request goes out RESTART_AFTER_HEAD_S after a burst began: the shutdown
        comes inside the next burst's MSM run, whose last MSM is `late_last_s` late. This QEMU hangs the next
        boot when RTCM arrives across a restart (tools/qemu/README.md, UART1 section), and the restart comes
        about 0.2 s after the casters close (the network's shutdown hook): the replay starts no burst after
        the one the shutdown comes in (a hold from RESTART_HOLD_AFTER_HEAD_S, that burst's late MSM still
        goes out) and stops as soon as that session has ended (a late last MSM still waiting then is dropped
        too). A shutdown that found no epoch in flight (the request was slow) proves nothing: tried again, up
        to `attempts` restarts: a later try's steps are only noted when they pass (the first try's are
        checks), and the sessions the earlier tries ended are judged with those since the last save (step 6),
        so the number of checks does not depend on the tries. The session after the restart streams two epochs
        before this returns: step 6 judges it too, however soon the scenario ends."""
        if self.caster is None:
            return
        k = 0

        def step(ok: bool, text: str) -> bool:
            if k == 0 or not ok:
                return self.check(ok, text)
            self.note(text)
            return True

        for k in range(attempts):
            if not self.two_more_epochs(f"restart {k + 1}"):
                return
            live = [c for c in self.sources() if c.get("end_mono") is None]
            if not step(len(live) == 1, f"restart {k + 1}: one live caster session before it ({len(live)})"):
                return
            n = live[0]["n"]
            at = len(self.console_text())
            self.replay.set_last_frame_delay(late_last_s)
            h = self.replay.wait_burst_head(5.0) or time.monotonic()
            time.sleep(max(0.0, h + RESTART_AFTER_HEAD_S - time.monotonic()))
            status, _ = self.api("POST", "/api/system/reboot", {})
            if not step(status == 202, f"POST /api/system/reboot inside an epoch: HTTP {status}"):
                self.replay.set_last_frame_delay(0)
                return
            self.planned_reboots += 1
            hold_at = h + RESTART_HOLD_AFTER_HEAD_S
            deadline = time.monotonic() + 10
            ended = None
            while time.monotonic() < deadline and ended is None:
                if time.monotonic() >= hold_at:
                    self.replay.hold()   # once; later calls do nothing
                ended = next((c for c in self.sources() if c["n"] == n and c.get("end_mono") is not None), None)
                time.sleep(0.01)
            self.replay.pause()
            self.replay.set_last_frame_delay(0)
            time.sleep(5)
            self.cookie = None
            back = self.wait_http(self.args.timeout)
            self.replay.resume()
            if not step(back and self.login(first=False), f"back after restart {k + 1} (inside an epoch)"):
                return
            shutdown = self.console_text()[at:].split(smoke.BOOT_MARK)[0]
            drained = DRAINED_LINE in shutdown
            cut = [x.strip()[:160] for x in shutdown.splitlines() if CUT_CLOSE_LINE in x]
            if ended is None or cut:
                self.check(False, f"restart {k + 1}: the caster session "
                                  + ("did not end within 10 s" if ended is None else f"was cut: {cut[0]}"))
                return
            if drained:
                self.check(True, f"restart {k + 1} came inside an epoch: the session finished it, then closed "
                                 f"(connection {n}; its record is checked below)")
                self.sgt_spans.append((n + 1, "since the restart inside an epoch"))
                self.check(self.streams_again(n, 90) and self.two_more_epochs(f"after restart {k + 1}"),
                           f"slot 1 streaming again after restart {k + 1}, two epochs sent")
                return
            self.note(f"restart {k + 1}: no epoch in flight when the casters closed (the request came late); "
                      f"trying again")
        self.check(False, f"none of {attempts} restarts came inside an epoch: the drain at the restart is untested")

    def signal_test(self) -> None:
        t_scn = time.monotonic()
        if self.args.caster == "docker":
            self.skip("signal-test: needs the fake caster's per-session records (not with --caster docker)")
            return
        self.stream_slot()
        first_conn = max((c["n"] for c in self.caster.all_connections()), default=0)
        self.sgt_spans = [(max(first_conn, 1), "since the scenario's start")]
        resp = Um982Responder(group=(4, 5), restart_s=3.0)
        self.replay.set_responder(resp)
        self.responder = resp

        # 1. A fixed profile through PUT: the station identifies the receiver, switches it to 3 6, locks.
        #    The stream's own position when it carries one (1005/1006), else a made-up one (the
        #    responder takes any base position).
        arp = self.gnss().get("arp") or {}
        if not arp.get("valid"):
            arp = {"lat_deg": 34.052235, "lon_deg": -118.243683, "height_m": 89.5}
        prof = dict(self.gnss().get("profile") or {})
        prof.update(signal_group_master=3, signal_group_slave=6, base_mode="fixed",
                    fixed={"lat_deg": round(arp["lat_deg"], 9), "lon_deg": round(arp["lon_deg"], 9),
                           "height_m": round(arp["height_m"], 4)})
        status, data = self.api("PUT", "/api/gnss/profile", {"profile": prof, "autocfg": True})
        self.save("sgt-put-profile.json", data)
        if not self.check(status == 202, f"PUT /api/gnss/profile (fixed, 3 6): HTTP {status} {data}"):
            return
        g = self.receiver_ready((3, 6), 150)
        if not self.check(g is not None and resp.signal_group() == (3, 6),
                          f"receiver identified, configured on 3 6 and locked (responder on {resp.signal_group()}, "
                          f"{resp.restarts} restart(s))"):
            self.save("sgt-gnss-not-ready.json", self.gnss())
            return

        # 2. Refusals.
        status, _ = self.api("PUT", "/api/gnss/profile", {"autocfg": False})
        status2, data = self.sgt_post([(3, 6), (7, 0)], SGT_PHASE_S)
        code = (data or {}).get("error", {}).get("code") if isinstance(data, dict) else None
        self.check(status == 202 and status2 == 409 and code == "autocfg_off",
                   f"comparison refused with automatic configuration off: HTTP {status2} {code}")
        self.api("PUT", "/api/gnss/profile", {"autocfg": True})
        status, _ = self.api("POST", "/api/gnss/maintenance", {"duration_s": 900})
        status2, data = self.sgt_post([(3, 6), (7, 0)], SGT_PHASE_S)
        code = (data or {}).get("error", {}).get("code") if isinstance(data, dict) else None
        self.check(status == 200 and status2 == 409 and code == "receiver_maintenance",
                   f"comparison refused in receiver maintenance: HTTP {status2} {code}")
        self.api("DELETE", "/api/gnss/maintenance")
        if not self.check(self.receiver_ready((3, 6), 120) is not None, "streaming again after maintenance"):
            return

        # 3. 7 0 refused by the receiver: rejected, back on 3 6, no config_failed.
        resp.reject = {(7, 0)}
        status, data = self.sgt_post([(7, 0), (5, 0)], SGT_PHASE_S)
        if self.check(status == 202, f"POST /api/gnss/signal-test 7 0 vs 5 0 (7 0 refused): HTTP {status} {data}"):
            t = self.poll(lambda: (lambda x: x if x.get("state") == "idle" and x.get("outcome") else None)(self.sgt()),
                          120, every=2.0)
            self.save("sgt-rejected.json", t)
            res0 = ((t or {}).get("results") or [{}])[0]
            self.check(t is not None and t.get("outcome") == "rejected" and res0.get("status") == "rejected",
                       f"outcome rejected: {(t or {}).get('outcome')}, {(t or {}).get('reason')!r}")
            g = self.receiver_ready((3, 6), 120) or self.gnss()
            pr = g.get("profile") or {}
            refused = g.get("signal_group_refused") or {}
            self.check((pr.get("signal_group_master"), pr.get("signal_group_slave")) == (3, 6)
                       and not g.get("config_failed") and resp.signal_group() == (3, 6),
                       f"profile back on 3 6, config_failed {g.get('config_failed')}, receiver on {resp.signal_group()}")
            self.check((refused.get("master"), refused.get("slave")) == (7, 0),
                       f"GET /api/gnss signal_group_refused names 7 0: {refused}")
        resp.reject = set()

        # 4. 3 6 against 7 0, with a restart while measuring the first.
        status, data = self.sgt_post([(3, 6), (7, 0)], SGT_PHASE_S)
        if not self.check(status == 202, f"POST /api/gnss/signal-test 3 6 vs 7 0, phase_s {SGT_PHASE_S}: HTTP {status} {data}"):
            return
        trace: list[tuple[float, str, object]] = []

        def look() -> dict:
            t = self.sgt()
            key = (t.get("state"), t.get("phase"))
            if not trace or (trace[-1][1], trace[-1][2]) != key:
                trace.append((time.monotonic(), key[0], key[1]))
                self.say(f"signal test: {key[0]} phase {key[1]}")
            return t
        t = self.poll(lambda: (lambda x: x if x.get("state") == "measuring" and
                               (x.get("results") or [{}])[0].get("measured_s", 0) >= 20 else None)(look()), 180, every=1.0)
        if not self.check(t is not None, "measuring 3 6 (no switch: it runs already), 20 s measured"):
            self.save("sgt-run-stuck.json", self.sgt())
            return
        measured = t["results"][0]["measured_s"]
        self.say(f"restart while measuring ({measured} s measured)")
        last = max((c["n"] for c in self.sources()), default=0)
        self.sgt_spans.append((last + 1, "since the restart in the comparison"))
        self.replay.pause()
        time.sleep(2)
        status, _ = self.api("POST", "/api/system/reboot", {})
        if not self.check(status == 202, f"POST /api/system/reboot during the comparison: HTTP {status}"):
            self.replay.resume()
            return
        self.planned_reboots += 1
        time.sleep(5)
        self.cookie = None
        back = self.wait_http(self.args.timeout)
        self.replay.resume()
        if not self.check(back and self.login(first=False), "back after the restart during the comparison"):
            return
        trace.append((time.monotonic(), "restart", None))
        t = look()
        res0 = (t.get("results") or [{}])[0]
        self.check(t.get("state") in ("settling", "measuring") and t.get("phase") == 0 and
                   res0.get("measured_s", 0) >= measured,
                   f"the comparison resumed: {t.get('state')} phase {t.get('phase')}, {res0.get('measured_s')} s "
                   f"measured (at least {measured})")
        t = self.poll(lambda: (lambda x: x if x.get("state") in ("deciding", "idle") else None)(look()), 420, every=1.0)
        self.save("sgt-deciding.json", t if t is not None else self.sgt())
        if not self.check(t is not None and t.get("state") == "deciding", "deciding after both groups"):
            return
        states = [x[1] for x in trace]
        want = ["settling", "measuring", "restart", "settling", "measuring", "switching", "settling", "measuring",
                "deciding"]
        it = iter(states)
        in_order = all(any(x == w for x in it) for w in want)
        self.check(in_order, f"states in order: {' > '.join(states)}")

        def span(name: str, phase: int) -> float | None:
            idx = [k for k, x in enumerate(trace) if x[1] == name and x[2] == phase]
            if not idx:
                return None
            k = idx[-1]
            return trace[k + 1][0] - trace[k][0] if k + 1 < len(trace) else None
        settle1, switch1, measure1 = span("settling", 1), span("switching", 1), span("measuring", 1)
        self.check(settle1 is not None and 55 <= settle1 <= 60 * 1.2 + 10 and switch1 is not None and switch1 <= 90
                   and measure1 is not None and SGT_PHASE_S - 5 <= measure1 <= SGT_PHASE_S * 3,
                   f"timing: switch {switch1 and round(switch1)} s, settle {settle1 and round(settle1)} s, "
                   f"measure {measure1 and round(measure1)} s (phase_s {SGT_PHASE_S})")
        res = t.get("results") or []
        ok_res = len(res) == 2 and all(r.get("status") == "done" and r.get("measured_s", 0) >= SGT_PHASE_S and
                                       r.get("systems") for r in res)
        self.check(ok_res, "both results: " + "; ".join(f"{r.get('group')}: {r.get('status')}, {r.get('measured_s')} s, "
                                                         f"best {r.get('best_sys')}/{r.get('best_bands')}" for r in res))
        status, _ = self.api("POST", "/api/gnss/signal-test/keep", {"group": 1})
        t = self.poll(lambda: (lambda x: x if x.get("state") == "idle" else None)(self.sgt()), 120, every=1.0)
        self.save("sgt-kept.json", t)
        g = self.gnss()
        pr = g.get("profile") or {}
        self.check(status == 202 and (t or {}).get("outcome") == "kept" and
                   (pr.get("signal_group_master"), pr.get("signal_group_slave")) == (7, 0) and resp.signal_group() == (7, 0),
                   f"keep 1: outcome {(t or {}).get('outcome')}, profile {pr.get('signal_group_master')} "
                   f"{pr.get('signal_group_slave')}, receiver on {resp.signal_group()}")

        # 5. Pauses at any moment of the second: the same profile applied again and again (each apply is a
        #    pause for receiver work), a random wait between them. Whenever the station decides to pause, the
        #    records below must show that pause between two epochs.
        self.pause_storm(SGT_PAUSES)
        self.pause_storm(SGT_PAUSES, LATE_LAST_S)
        # 5b. Planned closes inside an epoch: new settings, then a restart, while each epoch's last MSM comes
        #     late. The session sends the rest of its epoch before it closes; the records below show it.
        self.planned_closes(SGT_EDITS, LATE_LAST_S)
        self.restart_in_epoch(LATE_LAST_S)

        # 6. Every caster record of the scenario: whole epochs across every switch, pause and the restarts,
        #    against the fixture as the configured receiver sends it (its own 1005/1033/1230 open every epoch). A
        #    session that has ended (the station closed it during a pause, for new settings, or a restart did)
        #    ends on a whole epoch, and so does the part of the first session before the configuration's pause.
        time.sleep(3)
        station = resp.station_messages()
        configured = with_station(self.schedule, station, resp.station_types())
        self.check({rtcm.msg_type(rtcm.payload_of(f)) for f in station} == {1005, 1033, 1230},
                   f"the receiver sends the station messages the profile asks for: "
                   f"{sorted(rtcm.msg_type(rtcm.payload_of(f)) for f in station)}")
        # The session that streamed when the scenario began runs through the first configuration (a pause
        # keeps the session, unless it reaches the idle limit and the station closes it): the part before the
        # receiver's own station messages is the fixture as recorded. One check per span of the scenario (since its
        # start, since the restart in the comparison, since each save, since the restart inside an epoch) over the
        # records of its sessions: how many sessions a span has (a pause that reached the idle limit ends one,
        # the restart may take more tries) does not change the checks, and each span has carried epochs by now.
        conns = [c for c in self.caster.all_connections() if c["n"] >= max(first_conn, 1) and c["role"] == "source"]
        checked = 0
        spans = sorted(self.sgt_spans)
        judged: list[list[tuple[bool, str]]] = [[] for _ in spans]
        for c in conns:
            ended = c.get("end_mono") is not None   # read before the record: an ended record is complete
            path = self.out / "caster" / f"record-{c['n']:03d}.rtcm3"
            data = path.read_bytes() if path.exists() else b""
            before, after = split_at_station(data, station)
            span = max(i for i, (first, _) in enumerate(spans) if first <= c["n"])
            for part, sched, what, whole in ((before, self.schedule, "as recorded", ended or bool(after)),
                                             (after, configured, "configured", ended)):
                if not carries_epoch(part):
                    continue   # a session (or the part of one) that carried no epoch
                ok, detail, _sec = check_record(part, sched, whole_tail=whole)
                judged[span].append((ok, f"record {c['n']} ({what}): {detail}"))
                checked += 1
        for (_, what), parts in zip(spans, judged):
            self.check(bool(parts) and all(ok for ok, _ in parts),
                       f"signal-test caster records {what}: "
                       + ("; ".join(text + ("" if ok else " (FAILED)") for ok, text in parts) or "none carried an epoch"))
        self.check(checked >= 1, f"{checked} caster record(s) of the signal-test scenario checked")
        # Holds the station had to force (no epoch boundary within its 1 s wait) and the long ones it noted.
        for line in self.console_text().splitlines():
            if "station: casters held" in line:
                self.note(f"console: {line.strip()[:160]}")
        self.note(f"signal-test scenario took {time.monotonic() - t_scn:.0f} s")

    # -- console ----------------------------------------------------------------

    def console_checks(self) -> None:
        text = self.console.read_text(encoding="utf-8", errors="replace") if self.console.exists() else ""
        for marker in smoke.FATAL:
            for line in text.splitlines():
                if marker in line:
                    self.check(False, f"console: {line.strip()[:160]}")
                    break
        r = smoke.boot_report(text)
        n = r.boots
        allowed = 2 + self.planned_reboots
        self.check(n >= 1, f"{n} boot(s), {self.planned_reboots} of them asked for")
        self.check(not r.unplanned, smoke.boot_text(r))
        self.check(not r.off_pro, smoke.runner_text(r, len(smoke.OWN_REBOOT.findall(text))))
        self.check(n <= allowed, f"at most {allowed} boots (no reboot loop)")
        self.check(smoke.LADDER_REBOOT not in text, "no GNSS ladder restart during the run")

    # -- run ----------------------------------------------------------------------

    def run(self) -> int:
        try:
            self.start()
            if self.boot():
                # The guard first: the station repeats its unanswered receiver
                # query every 10 min, pausing the casters, and the guard's
                # timings are best measured away from that (its reboot starts
                # the clock again for the stream scenario).
                if self.args.scenario in ("onocoy-guard", "all"):
                    try:
                        self.onocoy_guard()
                    except SkipScenario as e:
                        self.skip(str(e))
                if self.args.scenario in ("stream", "all"):
                    self.stream()
                if self.args.scenario in ("signal-test", "all"):
                    self.signal_test()
            alive = self.proc.poll() is None
            self.check(alive, "QEMU still running at the end")
        except (OSError, http.client.HTTPException, ValueError, KeyError, RuntimeError) as e:
            self.check(False, f"{type(e).__name__}: {e}")
        finally:
            self.stop()
            self.console_checks()
        if smoke.slow_requests():
            self.note(f"{smoke.slow_requests()} request(s) took {smoke.SLOW_S:.0f} s or more: watched in "
                      f"{self.out / 'slow-requests.log'} (packets in qemu-net.pcap)")
        failed = [t for s, t in self.results if s == "FAIL"]
        summary = "\n".join(f"{s} {t}" for s, t in self.results)
        (self.out / "summary.txt").write_text(summary + "\n", encoding="utf-8")
        skipped = sum(1 for s, _ in self.results if s == "SKIP")
        print(f"\n{len(self.results) - len(failed) - skipped} passed, {len(failed)} failed, {skipped} skipped; "
              f"logs in {self.out}")
        self.events.close()
        return 1 if failed else 0


def _exit_on_signal(signum, frame) -> None:
    raise SystemExit(128 + signum)


def main() -> int:
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, _exit_on_signal)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(ROOT / "build-qemu" / "e2e"), help="output directory")
    ap.add_argument("--scenario", choices=("stream", "onocoy-guard", "signal-test", "all"), default="all")
    ap.add_argument("--long", action="store_true",
                    help="also the 5-minute wait after a rejected login and the whole in-use retry ladder")
    ap.add_argument("--caster", choices=("fake", "docker"), default="fake",
                    help="stream target: the fake caster (default) or the BKG caster in Docker")
    ap.add_argument("--fixture", default=str(DEFAULT_FIXTURE), help="RTCM file or capture run to replay")
    ap.add_argument("--observe", type=int, default=75,
                    help=f"seconds of caster record to collect and check (at least {MIN_RECORD_S:.0f})")
    ap.add_argument("--timeout", type=int, default=150, help="seconds for the web server to answer after a boot")
    args = ap.parse_args()
    if args.observe < MIN_RECORD_S:
        ap.error(f"--observe must be at least {MIN_RECORD_S:.0f}")
    if not os.environ.get("IDF_PATH"):
        print("e2e.py: run . $IDF_PATH/export.sh first (tools/qemu/run.sh needs it)", file=sys.stderr)
        return 2
    try:
        schedule = load(args.fixture)
    except (OSError, ValueError) as e:
        print(f"e2e.py: --fixture: {e}", file=sys.stderr)
        return 2
    return E2E(args, schedule).run()


if __name__ == "__main__":
    sys.exit(main())
