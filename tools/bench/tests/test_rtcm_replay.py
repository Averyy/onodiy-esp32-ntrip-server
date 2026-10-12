# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""rtcm_replay.py and benchlib.replay: epoch grouping, schedules, captures,
and the TCP server (byte identity over loops, pacing, what the client sends)."""

from __future__ import annotations

import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from benchlib import rtcm
from benchlib.replay import EpochGrouper, load, load_capture, load_rtcm, read_idx
from benchlib.stream import StreamSplitter
from builders import msm_frame
from rtcm_replay import ReplayServer, parse_hostport

BENCH = Path(__file__).resolve().parents[1]
FIX = BENCH.parents[1] / "test" / "fixtures" / "rtcm"
LAX = FIX / "centipede_LAX_30ep.rtcm3"


def frames_of(data: bytes) -> list[bytes]:
    sp = StreamSplitter()
    return [it.data for it in sp.feed(data) + sp.flush() if it.kind == "rtcm"]


def recv_exactly(sock: socket.socket, n: int, timeout: float = 20.0) -> tuple[bytes, list[tuple[float, int]]]:
    """Read n bytes; also the (monotonic time, total so far) of every read."""
    sock.settimeout(timeout)
    buf = bytearray()
    marks = []
    while len(buf) < n:
        data = sock.recv(min(65536, n - len(buf)))
        if not data:
            break
        buf += data
        marks.append((time.monotonic(), len(buf)))
    return bytes(buf), marks


# -- grouping ---------------------------------------------------------------------


def test_lax_fixture_groups_into_30_epochs_with_frames_in_file_order():
    s = load_rtcm(LAX)
    assert s.kind == "rtcm"
    assert len(s.bursts) == 30
    all_frames = frames_of(LAX.read_bytes())
    assert [f for b in s.bursts for f in b.frames] == all_frames
    assert s.data() == LAX.read_bytes()
    for b in s.bursts:
        # Each epoch's first MSM is GPS (its one 1230 comes in front of an
        # epoch, after the previous one closed) and its MSMs share one epoch
        # time per constellation.
        msms = [rtcm.decode_msm(rtcm.payload_of(f)) for f in b.frames
                if rtcm.msm_system(rtcm.msg_type(rtcm.payload_of(f)))
                and not rtcm.is_suspect_sbas(rtcm.msg_type(rtcm.payload_of(f)))]
        assert msms[0].mtype == 1077
        assert not msms[-1].mmb and all(m.mmb for m in msms[:-1])
        assert len({m.epoch for m in msms if m.system == "GPS"}) == 1
    # BeiDou's epoch time is 14 s behind GPS in this stream: still one epoch.
    assert s.bursts[0].epoch_s["BeiDou"] == s.bursts[0].epoch_s["GPS"] - 14


@pytest.mark.parametrize("speed, gap", [(1.0, 1.0), (2.0, 0.5)])
def test_schedule_spacing_follows_the_epoch_times_and_the_speed(speed, gap):
    s = load(LAX)
    times = [t for _k, _i, t, _b in s.iter_times(loop=False, speed=speed)]
    assert len(times) == 30
    for a, b in zip(times, times[1:]):
        assert b - a == pytest.approx(gap, abs=0.001)


def test_looping_keeps_a_continuous_time_base():
    s = load(LAX)
    it = s.iter_times(loop=True, speed=1.0)
    seq = [next(it) for _ in range(65)]
    times = [t for _k, _i, t, _b in seq]
    assert [k for k, _i, _t, _b in seq][29:31] == [0, 1]
    for a, b in zip(times, times[1:]):
        assert b - a == pytest.approx(1.0, abs=0.001)
    assert seq[30][3] is s.bursts[0]


def test_epoch_without_its_closing_msm_ends_at_the_next_epoch_time():
    # ROBU1: one epoch's closing MSM never comes (1077 and 1087 only, both with the bit set).
    s = load_rtcm(FIX / "centipede_ROBU1_35ep.rtcm3")
    assert len(s.bursts) == 35
    assert [b.t for b in s.bursts] == pytest.approx([float(i) for i in range(35)], abs=0.001)


def test_beidou_on_gps_time_still_groups_per_constellation():
    # This encoder puts GPS time into its BeiDou MSMs (no 14 s offset).
    s = load_rtcm(FIX / "rtklib_GMSD7_12ep.rtcm3")
    assert len(s.bursts) == 12
    assert [b.t for b in s.bursts] == pytest.approx([float(i) for i in range(12)], abs=0.001)


def test_grouper_rules_on_built_frames():
    cells = {(3, 2): 40.0, (3, 15): 38.0}
    g = EpochGrouper()
    seq = [
        (msm_frame(1077, 1, 1000, True, cells), True),    # first frame
        (msm_frame(1087, 1, 1000, False, cells), False),  # same epoch, closes it
        (msm_frame(1077, 1, 2000, True, cells), True),    # after MMB 0
        (msm_frame(1077, 1, 3000, True, cells), True),    # GPS time changed without a closing MSM
        (msm_frame(1097, 1, 3000, False, cells), False),
    ]
    assert [g.feed(f)[0] for f, _ in seq] == [want for _, want in seq]


# -- captures -----------------------------------------------------------------------


def write_part(d: Path, part: int, start_off: int, reads: list[tuple[float, bytes]]) -> bytes:
    data = b"".join(r for _, r in reads)
    (d / f"receiver-{part:04d}.bin").write_bytes(data)
    off = start_off
    lines = []
    for t, r in reads:
        lines.append(f"{t:.6f},{off},{len(r)}")
        off += len(r)
    (d / f"receiver-{part:04d}.idx").write_text("\n".join(lines) + "\n")
    return data


def two_part_capture(d: Path) -> tuple[list[tuple[float, bytes]], bytes]:
    cells = {(5, 2): 41.0}
    reads0 = [(1760000000.0, msm_frame(1077, 7, 1000, False, cells)), (1760000000.25, b"$junk line\r\n"),
              (1760000001.0, msm_frame(1077, 7, 2000, False, cells))]
    reads1 = [(1760000001.5, msm_frame(1077, 7, 3000, False, cells)[:10]),
              (1760000001.75, msm_frame(1077, 7, 3000, False, cells)[10:])]
    a = write_part(d, 0, 0, reads0)
    b = write_part(d, 1, len(a), reads1)
    return reads0 + reads1, a + b


def test_two_part_capture_loads_in_order_with_its_idx_gaps(tmp_path):
    reads, data = two_part_capture(tmp_path)
    s = load(tmp_path)
    assert s.kind == "capture"
    assert [b.data for b in s.bursts] == [r for _, r in reads]
    assert [b.t for b in s.bursts] == pytest.approx([t - reads[0][0] for t, _ in reads], abs=1e-6)
    assert s.data() == data
    assert s.frames == 3  # the frame cut across the two parts counts once
    assert read_idx(tmp_path / "receiver-0001.idx")[0][1] == len((tmp_path / "receiver-0000.bin").read_bytes())


def test_a_single_capture_part_with_its_idx_is_a_capture(tmp_path):
    two_part_capture(tmp_path)
    s = load(tmp_path / "receiver-0000.bin")
    assert s.kind == "capture" and len(s.bursts) == 3


def test_capture_with_a_wrong_idx_is_refused(tmp_path):
    two_part_capture(tmp_path)
    (tmp_path / "receiver-0001.idx").write_text("1760000001.5,999,10\n1760000001.75,1009,999\n")
    with pytest.raises(ValueError):
        load_capture(tmp_path)


def test_capture_replays_in_order_with_its_gaps(tmp_path):
    cap = tmp_path / "cap"
    cap.mkdir()
    reads, data = two_part_capture(cap)
    s = load(cap)
    speed = 2.0
    srv = ReplayServer(s, speed=speed, say=lambda m: None).start()
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            got, marks = recv_exactly(c, len(data))
    finally:
        srv.stop()
    assert got == data
    # The time each read's last byte arrived, against the .idx gaps / speed.
    ends, total = [], 0
    for _t, r in reads:
        total += len(r)
        ends.append(next(t for t, n in marks if n >= total))
    for i in range(1, len(reads)):
        want = (reads[i][0] - reads[0][0]) / speed
        assert ends[i] - ends[0] == pytest.approx(want, abs=0.08)


# -- the server ---------------------------------------------------------------------


def test_client_gets_byte_identical_data_over_two_loops_and_what_it_sends_is_logged(tmp_path):
    s = load(LAX)
    srv = ReplayServer(s, loop=True, speed=30.0, run_dir=tmp_path, say=lambda m: None).start()
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            c.sendall(b"\r\nVERSIONA\r\n")
            got, _ = recv_exactly(c, 2 * s.size)
            c.sendall(b"second write")
            time.sleep(0.3)
    finally:
        srv.stop()
    assert got == LAX.read_bytes() * 2
    assert (tmp_path / "from-firmware.bin").read_bytes() == b"\r\nVERSIONA\r\nsecond write"
    log = (tmp_path / "replay.log").read_text()
    assert "VERSIONA" in log and "loop 1: 30 bursts" in log and "connection 1 from" in log


def test_a_new_connection_starts_the_schedule_again(tmp_path):
    s = load(LAX)
    srv = ReplayServer(s, loop=True, speed=30.0, say=lambda m: None).start()
    first = s.bursts[0].data
    try:
        for _ in range(2):
            with socket.create_connection((srv.host, srv.port)) as c:
                got, _ = recv_exactly(c, len(first))
                assert got == first
    finally:
        srv.stop()
    assert srv.counts()["connections"] == 2


def test_pause_drops_the_bursts_that_fall_due_and_resume_goes_on_with_the_schedule(tmp_path):
    s = load(LAX)
    speed = 10.0  # 0.1 s per epoch
    srv = ReplayServer(s, loop=True, speed=speed, say=lambda m: None).start()
    first = s.bursts[0].data
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            got, _ = recv_exactly(c, len(first))
            assert got == first
            srv.pause()
            time.sleep(0.6)  # what was in flight arrives; nothing after it
            c.settimeout(0.2)
            try:
                while c.recv(65536):
                    pass
            except socket.timeout:
                pass
            sent = srv.counts()["bursts"]
            time.sleep(1.0)
            assert srv.counts()["bursts"] == sent
            with pytest.raises(socket.timeout):
                c.recv(65536)
            dropped = srv.counts()["bursts_paused"]
            assert dropped >= 8
            srv.resume()
            # The bursts now due arrive again, whole and CRC-valid.
            c.settimeout(5)
            data = bytearray(c.recv(65536))
            end = time.monotonic() + 0.5
            c.settimeout(0.1)
            while time.monotonic() < end:
                try:
                    data += c.recv(65536)
                except socket.timeout:
                    pass
            assert frames_of(bytes(data))
            assert srv.counts()["bursts"] > sent
    finally:
        srv.stop()


def test_line_rate_pacing_spreads_a_burst(tmp_path):
    f = tmp_path / "one.rtcm3"
    frame = msm_frame(1077, 1, 1000, False, {(s, 2): 40.0 for s in range(1, 13)})
    f.write_bytes(frame * 4)  # four bursts (each frame closes an epoch), all at the same time
    s = load(f)
    data = s.data()
    baud = 9600
    srv = ReplayServer(s, baud=baud, say=lambda m: None).start()
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            t0 = time.monotonic()
            got, _ = recv_exactly(c, len(data))
            took = time.monotonic() - t0
    finally:
        srv.stop()
    assert got == data
    assert took >= len(data) * 10 / baud * 0.85


def test_parse_hostport():
    assert parse_hostport("127.0.0.1:5600") == ("127.0.0.1", 5600)
    assert parse_hostport(":5600") == ("127.0.0.1", 5600)
    with pytest.raises(ValueError):
        parse_hostport("5600")


def test_cli_help_and_imports_without_the_bench_dependencies():
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--help"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 0 and "--listen" in out.stdout
    # tools/qemu/e2e.py imports these by path, with no third-party package installed.
    code = ("import sys; sys.path.insert(0, '.'); import rtcm_replay, fake_caster, ntrip_timing; "
            "bad = [m for m in ('serial', 'httpx', 'pytest') if m in sys.modules]; assert not bad, bad")
    out = subprocess.run([sys.executable, "-I", "-c", code], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 0, out.stderr
