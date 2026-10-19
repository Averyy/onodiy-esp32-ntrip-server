# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Frame/line splitter: RTCM 3 framing, text lines, junk, resync, chunking."""

from __future__ import annotations

import random

import pytest

from benchlib.rtcm import build_frame, crc24q
from benchlib.stream import StreamSplitter, check_text, crc32_unicore
from builders import arp_payload, msm_frame, nmea, unicore_line


def frames():
    a = build_frame(arp_payload(17, 1111111.1111, -4444444.4444, 4555555.5555))
    b = msm_frame(1077, 17, 345600000, True, {(5, 2): 45.0, (5, 15): 40.0, (12, 2): 38.5})
    c = msm_frame(1127, 17, 345586000, False, {(19, 2): 41.0, (19, 8): 39.0})
    return a, b, c


def split_all(data: bytes, chunks: list[int] | None = None):
    sp = StreamSplitter()
    items = []
    if chunks is None:
        items += sp.feed(data)
    else:
        pos = 0
        for n in chunks:
            items += sp.feed(data[pos : pos + n])
            pos += n
        items += sp.feed(data[pos:])
    items += sp.flush()
    return items


def signature(items):
    return [(it.kind, it.data if it.kind != "text" else it.text) for it in items]


def test_crc24q_catalogue_check_value():
    # CRC-24/LTE-A parameters (poly 0x864CFB, init 0, no reflection, no xorout).
    assert crc24q(b"123456789") == 0xCDE703


def test_build_frame_layout():
    payload = bytes(range(40))
    f = build_frame(payload)
    assert f[0] == 0xD3
    assert f[1] >> 2 == 0  # reserved bits
    assert ((f[1] & 3) << 8) | f[2] == 40
    assert len(f) == 40 + 6
    assert crc24q(f) == 0  # CRC over the whole frame including the parity is 0
    with pytest.raises(ValueError):
        build_frame(bytes(1024))


def test_mixed_stream_any_chunking():
    a, b, c = frames()
    ack = unicore_line("command,VERSIONA,response: OK")
    gga = nmea("GPGGA,120000.00,4500.0000,N,07500.0000,W,1,12,0.8,100.0,M,-34.0,M,,")
    junk = bytes([0x00, 0xFF, 0x80, 0x01])
    stream = a + (ack + "\r\n").encode() + b + junk + (gga + "\r\n").encode() + c
    whole = split_all(stream)
    assert signature(whole) == [
        ("rtcm", a), ("text", ack), ("rtcm", b), ("junk", junk), ("text", gga), ("rtcm", c),
    ]
    assert whole[1].check == "unicore"
    assert whole[4].check == "nmea"
    # Same result byte by byte and in random chunks.
    assert signature(split_all(stream, [1] * len(stream))) == signature(whole)
    rng = random.Random(1)
    for _ in range(20):
        cuts = [rng.randint(1, 60) for _ in range(40)]
        assert signature(split_all(stream, cuts)) == signature(whole)


def test_offsets_and_host_times():
    a, b, _ = frames()
    sp = StreamSplitter()
    items = sp.feed(a + b[:10], t=1.0) + sp.feed(b[10:], t=2.0) + sp.flush()
    assert [it.kind for it in items] == ["rtcm", "rtcm"]
    assert items[0].offset == 0 and items[0].t == 1.0
    assert items[1].offset == len(a) and items[1].t == 2.0  # completed by the second chunk


def test_payload_containing_preamble_bytes():
    payload = bytes([0x3E, 0xD0, 0x00, 0xD3, 0x00, 0x13, 0xD3, 0xD3, 0x55])
    f = build_frame(payload)
    assert signature(split_all(b"\x00" + f + b"\x00")) == [("junk", b"\x00"), ("rtcm", f), ("junk", b"\x00")]


def test_corrupt_frame_then_resync():
    a, b, c = frames()
    bad = bytearray(b)
    bad[10] ^= 0x40  # flip a payload bit: CRC fails
    items = split_all(a + bytes(bad) + c)
    kinds = [it.kind for it in items]
    assert kinds[0] == "rtcm" and items[0].data == a
    assert "rtcm_bad" in kinds  # plausible type 1077 with a failing CRC is reported
    assert items[-1].kind == "rtcm" and items[-1].data == c  # the next frame is found again
    assert sum(1 for k in kinds if k == "rtcm") == 2


def test_false_preamble_hides_no_frame():
    # A bogus header claiming 261 bytes, immediately followed by a real frame:
    # the bogus candidate fails its CRC and the scan restarts one byte later.
    _, b, _ = frames()
    bogus = b"\xD3\x01\x05"
    tail = bytes(300)  # enough bytes for the bogus candidate to be complete
    items = split_all(bogus + b + tail)
    assert [it.data for it in items if it.kind == "rtcm"] == [b]


def test_reserved_bits_reject_candidate():
    payload = bytes(range(10))
    head = bytes([0xD3, 0x04 | 0x00, len(payload)]) + payload  # a reserved bit set
    f = head + crc24q(head).to_bytes(3, "big")
    items = split_all(f)
    assert all(it.kind != "rtcm" for it in items)


def test_text_line_cut_by_binary_and_unterminated_tail():
    a, _, _ = frames()
    items = split_all(b"$command,MODE,resp" + a + b"#VERSIONA,partial")
    assert [it.kind for it in items] == ["text", "rtcm", "text"]
    assert items[0].note == "partial" and items[0].text == "$command,MODE,resp"
    assert items[2].note == "unterminated"


def test_continuation_and_plain_lines():
    items = split_all(b"<     3\r\n<     GPGGA COM1 1\r\nhello\r\n")
    assert [(it.text, it.check) for it in items] == [
        ("<     3", "none"), ("<     GPGGA COM1 1", "none"), ("hello", "none"),
    ]


def test_checksum_rules():
    assert check_text(nmea("GPGSA,A,3,,,,,,,,,,,,,1.0,0.8,0.6")) == "nmea"
    assert check_text(unicore_line("command,CONFIG,response: OK")) == "unicore"
    assert check_text(unicore_line("CONFIG,COM1,CONFIG COM1 115200")) == "unicore"
    assert check_text("$command,CONFIG,response: OK*00") == "bad"
    assert check_text("no checksum here") == "none"
    # Made-up lines in the layouts of the Unicore N4 command manual (made-up P/N, serial, chip ID
    # and date): #MODE uses a 2-hex XOR including '#'; #VERSIONA a CRC-32 over the bytes after '#'
    # (computed with zlib: crc32(data, 0xFFFFFFFF) ^ 0xFFFFFFFF).
    assert check_text("#MODE,80,GPS,FINE,2400,100000000,0,0,18,12;MODE ROVER SURVEY,*2F") == "unicore"
    versiona = ('#VERSIONA,80,GPS,FINE,2400,100000000,12345,0,18,12;"UM982","R4.10Build12345",'
                '"HRPT00-S10C-P","1234567890123-XX00A0000000001","0011223344556677","2025/01/02"*fcd153ad')
    assert check_text(versiona) == "crc32"
    assert check_text(versiona.replace("*fcd153ad", "*fcd153ac")) == "bad"


def test_unicore_binary_log_with_valid_crc():
    body = bytes(range(12))
    header = bytes([0xAA, 0x44, 0xB5, 0x00]) + (37).to_bytes(2, "little") + len(body).to_bytes(2, "little") + bytes(16)
    blk = header + body
    blk += crc32_unicore(blk).to_bytes(4, "little")
    a, _, _ = frames()
    items = split_all(blk + a)
    assert [it.kind for it in items] == ["binary", "rtcm"]
    assert "msg 37" in items[0].note
