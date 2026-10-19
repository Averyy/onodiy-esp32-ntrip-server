# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""rtcm_replay.py and benchlib.replay: epoch grouping, schedules, captures,
and the TCP server (byte identity over loops, pacing, what the client sends)."""

from __future__ import annotations

import json
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


def test_a_late_last_frame_comes_that_long_after_the_rest_of_its_burst(tmp_path):
    # Two epochs of three MSMs each, one second apart: the last of each held back 0.4 s, then at once again.
    f = tmp_path / "two.rtcm3"
    epochs = [[msm_frame(t, 1, 1000 * k, t != 1097, {(3, 2): 40.0}) for t in (1077, 1087, 1097)] for k in (1, 2, 3)]
    f.write_bytes(b"".join(b"".join(e) for e in epochs))
    s = load(f)
    assert len(s.bursts) == 3
    srv = ReplayServer(s, say=lambda m: None)
    srv.set_last_frame_delay(0.4)
    srv.start()
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            head = len(b"".join(epochs[0][:2]))
            got, marks = recv_exactly(c, len(b"".join(epochs[0])) + len(b"".join(epochs[1])))
            t_head = next(t for t, n in marks if n >= head)
            t_last = next(t for t, n in marks if n >= len(b"".join(epochs[0])))
            srv.set_last_frame_delay(0)
            rest, _ = recv_exactly(c, len(b"".join(epochs[2])))
    finally:
        srv.stop()
    assert got + rest == s.data()   # byte identical, nothing lost or reordered
    assert 0.3 <= t_last - t_head <= 0.9, t_last - t_head
    assert srv.counts()["bursts_late"] == 2


def test_a_pause_while_a_late_last_frame_waits_drops_it_and_bursts_can_be_timed(tmp_path):
    # As e2e.py stops the line across a restart that came inside an epoch: the head of the burst went out,
    # its late last frame falls due while paused and is never sent.
    f = tmp_path / "three.rtcm3"
    epochs = [[msm_frame(t, 1, 1000 * k, t != 1097, {(3, 2): 40.0}) for t in (1077, 1087, 1097)] for k in (1, 2, 3)]
    f.write_bytes(b"".join(b"".join(e) for e in epochs))
    s = load(f)
    srv = ReplayServer(s, say=lambda m: None)
    srv.set_last_frame_delay(0.5)
    srv.start()
    try:
        assert srv.wait_burst_head(0.2) is None   # nobody connected: nothing goes out
        with socket.create_connection((srv.host, srv.port)) as c:
            head = len(b"".join(epochs[0][:2]))
            got, _ = recv_exactly(c, head)
            t = srv.wait_burst_head(5.0)           # the second burst's head, about 1 s after the first
            assert t is not None and time.monotonic() - t < 1.0
            srv.pause()
            rest = bytearray()
            c.settimeout(0.3)
            end = time.monotonic() + 1.5
            while time.monotonic() < end:
                try:
                    data = c.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                rest += data
    finally:
        srv.stop()
    # The first burst whole (its late frame came before the pause), the second's head only.
    assert got + bytes(rest) == b"".join(epochs[0]) + b"".join(epochs[1][:2])
    assert srv.counts()["late_paused"] == 1 and srv.counts()["bursts_late"] == 1


def test_a_hold_lets_the_burst_going_out_finish_its_late_frame_too_and_starts_no_other(tmp_path):
    # As e2e.py holds the line from inside the epoch a restart comes in: that epoch's late last frame still
    # goes out (the session waits for it before it closes), nothing after it until resume().
    f = tmp_path / "four.rtcm3"
    epochs = [[msm_frame(t, 1, 1000 * k, t != 1097, {(3, 2): 40.0}) for t in (1077, 1087, 1097)]
              for k in (1, 2, 3, 4)]
    f.write_bytes(b"".join(b"".join(e) for e in epochs))
    s = load(f)
    srv = ReplayServer(s, say=lambda m: None)
    srv.set_last_frame_delay(0.5)
    srv.start()
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            got, _ = recv_exactly(c, len(b"".join(epochs[0][:2])))
            t = srv.wait_burst_head(5.0)           # the second burst's head
            assert t is not None
            srv.hold()
            rest = bytearray()
            c.settimeout(0.3)
            end = time.monotonic() + 1.6           # the third burst falls due meanwhile, the fourth after
            while time.monotonic() < end:
                try:
                    data = c.recv(65536)
                except socket.timeout:
                    continue
                if not data:
                    break
                rest += data
            held = srv.counts()
            srv.resume()
            c.settimeout(5)
            more, _ = recv_exactly(c, len(b"".join(epochs[3][:2])))
    finally:
        srv.stop()
    # The first two bursts whole (the second's late frame after the hold), the third dropped.
    assert got + bytes(rest) == b"".join(epochs[0]) + b"".join(epochs[1])
    assert held["bursts_late"] == 2 and held["late_paused"] == 0 and held["bursts_paused"] >= 1
    assert more == b"".join(epochs[3][:2])     # after resume() the schedule goes on


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


# -- the scripted receiver (--respond) --------------------------------------------------

from benchlib import unicore  # noqa: E402
from benchlib.stream import check_text  # noqa: E402
from benchlib.um982_sim import Um982Responder, parse_group  # noqa: E402
from receiver_25164 import GROUP_BODIES as R25164_BODIES  # noqa: E402
from receiver_25164 import LINES as R25164  # noqa: E402

HOST_VECTORS = BENCH.parents[1] / "test" / "host" / "main" / "um98x_test_vectors.h"
FIXTURE_25164 = BENCH.parents[1] / "test" / "fixtures" / "unicore" / "um982_build25164_replies.json"


class FakeClock:
    def __init__(self) -> None:
        self.t = 1000.0

    def __call__(self) -> float:
        return self.t


def host_vector(name: str) -> str:
    """One string vector of the host tests (adjacent C literals joined)."""
    import re
    lines = HOST_VECTORS.read_text().splitlines()
    i = next(k for k, line in enumerate(lines) if line.startswith(f"#define {name}"))
    block = [lines[i]]
    while block[-1].rstrip().endswith("\\"):   # a continued macro
        i += 1
        block.append(lines[i])
    parts = re.findall(r'"((?:[^"\\]|\\.)*)"', "\n".join(block))
    return "".join(parts).replace('\\"', '"')


def lines_of(data: bytes) -> list[str]:
    return [x for x in data.decode("latin-1").split("\r\n") if x]


def test_reply_builders_reproduce_the_host_test_vectors():
    assert unicore.version_line("UM982", "R4.10Build18975") == host_vector("TV_VERSIONA_UM982")
    assert unicore.ack_line("unlog") == host_vector("TV_ACK_UNLOG")
    assert unicore.ack_line("VERSIONA") == host_vector("TV_ACK_VERSIONA")
    assert unicore.config_line("SIGNALGROUP", "CONFIG SIGNALGROUP 3 6") == host_vector("TV_CONFIG_SG")
    assert unicore.banner_line("COM1") == host_vector("TV_BANNER")
    assert unicore.mode_line("MODE BASE 1234 TIME 600 0") == host_vector("TV_MODE_SURVEY")
    assert unicore.ack_line("rtcm1230 10", unicore.refusal_text("rtcm1230")) == host_vector("TV_NACK_1230")
    # Build 18975 and the public samples spell the refusal FAILD; the station reads both.
    assert unicore.ack_line("rtcm1230 10", unicore.refusal_text("rtcm1230", "FAILD")) == host_vector("TV_NACK_FAILD")
    assert unicore.ack_line("CONFIG SIGNALGROUP 1", unicore.GROUP_REFUSAL) == host_vector("TV_NACK_SG")
    assert unicore.restarting_line() == host_vector("TV_NOTICE")
    for line in (unicore.ack_line("MODE"), unicore.config_line("COM1", "CONFIG COM1 115200"),
                 unicore.version_line("UM982", "R4.10Build17000")):
        assert check_text(line) in ("unicore", "crc32")


def test_reply_builders_reproduce_build_25164():
    b = R25164
    assert unicore.ack_line("FOOBAR", unicore.refusal_text("FOOBAR")) == b["refused"]
    assert unicore.ack_line("CONFIG SIGNALGROUP 1", unicore.GROUP_REFUSAL) == b["group 1"]
    assert unicore.ack_line("CONFIG SIGNALGROUP 99 99", unicore.GROUP_REFUSAL) == b["group 99 99"]
    for g in ("3 6", "7 0", "5 0", "4 5", "5"):
        assert unicore.ack_line(f"CONFIG SIGNALGROUP {g}") == b[f"group {g}"]
    assert unicore.banner_line("COM2") == b["banner"]
    assert unicore.restarting_line() == b["restarting"]
    assert unicore.is_restarting_line(b["restarting"]) and not unicore.is_restarting_line(b["banner"])
    for key, line in b.items():
        assert check_text(line) == {"restarting": "none", "no satellites": "nmea"}.get(key, "unicore"), key
        assert (unicore.ACK.match(line) is not None) == key.startswith(("group", "refused")), key
    assert unicore.ACK.match(b["group 1"]).group(2) == "PARSING FAILED GRAMMAR ERROR,"


def test_responder_identifies_itself_and_dumps_its_configuration():
    clk = FakeClock()
    r = Um982Responder(group=(4, 5), clock=clk)
    r.feed(b"\r\nVERSIONA\r\nCONF")
    clk.t += 0.01
    assert r.due() == b""          # not yet: the receiver takes a moment
    r.feed(b"IG\r\nMODE\r\n")
    clk.t += 0.2
    out = lines_of(r.due())
    assert out[0] == unicore.ack_line("VERSIONA")
    ver = next(x for x in out if x.startswith("#VERSIONA"))
    assert check_text(ver) == "crc32"
    v = unicore.parse_versiona(ver)
    assert v["model"] == "UM982" and v["firmware"] == "R4.10Build25164" and v["build"] == 25164
    cfg = unicore.parse_config(out)
    assert unicore.signal_group(cfg) == (4, 5)
    assert {"SIGNALGROUP", "COM1", "MMP", "RTCMB1CB2A", "MASK", "BASEANTENNAMODEL"} <= set(cfg)
    mode = next(x for x in out if x.startswith("#MODEA"))
    assert unicore.parse_mode(mode) == "MODE ROVER" and check_text(mode) == "unicore"
    assert all(check_text(x) != "bad" for x in out)


def test_responder_takes_a_signal_group_restarts_and_comes_back_with_it():
    clk = FakeClock()
    r = Um982Responder(group=(4, 5), restart_s=3.0, clock=clk)
    assert r.rtcm_on()
    r.feed(b"UNLOG COM1\r\n")
    assert not r.rtcm_on()                          # quiet port
    clk.t += 0.1
    assert lines_of(r.due()) == [unicore.ack_line("UNLOG COM1")]
    r.feed(b"CONFIG SIGNALGROUP 3 6\r\n")
    clk.t += 0.1
    assert lines_of(r.due()) == [unicore.ack_line("CONFIG SIGNALGROUP 3 6")]
    assert r.restarting()
    clk.t += 0.4
    r.feed(b"VERSIONA\r\n")                         # not taken while restarting: the receiver says so
    clk.t += 0.1
    assert lines_of(r.due()) == [R25164["restarting"]]
    clk.t += 1.5
    assert r.due() == b"" and not r.rtcm_on()
    clk.t = r.restart_until - 0.01
    r.feed(b"VERSIONA\r\n")                         # just before the banner: its answer still comes first
    clk.t = r.restart_until + 0.01
    assert r.rtcm_on()                              # the saved outputs are back
    assert lines_of(r.due()) == [R25164["restarting"], unicore.banner_line("COM1")]
    assert [a for _t, c, a in r.log if c == "VERSIONA"] == ["restarting", "restarting"]
    r.feed(b"CONFIG\r\n")
    clk.t += 0.2
    assert unicore.signal_group(unicore.parse_config(lines_of(r.due()))) == (3, 6)
    assert r.signal_group() == (3, 6) and r.restarts == 1
    # The same group again: no restart.
    r.feed(b"CONFIG SIGNALGROUP 3 6\r\n")
    clk.t += 0.2
    assert lines_of(r.due()) == [unicore.ack_line("CONFIG SIGNALGROUP 3 6")] and r.restarts == 1


def test_responder_refuses_unknown_and_rejected_groups_without_restarting():
    clk = FakeClock()
    r = Um982Responder(group=(3, 6), reject=[(7, 0)], clock=clk)
    # 7 0 rejected by the test, every group build 25164 refused on the bench, and (not tried there) three
    # numbers and words.
    for g in ("7 0", "1", "1 0", "2 0", "7 6", "99 99", "3 0", "4 0", "3 6 1", "x y"):
        cmd = f"CONFIG SIGNALGROUP {g}"
        r.feed(cmd.encode() + b"\r\n")
        clk.t += 0.2
        (line,) = lines_of(r.due())
        assert line == unicore.ack_line(cmd, unicore.GROUP_REFUSAL)   # as build 25164 refuses a group
        assert check_text(line) == "unicore"
    r.feed(b"CONFIG SIGNALGROUP 1\r\nCONFIG SIGNALGROUP 99 99\r\n")
    clk.t += 0.2
    assert lines_of(r.due()) == [R25164["group 1"], R25164["group 99 99"]]
    assert r.signal_group() == (3, 6) and r.restarts == 0 and r.rtcm_on()
    r.reject = set()
    r.feed(b"CONFIG SIGNALGROUP 7 0\r\n")
    clk.t += 0.2
    assert lines_of(r.due()) == [R25164["group 7 0"]]
    assert r.signal_group() == (7, 0) and r.restarts == 1
    # One number is the master group with the slave off: "5" is 5 0.
    clk.t += 5.0
    r.due()
    r.feed(b"CONFIG SIGNALGROUP 5\r\n")
    clk.t += 0.2
    assert lines_of(r.due()) == [R25164["group 5"]]
    assert r.signal_group() == (5, 0) and r.restarts == 2


def test_responder_saves_outputs_and_mode_and_a_reset_brings_back_what_was_saved():
    clk = FakeClock()
    r = Um982Responder(clock=clk)
    for cmd in (b"UNLOG COM1", b"MODE BASE 1234 918129.1234 -4346071.2345 4561977.3456", b"RTCM1077 COM1 1",
                b"SAVECONFIG", b"UNLOG COM1", b"MODE BASE 1 TIME 60 0", b"RESET"):
        r.feed(cmd + b"\r\n")
        clk.t += 0.1
    acks = [a for a in lines_of(r.due()) if a.startswith("$command")]
    assert len(acks) == 7 and all(",response: OK*" in a for a in acks)
    assert r.saves == 1 and r.restarts == 1 and not r.rtcm_on()
    clk.t += 3.5
    assert r.rtcm_on()                              # saved with the output on
    r.feed(b"MODE\r\n")
    clk.t += 0.2
    mode = next(x for x in lines_of(r.due()) if x.startswith("#MODEA"))
    assert unicore.parse_mode(mode) == "MODE BASE 1234 918129.1234 -4346071.2345 4561977.3456"
    # Anything else gets the receiver's refusal.
    r.feed(b"FRESET\r\n")
    clk.t += 0.2
    assert lines_of(r.due()) == [unicore.ack_line("FRESET", unicore.refusal_text("FRESET"))]


def test_responder_takes_the_default_link_speed_and_a_reset_brings_back_the_saved_one():
    clk = FakeClock()
    r = Um982Responder(clock=clk)                   # COM1 at the factory 115200

    def com1() -> list[str]:
        r.feed(b"CONFIG\r\n")
        clk.t += 0.2
        return unicore.parse_config(lines_of(r.due())).get("COM1", [])

    assert com1() == ["CONFIG COM1 115200"]
    # The default profile's speed: OK, then in the dump; saved, it survives a restart.
    r.feed(b"CONFIG COM1 460800\r\n")
    clk.t += 0.1
    assert lines_of(r.due()) == [unicore.ack_line("CONFIG COM1 460800")]
    assert com1() == ["CONFIG COM1 460800"]
    for cmd in (b"SAVECONFIG", b"RESET"):
        r.feed(cmd + b"\r\n")
        clk.t += 0.1
    clk.t += 3.5
    r.due()
    assert r.saves == 1 and r.restarts == 1 and com1() == ["CONFIG COM1 460800"]
    # Not saved: a RESET (or a signal-group restart) goes back to the saved speed.
    r.feed(b"CONFIG COM1 230400\r\nRESET\r\n")
    clk.t += 3.6
    r.due()
    assert com1() == ["CONFIG COM1 460800"]
    r.feed(b"CONFIG COM1 230400\r\nCONFIG SIGNALGROUP 3 6\r\n")
    clk.t += 3.6
    r.due()
    assert r.signal_group() == (3, 6) and com1() == ["CONFIG COM1 460800"]


def test_station_message_builders_decode_with_benchlib():
    from benchlib.um982_sim import arp_frame, descriptor_frame, glo_bias_frame
    xyz = (-2503389.9916, -4660267.5866, 3551298.6913)
    for mtype in (1005, 1006):
        f = arp_frame(mtype, 1234, xyz, 1.5)
        assert rtcm.frame_ok(f)
        a = rtcm.decode_arp(rtcm.payload_of(f))
        assert a.mtype == mtype and a.station_id == 1234 and (a.gps, a.glonass, a.galileo) == (True, True, True)
        assert max(abs(a.x - xyz[0]), abs(a.y - xyz[1]), abs(a.z - xyz[2])) < 1e-4
        assert a.height == (1.5 if mtype == 1006 else None)
    f = descriptor_frame(7, "TRM59800.00     NONE", 3, "A123", "UM982", "R4.10Build18975", "SYNTH00000001")
    d = rtcm.decode_1033(rtcm.payload_of(f))
    assert (d.station_id, d.antenna, d.setup_id, d.antenna_serial) == (7, "TRM59800.00     NONE", 3, "A123")
    assert (d.receiver_type, d.receiver_firmware, d.receiver_serial) == ("UM982", "R4.10Build18975", "SYNTH00000001")
    g = rtcm.decode_1230(rtcm.payload_of(glo_bias_frame(7)))
    assert g.station_id == 7 and not g.aligned and g.biases == {}


def test_responder_sends_the_station_messages_it_was_told_to():
    clk = FakeClock()
    r = Um982Responder(clock=clk)
    for cmd in (b"UNLOG COM1", b"RTCM1005 COM1 1", b"RTCM1033 COM1 1", b"RTCM1230 COM1 2", b"RTCM1077 COM1 1"):
        r.feed(cmd + b"\r\n")
    types = lambda frames: [rtcm.msg_type(rtcm.payload_of(f)) for f in frames]  # noqa: E731
    # A rover: no position to send.
    assert r.station_types() == {1033, 1230} and types(r.station_messages()) == [1033, 1230]
    r.feed(b"MODE BASE 21 -2503389.9916 -4660267.5866 3551298.6913\r\n")
    assert types(r.station_messages()) == [1005, 1033, 1230]
    # One call per epoch: 1230 every second epoch (RTCM1230 COM1 2).
    assert [types(r.station_frames()) for _ in range(3)] == [[1005, 1033, 1230], [1005, 1033], [1005, 1033, 1230]]
    arp = rtcm.decode_arp(rtcm.payload_of(r.station_messages()[0]))
    assert arp.station_id == 21 and abs(arp.x + 2503389.9916) < 1e-4
    d = rtcm.decode_1033(rtcm.payload_of(r.station_messages()[1]))
    assert (d.station_id, d.antenna, d.receiver_type, d.receiver_firmware) == (21, "", "UM982", "R4.10Build25164")
    r.feed(b'CONFIG BASEANTENNAMODEL "HXCCGX601A      HXCS" "SN9" 2 USER\r\n')
    d = rtcm.decode_1033(rtcm.payload_of(r.station_messages()[1]))
    assert (d.antenna, d.antenna_serial, d.setup_id) == ("HXCCGX601A      HXCS", "SN9", 2)
    # A survey sends no fixed position; UNLOG COM1 stops them all; a restart brings back what was saved.
    r.feed(b"MODE BASE 21 TIME 60 0\r\n")
    assert types(r.station_messages()) == [1033, 1230]
    r.feed(b"MODE BASE 21 -2503389.9916 -4660267.5866 3551298.6913\r\nSAVECONFIG\r\nUNLOG COM1\r\n")
    assert r.station_messages() == [] and r.station_frames() == []
    r.feed(b"RESET\r\n")
    clk.t += 0.1
    assert r.station_frames() == []                 # restarting: nothing goes out
    clk.t += 3.5
    assert types(r.station_frames()) in ([1005, 1033, 1230], [1005, 1033])
    # Latitude, longitude, height work as well.
    r.feed(b"MODE BASE 21 34.052235 -118.243683 89.5\r\n")
    lat, lon, h = rtcm.decode_arp(rtcm.payload_of(r.station_messages()[0])).llh()
    assert abs(lat - 34.052235) < 1e-7 and abs(lon + 118.243683) < 1e-7 and abs(h - 89.5) < 1e-3


def test_parse_group():
    assert parse_group("3,6") == (3, 6) and parse_group("7 0") == (7, 0)
    for bad in ("3", "3,6,1", "a,b", "256,0"):
        with pytest.raises(ValueError):
            parse_group(bad)


def test_replay_answers_between_whole_frames_and_follows_the_receivers_output(tmp_path):
    s = load(LAX)
    resp = Um982Responder(group=(4, 5), restart_s=0.5)
    srv = ReplayServer(s, loop=True, speed=10.0, run_dir=tmp_path, say=lambda m: None, responder=resp).start()
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            c.settimeout(0.2)
            got = bytearray()

            def read_for(seconds: float) -> None:
                end = time.monotonic() + seconds
                while time.monotonic() < end:
                    try:
                        got.extend(c.recv(65536))
                    except socket.timeout:
                        pass

            read_for(0.5)
            c.sendall(b"VERSIONA\r\nCONFIG\r\n")
            read_for(0.8)
            sp = StreamSplitter()
            items = sp.feed(bytes(got)) + sp.flush()
            texts = [it.text for it in items if it.kind == "text"]
            assert unicore.ack_line("VERSIONA") in texts and any(t.startswith("#VERSIONA") for t in texts)
            assert unicore.signal_group(unicore.parse_config(texts)) == (4, 5)
            assert [it for it in items if it.kind == "rtcm_bad"] == []      # no reply inside a frame
            assert sum(1 for it in items if it.kind == "rtcm") > 20
            # A quiet port: no more bursts until an RTCM output comes back.
            c.sendall(b"UNLOG COM1\r\n")
            read_for(0.4)
            got.clear()
            read_for(0.6)
            sp = StreamSplitter()
            assert [it for it in sp.feed(bytes(got)) + sp.flush() if it.kind == "rtcm"] == []
            assert srv.counts()["bursts_withheld"] >= 4
            c.sendall(b"RTCM1077 COM1 1\r\n")
            read_for(0.6)
            sp = StreamSplitter()
            assert [it for it in sp.feed(bytes(got)) + sp.flush() if it.kind == "rtcm"]
            # Its own station messages open every epoch; the recording's 1230 is left out.
            c.sendall(b"MODE BASE 5 -2503389.9916 -4660267.5866 3551298.6913\r\nRTCM1005 COM1 1\r\n"
                      b"RTCM1230 COM1 1\r\n")
            read_for(0.4)
            got.clear()
            read_for(1.5)
            sp = StreamSplitter()
            frames = [it.data for it in sp.feed(bytes(got)) + sp.flush() if it.kind == "rtcm"]
            own = resp.station_messages()
            assert [rtcm.msg_type(rtcm.payload_of(f)) for f in own] == [1005, 1230]
            fixture_1230 = [f for b in s.bursts for f in b.frames if rtcm.msg_type(rtcm.payload_of(f)) == 1230]
            assert fixture_1230 and not any(f in frames for f in fixture_1230)
            starts = [k for k, f in enumerate(frames) if f == own[0]]
            assert len(starts) >= 5 and all(frames[k + 1] == own[1] for k in starts if k + 1 < len(frames))
            assert all(rtcm.msg_type(rtcm.payload_of(frames[k + 2])) == 1077 for k in starts if k + 2 < len(frames))
    finally:
        srv.stop()
    log = (tmp_path / "replay.log").read_text()
    assert "reply bytes" in log
    assert srv.counts()["replies_sent"] > 0


def test_cli_respond_options():
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--help"], cwd=BENCH, capture_output=True, text=True)
    assert "--respond" in out.stdout and "--respond-reject" in out.stdout
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond-reject",
                          "7,0"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 2 and "need --respond" in out.stderr
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond",
                          "--respond-group", "3,6,1"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 2 and "signal group" in out.stderr
    assert "--respond-restart" in subprocess.run([sys.executable, "rtcm_replay.py", "--help"], cwd=BENCH,
                                                 capture_output=True, text=True).stdout
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond-restart",
                          "9"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 2 and "need --respond" in out.stderr
    for bad in ("0", "-1", "61"):
        out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond",
                              "--respond-restart", bad], cwd=BENCH, capture_output=True, text=True)
        assert out.returncode == 2 and "--respond-restart must be" in out.stderr


def test_responder_reports_the_signals_of_its_group():
    clk = FakeClock()
    r = Um982Responder(group=(3, 6), clock=clk)
    r.feed(b"ANT1GROUPA\r\nANT2GROUPA\r\n")
    clk.t += 0.2
    out = lines_of(r.due())
    assert out[0] == unicore.ack_line("ANT1GROUPA")
    logs = [x for x in out if x.startswith("#ANT")]
    assert logs == [unicore.groups_line(unicore.GROUP_BODIES[(3, 6)][0], 1),
                    unicore.groups_line(unicore.GROUP_BODIES[(3, 6)][1], 2)]
    assert all(check_text(x) == "crc32" for x in logs)
    assert logs[0].split(";", 1)[1].startswith("3,5,GP,0x1D,GL,0x03,GA,0x0F")


def test_responder_signal_group_bodies_are_the_bench_ones():
    """The scripted receiver's bodies, the bench tools' copy and the firmware's fixture are the same lines."""
    assert unicore.GROUP_BODIES == R25164_BODIES
    fixture = {}
    for x in json.loads(FIXTURE_25164.read_text())["signal_group_bodies"]:
        group = tuple(int(n) for n in x["group"].split())
        fixture.setdefault(group, ["", ""])[0 if x["query"] == "ANT1GROUPA" else 1] = x["body"]
    assert {g: tuple(b) for g, b in fixture.items()} == unicore.GROUP_BODIES


def test_responder_reports_its_own_position_or_none():
    """BASEPOSA and SPPNAVA (one-shot): its own single-point solution where its antenna is, height above sea level and
    separation adding up to the ellipsoidal height; no solution without a position (no sky)."""
    clk = FakeClock()
    r = Um982Responder(clock=clk, own=(45.4, -75.7, 100.0), separation=-34.0)
    r.feed(b"BASEPOSA\r\nSPPNAVA\r\n")
    clk.t += 0.2
    out = lines_of(r.due())
    assert unicore.ack_line("BASEPOSA") in out and unicore.ack_line("SPPNAVA") in out
    logs = [x for x in out if x.startswith("#")]
    assert [x.split(",", 1)[0] for x in logs] == ["#BASEPOSA", "#SPPNAVA"]
    assert all(check_text(x) == "crc32" for x in logs)
    b = unicore.parse_bestnav(logs[0])
    assert b["sol_status"] == "SOL_COMPUTED" and b["pos_type"] == "SINGLE" and b["sats_used"] >= 6
    assert b["lat_deg"] == pytest.approx(45.4) and b["lon_deg"] == pytest.approx(-75.7)
    assert b["height_msl_m"] + b["undulation_m"] == pytest.approx(100.0)
    assert b["undulation_m"] == pytest.approx(-34.0)
    # The height deviation it reports: 2.1 m, or what it was given (a poor sky's solution).
    r = Um982Responder(clock=clk, own=(45.4, -75.7, 87.0), separation=-15.0, sd_hgt=8.0)
    r.feed(b"SPPNAVA\r\n")
    clk.t += 0.2
    body = [x for x in lines_of(r.due()) if x.startswith("#")][0].split(";", 1)[1].split(",")
    assert float(body[9]) == pytest.approx(8.0) and float(body[4]) == pytest.approx(102.0)
    body = logs[0].split(";", 1)[1].split(",")
    assert float(body[9]) == pytest.approx(2.1)

    r = Um982Responder(clock=clk)
    r.feed(b"SPPNAVA\r\n")
    clk.t += 0.2
    b = unicore.parse_bestnav([x for x in lines_of(r.due()) if x.startswith("#")][0])
    assert b["sol_status"] == "INSUFFICIENT_OBS" and b["pos_type"] == "NONE"


def test_cli_respond_position_options():
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--help"], cwd=BENCH, capture_output=True, text=True)
    assert "--respond-position" in out.stdout and "--respond-geoid" in out.stdout
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond-position",
                          "45.4,-75.7,100"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 2 and "need --respond" in out.stderr
    for bad in ("45.4,-75.7", "95,0,0", "a,b,c"):
        out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond",
                              "--respond-position", bad], cwd=BENCH, capture_output=True, text=True)
        assert out.returncode == 2 and "--respond-position" in out.stderr, bad
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond",
                          "--respond-geoid", "200"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 2 and "--respond-geoid" in out.stderr
    assert "--respond-sd" in subprocess.run([sys.executable, "rtcm_replay.py", "--help"], cwd=BENCH, capture_output=True,
                                            text=True).stdout
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond-sd", "8"],
                         cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 2 and "need --respond" in out.stderr
    out = subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", str(LAX), "--respond",
                          "--respond-sd", "-1"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 2 and "--respond-sd" in out.stderr


def test_responder_sends_a_line_nobody_asked_for_every_n_seconds():
    """The satellite-in-view output without an antenna: one sentence listing no satellites every `text_every_s`,
    none while it restarts, none piled up after a gap; an NMEA sentence by its checksum (not a command reply)."""
    clk = FakeClock()
    r = Um982Responder(clock=clk, text_every_s=10.0, restart_s=3.0)
    clk.t += 9.9
    assert r.due() == b""
    clk.t += 0.1
    out = lines_of(r.due())
    assert out == ["$GPGSV,1,1,00*79"] and check_text(out[0]) == "nmea"
    assert unicore.ACK.match(out[0]) is None
    clk.t += 9.95
    assert r.due() == b""
    clk.t += 0.05
    assert lines_of(r.due()) == ["$GPGSV,1,1,00*79"]
    # A RESET at 21 s: quiet until its banner at 24 s; the sentence of 30 s comes.
    clk.t += 1.0
    r.feed(b"RESET\r\n")
    clk.t += 0.1
    assert [x for x in lines_of(r.due()) if x.startswith("$GPGSV")] == []
    clk.t += 8.9
    assert "$GPGSV,1,1,00*79" in lines_of(r.due())
    # Nobody read it for 55 s: one sentence, not five.
    clk.t += 55.0
    assert lines_of(r.due()) == ["$GPGSV,1,1,00*79"]
    assert r.texts == 4
    # Without the option: never.
    q = Um982Responder(clock=clk)
    clk.t += 100.0
    assert q.due() == b""


def test_no_rtcm_input_and_the_unasked_line_over_the_wire(tmp_path):
    """The input "none": no RTCM at all, the responder answers and sends its unasked sentence on its period."""
    s = load("none")
    assert s.frames == 0 and s.size == 0
    resp = Um982Responder(text_every_s=0.2)
    srv = ReplayServer(s, run_dir=tmp_path, say=lambda m: None, responder=resp).start()
    try:
        with socket.create_connection((srv.host, srv.port)) as c:
            c.settimeout(0.1)
            got = bytearray()
            end = time.monotonic() + 1.1
            asked = False
            while time.monotonic() < end:
                if not asked and time.monotonic() > end - 0.6:
                    c.sendall(b"\r\nVERSIONA\r\n")
                    asked = True
                try:
                    got.extend(c.recv(65536))
                except socket.timeout:
                    pass
            sp = StreamSplitter()
            items = sp.feed(bytes(got)) + sp.flush()
            texts = [it.text for it in items if it.kind == "text"]
            assert [it for it in items if it.kind.startswith("rtcm")] == []
            assert 4 <= texts.count("$GPGSV,1,1,00*79") <= 6
            assert unicore.ack_line("VERSIONA") in texts
    finally:
        srv.stop()


def test_no_rtcm_input_sends_no_station_messages_on_a_loop_or_a_new_connection(tmp_path):
    """The input "none" once the station configured 1033 and 1230 (always producible): still no RTCM at all, looping
    or not, on the connection that configured them and on the next one (a QEMU restart between boots)."""
    for loop in (False, True):
        resp = Um982Responder()
        run_dir = tmp_path / f"loop{loop}"
        run_dir.mkdir()
        srv = ReplayServer(load("none"), loop=loop, speed=10.0, run_dir=run_dir, say=lambda m: None,
                           responder=resp).start()
        try:
            got = []
            for conn in range(2):
                with socket.create_connection((srv.host, srv.port)) as c:
                    c.settimeout(0.1)
                    if conn == 0:
                        c.sendall(b"\r\nRTCM1033 COM1 1\r\nRTCM1230 COM1 1\r\n")
                    data = bytearray()
                    end = time.monotonic() + 1.2
                    while time.monotonic() < end:
                        try:
                            data.extend(c.recv(65536))
                        except socket.timeout:
                            pass
                    got.append(bytes(data))
            assert resp.station_types() == {1033, 1230}
            for data in got:
                sp = StreamSplitter()
                assert [it for it in sp.feed(data) + sp.flush() if it.kind.startswith("rtcm")] == [], loop
            assert unicore.ack_line("RTCM1033 COM1 1") in got[0].decode(errors="replace")
        finally:
            srv.stop()


def test_cli_text_every_and_no_rtcm_options():
    run = lambda *a: subprocess.run([sys.executable, "rtcm_replay.py", "--listen", "127.0.0.1:0", *a], cwd=BENCH,
                                    capture_output=True, text=True)
    assert "--respond-text-every" in subprocess.run([sys.executable, "rtcm_replay.py", "--help"], cwd=BENCH,
                                                    capture_output=True, text=True).stdout
    out = run(str(LAX), "--respond-text-every", "10")
    assert out.returncode == 2 and "need --respond" in out.stderr
    for bad in ("0", "-5", "3601"):
        out = run(str(LAX), "--respond", "--respond-text-every", bad)
        assert out.returncode == 2 and "--respond-text-every must be" in out.stderr
    out = run("none")
    assert out.returncode == 2 and "needs --respond" in out.stderr
