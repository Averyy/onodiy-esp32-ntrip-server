# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The BKG NtripCaster test peer in Docker (tools/bench/docker/bkg-caster).

Opt-in: runs only with RUN_DOCKER_TESTS=1 and a working `docker info`;
otherwise every test reports skipped with the reason. Builds the image,
runs it on a free 127.0.0.1 port, and checks what a source and a client
get. Local only; every credential is a made-up bench value."""

from __future__ import annotations

import os
import shutil
import socket
import subprocess
import time
import uuid
from pathlib import Path

import pytest

from benchlib.stream import StreamSplitter
from builders import msm_frame

pytestmark = pytest.mark.docker

HERE = Path(__file__).resolve().parents[1] / "docker" / "bkg-caster"
IMAGE = os.environ.get("BKG_CASTER_IMAGE", "onocoydiy-bench-bkg-caster")
SOURCE_PW = "bench-src"


def _docker_ok() -> str | None:
    if os.environ.get("RUN_DOCKER_TESTS") != "1":
        return "set RUN_DOCKER_TESTS=1 to build and run the Docker caster"
    if not shutil.which("docker"):
        return "docker not found"
    try:
        r = subprocess.run(["docker", "info", "--format", "{{.ServerVersion}}"], capture_output=True, text=True,
                           timeout=30)
    except (OSError, subprocess.TimeoutExpired) as e:
        return f"docker info failed: {e}"
    if r.returncode != 0:
        return f"docker info failed: {(r.stderr or r.stdout).strip()[:200]}"
    return None


SKIP = _docker_ok()
if SKIP:
    pytestmark = [pytest.mark.docker, pytest.mark.skip(reason=SKIP)]


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


@pytest.fixture(scope="module")
def caster():
    r = subprocess.run([str(HERE / "build.sh"), "-q"], capture_output=True, text=True, timeout=1200)
    assert r.returncode == 0, r.stdout + r.stderr
    port = free_port()
    name = f"onocoydiy-bench-bkg-test-{uuid.uuid4().hex[:8]}"
    r = subprocess.run(["docker", "run", "-d", "--rm", "--name", name, "-p", f"127.0.0.1:{port}:2101", IMAGE],
                       capture_output=True, text=True, timeout=60)
    assert r.returncode == 0, r.stderr
    try:
        deadline = time.monotonic() + 30
        while True:
            try:
                if get(port, b"GET / HTTP/1.0\r\nUser-Agent: NTRIP bench-test/1.0\r\n\r\n").startswith(b"SOURCETABLE"):
                    break
            except OSError:
                pass
            assert time.monotonic() < deadline, "the caster did not answer within 30 s"
            time.sleep(0.5)
        yield port, name
    finally:
        subprocess.run(["docker", "rm", "-f", name], capture_output=True, timeout=60)


def get(port: int, request: bytes, timeout: float = 3.0) -> bytes:
    with socket.create_connection(("127.0.0.1", port), timeout=5) as s:
        s.sendall(request)
        s.settimeout(timeout)
        buf = b""
        try:
            while True:
                d = s.recv(4096)
                if not d:
                    break
                buf += d
        except socket.timeout:
            pass
        return buf


def source(port: int, agent: str, password: str = SOURCE_PW) -> tuple[socket.socket, bytes]:
    s = socket.create_connection(("127.0.0.1", port), timeout=5)
    s.sendall(f"SOURCE {password} /BENCH\r\nSource-Agent: {agent}\r\n\r\n".encode())
    s.settimeout(3)
    buf = b""
    try:
        while b"\n" not in buf:
            d = s.recv(4096)
            if not d:
                break
            buf += d
    except socket.timeout:
        pass
    return s, buf


def source_when_free(port: int, agent: str, password: str = SOURCE_PW) -> bytes:
    """The reply once the caster has let go of an earlier test's source (it
    answers "Mount Point Taken" until it notices that source closed)."""
    deadline = time.monotonic() + 15
    while True:
        s, reply = source(port, agent, password)
        s.close()
        if not reply.startswith(b"ERROR - Mount Point Taken") or time.monotonic() > deadline:
            return reply
        time.sleep(0.5)


def test_sourcetable_lists_the_bench_mountpoint(caster):
    port, _ = caster
    table = get(port, b"GET / HTTP/1.0\r\nUser-Agent: NTRIP bench-test/1.0\r\n\r\n")
    assert table.startswith(b"SOURCETABLE 200 OK\r\n") and b"STR;BENCH;" in table and table.endswith(b"ENDSOURCETABLE\r\n")


def test_ntrip_source_gets_ok_and_its_frames_reach_a_client(caster):
    port, _ = caster
    src, reply = source(port, "NTRIP bench-test/1.0")
    with src:
        assert reply == b"OK\r\n"
        frames = [msm_frame(1077, 3, 1000 * k, False, {(5, 2): 40.0, (9, 2): 44.0}) for k in range(1, 40)]
        src.sendall(frames[0])
        time.sleep(0.5)
        with socket.create_connection(("127.0.0.1", port), timeout=5) as cl:
            cl.sendall(b"GET /BENCH HTTP/1.0\r\nUser-Agent: NTRIP bench-test/1.0\r\n\r\n")
            cl.settimeout(5)
            head = b""
            while b"\r\n" not in head:
                head += cl.recv(1)
            assert head == b"ICY 200 OK\r\n"
            time.sleep(0.3)
            for f in frames[1:]:
                src.sendall(f)
                time.sleep(0.02)
            # Keep the source going (later epochs) until the last test frame
            # has come through.
            got = b""
            deadline = time.monotonic() + 15
            k = 100
            cl.settimeout(0.1)
            while frames[-1] not in got and time.monotonic() < deadline:
                src.sendall(msm_frame(1077, 3, 1000 * k, False, {(5, 2): 40.0}))
                k += 1
                try:
                    got += cl.recv(65536)
                except socket.timeout:
                    pass
    sp = StreamSplitter()
    seen = [it.data for it in sp.feed(got) + sp.flush() if it.kind == "rtcm"]
    ours = [frames.index(f) for f in seen if f in frames]
    # Seen on this build (2026-10-12): a new client starts inside older data
    # (part of a frame first) and misses the first few frames sent after it
    # joined; from then on every frame comes through, in order.
    assert ours and ours[-1] == len(frames) - 1 and ours == list(range(ours[0], len(frames)))
    assert len(ours) >= len(frames) - 8


def test_source_with_a_curl_agent_is_refused(caster):
    port, _ = caster
    # The caster's exact reply, recorded against this build on 2026-10-12
    # (the firmware classes it as an agent refusal).
    assert source_when_free(port, "curl/8.5.0") == b"Not authorized (no NTRIP source)\r\n\r\n"


def test_wrong_source_password_is_refused(caster):
    port, _ = caster
    assert source_when_free(port, "NTRIP bench-test/1.0", password="wrong-pw") == b"ERROR - Bad Password\r\n\r\n"


def test_a_second_source_on_a_held_mountpoint_is_refused(caster):
    port, _ = caster
    assert source_when_free(port, "NTRIP bench-test/1.0", password="wrong-pw").startswith(b"ERROR - Bad")
    first, reply = source(port, "NTRIP bench-test/1.0")
    with first:
        assert reply == b"OK\r\n"
        second, reply2 = source(port, "NTRIP bench-test/1.0")
        second.close()
    assert reply2 == b"ERROR - Mount Point Taken or Invalid\r\n\r\n"
