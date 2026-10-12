# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""ntrip_timing.py: exact latencies from a synthetic capture (GPS, BeiDou,
GLONASS time scales), a live capture through fake_caster.py's relay, and
the password never reaching the output or the run directory."""

from __future__ import annotations

import base64
import json
import os
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from benchlib.rtcm import build_frame
from builders import arp_payload, msm_frame
from fake_caster import FakeCaster
from ntrip_timing import (
    GPS_EPOCH_UNIX, GPS_UTC_S, analyse, build_request, latency_s, percentile, read_reply,
)
from benchlib import rtcm

BENCH = Path(__file__).resolve().parents[1]
CELLS = {(3, 2): 40.0, (9, 2): 43.0}
GLO_CELLS = {(2, 2): 40.0}


def gps_tow_ms(unix: float) -> int:
    return round(((unix - GPS_EPOCH_UNIX + GPS_UTC_S) % 604800) * 1000)


def glo_epoch(unix: float) -> int:
    """GLONASS MSM epoch field: day of week (Moscow time) and time of day in ms."""
    moscow = unix + 3 * 3600
    dow = int(((moscow // 86400) + 4) % 7)  # 1970-01-01 was a Thursday; Sunday = 0
    tod = round((moscow % 86400) * 1000)
    return (dow << 27) | tod


def epoch_frames(unix: float) -> list[bytes]:
    """One epoch stamped at `unix`: GPS, GLONASS, then BeiDou closing it."""
    gps = gps_tow_ms(unix)
    return [
        msm_frame(1077, 1, gps, True, CELLS),
        msm_frame(1087, 1, glo_epoch(unix), True, GLO_CELLS),
        msm_frame(1127, 1, gps - 14000, False, CELLS),
    ]


def test_latency_time_scales():
    t = 1760000000.0
    gps = rtcm.decode_msm(rtcm.payload_of(msm_frame(1077, 1, gps_tow_ms(t - 0.25), False, CELLS)))
    bds = rtcm.decode_msm(rtcm.payload_of(msm_frame(1127, 1, gps_tow_ms(t - 0.4) - 14000, False, CELLS)))
    glo = rtcm.decode_msm(rtcm.payload_of(msm_frame(1087, 1, glo_epoch(t - 0.6), False, GLO_CELLS)))
    assert latency_s(gps, t) == pytest.approx(0.25, abs=1e-6)
    assert latency_s(bds, t) == pytest.approx(0.4, abs=1e-6)
    assert latency_s(glo, t) == pytest.approx(0.6, abs=1e-6)
    # Across the GPS week and the GLONASS day boundaries.
    week_start = GPS_EPOCH_UNIX - GPS_UTC_S + 604800 * 2400
    late = rtcm.decode_msm(rtcm.payload_of(msm_frame(1077, 1, gps_tow_ms(week_start - 0.5), False, CELLS)))
    assert latency_s(late, week_start + 0.3) == pytest.approx(0.8, abs=1e-6)
    midnight = 1760054400.0 - 3 * 3600  # 00:00 Moscow time
    g = rtcm.decode_msm(rtcm.payload_of(msm_frame(1087, 1, glo_epoch(midnight - 0.2), False, GLO_CELLS)))
    assert latency_s(g, midnight + 0.1) == pytest.approx(0.3, abs=1e-6)


def test_percentile():
    assert percentile([1, 2, 3, 4], 50) == 2.5
    assert percentile([5], 95) == 5
    assert percentile(list(range(101)), 95) == 95


def write_capture(d: Path, stem: str, reads: list[tuple[float, bytes]], part: int = 0, offset: int = 0) -> int:
    data = b"".join(r for _, r in reads)
    (d / f"{stem}-{part:04d}.bin").write_bytes(data)
    lines = []
    for t, r in reads:
        lines.append(f"{t:.6f},{offset},{len(r)}")
        offset += len(r)
    (d / f"{stem}-{part:04d}.idx").write_text("\n".join(lines) + "\n")
    return offset


@pytest.mark.parametrize("stem", ["stream", "receiver"])
def test_synthetic_capture_gives_exact_latencies(tmp_path, stem):
    base = 1760000000.0
    reads = []
    # Epoch k is stamped at base + k; its GPS MSM arrives 0.2 s later, GLONASS
    # 0.3 s, BeiDou 0.45 s (separate reads), plus a 1005 between epochs.
    for k in range(10):
        ep = base + k
        g, r, b = epoch_frames(ep)
        reads += [(ep + 0.2, g), (ep + 0.3, r), (ep + 0.45, b[:20]), (ep + 0.46, b[20:])]
        if k % 3 == 0:
            reads.append((ep + 0.5, build_frame(arp_payload(1, 1113194.9, -4842853.7, 3985496.0))))
    half = len(reads) // 2
    off = write_capture(tmp_path, stem, reads[:half])
    write_capture(tmp_path, stem, reads[half:], part=1, offset=off)
    d = analyse(tmp_path)
    assert d["systems"]["GPS"]["epochs"] == 10
    assert d["systems"]["GPS"]["p50_ms"] == pytest.approx(200.0, abs=0.01)
    assert d["systems"]["GPS"]["max_ms"] == pytest.approx(200.0, abs=0.01)
    assert d["systems"]["GLONASS"]["p95_ms"] == pytest.approx(300.0, abs=0.01)
    # The read that completed the BeiDou frame is the second one.
    assert d["systems"]["BeiDou"]["p50_ms"] == pytest.approx(460.0, abs=0.01)
    assert d["overall"]["epochs"] == 10 and d["overall"]["p50_ms"] == pytest.approx(200.0, abs=0.01)
    assert d["verdict"] == "PASS"
    assert all(s["type"] == 1077 for s in d["samples"])


def test_a_late_stream_fails(tmp_path):
    base = 1760000000.0
    reads = []
    for k in range(5):
        for f in epoch_frames(base + k):
            reads.append((base + k + 1.5, f))
    write_capture(tmp_path, "stream", reads)
    d = analyse(tmp_path)
    assert d["verdict"] == "FAIL" and d["overall"]["p95_ms"] == pytest.approx(1500.0, abs=0.01)
    out = subprocess.run([sys.executable, "ntrip_timing.py", "report", str(tmp_path)], cwd=BENCH,
                         capture_output=True, text=True)
    assert out.returncode == 1 and "FAIL" in out.stdout


def test_request_and_reply_parsing():
    r = build_request("127.0.0.1", 2101, "BENCH", "testuser01", "fakepass123", 2).decode()
    assert r.startswith("GET /BENCH HTTP/1.1\r\n") and "Ntrip-Version: Ntrip/2.0\r\n" in r and r.endswith("\r\n\r\n")
    assert "Authorization: Basic " + base64.b64encode(b"testuser01:fakepass123").decode() in r
    r1 = build_request("127.0.0.1", 2101, "BENCH", None, None, 1).decode()
    assert r1.startswith("GET /BENCH HTTP/1.0\r\n") and "Authorization" not in r1
    a, b = socket.socketpair()
    with a, b:
        a.sendall(b"ICY 200 OK\r\n\xd3\x00\x01")
        head, body, chunked = read_reply(b)
        assert (head, body, chunked) == (b"ICY 200 OK\r\n", b"\xd3\x00\x01", False)
        a.sendall(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n")
        head, body, chunked = read_reply(b)
        assert chunked and body == b"3\r\nabc\r\n" and head.endswith(b"\r\n\r\n")


def run_source(port: int, stop: threading.Event, delay_s: float) -> None:
    """An NTRIP 1.0 source sending one epoch every 0.2 s, stamped now - delay."""
    with socket.create_connection(("127.0.0.1", port)) as s:
        s.sendall(b"SOURCE bench-pass-1 /BENCH\r\nSource-Agent: NTRIP bench-test/1.0\r\n\r\n")
        s.settimeout(5)
        assert s.recv(12) == b"ICY 200 OK\r\n"
        while not stop.is_set():
            s.sendall(b"".join(epoch_frames(time.time() - delay_s)))
            stop.wait(0.2)


@pytest.mark.parametrize("version", [1, 2])
def test_capture_through_the_relay_measures_the_delay_and_hides_the_password(tmp_path, version):
    caster = FakeCaster(mount="BENCH", relay=True, say=lambda m: None).start()
    stop = threading.Event()
    src = threading.Thread(target=run_source, args=(caster.port, stop, 0.25), daemon=True)
    src.start()
    secret = "fakepass123"
    env = dict(os.environ, TIMING_PW=secret)
    try:
        out = subprocess.run(
            [sys.executable, "ntrip_timing.py", "capture", "--host", "127.0.0.1", "--port", str(caster.port),
             "--mount", "BENCH", "--user", "testuser01", "--password-env", "TIMING_PW", "--ntrip-version",
             str(version), "--duration", "3", "--log-dir", str(tmp_path), "--label", "relay"],
            cwd=BENCH, capture_output=True, text=True, env=env, timeout=60)
    finally:
        stop.set()
        src.join(timeout=5)
        caster.stop()
    assert out.returncode == 0, out.stdout + out.stderr
    runs = list(tmp_path.iterdir())
    assert len(runs) == 1
    run = runs[0]
    d = json.loads((run / "report.json").read_text())
    assert d["overall"]["epochs"] >= 8
    assert 200.0 <= d["overall"]["p50_ms"] <= 300.0
    for name in ("GPS", "GLONASS", "BeiDou"):
        assert 200.0 <= d["systems"][name]["p50_ms"] <= 300.0
    assert "PASS p95 < 1000 ms" in out.stdout
    head = (run / "reply-header.txt").read_bytes()
    assert head.startswith(b"ICY 200 OK" if version == 1 else b"HTTP/1.1 200 OK")
    # The password (and its Basic form) appears nowhere.
    b64 = base64.b64encode(f"testuser01:{secret}".encode())
    for blob in (out.stdout.encode(), out.stderr.encode(), *(f.read_bytes() for f in run.iterdir())):
        assert secret.encode() not in blob and b64 not in blob


def test_host_is_required():
    out = subprocess.run([sys.executable, "ntrip_timing.py", "capture", "--mount", "BENCH"], cwd=BENCH,
                         capture_output=True, text=True)
    assert out.returncode == 2 and "--host" in out.stderr


def test_refused_reply_is_reported(tmp_path):
    caster = FakeCaster(say=lambda m: None).start()  # GET without relay: the sourcetable
    try:
        out = subprocess.run([sys.executable, "ntrip_timing.py", "capture", "--host", "127.0.0.1", "--port",
                              str(caster.port), "--mount", "BENCH", "--duration", "2", "--log-dir", str(tmp_path)],
                             cwd=BENCH, capture_output=True, text=True, timeout=30)
    finally:
        caster.stop()
    assert out.returncode == 1 and "SOURCETABLE 200 OK" in out.stderr
