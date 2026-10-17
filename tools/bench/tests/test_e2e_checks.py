# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The pure checks of tools/qemu/e2e.py: what the signal view must list for
a fixture, and the caster record against the fixture, strictly (it starts at
an epoch's first frame after the sticky copies; order modulo the loop;
frames the station never forwards; gaps only on epoch boundaries; bytes
outside frames; the cut end)."""

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
    # benchlib's band table is the firmware's (NavIC L5 SPS is band L5); SBAS is never listed.
    assert e2e.expected_signals(lax) == {
        "beidou": ["l1", "l2", "l5", "l6"], "galileo": ["l1", "l2", "l5", "l6"], "glonass": ["l1", "l2"],
        "gps": ["l1", "l2", "l5"], "navic": ["l5"], "qzss": [],
    }
    assert "sbas" not in e2e.expected_signals(lax)


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


def test_a_record_must_start_at_an_epochs_first_frame(lax):
    frames = forwarded(lax, loops=2)
    ok, detail, _ = e2e.check_record(b"".join(frames) + frames[0][:9], lax)  # and a cut frame at the end
    assert ok, detail
    # The tail of an epoch first (a caster that resumed mid-epoch): refused.
    ok, detail, _ = e2e.check_record(b"".join(frames[3:]), lax)
    assert not ok and "does not start at an epoch's first frame" in detail


def test_gaps_on_epoch_boundaries_pass_and_gaps_inside_an_epoch_fail(lax):
    epochs = [[f for f in b.frames if not 1101 <= e2e.frame_type(f) <= 1107] for b in lax.bursts]
    fx = [f for b in lax.bursts for f in b.frames]
    sticky = [f for f in fx if e2e.frame_type(f) in e2e.STICKY_TYPES][:1]
    flat = lambda eps: b"".join(f for e in eps for f in e)  # noqa: E731
    # A pause: epochs 6-9 never sent, the sticky copies, then epoch 10 from its first frame.
    record = flat(epochs[2:6]) + b"".join(sticky) + flat(epochs[10:20])
    ok, detail, _ = e2e.check_record(record, lax)
    assert ok and "1 gap on epoch boundaries" in detail, detail
    # Without the copies too.
    ok, detail, _ = e2e.check_record(flat(epochs[2:6]) + flat(epochs[10:20]), lax)
    assert ok, detail
    # Cut inside epoch 5 (its last frames lost), then a whole epoch: refused.
    ok, detail, _ = e2e.check_record(flat(epochs[2:5]) + b"".join(epochs[5][:3]) + flat(epochs[10:20]), lax)
    assert not ok and "inside its epoch" in detail
    # Resumed inside epoch 10 (its first MSM lost): refused.
    ok, detail, _ = e2e.check_record(flat(epochs[2:6]) + b"".join(epochs[10][2:]) + flat(epochs[11:20]), lax)
    assert not ok and "does not resume at an epoch's first frame" in detail


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
    # Epochs 0-2 of the next loop never came: a gap on an epoch boundary (a pause).
    ok, detail, _ = e2e.check_record(b"".join(b"".join(e) for e in epochs[2:] + epochs[3:]), sched)
    assert ok and "1 gap" in detail, detail
    bad = b"".join(b"".join(e) for e in epochs[2:6]) + b"".join(epochs[6][1:])  # epoch 6 lost its 1005
    ok, detail, _ = e2e.check_record(bad, sched)
    assert not ok and "station messages are missing" in detail
    # After a pause (epoch 5 never came) the epoch comes whole: frames carry their interrupt arrival
    # time, so no stalled reader lets the hub start it at its first MSM. Without its 1005: refused.
    resumed = b"".join(b"".join(e) for e in epochs[2:5]) + b"".join(epochs[6][1:]) + b"".join(epochs[7])
    ok, detail, _ = e2e.check_record(resumed, sched)
    assert not ok and "resumes after a pause" in detail and "station messages are missing" in detail, detail
    # With it: a gap on an epoch boundary.
    resumed = b"".join(b"".join(e) for e in epochs[2:5] + epochs[6:8])
    ok, detail, _ = e2e.check_record(resumed, sched)
    assert ok and "1 gap" in detail, detail
    # Its MSMs never: epoch 6 without its 1077 fails.
    ok, detail, _ = e2e.check_record(b"".join(b"".join(e) for e in epochs[2:5]) + epochs[6][2], sched)
    assert not ok


def test_only_the_first_epoch_without_sticky_copies_may_lack_its_station_messages(tmp_path):
    # Every epoch opens with its own 1005 and 1033, then two MSMs.
    from builders import arp_payload, descriptor_payload, msm_frame
    arp = rtcm.build_frame(arp_payload(1, 1113194.9, -4842853.7, 3985496.0))
    desc = rtcm.build_frame(descriptor_payload(1, "", 0, "", "UM982", "R4.10", "SYNTH1"))
    assert e2e.frame_type(desc) == 1033
    epochs = [[arp, desc, msm_frame(1077, 1, 1000 * k, True, {(3, 2): 40.0}),
               msm_frame(1097, 1, 1000 * k, False, {(4, 2): 41.0})] for k in range(1, 11)]
    f = tmp_path / "base.rtcm3"
    f.write_bytes(b"".join(b"".join(e) for e in epochs))
    sched = load(f)
    flat = lambda eps: b"".join(f for e in eps for f in e)  # noqa: E731
    # The very first epoch without its station messages and no copies in front (the hub had none): allowed.
    ok, detail, _ = e2e.check_record(b"".join(epochs[2][2:]) + flat(epochs[3:9]), sched)
    assert ok, detail
    # The same after the hub's sticky copies: those came from somewhere, so the epoch's own are lost frames.
    ok, detail, _ = e2e.check_record(arp + b"".join(epochs[2][2:]) + flat(epochs[3:9]), sched)
    assert not ok and "sticky copies" in detail and "station messages are missing" in detail, detail
    # The copies cover what the epoch lacks (the hub sends the epoch from its first frame after them).
    ok, detail, _ = e2e.check_record(arp + desc + b"".join(epochs[2][2:]) + flat(epochs[3:9]), sched)
    assert ok, detail
    # After a pause, never: the next epoch comes whole.
    ok, detail, _ = e2e.check_record(flat(epochs[2:4]) + b"".join(epochs[6][2:]) + flat(epochs[7:9]), sched)
    assert not ok and "resumes after a pause" in detail, detail


def test_a_frame_that_is_no_fixture_frame_fails(lax):
    frames = forwarded(lax, loops=1)
    payload = bytearray(rtcm.payload_of(frames[5]))
    payload[-1] ^= 0x01
    frames[5] = rtcm.build_frame(bytes(payload))
    ok, detail, _ = e2e.check_record(b"".join(frames), lax)
    assert not ok and "no fixture frame" in detail


def test_a_session_that_starts_with_the_hubs_sticky_replay_passes(lax):
    # The hub replays its sticky 1005/1006/1033/1230 copies (up to 30 s old) at
    # every session start, then live data from the next epoch. The fixture's
    # only 1230 opens one of its epochs; replayed ahead of another epoch it is
    # no epoch boundary of the record.
    fx = [f for b in lax.bursts for f in b.frames]
    sticky = [f for f in fx if e2e.frame_type(f) == 1230]
    assert len(sticky) == 1 and fx.count(sticky[0]) == 1
    i1230 = fx.index(sticky[0])
    opens = [i for i, b in enumerate(lax.bursts) if b.frames and b.frames[0] == sticky[0]]
    assert opens, "the fixture's 1230 opens an epoch"
    for start in (5, 20, 22, 25):
        live = forwarded(lax, loops=2, start_epoch=start)
        ok, detail, _ = e2e.check_record(sticky[0] + b"".join(live), lax)
        assert ok, f"live from epoch {start}: {detail}"
    # The replay right before the epoch the 1230 itself opens: the 1230 twice, then that epoch.
    live = forwarded(lax, loops=2, start_epoch=opens[0])
    assert live[0] == fx[i1230]
    ok, detail, _ = e2e.check_record(sticky[0] + b"".join(live), lax)
    assert ok, detail
    # A frame that is really missing after the start is still found.
    live = forwarded(lax, loops=2, start_epoch=20)
    del live[60]
    ok, detail, _ = e2e.check_record(sticky[0] + b"".join(live), lax)
    assert not ok and "missing" in detail


def test_a_record_of_the_configured_receiver_is_checked_against_its_own_station_messages(lax):
    # The scripted receiver opens every epoch with its own 1005/1033/1230 and leaves out the fixture's 1230.
    from benchlib.um982_sim import arp_frame, descriptor_frame, glo_bias_frame
    own = [arp_frame(1005, 0, (-2503389.9916, -4660267.5866, 3551298.6913)),
           descriptor_frame(0, "", 0, "", "UM982", "R4.10Build18975", "SYNTH00000001"), glo_bias_frame(0)]
    sched = e2e.with_station(lax, own, {1005, 1033, 1230})
    assert all(b.frames[:3] == own and b.data == b"".join(b.frames) for b in sched.bursts)
    assert sum(1 for b in sched.bursts for f in b.frames if e2e.frame_type(f) == 1230) == len(sched.bursts)
    live = forwarded(sched, loops=2, start_epoch=4)
    ok, detail, _ = e2e.check_record(b"".join(own) + b"".join(live), sched)   # the sticky copies first
    assert ok, detail
    ok, detail, _ = e2e.check_record(b"".join(own) + b"".join(live), lax)
    assert not ok and "no fixture frame" in detail
    # Paused for a switch: the copies again, then an epoch from its own station messages on.
    epochs = [[f for f in b.frames if not 1101 <= e2e.frame_type(f) <= 1107] for b in sched.bursts]
    record = b"".join(own) + b"".join(f for e in epochs[2:6] + [own] + epochs[12:20] for f in e)
    ok, detail, _ = e2e.check_record(record, sched)
    assert ok and "1 gap" in detail, detail


def test_a_record_that_ended_at_a_pause_must_end_on_a_whole_epoch(lax):
    # A session the station closed while the casters were paused, or the part of one before a pause: the
    # data stopped at the hold point, an epoch boundary. The configured receiver opens every epoch with its
    # own station messages, which a caster may have sent before the hold (never part of an MSM run).
    from benchlib.um982_sim import arp_frame, descriptor_frame, glo_bias_frame
    own = [arp_frame(1005, 0, (-2503389.9916, -4660267.5866, 3551298.6913)),
           descriptor_frame(0, "", 0, "", "UM982", "R4.10Build18975", "SYNTH00000001"), glo_bias_frame(0)]
    sched = e2e.with_station(lax, own, {1005, 1033, 1230})
    epochs = [[f for f in b.frames if not 1101 <= e2e.frame_type(f) <= 1107] for b in sched.bursts]
    assert [e2e.frame_type(f) for f in epochs[9][:4]] == [1005, 1033, 1230, 1077]
    whole = b"".join(own) + b"".join(f for e in epochs[4:9] for f in e)
    ok, detail, _ = e2e.check_record(whole, sched, whole_tail=True)
    assert ok, detail
    # The next epoch's station messages after it: still whole.
    ok, detail, _ = e2e.check_record(whole + b"".join(epochs[9][:3]), sched, whole_tail=True)
    assert ok, detail
    # Its first MSM too (an MSM that arrived after the pause was decided went out): an epoch cut.
    cut = whole + b"".join(epochs[9][:4])
    ok, detail, _ = e2e.check_record(cut, sched, whole_tail=True)
    assert not ok and "ends inside an epoch" in detail and "type 1077" in detail, detail
    # A record still growing when it is read may end anywhere.
    ok, detail, _ = e2e.check_record(cut, sched)
    assert ok, detail
    # A cut frame at the end of a record that ended.
    ok, detail, _ = e2e.check_record(whole + epochs[9][3][:7], sched, whole_tail=True)
    assert not ok and "after the last whole frame" in detail, detail
    # After a gap (a pause the session stayed through), the same rule at its end.
    resumed = whole + b"".join(own) + b"".join(f for e in epochs[14:17] for f in e)
    ok, detail, _ = e2e.check_record(resumed, sched, whole_tail=True)
    assert ok and "1 gap" in detail, detail
    ok, detail, _ = e2e.check_record(resumed + b"".join(epochs[17][:5]), sched, whole_tail=True)
    assert not ok and "ends inside an epoch" in detail, detail


def test_a_part_carries_an_epoch_only_with_a_frame_that_is_no_sticky_copy(lax):
    # The signal-test scenario judges a caster record, or the part of one, only once it carried an epoch:
    # sticky copies alone (a session the station ended right after its start) are none, one epoch is.
    from benchlib.um982_sim import arp_frame, glo_bias_frame
    own = [arp_frame(1005, 0, (-2503389.9916, -4660267.5866, 3551298.6913)), glo_bias_frame(0)]
    epoch = forwarded(lax, loops=1, start_epoch=4)[:6]
    assert all(e2e.frame_type(f) not in e2e.STICKY_TYPES for f in epoch)
    assert not e2e.carries_epoch(b"")
    assert not e2e.carries_epoch(b"".join(own))
    assert not e2e.carries_epoch(b"".join(own) + epoch[0][:9])   # an MSM cut at the end: not yet
    assert e2e.carries_epoch(b"".join(own) + epoch[0])
    assert e2e.carries_epoch(b"".join(epoch))
    ok, detail, _ = e2e.check_record(b"".join(epoch), lax, whole_tail=True)   # one epoch, as recorded
    assert ok, detail
