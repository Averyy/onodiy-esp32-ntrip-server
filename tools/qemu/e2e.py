#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""QEMU end to end: recorded RTCM through the firmware to a caster on this machine.

    uv run tools/qemu/e2e.py [--out DIR] [--scenario stream|onocoy-guard|all] [--long]
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
     password and, once the station's start-up receiver query is over,
     configures slot 1: {name "Bench", host 10.0.2.2, the caster's port,
     NTRIP 1.0, mountpoint BENCH, a made-up password, standard profile,
     enabled};
  scenario "stream":
  3. slot 1 streaming within 60 s ("ICY 200 OK"; "OK" from the BKG caster);
     GET /api/gnss/signals lists the fixture's constellations with their
     bands (computed from the fixture with benchlib); /api/health
     hub.frames_in grows by at least 5 x the fixture's frames per epoch in
     10 s and the slot's epochs_sent rises;
  4. the caster's record holds at least 60 s of frames (--observe): every
     one CRC-valid and byte-identical to a fixture frame, in fixture order
     from the first epoch boundary on (modulo the loop; fixture frames the
     station never forwards, such as the SBAS numbers 1101-1107, are
     skipped), nothing between frames, a partial frame only at the very end;
  scenario "onocoy-guard" (firmware with test-Onocoy slots; SKIP otherwise;
  with --scenario all it runs before "stream"):
  5. slot 0 as a test-Onocoy slot (test_onocoy) against a second fake caster
     scripted with Onocoy's termination reasons: a credential in use on
     credentials that never worked stops the slot at once; the guard's
     waits after rejections, login blocks and in-use replies (each delay in
     [entry, 1.2 x entry] + 2 s for QEMU, from next_retry_s and from the
     connection times); sessions the caster ends quickly (and a "Device
     reconnected" line, which scores 2 at once) stop the slot after a score
     of 3, waiting 5 s at a score of 1 and 10 s at 2; duplicate and banned
     reports stop it at once; a stop survives a reboot (the replay pauses
     across it, see tools/qemu/README.md) and POST /api/casters/0/retry
     clears it.
     --long adds the 5-minute wait after a rejected login and the whole
     in-use retry ladder;
  6. throughout: no panic, abort, watchdog or reboot loop on the console
     (smoke.py's checks).

Everything lands in --out: the console log, replay/ (what the firmware sent
on UART1), caster/ and guard/ (connections.jsonl, the records), the HTTP
answers and summary.txt. Exit status 0 when every check passed (a SKIP
passes), 1 otherwise.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
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
from benchlib.replay import EpochGrouper, Schedule, load, msm_of
from benchlib.stream import StreamSplitter
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
# The firmware maps no band for NavIC and SBAS signals (gnss_rtcm3.c), while
# benchlib's band table gives NavIC L5: the signal view lists those
# constellations without bands.
FIRMWARE_UNMAPPED = {"NavIC", "SBAS"}
STOP_REASONS = ["none", "rejected", "login_blocked", "in_use", "kicked", "duplicate", "banned"]


class SkipScenario(Exception):
    pass


# ---------------------------------------------------------------------------
# fixture expectations and record checks


def expected_signals(schedule: Schedule) -> dict[str, list[str]]:
    """What /api/gnss/signals lists for the fixture: every constellation with
    an MSM the station decodes (not the SBAS numbers), with the bands of the
    signals that carry cells."""
    out: dict[str, set[str]] = {}
    for b in schedule.bursts:
        for f in b.frames:
            m = msm_of(f)
            if m is None:
                continue
            bands = out.setdefault(m.system.lower(), set())
            if m.system in FIRMWARE_UNMAPPED:
                continue
            for c in m.cells:
                band = rtcm.band_of(m.system, c.sig)
                if band:
                    bands.add(band.lower())
    return {k: sorted(v) for k, v in sorted(out.items())}


def frame_type(f: bytes) -> int | None:
    return rtcm.msg_type(rtcm.payload_of(f))


def frames_per_epoch(schedule: Schedule) -> float:
    """Frames per epoch of one loop, epochs as benchlib.replay groups them
    (a capture's bursts are reads, not epochs)."""
    g = EpochGrouper()
    frames = [f for b in schedule.bursts for f in b.frames]
    epochs = sum(1 for f in frames if g.feed(f)[0])
    return len(frames) / epochs if epochs else 0.0


def check_record(data: bytes, schedule: Schedule, leading_junk_ok: bool = False) -> tuple[bool, str, float]:
    """The record against the fixture. Returns (ok, detail, seconds of the
    fixture's time the in-order part of the record covers)."""
    split = StreamSplitter()
    items = split.feed(data) + split.flush()
    fx: list[bytes] = []
    burst_of: list[int] = []
    starts: set[int] = set()
    for i, b in enumerate(schedule.bursts):
        starts.add(len(fx))
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
    # Start at an epoch's (a burst's) first frame that occurs only once in the
    # fixture: a 1005 or 1230 can repeat unchanged, an MSM cannot.
    openers: set[int] = set()
    for s in starts:
        q = s
        while q < len(fx) and burst_of[q] == burst_of[s]:
            if len(pos[fx[q]]) == 1:
                openers.add(q)
                break
            q += 1
    k0 = next((k for k, it in enumerate(frames) if len(pos[it.data]) == 1 and pos[it.data][0] in openers), None)
    if k0 is None:
        return False, "no epoch boundary in the record", 0.0
    seen_types = {frame_type(it.data) for it in frames[k0:]}
    skipped_types: dict[int, int] = {}
    p = pos[frames[k0].data][0]
    epochs = 1
    seconds = 0.0
    times = [b.t for b in schedule.bursts]
    for k in range(k0 + 1, len(frames)):
        q = (p + 1) % len(fx)
        hops = 0
        while fx[q] != frames[k].data:
            t = frame_type(fx[q])
            if t in seen_types:
                return False, (f"record frame #{k} (type {frame_type(frames[k].data)}) is not the next fixture frame: "
                               f"fixture frame {q} (type {t}) is missing"), 0.0
            skipped_types[t] = skipped_types.get(t, 0) + 1
            q = (q + 1) % len(fx)
            hops += 1
            if hops > len(fx):
                return False, f"record frame #{k} not found after fixture frame {p}", 0.0
        if burst_of[q] != burst_of[p] or q <= p:
            dt = times[burst_of[q]] - times[burst_of[p]]
            seconds += dt if q > p else dt + schedule.duration  # across the loop's seam
        if q in openers:
            epochs += 1
        p = q
    unit = "epochs" if schedule.kind == "rtcm" else "recorded reads"
    detail = (f"{len(frames)} frames, {len(frames) - k0} from the first epoch boundary in fixture order over "
              f"{epochs} {unit}; skipped fixture types never forwarded: "
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
        self.cookie: str | None = None
        self.password = "e2e-" + secrets.token_hex(8)
        self.http_port = smoke.free_port()
        self.planned_reboots = 0
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
                status, _h, _b = smoke.request(self.http_port, "GET", "/", timeout=10)
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
        cmd = [str(smoke.RUN_SH), "--no-build", "--headless", str(self.console), "--uart1",
               f"connect:{self.replay.port}", "--http-port", str(self.http_port), "--seconds", str(int(backstop))]
        print("starting: " + " ".join(cmd), flush=True)
        self.console.unlink(missing_ok=True)
        self.proc = subprocess.Popen(cmd, stdout=open(self.out / "run.log", "w"), stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)

    def budget_s(self) -> float:
        """Upper bound on the whole run (run.sh's backstop)."""
        s = 240 + self.args.observe
        if self.args.scenario in ("onocoy-guard", "all"):
            s += 900 + (900 if self.args.long else 0)
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
        """Streaming, and the start-up receiver query over (it pauses the casters while it runs)."""
        def ready():
            h = self.health()
            st = h.get("station", {})
            return st.get("state") == "streaming" and not st.get("lease_held") and \
                st.get("since_lease_release_ms") is not None and h
        h = self.poll(ready, 90)
        if h is None:
            h = self.health()
            st = h.get("station", {})
            ok = st.get("state") == "streaming"
            self.check(ok, f"station streaming the replay (state {st.get('state')}; no receiver query seen in 90 s)")
            return ok
        self.check(True, f"station streaming the replay; its start-up receiver query is over "
                         f"(hub {h['hub']['frames_in']} frames in)")
        return True

    # -- scenario: stream -------------------------------------------------------

    def stream(self) -> None:
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
        status, data = self.api("PUT", "/api/casters/1", body)
        self.save("put-caster-1.json", data)
        if not self.check(status == 200, f"PUT /api/casters/1 (slot 1 'Bench' -> {HOST_FROM_GUEST}:{port}): "
                                         f"HTTP {status}"):
            return
        t_put = time.monotonic()

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
        def started():
            c = [x for x in self.guard.all_connections() if x["n"] == n]
            return c[0] if c else None
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
        # Every class but "none": the eleven of every slot and the guard's three.
        self.check(len(fb) == 14 and {"login_blocked", "in_use", "replaced"} <= set(fb),
                   f"failures_by_class has {len(fb)} classes")
        if self.wait_stopped("in_use"):
            self.quiet(15, "stopped (in use)")

        # 2. A rejected login on credentials that never worked: 5 minutes.
        n = self.retry("onocoy_auth_failed")
        self.next_retry_after(n, 300, "rejected login, credentials never worked")
        if self.args.long:
            self.gap(n, 300, "rejected login: the wait before the second attempt", timeout=420)
            self.next_retry_after(n + 1, 1800, "second rejected login")
        self.quiet(10, "waiting after a rejected login")

        # 3. Too many unsuccessful logins: 60 minutes.
        n = self.retry("onocoy_login_blocked")
        self.next_retry_after(n, 3600, "login blocked")
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
        self.journal_check()

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

    # -- console ----------------------------------------------------------------

    def console_checks(self) -> None:
        text = self.console.read_text(encoding="utf-8", errors="replace") if self.console.exists() else ""
        for marker in smoke.FATAL:
            for line in text.splitlines():
                if marker in line:
                    self.check(False, f"console: {line.strip()[:160]}")
                    break
        boots = text.split(smoke.BOOT_MARK)
        n = len(boots) - 1
        crashes = sum(1 for before in boots[1:-1] if not smoke.OWN_REBOOT.search(before))
        allowed = 2 + self.planned_reboots
        self.check(n >= 1, f"{n} boot(s), {self.planned_reboots} of them asked for")
        self.check(crashes == 0, f"{crashes} restart(s) without the firmware's own reboot line")
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
            alive = self.proc.poll() is None
            self.check(alive, "QEMU still running at the end")
        except (OSError, http.client.HTTPException, ValueError, KeyError, RuntimeError) as e:
            self.check(False, f"{type(e).__name__}: {e}")
        finally:
            self.stop()
            self.console_checks()
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
    ap.add_argument("--scenario", choices=("stream", "onocoy-guard", "all"), default="all")
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
