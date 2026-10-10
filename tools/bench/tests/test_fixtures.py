# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The bench decoders against real-world streams and independent decodes.

The fixtures and their oracles live in test/fixtures (see its README): RTCM
streams from several receivers and encoders decoded by pyrtcm (MSM content
cross-checked against go-gnss), NMEA from a Unicore receiver decoded by
pynmeagps, and receiver reply samples. Every check compares our output
with the oracle field by field.
"""

from __future__ import annotations

import json
from collections import Counter
from pathlib import Path

import pytest

from benchlib import rtcm, unicore
from benchlib.rtcmstats import RtcmStats
from benchlib.stream import StreamSplitter, check_text, crc32_unicore

FIX = Path(__file__).resolve().parents[3] / "test" / "fixtures"
INDEX = json.loads((FIX / "oracle" / "index.json").read_text())
RTCM_FIXTURES = sorted(name for name, e in INDEX.items() if "frames_ok" in e)
MSM_FIXTURES = [n for n in RTCM_FIXTURES if INDEX[n]["msm_messages"]]


def dechunk(raw: bytes) -> tuple[bytes, int, bool]:
    """HTTP chunked body -> (data, chunks, ended by the last chunk)."""
    out = bytearray()
    i = chunks = 0
    while i < len(raw):
        j = raw.find(b"\r\n", i)
        if j < 0:
            break
        try:
            n = int(raw[i:j].split(b";")[0], 16)
        except ValueError:
            break
        if n == 0:
            return bytes(out), chunks, True
        if raw[j + 2 + n : j + 4 + n] != b"\r\n":
            break
        out += raw[j + 2 : j + 2 + n]
        chunks += 1
        i = j + 4 + n
    return bytes(out), chunks, False


def stream_of(name: str) -> bytes:
    e = INDEX[name]
    raw = (FIX / e["path"]).read_bytes()
    assert len(raw) == e["size"]
    return dechunk(raw)[0] if e.get("dechunk") else raw


def oracle(name: str) -> list[dict]:
    return [json.loads(line) for line in (FIX / "oracle" / f"{name}.msm.jsonl").read_text().splitlines()]


def split(data: bytes, piece: int = 0) -> list:
    sp = StreamSplitter()
    items = []
    if piece <= 0:
        items += sp.feed(data)
    else:
        for i in range(0, len(data), piece):
            items += sp.feed(data[i : i + piece])
    return items + sp.flush()


@pytest.mark.parametrize("name", RTCM_FIXTURES)
@pytest.mark.parametrize("piece", [0, 7, 100])
def test_splitter_finds_the_oracles_frames_and_lines(name, piece):
    e = INDEX[name]
    items = split(stream_of(name), piece)
    frames = [it for it in items if it.kind == "rtcm"]
    assert len(frames) == e["frames_ok"]
    assert all(rtcm.frame_ok(it.data) for it in frames)
    assert Counter(str(it.rtcm_type) for it in frames) == Counter(
        {k: v for k, v in e["message_counts"].items()})
    good_lines = [it for it in items if it.kind == "text" and it.check in ("nmea", "unicore", "crc32")]
    assert len(good_lines) == e["text_lines"]
    # Frames come out at their own offsets, in order.
    data = stream_of(name)
    for it in frames:
        assert data[it.offset : it.offset + len(it.data)] == it.data


def test_a_sentence_right_after_a_stray_lead_byte_is_kept():
    # A '#' (last byte of a binary message) right in front of "$GNRMC...".
    data = stream_of("pygnssutils_nmea_rtcm3_excerpt.log")
    line = "$GNRMC,084220.00,A,3203.94995,N,03446.42914,E,0.000,,080222,,,D,V*12"
    at = data.find(line.encode())
    assert at > 0 and data[at - 1 : at] == b"#"
    items = split(data)
    hits = [it for it in items if it.kind == "text" and it.text == line]
    assert len(hits) == 1 and hits[0].check == "nmea" and hits[0].offset == at
    # And through a doubled lead byte, which the '$'-inclusive rule would pass.
    items = split(b"$" + line.encode() + b"\r\nx$devicename,COM1*67\r\n")
    texts = [(it.text, it.check) for it in items if it.kind == "text"]
    assert texts == [(line, "nmea"), ("$devicename,COM1*67", "unicore")]


def _cells(m: rtcm.Msm) -> list[list]:
    out = []
    for c in m.cells:
        if m.msm <= 3:
            raw = None
        elif c.cnr is None:
            raw = 0
        else:
            raw = round(c.cnr * 16) if m.msm >= 6 else round(c.cnr)
        out.append([c.sat, c.sig, raw])
    return out


@pytest.mark.parametrize("name", MSM_FIXTURES)
def test_msm_decode_matches_the_oracle(name):
    recs = oracle(name)
    data = stream_of(name)
    items = [it for it in split(data) if it.kind == "rtcm" and rtcm.msm_system(it.rtcm_type)]
    assert len(items) == len(recs)
    for it, r in zip(items, recs):
        assert it.offset == r["off"]
        m = rtcm.decode_msm(rtcm.payload_of(it.data))
        epoch = r["epoch"] | (r["dow"] << 27) if "dow" in r else r["epoch"]
        got = (m.mtype, m.system, m.msm, m.station_id, m.epoch, int(m.mmb), m.iods, m.sats, m.sigs, _cells(m))
        want = (r["type"], r["sys"], r["msm"], r["station"], epoch, r["mmb"], r["iods"], r["sats"], r["sigs"],
                r["cells"])
        assert got == want, (name, r["off"])


def test_zero_satellite_msm_and_a_signal_without_cells():
    one = rtcm.decode_msm(rtcm.payload_of((FIX / "rtcm/go-gnss_1104_frame.bin").read_bytes()))
    assert (one.system, one.msm, one.sats, one.sigs, one.cells) == ("SBAS", 4, [], [], [])
    lax = [rtcm.decode_msm(rtcm.payload_of(it.data)) for it in split(stream_of("centipede_LAX_30ep.rtcm3"))
           if it.kind == "rtcm" and it.rtcm_type == 1117]
    assert len(lax) == 30 and all(m.sats == [] and m.cells == [] for m in lax)
    robu = [rtcm.decode_msm(rtcm.payload_of(it.data)) for it in split(stream_of("centipede_ROBU1_35ep.rtcm3"))
            if it.kind == "rtcm" and it.rtcm_type == 1127]
    assert len(robu) == 34
    for m in robu:
        assert 14 in m.sigs and all(c.sig != 14 for c in m.cells)
        assert len(m.cells) < len(m.sats) * len(m.sigs) <= 64


def test_1033_strings_match_the_oracle():
    checked = 0
    for name in RTCM_FIXTURES:
        data = stream_of(name)
        for d in INDEX[name]["d1033"]:
            off = d["offset"]
            n = ((data[off + 1] & 3) << 8) | data[off + 2]
            frame = data[off : off + n + 6]
            assert rtcm.frame_ok(frame)
            got = rtcm.decode_1033(rtcm.payload_of(frame))
            assert (got.station_id, got.antenna, got.setup_id, got.antenna_serial, got.receiver_type,
                    got.receiver_firmware, got.receiver_serial) == (
                d["station"], d["antenna"], d["setup_id"], d["antenna_serial"], d["receiver_type"],
                d["receiver_fw"], d["receiver_serial"])
            checked += 1
    assert checked >= 4


def test_binary_logs_of_a_real_receiver():
    items = split((FIX / "unicore/pyunigps_um981.log").read_bytes(), 50)
    assert [it.kind for it in items] == ["binary"] * 39
    assert "msg 37 " in items[0].note  # the version log first
    items = split(stream_of("pyunigps_mixed.log"))
    kinds = Counter(it.kind for it in items)
    assert kinds["binary"] == 39 and kinds["rtcm"] == INDEX["pyunigps_mixed.log"]["frames_ok"]


def _groups(recs: list[dict]) -> int:
    """Epochs by the oracle: a closing MSM (SBAS included) or a new epoch time
    of a constellation already in the epoch."""
    groups = 0
    closed = True
    seen: dict[str, int] = {}
    for r in recs:
        if closed or seen.get(r["sys"], r["epoch"]) != r["epoch"]:
            groups += 1
            seen = {}
        seen[r["sys"]] = r["epoch"]
        closed = r["mmb"] == 0
    return groups


@pytest.mark.parametrize("name, mmb_ok", [("centipede_LAX_30ep.rtcm3", True), ("centipede_ROBU1_35ep.rtcm3", False),
                                          ("centipede_BGDD_30ep.rtcm3", True)])
def test_summary_of_real_streams(name, mmb_ok):
    recs = oracle(name)
    st = RtcmStats()
    for it in split(stream_of(name)):
        st.add_item(it)
    st.finish()
    d = st.to_dict()
    # Epochs per type and constellation: distinct epoch times in the oracle.
    for t in {r["type"] for r in recs}:
        if rtcm.is_suspect_sbas(t):
            continue
        times = [r["epoch"] for r in recs if r["type"] == t]
        distinct = sum(1 for i, x in enumerate(times) if i == 0 or times[i - 1] != x)
        assert st.types[t].epochs == distinct
        assert st.rate_hz(t) == pytest.approx(1.0)
    for s in d["systems"]:
        sats = {c[0] for r in recs if r["sys"] == s["system"] for c in r["cells"]}
        assert set(s["distinct_sats"]) == sats
    # Bands: only signals that carry cells count.
    for s in d["systems"]:
        sigs = {c[1] for r in recs if r["sys"] == s["system"] for c in r["cells"]}
        bands = {rtcm.band_of(s["system"], g) or f"sig{g}" for g in sigs}
        assert set(s["bands"]) == bands
    # One burst per epoch; the stream without a closing MSM in one epoch is flagged.
    assert st.bursts == _groups([r for r in recs if not rtcm.is_suspect_sbas(r["type"])])
    assert (st.bursts_mmb_ok == st.bursts) == mmb_ok
    checks = {c["check"]: c["status"] for c in st.checks()}
    assert checks["MMB per epoch"] == ("PASS" if mmb_ok else "WARN")
    assert st.crc_bad == 0 and st.junk_bytes == 0 and not st.decode_errors


def test_summary_flags_beidou_msms_that_carry_gps_time():
    # A 2012 encoder put GPS time into its BeiDou MSMs (BeiDou time is 14 s
    # behind): on GPS time its BeiDou MSMs land 14 s away from their epoch,
    # and the epoch check says so instead of passing.
    recs = oracle("rtklib_GMSD7_12ep.rtcm3")
    gps = {r["epoch"] for r in recs if r["sys"] == "GPS"}
    assert {r["epoch"] for r in recs if r["sys"] == "BeiDou"} == gps
    st = RtcmStats()
    for it in split(stream_of("rtklib_GMSD7_12ep.rtcm3")):
        st.add_item(it)
    st.finish()
    assert st.bursts_mmb_ok < st.bursts
    assert {c["check"]: c["status"] for c in st.checks()}["MMB per epoch"] == "WARN"
    assert st.types[1127].epochs == 12 and st.rate_hz(1127) == pytest.approx(1.0)


def test_reply_samples_from_a_receiver():
    samples = json.loads((FIX / "unicore/sparkfun_reply_samples.json").read_text())["samples"]
    assert len(samples) == 8
    for s in samples:
        assert check_text(s["text"]) == "unicore", s["text"]
    by_kind = {}
    for s in samples:
        by_kind.setdefault(s["kind"], []).append(s["text"])
    m = unicore.ACK.match(by_kind["command_ok"][0])
    assert m and m.group(1) == "MODE" and m.group(2) == "OK"
    m = unicore.ACK.match(by_kind["command_error"][0])
    assert m and m.group(1) == "badResponse"
    assert m.group(2) == "PARSING FAILD NO MATCHING FUNC  BADRESPONSE"
    modes = [unicore.parse_mode(t) for t in by_kind["mode_query"]]
    assert modes == ["MODE ROVER SURVEY", "MODE BASE TIME 60",
                     "MODE BASE -1280206.5680 -4716804.4030 4086665.4840"]
    assert all(unicore.data_matcher("MODE")(t) for t in by_kind["mode_query"])
    cfg = unicore.parse_config(by_kind["config_dump"])
    assert cfg == {"COM3": ["CONFIG COM3 115200"], "MASK": ["MASK 5.0"]}
    assert unicore.data_matcher("CONFIG")(by_kind["config_dump"][0])
    banner = by_kind["boot_banner"][0]
    assert check_text(banner) == "unicore" and banner.startswith("$devicename")


def test_binary_log_crc_is_the_ascii_log_crc():
    # The CRC-32 of the receiver's binary logs (over the whole frame, sync
    # included, stored little-endian) is the one its '#' logs use: all 39
    # logs of a real capture verify with it.
    raw = (FIX / "unicore/pyunigps_um981.log").read_bytes()
    i = n = 0
    while i < len(raw):
        assert raw[i : i + 3] == b"\xaa\x44\xb5"
        total = 24 + int.from_bytes(raw[i + 6 : i + 8], "little") + 4
        frame = raw[i : i + total]
        assert crc32_unicore(frame[:-4]) == int.from_bytes(frame[-4:], "little")
        i += total
        n += 1
    assert n == 39 and i == len(raw)


GSV_SYSTEM = {"GP": "GPS", "GL": "GLONASS", "GA": "Galileo", "GB": "BeiDou", "GQ": "QZSS"}


def _gsv_system(talker: str, n: int) -> str:
    if talker in ("GP", "GN"):
        if 33 <= n <= 64 or 120 <= n <= 158:
            return "SBAS"
        if 65 <= n <= 96:
            return "GLONASS"
        if 193 <= n <= 202:
            return "QZSS"
    return GSV_SYSTEM[talker]


def test_satellites_in_view_of_a_real_unicore_capture():
    data = (FIX / "nmea/gpsd_unicore-um621_excerpt.log").read_bytes()
    lines = [it.text for it in split(data) if it.kind == "text" and it.check == "nmea"]
    assert len(lines) == INDEX["gpsd_unicore-um621_excerpt.log"]["text_lines"]
    gsv = [ln for ln in lines if ln[3:7] == "GSV,"]
    recs = [json.loads(x) for x in (FIX / "oracle/gpsd_unicore-um621_excerpt.log.gsv.jsonl").read_text().splitlines()]
    assert len(gsv) == len(recs)
    view = unicore.parse_gsv(gsv)
    # Every (system, satellite, C/N0) report of the oracle, per signal name.
    want: dict[str, Counter] = {}
    for r in recs:
        sid = r["signal_id"]
        for sv, el, az, cn in r["sats"]:
            system = _gsv_system(r["talker"], sv)
            name = unicore.NMEA_SIGNALS.get(system, {}).get(sid, (f"sig{sid}", None))[0]
            want.setdefault(system, Counter())[(name, cn)] += 1
    got = {system: Counter((name, cn) for name, obs in sigs.items() for _, cn in obs)
           for system, sigs in view.signals.items()}
    assert got == want
    assert set(view.signals) == {"GPS", "GLONASS", "Galileo", "BeiDou", "SBAS"}
    assert view.band_list("GPS") == ["L1", "L5"] and view.band_list("BeiDou") == ["L1", "L5"]
    assert view.band_list("Galileo") == ["L1", "L5"] and view.band_list("GLONASS") == ["L1"]
    # SBAS satellites come under the GPS talker (NMEA numbers 33-64), never as GPS.
    assert all(int(sat) <= 32 for obs in view.signals["GPS"].values() for sat, _ in obs)
