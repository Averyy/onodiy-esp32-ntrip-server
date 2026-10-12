# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""NTRIP helpers for the local test tools (stdlib only): HTTP chunked
transfer coding, request parsing with the password masked, and a frame
recorder that checks an RTCM byte stream as it arrives.

Public protocol only: NTRIP 1.0 ("SOURCE <password> /<mountpoint>",
"ICY 200 OK") and NTRIP 2.0 (HTTP/1.1 with "Ntrip-Version: Ntrip/2.0",
Basic authentication, chunked bodies).
"""

from __future__ import annotations

import base64
from collections import Counter
from dataclasses import dataclass, field

from . import rtcm
from .stream import StreamSplitter

MASK = "***"


def chunk(data: bytes) -> bytes:
    """One HTTP chunk: hex size, CR LF, the data, CR LF."""
    return f"{len(data):x}\r\n".encode() + data + b"\r\n"


LAST_CHUNK = b"0\r\n\r\n"


class Dechunker:
    """Incremental HTTP chunked-body decoder.

    feed() returns the data bytes as they complete. `done` is set by the
    last chunk ("0" size line and its blank line); `error` names the first
    framing error, after which the rest is passed through as plain data (so
    nothing the peer sent is lost from the byte count)."""

    def __init__(self) -> None:
        self.buf = bytearray()
        self.left = 0  # data bytes still due in the current chunk
        self.state = "size"  # size, data, crlf, trailer, done, raw
        self.done = False
        self.error: str | None = None
        self.chunks = 0

    def feed(self, data: bytes) -> bytes:
        out = bytearray()
        self.buf += data
        while self.buf:
            if self.state == "raw":
                out += self.buf
                self.buf.clear()
            elif self.state == "done":
                if self.buf:
                    self._fail(f"{len(self.buf)} bytes after the last chunk")
                    continue
            elif self.state == "size":
                nl = self.buf.find(b"\r\n")
                if nl < 0:
                    if len(self.buf) > 64:
                        self._fail("chunk size line longer than 64 bytes")
                        continue
                    break
                line = bytes(self.buf[:nl]).split(b";", 1)[0].strip()
                try:
                    size = int(line, 16)
                except ValueError:
                    self._fail(f"bad chunk size line {bytes(self.buf[:nl])[:20]!r}")
                    continue
                del self.buf[: nl + 2]
                if size == 0:
                    self.state = "trailer"
                else:
                    self.left = size
                    self.state = "data"
            elif self.state == "data":
                take = min(self.left, len(self.buf))
                out += self.buf[:take]
                del self.buf[:take]
                self.left -= take
                if self.left == 0:
                    self.state = "crlf"
            elif self.state == "crlf":
                if len(self.buf) < 2:
                    break
                if self.buf[:2] != b"\r\n":
                    self._fail("chunk data not followed by CR LF")
                    continue
                del self.buf[:2]
                self.chunks += 1
                self.state = "size"
            elif self.state == "trailer":
                nl = self.buf.find(b"\r\n")
                if nl < 0:
                    break
                line = bytes(self.buf[:nl])
                del self.buf[: nl + 2]
                if not line:
                    self.state = "done"
                    self.done = True
        return bytes(out)

    def _fail(self, why: str) -> None:
        if self.error is None:
            self.error = why
        self.state = "raw"


def basic_auth(user: str, password: str) -> str:
    return "Basic " + base64.b64encode(f"{user}:{password}".encode()).decode()


@dataclass
class Request:
    """A request as a caster sees it. The password never leaves this object
    except through `password` (masked_line and masked_headers hide it)."""

    first_line: str = field(default="", repr=False)  # NTRIP 1.0 carries the password here
    method: str = ""
    mount: str = ""
    ntrip_version: int | None = None
    agent: str = ""
    user: str | None = None
    password: str | None = field(default=None, repr=False)
    headers: dict[str, str] = field(default_factory=dict, repr=False)
    chunked: bool = False
    complete: bool = False  # the blank line ending the header was seen

    @property
    def role(self) -> str:
        if self.method in ("SOURCE", "POST"):
            return "source"
        if self.method == "GET":
            return "client"
        return "unknown"

    @property
    def masked_line(self) -> str:
        if self.method == "SOURCE":
            parts = self.first_line.split(" ")
            if len(parts) >= 2:
                parts[1] = MASK
            return " ".join(parts)
        return self.first_line

    def masked_headers(self) -> dict[str, str]:
        out = {}
        for k, v in self.headers.items():
            if k == "authorization":
                scheme = v.split(" ", 1)[0]
                v = f"{scheme} {MASK}"
            out[k] = v
        return out


def parse_request(head: bytes) -> Request:
    """Parse the request head (everything before the blank line, or what
    arrived of it)."""
    text = head.decode("latin-1")
    lines = text.split("\r\n") if "\r\n" in text else text.split("\n")
    req = Request()
    req.first_line = lines[0].rstrip("\r") if lines else ""
    parts = req.first_line.split(" ")
    req.method = parts[0].upper() if parts and parts[0] else ""
    for line in lines[1:]:
        line = line.rstrip("\r")
        if not line or ":" not in line:
            continue
        k, v = line.split(":", 1)
        req.headers[k.strip().lower()] = v.strip()
    nv = req.headers.get("ntrip-version", "")
    if req.method == "SOURCE":
        req.ntrip_version = 1
        if len(parts) >= 2:
            req.password = parts[1]
        if len(parts) >= 3:
            req.mount = parts[2].lstrip("/")
        req.agent = req.headers.get("source-agent", req.headers.get("user-agent", ""))
    else:
        if len(parts) >= 2:
            req.mount = parts[1].split("?", 1)[0].lstrip("/")
        req.agent = req.headers.get("user-agent", req.headers.get("source-agent", ""))
        if "2.0" in nv:
            req.ntrip_version = 2
        elif req.method == "GET":
            req.ntrip_version = 1
    auth = req.headers.get("authorization", "")
    if auth.lower().startswith("basic "):
        try:
            user, _, pw = base64.b64decode(auth[6:].strip(), validate=True).decode("latin-1").partition(":")
            req.user = user
            req.password = pw
        except (ValueError, UnicodeDecodeError):
            req.user = None
    req.chunked = "chunked" in req.headers.get("transfer-encoding", "").lower()
    return req


class FrameRecord:
    """Checks an RTCM byte stream as it arrives: CRC-valid frames, CRC
    failures, bytes outside frames and a partial frame at the end.

    feed() returns the CRC-valid frames it completed (for relaying)."""

    def __init__(self, keep_frames: bool = True) -> None:
        self.split = StreamSplitter()
        self.bytes = 0
        self.frames_ok = 0
        self.frame_bytes = 0
        self.crc_failures = 0
        self.types: Counter = Counter()
        self.epochs = 0  # MSMs with the multiple-message bit clear
        self.first_t: float | None = None
        self.last_t: float | None = None
        self.keep = keep_frames
        self.frames: list[tuple[float | None, bytes]] = []
        self.partial_tail = 0
        self.finished = False

    def feed(self, data: bytes, t: float | None = None) -> list[bytes]:
        self.bytes += len(data)
        return self._items(self.split.feed(data, t))

    def _items(self, items) -> list[bytes]:
        out = []
        for it in items:
            if it.kind == "rtcm":
                self.frames_ok += 1
                self.frame_bytes += len(it.data)
                mtype = it.rtcm_type
                self.types[mtype] += 1
                if rtcm.msm_system(mtype) and not rtcm.is_suspect_sbas(mtype):
                    try:
                        if not rtcm.decode_msm(rtcm.payload_of(it.data)).mmb:
                            self.epochs += 1
                    except ValueError:
                        pass
                if it.t is not None:
                    if self.first_t is None:
                        self.first_t = it.t
                    self.last_t = it.t
                if self.keep:
                    self.frames.append((it.t, it.data))
                out.append(it.data)
            elif it.kind == "rtcm_bad":
                self.crc_failures += 1
        return out

    def finish(self) -> dict:
        """End of stream: account for what is left and return the summary."""
        if not self.finished:
            buf = bytes(self.split.buf)
            if buf and buf[0] == rtcm.PREAMBLE:
                if len(buf) < 3:
                    self.partial_tail = len(buf)
                else:
                    length = ((buf[1] & 0x03) << 8) | buf[2]
                    if not buf[1] >> 2 and len(buf) < length + 6:
                        self.partial_tail = len(buf)
            self._items(self.split.flush())
            self.finished = True
        return self.summary()

    def summary(self) -> dict:
        return {
            "bytes": self.bytes,
            "frames_ok": self.frames_ok,
            "crc_failures": self.crc_failures,
            "bytes_outside_frames": self.bytes - self.frame_bytes - self.partial_tail,
            "partial_tail": self.partial_tail,
            "types": {str(k): v for k, v in sorted(self.types.items())},
            "epochs": self.epochs,
            "first_frame_unix": self.first_t,
            "last_frame_unix": self.last_t,
            "frames_span_s": (self.last_t - self.first_t) if self.first_t is not None else None,
        }
