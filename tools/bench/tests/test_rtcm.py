# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""RTCM decoders and the stream summariser (rtcm_inspect's engine)."""

from __future__ import annotations

import pytest
from pyrtcm import RTCMReader, parse_msm

from benchlib import rtcm
from benchlib.rtcmstats import RtcmStats
from benchlib.stream import StreamSplitter
from builders import arp_payload, descriptor_payload, glo_bias_payload, msm_frame, msm_payload

GAL_CELLS = {(3, 2): 44.5, (3, 8): 40.0, (3, 15): 42.0, (3, 22): 43.0,
             (5, 2): 47.25, (5, 8): 39.5, (5, 15): 41.0, (5, 22): 42.5}


def test_msm7_decode_matches_independent_decoder():
    frame = msm_frame(1097, 12, 123456000, True, GAL_CELLS)
    ours = rtcm.decode_msm(rtcm.payload_of(frame))
    assert (ours.system, ours.msm, ours.station_id, ours.epoch, ours.mmb) == ("Galileo", 7, 12, 123456000, True)
    assert ours.sats == [3, 5] and ours.sigs == [2, 8, 15, 22]
    meta, _, cells = parse_msm(RTCMReader.parse(frame))
    assert meta["sats"] == 2 and meta["cells"] == len(GAL_CELLS)
    theirs = [(int(c["CELLPRN"]), c["CELLSIG"], c["DF408"]) for c in cells]
    mine = [(c.sat, rtcm.SIGNAL_CODES["Galileo"][c.sig], c.cnr) for c in ours.cells]
    assert mine == theirs


@pytest.mark.parametrize("mtype", [1074, 1075, 1076, 1077, 1084, 1124, 1127])
def test_msm4_to_7_cnr(mtype):
    cells = {(1, 2): 33.0, (2, 2): 45.0, (2, 8): 21.0}
    m = rtcm.decode_msm(msm_payload(mtype, 1, 1000, False, cells))
    assert [(c.sat, c.sig, c.cnr) for c in m.cells] == [(1, 2, 33.0), (2, 2, 45.0), (2, 8, 21.0)]


def test_msm_too_short_is_an_error():
    p = msm_payload(1077, 1, 1000, False, {(1, 2): 40.0, (2, 2): 41.0})
    with pytest.raises(ValueError):
        rtcm.decode_msm(p[:-8])


def test_arp_1005_and_1006():
    lat, lon, h = 45.123456789, -75.987654321, 85.4321
    x, y, z = rtcm.llh_to_ecef(lat, lon, h)
    a = rtcm.decode_arp(arp_payload(1234, x, y, z))
    assert (a.mtype, a.station_id, a.gps, a.glonass, a.galileo, a.ref_station) == (1005, 1234, True, True, True, False)
    assert abs(a.x - x) < 6e-5 and abs(a.y - y) < 6e-5 and abs(a.z - z) < 6e-5
    la, lo, hh = a.llh()
    assert abs(la - lat) < 1e-8 and abs(lo - lon) < 1e-8 and abs(hh - h) < 2e-4
    parsed = RTCMReader.parse(rtcm.build_frame(arp_payload(1234, x, y, z)))
    assert abs(parsed.DF025 - a.x) < 1e-6 and abs(parsed.DF027 - a.z) < 1e-6
    b = rtcm.decode_arp(arp_payload(9, x, y, z, mtype=1006, height=1.5))
    assert b.mtype == 1006 and abs(b.height - 1.5) < 1e-9


def test_ecef_llh_poles_and_equator():
    for lat, lon, h in ((0.0, 0.0, 0.0), (89.9999, 10.0, 500.0), (-33.5, 151.25, -20.0)):
        la, lo, hh = rtcm.ecef_to_llh(*rtcm.llh_to_ecef(lat, lon, h))
        assert abs(la - lat) < 1e-9 and abs(lo - lon) < 1e-9 and abs(hh - h) < 1e-4


def test_1033_and_1230():
    d = rtcm.decode_1033(descriptor_payload(7, "TESTANT         NONE", 3, "AS-77", "RX-TYPE", "FW-1.0", "S-42"))
    assert (d.station_id, d.antenna, d.setup_id, d.antenna_serial) == (7, "TESTANT         NONE", 3, "AS-77")
    assert (d.receiver_type, d.receiver_firmware, d.receiver_serial) == ("RX-TYPE", "FW-1.0", "S-42")
    g = rtcm.decode_1230(glo_bias_payload(7, True, {0: 1.0, 2: -0.5}))
    assert g.aligned and g.biases == {"L1 C/A": 1.0, "L2 C/A": -0.5}
    with pytest.raises(ValueError):
        rtcm.decode_1033(descriptor_payload(7, "X", 0, "", "", "", "")[:4])


def test_band_mapping():
    assert rtcm.band_of("Galileo", 8) == "L6"  # E6C
    assert rtcm.band_of("Galileo", 22) == "L5"  # E5a
    assert rtcm.band_of("Galileo", 15) == "L2"  # E5b
    assert rtcm.band_of("Galileo", 20) == "E5ab"
    assert rtcm.band_of("BeiDou", 8) == "L6"  # B3I
    assert rtcm.band_of("BeiDou", 25) == "L2"  # B2b
    assert rtcm.band_of("BeiDou", 22) == "L5"  # B2a
    assert rtcm.band_of("GPS", 30) == "L1"  # L1C
    assert rtcm.msm_system(1137) == "NavIC" and rtcm.msm_system(1005) is None
    assert rtcm.is_suspect_sbas(1104) and not rtcm.is_suspect_sbas(1114)


# ---------------------------------------------------------------------------
# Summariser


def station_stream(seconds: int = 12, msm_rate_s: float = 1.0, with_station_msgs: bool = True,
                   msm_level: int = 7, include=("GPS", "GLONASS", "Galileo", "BeiDou", "QZSS")) -> bytes:
    """A constructed 1 Hz base-station stream: one burst per epoch."""
    gps_cells = {(5, 2): 45.0, (5, 10): 38.0, (5, 23): 44.0, (12, 2): 41.0, (12, 10): 35.0}
    glo_cells = {(3, 2): 43.0, (3, 8): 40.0, (14, 2): 39.0}
    bds_cells = {(19, 2): 42.0, (19, 8): 41.0, (19, 14): 40.0, (30, 2): 44.0}
    qzs_cells = {(2, 2): 40.0, (2, 23): 41.0}
    systems = [("GPS", 1070, gps_cells), ("GLONASS", 1080, glo_cells), ("Galileo", 1090, GAL_CELLS),
               ("BeiDou", 1120, bds_cells), ("QZSS", 1110, qzs_cells)]
    systems = [s for s in systems if s[0] in include]
    out = bytearray()
    t0_ms = 345600000  # Monday 00:00 GPS time of week
    steps = int(seconds / msm_rate_s)
    for k in range(steps):
        tow = t0_ms + int(k * msm_rate_s * 1000)
        for i, (name, base, cells) in enumerate(systems):
            last = i == len(systems) - 1
            if name == "GLONASS":
                tod = (tow + (3 * 3600 - 18) * 1000) % 86400000
                dow = ((tow + (3 * 3600 - 18) * 1000) // 86400000) % 7
                epoch = (dow << 27) | tod
            elif name == "BeiDou":
                epoch = tow - 14000
            else:
                epoch = tow
            out += msm_frame(base + msm_level, 100, epoch, not last, cells)
        if with_station_msgs and k % int(1 / msm_rate_s) == 0:
            out += rtcm.build_frame(arp_payload(100, 1111111.1111, -4444444.4444, 4555555.5555))
            out += rtcm.build_frame(descriptor_payload(100, "TESTANT         NONE", 0, "", "RX", "FW", "SN"))
            out += rtcm.build_frame(glo_bias_payload(100, True, {0: 0.5, 2: -0.26}))
    return bytes(out)


def summarise(stream: bytes) -> RtcmStats:
    sp = StreamSplitter()
    st = RtcmStats()
    for it in sp.feed(stream) + sp.flush():
        st.add_item(it)
    st.finish()
    return st


def checks_by_name(st: RtcmStats) -> dict[str, dict]:
    return {c["check"]: c for c in st.checks()}


def test_summary_of_a_good_station_stream():
    st = summarise(station_stream(seconds=12))
    d = st.to_dict()
    msgs = {m["type"]: m for m in d["messages"]}
    assert set(msgs) == {1077, 1087, 1097, 1127, 1117, 1005, 1033, 1230}
    for t in (1077, 1087, 1097, 1127, 1117):
        assert msgs[t]["epochs"] == 12 and abs(msgs[t]["rate_hz"] - 1.0) < 1e-6
    for t in (1005, 1033, 1230):
        assert msgs[t]["frames"] == 12 and abs(msgs[t]["rate_hz"] - 1.0) < 1e-6
        assert msgs[t]["time_source"] == "stream"
    systems = {s["system"]: s for s in d["systems"]}
    gal = systems["Galileo"]
    assert gal["bands_present"] == ["L1", "L2", "L5", "L6"]
    assert gal["max_sats"] == 2 and gal["distinct_sats"] == [3, 5]
    assert gal["bands"]["L1"]["mean_cnr_dbhz"] == pytest.approx((44.5 + 47.25) / 2)
    assert gal["bands"]["L6"]["mean_cnr_dbhz"] == pytest.approx((40.0 + 39.5) / 2)
    assert systems["BeiDou"]["bands_present"] == ["L1", "L2", "L6"]
    assert systems["GPS"]["bands_present"] == ["L1", "L2", "L5"]
    assert d["arp"]["station_id"] == 100 and d["arp"]["max_deviation_mm"] == 0.0
    assert d["descriptor_1033"]["antenna"] == "TESTANT         NONE"
    assert d["glonass_biases_1230"]["biases_m"] == pytest.approx({"L1 C/A": 0.5, "L2 C/A": -0.26})
    assert d["stream"]["crc_failures"] == 0 and d["stream"]["junk_bytes"] == 0
    c = checks_by_name(st)
    for name in ("GPS MSM7 at 1 Hz", "GLONASS MSM7 at 1 Hz", "Galileo MSM7 at 1 Hz", "BeiDou MSM7 at 1 Hz",
                 "QZSS MSM7 at 1 Hz", "1005 present", "1033 present", "1230 present",
                 "constellation with 4 bands", "MMB per epoch"):
        assert c[name]["status"] == "PASS", (name, c[name])
    assert "Galileo" in c["constellation with 4 bands"]["detail"]
    assert st.bursts == 12 and st.bursts_mmb_ok == 12
    text = st.report()
    assert "Onocoy checks" in text and "1097" in text


def test_summary_flags_what_onocoy_would_miss():
    stream = station_stream(seconds=6, with_station_msgs=False, msm_level=4, include=("GPS", "BeiDou"))
    c = checks_by_name(summarise(stream))
    assert c["GPS MSM7 at 1 Hz"]["status"] == "WARN"  # MSM4 only
    assert c["GLONASS MSM7 at 1 Hz"]["status"] == "FAIL"
    assert c["Galileo MSM7 at 1 Hz"]["status"] == "FAIL"
    assert c["1005 present"]["status"] == "FAIL"
    assert c["1033 present"]["status"] == "WARN"
    assert c["constellation with 4 bands"]["status"] == "FAIL"


def test_summary_flags_rates_above_1hz():
    c = checks_by_name(summarise(station_stream(seconds=4, msm_rate_s=0.5)))
    assert c["GPS MSM7 at 1 Hz"]["status"] == "FAIL"
    assert "above 1 Hz" in c["GPS MSM7 at 1 Hz"]["detail"]


def test_summary_counts_crc_failures_and_survives_them():
    stream = bytearray(station_stream(seconds=3))
    stream[40] ^= 0x01  # damage the first frame
    st = summarise(bytes(stream))
    assert st.crc_bad >= 1
    assert st.types[1127].epochs == 3  # later frames still counted


def test_summary_tracks_a_moving_1005():
    out = bytearray()
    for k in range(5):
        out += msm_frame(1077, 1, 345600000 + 1000 * k, False, {(1, 2): 40.0})
        out += rtcm.build_frame(arp_payload(1, 1111111.0 + 0.002 * k, -4444444.0, 4555555.0))
    st = summarise(bytes(out))
    assert st.arp_count == 5 and st.arp_max_dev_m == pytest.approx(0.008, abs=1e-4)
    assert checks_by_name(st)["1005 position stable"]["status"] == "INFO"
