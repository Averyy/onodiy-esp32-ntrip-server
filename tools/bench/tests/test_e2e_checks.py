# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The pure checks of tools/qemu/e2e.py: what the signal view must list for
a fixture, and the caster record against the fixture (order modulo the loop,
frames the station never forwards, bytes outside frames, the cut end)."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

from benchlib import rtcm
from benchlib.replay import load

ROOT = Path(__file__).resolve().parents[3]
LAX = ROOT / "test" / "fixtures" / "rtcm" / "centipede_LAX_30ep.rtcm3"


def _load_e2e():
    sys.path.insert(0, str(ROOT / "tools" / "qemu"))
    spec = importlib.util.spec_from_file_location("e2e", ROOT / "tools" / "qemu" / "e2e.py")
    mod = importlib.util.module_from_spec(spec)
    before = sys.dont_write_bytecode
    try:
        spec.loader.exec_module(mod)  # sets dont_write_bytecode for its own imports (smoke.py)
    finally:
        sys.dont_write_bytecode = before
    return mod


e2e = _load_e2e()


@pytest.fixture(scope="module")
def lax():
    return load(LAX)


def forwarded(schedule, loops: int = 3, start_epoch: int = 0) -> list[bytes]:
    """The fixture's frames as the station forwards them: looped, without 1101-1107."""
    out = []
    for k in range(loops):
        for i, b in enumerate(schedule.bursts):
            if k == 0 and i < start_epoch:
                continue
            out += [f for f in b.frames if not 1101 <= e2e.frame_type(f) <= 1107]
    return out


def test_expected_signals_for_the_lax_fixture(lax):
    assert e2e.expected_signals(lax) == {
        "beidou": ["l1", "l2", "l5", "l6"], "galileo": ["l1", "l2", "l5", "l6"], "glonass": ["l1", "l2"],
        "gps": ["l1", "l2", "l5"], "navic": [], "qzss": [],
    }


def test_frames_per_epoch_counts_epochs_not_reads(lax, tmp_path):
    assert e2e.frames_per_epoch(lax) == pytest.approx(211 / 30)
    # The same stream as a capture run read in 700-byte pieces: still per epoch.
    data = LAX.read_bytes()
    pieces = [data[i : i + 700] for i in range(0, len(data), 700)]
    (tmp_path / "receiver-0000.bin").write_bytes(data)
    off, lines = 0, []
    for k, p in enumerate(pieces):
        lines.append(f"{1760000000 + k * 0.4:.6f},{off},{len(p)}")
        off += len(p)
    (tmp_path / "receiver-0000.idx").write_text("\n".join(lines) + "\n")
    cap = load(tmp_path)
    assert cap.kind == "capture" and len(cap.bursts) == len(pieces)
    assert e2e.frames_per_epoch(cap) == pytest.approx(211 / 30)


def test_record_in_fixture_order_over_the_loop_passes(lax):
    frames = forwarded(lax, loops=3, start_epoch=4)
    ok, detail, seconds = e2e.check_record(b"".join(frames), lax)
    assert ok, detail
    assert seconds == pytest.approx(86.0)  # 26 + 30 + 30 epochs at 1 s
    assert "1107 x" in detail


def test_record_starting_mid_epoch_is_checked_from_the_first_boundary(lax):
    frames = forwarded(lax, loops=2)[3:]
    ok, detail, _ = e2e.check_record(b"".join(frames) + frames[0][:9], lax)  # and a cut frame at the end
    assert ok, detail


def test_a_missing_forwarded_frame_fails(lax):
    frames = forwarded(lax, loops=2)
    del frames[40]
    ok, detail, _ = e2e.check_record(b"".join(frames), lax)
    assert not ok and "missing" in detail


def test_bytes_between_frames_fail_unless_before_the_first_frame_for_a_relay(lax):
    frames = forwarded(lax, loops=2)
    junk = b"\x01\x02junk"
    ok, detail, _ = e2e.check_record(b"".join(frames[:10]) + junk + b"".join(frames[10:]), lax)
    assert not ok and "outside frames" in detail
    ok, detail, _ = e2e.check_record(junk + b"".join(frames), lax)
    assert not ok
    ok, detail, _ = e2e.check_record(junk + b"".join(frames), lax, leading_junk_ok=True)
    assert ok, detail


def test_repeated_identical_frames_do_not_confuse_the_start(tmp_path):
    # Every epoch opens with the same 1005 (a fixed base sends it unchanged).
    from builders import arp_payload, msm_frame
    arp = rtcm.build_frame(arp_payload(1, 1113194.9, -4842853.7, 3985496.0))
    epochs = [[arp, msm_frame(1077, 1, 1000 * k, True, {(3, 2): 40.0}),
               msm_frame(1097, 1, 1000 * k, False, {(4, 2): 41.0})] for k in range(1, 11)]
    f = tmp_path / "base.rtcm3"
    f.write_bytes(b"".join(b"".join(e) for e in epochs))
    sched = load(f)
    assert len(sched.bursts) == 10
    record = b"".join(b"".join(e) for e in epochs[2:] + epochs)  # starts with a 1005, then the loop
    ok, detail, seconds = e2e.check_record(record, sched)
    assert ok, detail
    assert seconds == pytest.approx(18.0)  # 8 + 10 epochs; the first counted from its 1077
    ok, detail, _ = e2e.check_record(b"".join(b"".join(e) for e in epochs[2:] + epochs[3:]), sched)
    assert not ok and "missing" in detail  # epochs 0-2 of the next loop never came
    bad = b"".join(b"".join(e) for e in epochs[2:5]) + b"".join(epochs[6][1:])  # epoch 6 lost its 1005
    ok, detail, _ = e2e.check_record(bad, sched)
    assert not ok and "missing" in detail


def test_a_frame_that_is_no_fixture_frame_fails(lax):
    frames = forwarded(lax, loops=1)
    payload = bytearray(rtcm.payload_of(frames[5]))
    payload[-1] ^= 0x01
    frames[5] = rtcm.build_frame(bytes(payload))
    ok, detail, _ = e2e.check_record(b"".join(frames), lax)
    assert not ok and "no fixture frame" in detail
