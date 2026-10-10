# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Split a receiver byte stream into RTCM 3 frames, text lines and junk.

The receiver port interleaves RTCM 3 frames, ASCII lines (NMEA sentences,
`$command,...` acknowledgements, `$CONFIG` dump lines, `#` logs, the `<`
continuation lines of list logs) and, if anyone enables them, Unicore binary
logs (sync AA 44 B5). The splitter takes bytes in chunks of any size and
returns items in stream order:

  rtcm      a CRC-valid RTCM 3 frame
  rtcm_bad  a 0xD3 candidate with a plausible header whose CRC failed; the
            splitter then re-scans from the byte after its preamble, so a
            valid frame that starts inside it is never lost
  text      one line without its CR/LF, with the checksum verdict
  binary    a Unicore binary log whose CRC-32 verifies
  junk      bytes that are none of the above (merged into runs)

Each item carries its stream offset and the host time of the chunk that
completed it (when the caller passes times to feed()).
"""

from __future__ import annotations

import bisect
import re
from dataclasses import dataclass

from .rtcm import PREAMBLE, crc24q

MAX_TEXT = 65536  # long ASCII logs (e.g. OBSVMA) are several KB on one line
JUNK_CHUNK = 4096

_TEXT_BYTES = re.compile(rb"[\x20-\x7e\t\r]*")
_PRINTABLE = set(range(0x20, 0x7F)) | {0x09}
_CHECKSUM = re.compile(r"^([$#])(.*)\*([0-9A-Fa-f]{8}|[0-9A-Fa-f]{2})$")


def xor_checksum(text: str) -> int:
    x = 0
    for c in text.encode("latin-1", "replace"):
        x ^= c
    return x


def crc32_unicore(data: bytes) -> int:
    """CRC-32 of `#` logs and binary logs: reflected 0xEDB88320, init 0, no final XOR."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = (crc >> 1) ^ 0xEDB88320 if crc & 1 else crc >> 1
    return crc


def check_text(line: str) -> str:
    """Checksum verdict for one text line.

    nmea     `$...*hh`, XOR of the bytes after `$` (NMEA rule)
    unicore  `$...*hh`, XOR including the `$` (receiver acks, $CONFIG, banners)
             or `#...*hh`, XOR including the `#` (short logs such as #MODE)
    crc32    `#...*hhhhhhhh`, CRC-32 over the bytes after `#`
    bad      a checksum is present but does not verify
    none     no checksum field
    """
    m = _CHECKSUM.match(line)
    if not m:
        return "none"
    lead, body, ck = m.groups()
    want = int(ck, 16)
    if lead == "$":
        if len(ck) != 2:
            return "bad"
        if xor_checksum(body) == want:
            return "nmea"
        if xor_checksum("$" + body) == want:
            return "unicore"
        return "bad"
    if len(ck) == 8:
        return "crc32" if crc32_unicore(body.encode("latin-1", "replace")) == want else "bad"
    return "unicore" if xor_checksum("#" + body) == want else "bad"


@dataclass
class Item:
    kind: str  # rtcm, rtcm_bad, text, binary, junk
    data: bytes
    offset: int
    t: float | None = None
    text: str = ""
    check: str = ""  # text: see check_text(); plus "partial"/"overflow" flags in `note`
    note: str = ""

    @property
    def rtcm_type(self) -> int | None:
        if self.kind not in ("rtcm", "rtcm_bad") or len(self.data) < 5:
            return None
        return (self.data[3] << 4) | (self.data[4] >> 4)


class StreamSplitter:
    def __init__(self) -> None:
        self.buf = bytearray()
        self.base = 0  # stream offset of buf[0]
        self.junk = bytearray()
        self.junk_offset = 0
        self._chunk_ends: list[int] = []
        self._chunk_times: list[float | None] = []
        self.stats = {
            "bytes": 0, "rtcm": 0, "rtcm_bad": 0, "text": 0, "text_bad": 0,
            "binary": 0, "junk_bytes": 0,
        }

    # -- public ---------------------------------------------------------------

    def feed(self, data: bytes, t: float | None = None) -> list[Item]:
        if data:
            self.buf += data
            self.stats["bytes"] += len(data)
            self._chunk_ends.append(self.base + len(self.buf))
            self._chunk_times.append(t)
        return self._run(final=False)

    def flush(self) -> list[Item]:
        """End of stream: emit whatever is buffered (partial lines, junk)."""
        out = self._run(final=True)
        self._flush_junk(out)
        return out

    # -- internals ------------------------------------------------------------

    def _time_at(self, end_offset: int) -> float | None:
        i = bisect.bisect_left(self._chunk_ends, end_offset)
        if i < len(self._chunk_times):
            return self._chunk_times[i]
        return self._chunk_times[-1] if self._chunk_times else None

    def _prune_times(self) -> None:
        i = bisect.bisect_right(self._chunk_ends, self.base)
        if i > 1:
            del self._chunk_ends[: i - 1]
            del self._chunk_times[: i - 1]

    def _add_junk(self, data: bytes | bytearray, offset: int, out: list[Item]) -> None:
        if not self.junk:
            self.junk_offset = offset
        self.junk += data
        if len(self.junk) >= JUNK_CHUNK:
            self._flush_junk(out)

    def _flush_junk(self, out: list[Item]) -> None:
        if self.junk:
            end = self.junk_offset + len(self.junk)
            out.append(Item("junk", bytes(self.junk), self.junk_offset, self._time_at(end)))
            self.stats["junk_bytes"] += len(self.junk)
            self.junk = bytearray()

    def _emit(self, out: list[Item], item: Item) -> None:
        self._flush_junk(out)
        item.t = self._time_at(item.offset + len(item.data))
        out.append(item)

    def _run(self, final: bool) -> list[Item]:
        out: list[Item] = []
        buf = self.buf
        i = 0
        n = len(buf)
        while i < n:
            b = buf[i]
            off = self.base + i
            if b == PREAMBLE:
                if n - i < 3:
                    if not final:
                        break
                    self._add_junk(buf[i : i + 1], off, out)
                    i += 1
                    continue
                reserved = buf[i + 1] >> 2
                length = ((buf[i + 1] & 0x03) << 8) | buf[i + 2]
                if reserved or length < 2:
                    self._add_junk(buf[i : i + 1], off, out)
                    i += 1
                    continue
                if n - i < length + 6:
                    if not final:
                        break
                    self._add_junk(buf[i : i + 1], off, out)
                    i += 1
                    continue
                frame = bytes(buf[i : i + length + 6])
                if crc24q(frame[: length + 3]) == int.from_bytes(frame[length + 3 :], "big"):
                    self._emit(out, Item("rtcm", frame, off))
                    self.stats["rtcm"] += 1
                    i += length + 6
                else:
                    mtype = (frame[3] << 4) | (frame[4] >> 4)
                    if 1001 <= mtype <= 1300 or 4001 <= mtype <= 4095:
                        self._emit(out, Item("rtcm_bad", frame, off, note=f"crc mismatch, type {mtype}?"))
                        self.stats["rtcm_bad"] += 1
                    else:
                        self._add_junk(buf[i : i + 1], off, out)
                    i += 1  # re-scan from the byte after this preamble
                continue
            if b in _PRINTABLE:
                m = _TEXT_BYTES.match(buf, i)
                j = m.end()
                if j < n and buf[j] == 0x0A:
                    self._emit_text(out, buf[i:j], off, "")
                    i = j + 1
                    continue
                if j < n:  # a non-text byte ended the line early
                    self._emit_text(out, buf[i:j], off, "partial")
                    i = j
                    continue
                if j - i > MAX_TEXT:
                    self._emit_text(out, buf[i:j], off, "overflow")
                    i = j
                    continue
                if final:
                    self._emit_text(out, buf[i:j], off, "unterminated")
                    i = j
                    continue
                break
            if b in (0x0D, 0x0A):
                i += 1  # stray line terminators
                continue
            if b == 0xAA:
                if n - i < 3 and not final:
                    break
                if buf[i + 1 : i + 3] == b"\x44\xb5":
                    if n - i < 24:
                        if not final:
                            break
                    else:
                        msg_len = buf[i + 6] | (buf[i + 7] << 8)
                        total = 24 + msg_len + 4
                        if msg_len <= 4096:
                            if n - i < total and not final:
                                break
                            if n - i >= total:
                                blk = bytes(buf[i : i + total])
                                if crc32_unicore(blk[:-4]) == int.from_bytes(blk[-4:], "little"):
                                    msg_id = blk[4] | (blk[5] << 8)
                                    self._emit(out, Item("binary", blk, off, note=f"msg {msg_id} len {msg_len}"))
                                    self.stats["binary"] += 1
                                    i += total
                                    continue
            self._add_junk(buf[i : i + 1], off, out)
            i += 1
        if i:
            del buf[:i]
            self.base += i
            self._prune_times()
        return out

    def _emit_text(self, out: list[Item], raw: bytes | bytearray, off: int, note: str) -> None:
        line = bytes(raw).rstrip(b"\r").decode("latin-1")
        check = check_text(line)
        if not line.strip():
            return
        self._emit(out, Item("text", bytes(raw), off, text=line, check=check, note=note))
        self.stats["text"] += 1
        if check == "bad":
            self.stats["text_bad"] += 1
