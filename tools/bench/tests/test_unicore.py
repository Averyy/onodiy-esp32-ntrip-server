# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Receiver command safety rules and reply parsers."""

from __future__ import annotations

import pytest

from benchlib.stream import StreamSplitter, crc32_unicore
from benchlib.unicore import (
    DAY_ONE, blocked_reason, data_matcher, four_band_systems, is_read_only, is_restarting_line, parse_antgroup,
    parse_config, parse_gsv, parse_obsvm, parse_versiona, refusal_text, signal_group, warnings_for,
)
from builders import nmea, unicore_line
from receiver_25164 import GROUP_BODIES, LINES, group_log


@pytest.mark.parametrize("cmd", ["FRESET", "freset", "  FReset now", "RESET ALL", "reset ephem", "T@T@T@",
                                 "6", " 2 ", "XMODEM", "upgrade com1", "VERSIONA\x00", "boot"])
def test_never_sent(cmd):
    assert blocked_reason(cmd)


@pytest.mark.parametrize("cmd", ["CONFIG SIGNALGROUP 3 6", "SAVECONFIG", "RESET", "UNLOG COM2 RTCM1097",
                                 "RTCM1097 COM2 1", "GPGSV 1", "MODE BASE 1 TIME 60 0"])
def test_needs_write(cmd):
    assert not blocked_reason(cmd)
    assert not is_read_only(cmd)


def test_day_one_set_is_read_only():
    for cmd in DAY_ONE:
        assert is_read_only(cmd) and not blocked_reason(cmd)
    assert is_read_only(" versiona ")
    assert not is_read_only("GPGSV COM1")  # a port makes it output elsewhere
    assert warnings_for("CONFIG SIGNALGROUP 3 6") and warnings_for("unlog com1")


def test_group_queries_are_one_shots_only():
    assert {"ANT1GROUPA", "ANT2GROUPA"} <= set(DAY_ONE)
    assert is_read_only("ANT1GROUPA") and is_read_only("ant2groupa")
    # With a period it would become a periodic log: not read-only, and it says why.
    for cmd in ("ANT1GROUPA 1", "ANT2GROUPA COM2 1", "ANT1GROUP 1"):
        assert not is_read_only(cmd) and not blocked_reason(cmd)
        assert any("periodic log" in w for w in warnings_for(cmd))
    assert not warnings_for("ANT1GROUPA")


def test_signal_group_warning_has_the_measured_restart():
    (w,) = warnings_for("CONFIG SIGNALGROUP 3 6")
    assert "8-9 s" in w and "30 s" not in w


def test_refusal_spellings():
    assert refusal_text("rtcm1230 10") == "PARSING FAILED NO MATCHING FUNC  10"
    assert refusal_text("FOOBAR") == "PARSING FAILED NO MATCHING FUNC  FOOBAR"   # build 25164
    assert refusal_text("FOOBAR", "FAILD") == "PARSING FAILD NO MATCHING FUNC  FOOBAR"   # build 18975


def test_restarting_line_is_recognised_not_garbage():
    line = LINES["restarting"]
    assert is_restarting_line(line) and is_restarting_line("The Board is resetting")
    for other in (LINES["banner"], LINES["refused"], "$GNGGA,,*56", "", "the board is fine"):
        assert not is_restarting_line(other)
    # On the wire it is a whole text line (no `$`, so no checksum verdict), not junk bytes.
    items = StreamSplitter().feed((line + "\r\n" + LINES["banner"] + "\r\n").encode())
    assert [(i.kind, i.text) for i in items] == [("text", line), ("text", LINES["banner"])]


# What the bench notes read out of each ANT1GROUPA body, per signal group (master antenna).
MASTER = {
    (4, 5): {"GPS": ["L1C/A", "L2C", "L2P", "L5"], "GLONASS": ["G1", "G2"], "Galileo": ["E1", "E5a", "E5b"],
             "BeiDou": ["B1I", "B2I", "B3I"], "QZSS": ["L1C/A", "L2C", "L5"]},
    (3, 6): {"GPS": ["L1C/A", "L2C", "L2P", "L5"], "GLONASS": ["G1", "G2"], "Galileo": ["E1", "E5a", "E5b", "E6"],
             "BeiDou": ["B1I", "B3I", "B1C", "B2b"], "QZSS": ["L1C/A", "L2C", "L5", "L6"]},
    (7, 0): {"GPS": ["L1C/A", "L2C", "L2P", "L5"], "GLONASS": ["G1", "G2"], "Galileo": ["E1", "E5a", "E5b"],
             "BeiDou": ["B1I", "B2I", "B3I", "B1C", "B2a", "B2b"], "QZSS": ["L1C/A", "L2C", "L5"]},
    (5, 0): {"GPS": ["L1C/A", "L2C", "L2P"], "GLONASS": ["G1", "G2"], "Galileo": ["E1", "E5b"],
             "BeiDou": ["B1I", "B2I", "B3I"], "QZSS": ["L1C/A", "L2C"]},
}
FOUR_BAND = {(4, 5): [], (3, 6): ["Galileo"], (7, 0): ["BeiDou"], (5, 0): []}


@pytest.mark.parametrize("group", sorted(GROUP_BODIES))
def test_group_signals_of_build_25164(group):
    master_body, slave_body = GROUP_BODIES[group]
    m = parse_antgroup(master_body, "master")
    assert not m.error and m.group == group[0]
    assert m.signals == MASTER[group]
    assert m.four_band() == FOUR_BAND[group]
    # The whole log line decodes the same.
    assert parse_antgroup(group_log("ANT1GROUPA", master_body)).signals == MASTER[group]
    s = parse_antgroup(slave_body, "slave")
    assert not s.error and s.group == group[1]
    assert s.off == (group[1] == 0)
    if s.off:
        assert s.four_band() == []
    text = "\n".join(s.lines())
    assert ("slave antenna is off" in text) == s.off
    d = m.to_dict()
    assert d["group"] == group[0] and d["four_band"] == FOUR_BAND[group]


def test_group_signals_bands_and_odd_input():
    m = parse_antgroup(GROUP_BODIES[(3, 6)][0])
    assert m.bands("Galileo") == ["L1", "L2", "L5", "L6"]
    assert m.bands("QZSS") == ["L1", "L2", "L5", "L6"]       # four bands, but QZSS earns no band reward
    assert m.bands("BeiDou") == ["L1", "L2", "L6"] and m.bands("GLONASS") == ["L1", "L2"]
    assert m.masks["BeiDou"] == 0x2D and "E6" in "\n".join(m.lines())
    # The slave of 3 6: GPS L1C/A, L2C, L2P; no GLONASS.
    s = parse_antgroup(GROUP_BODIES[(3, 6)][1], "slave")
    assert s.signals["GPS"] == ["L1C/A", "L2C", "L2P"] and "GLONASS" not in s.signals and not s.off
    # A system or bit not in the table is kept by name.
    odd = parse_antgroup("3,2,GQ,0x08,GI,0x01")
    assert odd.signals == {"QZSS": ["bit3"], "GI": ["bit0"]}
    for bad in ("", "x,y", "3,2,GP,0x1D", "3,1,GP,zz", "3,1,GP"):
        assert parse_antgroup(bad).error, bad
    assert "could not decode" in parse_antgroup("3,1,GP").lines()[0]


def test_group_matcher_takes_the_answer_in_any_header():
    match = data_matcher("ANT1GROUPA")
    body = GROUP_BODIES[(4, 5)][0]
    for line in (group_log("ANT1GROUPA", body), body, f"$ANT1GROUPA,{body}*00", "#ANT1GROUP,1;" + body):
        assert match(line), line
        assert parse_antgroup(line).signals == MASTER[(4, 5)]
    for line in ("$GPGGA,1,2*00", "#VERSIONA,1;x", LINES["restarting"], "$command,ANT1GROUPA,response: OK*00"):
        assert not match(line), line
    assert data_matcher("ANT2GROUPA")(group_log("ANT2GROUPA", GROUP_BODIES[(3, 6)][1]))


def test_data_matchers():
    assert data_matcher("CONFIG")("$CONFIG,COM1,CONFIG COM1 115200*23")
    assert data_matcher("MASK")("$CONFIG,MASK,MASK 5.000000*15")
    assert data_matcher("UNILOGLIST")("<     GPGGA COM1 1")
    assert data_matcher("VERSIONA")("#VERSIONA,79,GPS;...")
    assert not data_matcher("VERSIONA")("#MODE,1;x")
    assert data_matcher("GPGSV")("$GAGSV,1,1,01,05,61,163,49,5*00")
    assert data_matcher("RTCM1077 COM2 1") is None


def test_parse_versiona_body_rule():
    body = '"UM982","R4.10Build15434","HRPT00-S10C-P","2310415000012-LR23A2225208904","ff2740966a10124c","2024/08/08"'
    line = "#VERSIONA,79,GPS,FINE,2326,378237000,15434,0,18,889;" + body
    v = parse_versiona(f"{line}*{crc32_unicore(line[1:].encode()):08x}")
    assert v["model"] == "UM982" and v["build"] == 15434 and v["firmware"] == "R4.10Build15434"
    assert v["pn"] == "2310415000012" and v["serial"] == "LR23A2225208904"
    assert v["compile_date"] == "2024/08/08" and v["header_fields"] == 10


def test_parse_config_and_signal_group():
    lines = [unicore_line("CONFIG,SIGNALGROUP,CONFIG SIGNALGROUP 3 6"),
             unicore_line("CONFIG,COM1,CONFIG COM1 115200"),
             unicore_line("CONFIG,MASK,MASK 5.000000"), unicore_line("CONFIG,MASK,MASK GPS")]
    cfg = parse_config(lines)
    assert signal_group(cfg) == (3, 6)
    assert cfg["MASK"] == ["MASK 5.000000", "MASK GPS"]
    assert signal_group({}) is None


def test_parse_gsv_signal_ids():
    lines = [
        nmea("GAGSV,1,1,02,05,61,163,49,09,12,145,31,7"),  # E1-BC
        nmea("GAGSV,1,1,02,05,61,163,45,09,12,145,,5"),   # E6-BC, one satellite without C/N0
        nmea("GAGSV,1,1,01,05,61,163,47,1"),               # E5a
        nmea("GAGSV,1,1,01,05,61,163,46,2"),               # E5b
        nmea("GBGSV,1,1,01,19,40,100,41,5"),               # B2a
    ]
    view = parse_gsv(lines)
    assert view.band_list("Galileo") == ["L1", "L2", "L5", "L6"]
    assert view.band_list("BeiDou") == ["L5"]
    assert four_band_systems(view) == ["Galileo"]
    assert [c for _, c in view.signals["Galileo"]["E6-BC"]] == [45.0, None]


def _obs(sys_id: int, sig: int, prn: int, cn0: float) -> str:
    status = (sys_id << 16) | (sig << 21) | 0x1C00
    return f"0,{prn},20000000.000,-100000000.000000,50,150,-100.000,{int(cn0 * 100)},0,100.000,{status:08x}"


def test_parse_obsvm_tracking_status():
    obs = [_obs(3, 2, 5, 47.0), _obs(3, 22, 5, 44.0), _obs(3, 12, 5, 45.0), _obs(3, 17, 5, 46.0),
           _obs(4, 0, 19, 42.0), _obs(0, 0, 7, 40.0)]
    line = "#OBSVMA,94,GPS,FINE,2190,117395000,0,0,18,17;" + f"{len(obs)}," + ",".join(obs)
    view = parse_obsvm(f"{line}*{crc32_unicore(line[1:].encode()):08x}")
    assert view.band_list("Galileo") == ["L1", "L2", "L5", "L6"]
    assert "E6C" in view.signals["Galileo"]
    assert view.band_list("BeiDou") == ["L1"] and view.band_list("GPS") == ["L1"]


def test_four_band_systems_counts_rewarded_constellations_only():
    # QZSS and Galileo both with L1, L2, L5 and L6: only Galileo earns the band reward (as um982.four_band).
    obs = [_obs(3, 2, 5, 47.0), _obs(3, 17, 5, 46.0), _obs(3, 12, 5, 45.0), _obs(3, 22, 5, 44.0),
           _obs(5, 0, 194, 43.0), _obs(5, 17, 194, 42.0), _obs(5, 6, 194, 41.0), _obs(5, 21, 194, 40.0)]
    line = "#OBSVMA,94,GPS,FINE,2190,117395000,0,0,18,17;" + f"{len(obs)}," + ",".join(obs)
    view = parse_obsvm(f"{line}*{crc32_unicore(line[1:].encode()):08x}")
    assert view.band_list("QZSS") == ["L1", "L2", "L5", "L6"]
    assert view.band_list("Galileo") == ["L1", "L2", "L5", "L6"]
    assert four_band_systems(view) == ["Galileo"]


def test_four_band_systems_qzss_alone_is_none():
    lines = [
        nmea("GPGSV,1,1,01,194,61,163,44,1"),  # QZSS L1C/A
        nmea("GPGSV,1,1,01,194,61,163,43,5"),  # L2C-M
        nmea("GPGSV,1,1,01,194,61,163,42,7"),  # L5-I
        nmea("GPGSV,1,1,01,194,61,163,41,9"),  # L6D
        nmea("GAGSV,1,1,01,05,61,163,49,7"),   # Galileo E1 only
    ]
    view = parse_gsv(lines)
    assert view.band_list("QZSS") == ["L1", "L2", "L5", "L6"]
    assert view.band_list("Galileo") == ["L1"]
    assert four_band_systems(view) == []
