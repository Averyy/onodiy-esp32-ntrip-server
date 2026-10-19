#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Read an NTRIP caster as a rover does: its source table, or a mountpoint's RTCM checked frame by frame.

  uv run ntrip_client.py --host HOST [--port 2101] [--mount NAME | --sourcetable] [--user rover]
                         [--password-env VAR | --password-prompt] [--ntrip-version 1|2] [--duration S]
                         [--min-frames N] [--save] [--json] [--log-dir DIR]

Source table (--sourcetable, or no --mount): GET /. Prints the reply's
status line and headers, the table, and each STR record field by field.
Exit 0 when the table ends with ENDSOURCETABLE and holds at least one STR
record of 19 fields, 1 when it does not, 2 when the caster refused.

Stream (--mount NAME): GET /NAME, with Basic authentication when a
password is given (NTRIP 2.0: over HTTP/1.1 with Ntrip-Version and Host;
NTRIP 1.0: over HTTP/1.0). Prints the reply's status line and headers
("ICY 200 OK" is taken with or without a blank line after it), decodes
NTRIP 2.0 chunks (each chunk must hold whole frames), checks every RTCM
frame, and every 10 s prints frames, bytes, frames/s, epochs, CRC
failures, bytes outside frames and the message types. At the end
(--duration, the caster closing, Ctrl-C): exit 0 when at least
--min-frames frames came, no CRC failed, no byte was outside a frame, the
chunk framing was clean and every chunk held whole frames; 1 when a check
failed; 2 when the caster refused (its code and reason are printed: 401,
403, 429 with its Retry-After, 503, or its source table for a mountpoint
it does not have).

Read-only: one GET and nothing else (no position, no GGA). There is no
default host. The password comes from an environment variable
(--password-env) or a prompt (--password-prompt), never from the command
line; it is never printed or written to the run directory, and the
request is printed without its Authorization header. Without a password
no Authorization header is sent.

Each run writes to a new directory under --log-dir: command.txt,
reply-header.txt (the caster's reply header as received), summary.json,
sourcetable.txt for a table, and with --save the stream as
stream-NNNN.bin + .idx (NTRIP 2.0 chunking removed; ntrip_timing.py's
format, so `ntrip_timing.py report` reads it).

Examples:
  uv run ntrip_client.py --host onodiy.local --sourcetable
  export ROVER_PASSWORD=...     # the password chosen on the station's "Your rover" card
  uv run ntrip_client.py --host onodiy.local --mount onodiy --password-env ROVER_PASSWORD \\
      --ntrip-version 2 --duration 600
  uv run ntrip_client.py --host 127.0.0.1 --port 2101 --mount ROVER --password-prompt --save --json

Importable (tools/qemu/e2e.py, standard library only): fetch_sourcetable(),
open_stream(), read_for().
"""

from __future__ import annotations

import argparse
import getpass
import json
import os
import socket
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

from benchlib.common import (
    FLUSH_EVERY_S, RotatingFile, StopFlag, add_log_args, human_bytes, make_run_dir, write_command_line,
)
from benchlib.ntrip import SOURCETABLE_END, STR_FIELDS, Dechunker, FrameRecord, basic_auth, parse_sourcetable

AGENT = "NTRIP onodiy-ntrip-client/1.0"
DEFAULT_PORT = 2101
DEFAULT_USER = "rover"          # the station's default rover username
REPLY_TIMEOUT_S = 15.0          # connect and the whole reply header
BODY_TIMEOUT_S = 5.0            # a refusal's or a table's body, and the close after it
AFTER_LAST_CHUNK_S = 2.0        # NTRIP 2.0: how long the close may take after the last chunk
HEAD_MAX = 16384
STATUS_EVERY_S = 10.0
PART_LIMIT = 100 * 1024 * 1024  # --save: bytes per stream part
EXIT_OK, EXIT_FAILED, EXIT_REFUSED = 0, 1, 2


# ---------------------------------------------------------------------------
# request


def host_header(host: str, port: int) -> str:
    h = f"[{host}]" if ":" in host and not host.startswith("[") else host
    return f"{h}:{port}"


def build_request(host: str, port: int, mount: str, user: str | None = DEFAULT_USER, password: str | None = None,
                  ntrip_version: int = 1, http_version: str | None = None,
                  extra_headers: dict[str, str] | None = None) -> bytes:
    """The GET a rover sends. `http_version` "1.0" or "1.1" (default: 1.1 for
    NTRIP 2.0, 1.0 for 1.0); Authorization only with a password."""
    if http_version is None:
        http_version = "1.1" if ntrip_version == 2 else "1.0"
    lines = [f"GET /{mount} HTTP/{http_version}"]
    if ntrip_version == 2:
        lines.append(f"Host: {host_header(host, port)}")
        lines.append("Ntrip-Version: Ntrip/2.0")
    lines.append(f"User-Agent: {AGENT}")
    if password is not None:
        lines.append(f"Authorization: {basic_auth(user or '', password)}")
    for k, v in (extra_headers or {}).items():
        lines.append(f"{k}: {v}")
    if ntrip_version == 2:
        lines.append("Connection: close")
    return ("\r\n".join(lines) + "\r\n\r\n").encode("latin-1")


def shown_request(req: bytes) -> list[str]:
    """The request's lines without the Authorization header (never printed or saved)."""
    out = []
    for line in req.decode("latin-1").split("\r\n"):
        if line and not line.lower().startswith("authorization:"):
            out.append(line)
    return out


# ---------------------------------------------------------------------------
# reply


@dataclass
class Reply:
    status_line: str = ""
    kind: str = "unknown"      # icy, sourcetable, http, unknown
    code: int | None = None
    reason: str = ""
    http: str | None = None    # "1.0" / "1.1" for an HTTP status line
    headers: list[tuple[str, str]] = field(default_factory=list)
    head: bytes = b""          # as received: status line, header lines, the blank line (ICY: once seen)

    def header(self, name: str) -> str | None:
        n = name.lower()
        return next((v for k, v in self.headers if k.lower() == n), None)

    @property
    def accepted(self) -> bool:
        """A stream starts: ICY 200 OK, or HTTP 200 that is no source table."""
        return self.code == 200 and self.kind in ("icy", "http")

    @property
    def chunked(self) -> bool:
        return self.kind == "http" and "chunked" in (self.header("Transfer-Encoding") or "").lower()

    def to_json(self) -> dict:
        return {"status_line": self.status_line, "kind": self.kind, "code": self.code, "reason": self.reason,
                "headers": [[k, v] for k, v in self.headers]}


def parse_status(line: str) -> Reply:
    r = Reply(status_line=line)
    parts = line.split(" ", 2)
    if parts[0] == "ICY":
        r.kind = "icy"
    elif parts[0] == "SOURCETABLE":
        r.kind = "sourcetable"
    elif parts[0] in ("HTTP/1.0", "HTTP/1.1"):
        r.kind = "http"
        r.http = parts[0][5:]
    else:
        return r
    if len(parts) >= 2 and parts[1].isdigit():
        r.code = int(parts[1])
        r.reason = parts[2] if len(parts) > 2 else ""
    return r


def parse_headers(text: str) -> list[tuple[str, str]]:
    out = []
    for line in text.replace("\r\n", "\n").split("\n"):
        if ":" in line:
            k, v = line.split(":", 1)
            out.append((k.strip(), v.strip()))
    return out


def blank_end(buf: bytes | bytearray) -> int | None:
    """Offset just after the blank line that ends a header (CR LF CR LF, or LF LF), or None."""
    ends = [i + 4 for i in [buf.find(b"\r\n\r\n")] if i >= 0] + [i + 2 for i in [buf.find(b"\n\n")] if i >= 0]
    return min(ends) if ends else None


def read_head(sock: socket.socket, timeout: float = REPLY_TIMEOUT_S) -> tuple[Reply, bytes]:
    """The caster's reply header and the bytes that came after it. After
    "ICY 200 OK" only the status line is taken: the stream decides on a blank
    line or header lines from what follows (Stream.feed)."""
    buf = bytearray()
    deadline = time.monotonic() + timeout
    while True:
        nl = buf.find(b"\n")
        if nl >= 0:
            r = parse_status(bytes(buf[:nl]).rstrip(b"\r").decode("latin-1"))
            if r.kind == "icy":
                r.head = bytes(buf[: nl + 1])
                return r, bytes(buf[nl + 1 :])
            end = blank_end(buf)
            if end is not None:
                r.head = bytes(buf[:end])
                r.headers = parse_headers(r.head[nl + 1 :].decode("latin-1"))
                if r.kind == "http" and r.code == 200 and "sourcetable" in (r.header("Content-Type") or "").lower():
                    r.kind = "sourcetable"
                return r, bytes(buf[end:])
        if len(buf) > HEAD_MAX:
            raise ValueError(f"reply header longer than {HEAD_MAX} bytes")
        left = deadline - time.monotonic()
        if left <= 0:
            raise TimeoutError(f"no complete reply within {timeout:.0f} s (got {bytes(buf[:80])!r})")
        sock.settimeout(left)
        data = sock.recv(4096)
        if not data:
            if not buf:
                raise ConnectionError("the caster closed without a reply")
            # A reply line without the blank line (some 1.0 casters' error lines), then the close.
            nl = buf.find(b"\n")
            r = parse_status(bytes(buf[: nl if nl >= 0 else len(buf)]).rstrip(b"\r").decode("latin-1"))
            r.head = bytes(buf)
            if nl >= 0:
                r.headers = parse_headers(bytes(buf[nl + 1 :]).decode("latin-1"))
            return r, b""
        buf += data


def read_body(sock: socket.socket, reply: Reply, rest: bytes, timeout: float = BODY_TIMEOUT_S) -> tuple[bytes, bool]:
    """A reply's body: everything until the caster closes (or `timeout`).
    Returns (body, the caster closed)."""
    body = bytearray(rest)
    deadline = time.monotonic() + timeout
    while True:
        left = deadline - time.monotonic()
        if left <= 0:
            return bytes(body), False
        sock.settimeout(left)
        try:
            data = sock.recv(65536)
        except socket.timeout:
            return bytes(body), False
        except OSError:
            return bytes(body), True   # reset: closed all the same
        if not data:
            return bytes(body), True
        body += data


def connect(host: str, port: int, timeout: float = REPLY_TIMEOUT_S, rcvbuf: int | None = None) -> socket.socket:
    """A TCP connection; `rcvbuf` sets SO_RCVBUF before the connect (a small window for a rover that stalls)."""
    err: OSError | None = None
    for family, kind, proto, _name, addr in socket.getaddrinfo(host, port, type=socket.SOCK_STREAM):
        s = socket.socket(family, kind, proto)
        try:
            if rcvbuf:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, rcvbuf)
            s.settimeout(timeout)
            s.connect(addr)
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            return s
        except OSError as e:
            err = e
            s.close()
    raise err or OSError(f"cannot resolve {host}")


# ---------------------------------------------------------------------------
# source table


@dataclass
class Table:
    reply: Reply
    body: bytes = b""
    eof: bool = False           # the caster closed after the body
    request: list[str] = field(default_factory=list)

    @property
    def text(self) -> str:
        return self.body.decode("latin-1")

    @property
    def records(self) -> list[dict]:
        return parse_sourcetable(self.text)

    @property
    def is_table(self) -> bool:
        return self.reply.code == 200 and self.reply.kind in ("sourcetable", "http")

    @property
    def ended(self) -> bool:
        lines = [x for x in self.text.replace("\r\n", "\n").split("\n") if x.strip()]
        return bool(lines) and lines[-1].strip() == SOURCETABLE_END

    def problems(self) -> list[str]:
        if not self.is_table:
            return [f"no source table: {self.reply.status_line!r}"]
        p = []
        if not self.ended:
            p.append(f"the table does not end with {SOURCETABLE_END}")
        good = [r for r in self.records if r["field_count"] == len(STR_FIELDS)]
        if not good:
            p.append(f"no STR record of {len(STR_FIELDS)} fields ({len(self.records)} STR record(s))")
        n = self.reply.header("Content-Length")
        if n is not None and (not n.isdigit() or int(n) != len(self.body)):
            p.append(f"Content-Length {n} but {len(self.body)} body bytes")
        return p

    def to_json(self) -> dict:
        return {"mode": "sourcetable", "request": self.request, "reply": self.reply.to_json(),
                "body_bytes": len(self.body), "closed_after_body": self.eof, "records": self.records,
                "problems": self.problems()}


def fetch_sourcetable(host: str, port: int = DEFAULT_PORT, ntrip_version: int = 1, mount: str = "",
                      http_version: str | None = None, extra_headers: dict[str, str] | None = None,
                      timeout: float = REPLY_TIMEOUT_S) -> Table:
    """GET /<mount> (default /) without a login, and the whole reply until the caster closes."""
    req = build_request(host, port, mount, None, None, ntrip_version, http_version, extra_headers)
    with connect(host, port, timeout) as sock:
        sock.sendall(req)
        reply, rest = read_head(sock, timeout)
        body, eof = read_body(sock, reply, rest)
    return Table(reply, body, eof, shown_request(req))


# ---------------------------------------------------------------------------
# stream


class Saver:
    """--save: the decoded stream as stream-NNNN.bin + .idx (one "<unix>,<offset>,<bytes>" line per piece)."""

    def __init__(self, run_dir: Path) -> None:
        self.run_dir = run_dir
        self.rot = RotatingFile(run_dir, "stream", ".bin", PART_LIMIT, binary=True)
        self.part = self.rot.part
        self.idx = open(run_dir / f"stream-{self.part:04d}.idx", "a", encoding="utf-8")
        self.offset = 0

    def write(self, data: bytes, t: float) -> None:
        self.rot.write(data)
        if self.rot.part != self.part:
            self.idx.close()
            self.part = self.rot.part
            self.idx = open(self.run_dir / f"stream-{self.part:04d}.idx", "a", encoding="utf-8")
        self.idx.write(f"{t:.6f},{self.offset},{len(data)}\n")
        self.offset += len(data)

    def flush(self) -> None:
        self.rot.flush()
        self.idx.flush()

    def close(self) -> None:
        self.rot.close()
        self.idx.close()


class Stream:
    """One rover session: the reply, then the data as it comes (read_for)."""

    def __init__(self, sock: socket.socket, host: str, port: int, mount: str, ntrip_version: int, reply: Reply,
                 rest: bytes, request: list[str], keep: bool = True, save_dir: Path | None = None) -> None:
        self.sock = sock
        self.host, self.port, self.mount, self.ntrip_version = host, port, mount, ntrip_version
        self.reply = reply
        self.rest = rest                # bytes that came with the reply header, not read yet
        self.request = request
        self.refused = not reply.accepted
        self.body = b""                 # a refusal's body
        self.body_eof = False           # ... and the caster closed after it
        self.chunked = reply.chunked
        self.dechunk = Dechunker() if self.chunked else None
        self.record = FrameRecord(keep_frames=keep)
        self.keep = keep
        self.data = bytearray()         # the decoded data (keep)
        self.saver = Saver(save_dir) if save_dir is not None else None
        self.raw_bytes = 0              # bytes after the reply header, as received
        self.first_byte: int | None = None
        self.blank_line = False         # ICY: a blank line (or header lines) followed the status line
        self._pre: bytearray | None = bytearray() if reply.kind == "icy" else None
        self._bi = 0                    # chunk boundaries checked so far
        self.chunks_split = 0           # chunks that ended inside a frame
        self.first_split: str | None = None
        self.start_t = time.monotonic()
        self.end: str | None = None     # why reading ended
        self.end_t: float | None = None
        self.eof = False                # the caster closed (FIN)
        self.reset = False              # the connection was reset
        self.finished = False

    # -- data ---------------------------------------------------------------------

    @property
    def last_chunk(self) -> bool:
        return bool(self.dechunk and self.dechunk.done)

    def feed(self, raw: bytes, t: float) -> None:
        self.raw_bytes += len(raw)
        if self._pre is not None:
            # ICY 200 OK: a blank line, header lines up to a blank line, or the data at once.
            self._pre += raw
            p = self._pre
            if not p or (p[:1] == b"\r" and len(p) < 2):
                return
            if p.startswith(b"\r\n") or p.startswith(b"\n"):
                n = 2 if p.startswith(b"\r\n") else 1
                self.reply.head += bytes(p[:n])
                self.blank_line = True
                raw = bytes(p[n:])
            elif 0x41 <= p[0] <= 0x7A:
                end = blank_end(p)
                if end is None and len(p) <= HEAD_MAX:
                    return
                end = end or 0
                self.reply.head += bytes(p[:end])
                self.reply.headers += parse_headers(bytes(p[:end]).decode("latin-1"))
                self.blank_line = end > 0
                raw = bytes(p[end:])
            else:
                raw = bytes(p)
            self._pre = None
            if not raw:
                return
        if self.dechunk is None:
            self._take(raw, t)
            return
        data = self.dechunk.feed(raw)
        base = self.record.bytes
        bounds = self.dechunk.chunk_boundaries
        i = 0
        while self._bi < len(bounds):
            cut = bounds[self._bi] - base
            self._take(data[i:cut], t)
            i = cut
            self._bi += 1
            pending = len(self.record.split.buf)
            if pending:
                self.chunks_split += 1
                if self.first_split is None:
                    self.first_split = (f"chunk {self._bi} ended {pending} bytes into a frame "
                                        f"(stream offset {bounds[self._bi - 1]})")
        self._take(data[i:], t)

    def _take(self, data: bytes, t: float) -> None:
        if not data:
            return
        if self.first_byte is None:
            self.first_byte = data[0]
        self.record.feed(data, t)
        if self.keep:
            self.data += data
        if self.saver:
            self.saver.write(data, t)

    def finish(self) -> None:
        if not self.finished:
            self.record.finish()
            if self.saver:
                self.saver.close()
            self.finished = True

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass

    # -- results --------------------------------------------------------------------

    def frames(self) -> list[tuple[float | None, bytes]]:
        """The CRC-valid frames so far with their arrival times (keep)."""
        return list(self.record.frames)

    def summary(self) -> dict:
        s = self.record.summary()
        if not self.finished:
            s["bytes_outside_frames"] -= len(self.record.split.buf)   # a frame still arriving is not outside
        end = self.end_t if self.end_t is not None else time.monotonic()
        secs = end - self.start_t
        out = {
            "host": self.host, "port": self.port, "mount": self.mount, "ntrip_version": self.ntrip_version,
            "status_line": self.reply.status_line, "code": self.reply.code, "refused": self.refused,
            "chunked": self.chunked, "blank_line": self.blank_line, "head_bytes": len(self.reply.head),
            "raw_bytes": self.raw_bytes, "seconds": round(secs, 3),
            "frames": s["frames_ok"], "frames_per_s": round(s["frames_ok"] / secs, 2) if secs > 0 else None,
        }
        out.update({k: s[k] for k in ("bytes", "crc_failures", "bytes_outside_frames", "partial_tail", "types",
                                       "epochs", "first_frame_unix", "last_frame_unix")})
        if self.chunked:
            d = self.dechunk
            out.update({"chunks": d.chunks, "chunk_error": d.error, "chunks_split": self.chunks_split,
                        "first_split": self.first_split, "last_chunk": d.done,
                        "inside_chunk_at_end": d.state not in ("size", "done") or bool(d.buf)})
        out.update({"end": self.end, "eof": self.eof, "reset": self.reset})
        return out

    def problems(self, min_frames: int = 1) -> list[str]:
        if self.refused:
            return [f"refused: {self.reply.status_line!r}"]
        s = self.summary()
        p = []
        if s["frames"] < min_frames:
            p.append(f"{s['frames']} frames (at least {min_frames} wanted)")
        if s["crc_failures"]:
            p.append(f"{s['crc_failures']} CRC failures")
        if s["bytes_outside_frames"]:
            p.append(f"{s['bytes_outside_frames']} bytes outside frames")
        if self.chunked:
            if self.dechunk.error:
                p.append(f"chunk framing: {self.dechunk.error}")
            if self.chunks_split:
                p.append(f"{self.chunks_split} chunk(s) did not hold whole frames (first: {self.first_split})")
        return p

    def to_json(self, min_frames: int = 1) -> dict:
        return {"mode": "stream", "request": self.request, "reply": dict(self.reply.to_json(),
                                                                          blank_line=self.blank_line),
                "summary": self.summary(), "problems": self.problems(min_frames),
                "refusal_body": self.body.decode("latin-1") if self.refused else None}


def open_stream(host: str, port: int, mount: str, user: str | None = DEFAULT_USER, password: str | None = None,
                ntrip_version: int = 1, *, http_version: str | None = None,
                extra_headers: dict[str, str] | None = None, rcvbuf: int | None = None,
                timeout: float = REPLY_TIMEOUT_S, keep: bool = True, save_dir: Path | None = None) -> Stream:
    """Connect, send the GET and read the reply header. A refusal (anything
    but ICY 200 or HTTP 200 with data) is read to its end and closed:
    `refused`, `body`. Otherwise read the data with read_for(). `keep`: hold
    the decoded data and every frame with its arrival time (a bench run of
    hours passes False)."""
    req = build_request(host, port, mount, user, password, ntrip_version, http_version, extra_headers)
    password = None
    sock = connect(host, port, timeout, rcvbuf)
    try:
        sock.sendall(req)
        reply, rest = read_head(sock, timeout)
    except BaseException:
        sock.close()
        raise
    stream = Stream(sock, host, port, mount, ntrip_version, reply, rest, shown_request(req), keep, save_dir)
    req = b""
    if stream.refused:
        stream.body, stream.body_eof = read_body(sock, reply, rest)
        stream.rest = b""
        stream.end, stream.end_t = "refused", time.monotonic()
        stream.close()
        stream.finish()
    return stream


def progress_line(st: Stream) -> str:
    s = st.summary()
    types = ", ".join(f"{k} x{v}" for k, v in s["types"].items()) or "none"
    return (f"{s['seconds']:.0f} s: {s['frames']} frames, {human_bytes(s['bytes'])}, "
            f"{s['frames_per_s'] or 0:.1f} frames/s, {s['epochs']} epochs, {s['crc_failures']} CRC failures, "
            f"{s['bytes_outside_frames']} bytes outside frames; types {types}")


def read_for(stream: Stream, seconds: float | None, stop=None, say=None,
             status_every: float = STATUS_EVERY_S) -> dict:
    """Read until `seconds` passed (None or 0: no limit), `stop` (a
    threading.Event) is set, or the caster closes or resets the connection;
    then close it. `say(text)` gets a progress line every `status_every` s.
    After NTRIP 2.0's last chunk the close must follow within
    AFTER_LAST_CHUNK_S. Returns the summary; `stream.end` says why it ended
    and `stream.end_t` when (time.monotonic())."""
    if stream.refused:
        return stream.summary()
    start = time.monotonic()
    next_status = start + status_every if say and status_every else None
    next_flush = start + FLUSH_EVERY_S
    last_chunk_at = None
    reason = "stopped"
    try:
        if stream.rest:
            stream.feed(stream.rest, time.time())
            stream.rest = b""
        while True:
            now = time.monotonic()
            if seconds and now - start >= seconds:
                reason = "duration reached"
                break
            if stop is not None and stop.is_set():
                reason = "stopped"
                break
            if stream.last_chunk:
                last_chunk_at = last_chunk_at or now
                if now - last_chunk_at >= AFTER_LAST_CHUNK_S:
                    reason = f"the last chunk came, but no close within {AFTER_LAST_CHUNK_S:.0f} s"
                    break
            if stream.saver and now >= next_flush:
                stream.saver.flush()
                next_flush = now + FLUSH_EVERY_S
            if next_status is not None and now >= next_status:
                say(progress_line(stream))
                next_status = now + status_every
            stream.sock.settimeout(0.2)
            try:
                data = stream.sock.recv(65536)
            except socket.timeout:
                continue
            except ConnectionResetError:
                stream.reset = True
                reason = "the caster reset the connection"
                break
            except OSError as e:
                reason = f"read failed: {e}"
                break
            if not data:
                stream.eof = True
                reason = "the caster closed the stream" + (" after the last chunk" if stream.last_chunk else "")
                break
            stream.feed(data, time.time())
    finally:
        stream.end, stream.end_t = reason, time.monotonic()
        stream.close()
        stream.finish()
    return stream.summary()


# ---------------------------------------------------------------------------
# command line


def print_head(req: list[str], reply: Reply, out) -> None:
    for line in req:
        print("> " + line, file=out)
    for line in reply.head.decode("latin-1").replace("\r\n", "\n").split("\n"):
        if line:
            print("< " + line, file=out)


def refusal_text(st: Stream) -> str:
    r = st.reply
    if r.kind == "sourcetable" or (r.code == 200 and not r.accepted):
        what = f"the caster sent its source table instead: it has no mountpoint {st.mount!r}"
    elif r.code is not None:
        what = f"{r.code} {r.reason}"
        if r.code == 429 and r.header("Retry-After"):
            what += f" (Retry-After: {r.header('Retry-After')} s)"
        if r.code == 401 and r.header("WWW-Authenticate"):
            what += f" ({r.header('WWW-Authenticate')})"
    else:
        what = f"not an NTRIP reply: {r.status_line!r}"
    body = st.body.decode("latin-1").strip()
    return f"refused: {what}" + (f"; body: {body[:200]!r}" if body else "")


def run_sourcetable(args, run_dir: Path) -> int:
    out = sys.stderr if args.json else sys.stdout
    try:
        t = fetch_sourcetable(args.host, args.port, args.ntrip_version)
    except (OSError, ValueError, TimeoutError) as e:
        print(f"ntrip_client: {args.host}:{args.port}: {e}", file=sys.stderr)
        return EXIT_FAILED
    (run_dir / "reply-header.txt").write_bytes(t.reply.head)
    (run_dir / "sourcetable.txt").write_bytes(t.body)
    d = t.to_json()
    (run_dir / "summary.json").write_text(json.dumps(d, indent=1) + "\n", encoding="utf-8")
    if args.json:
        print(json.dumps(d, indent=1))
    else:
        print_head(t.request, t.reply, out)
        if t.is_table:
            print(t.text.rstrip("\r\n"), file=out)
            recs = t.records
            for i, rec in enumerate(recs, 1):
                print(f"STR {i} of {len(recs)} ({rec['field_count']} fields):", file=out)
                for name in STR_FIELDS:
                    if name in rec:
                        print(f"  {name:<15} {rec[name]}", file=out)
    if not t.is_table:
        print(f"ntrip_client: no source table: {t.reply.status_line!r}", file=sys.stderr)
        return EXIT_REFUSED if t.reply.code not in (None, 200) else EXIT_FAILED
    p = t.problems()
    if not args.json:
        print(("FAIL: " + "; ".join(p)) if p else
              f"PASS: {SOURCETABLE_END} at the end, {len(t.records)} STR record(s) of {len(STR_FIELDS)} fields",
              file=out)
    return EXIT_FAILED if p else EXIT_OK


def run_stream(args, password: str | None, run_dir: Path) -> int:
    out = sys.stderr if args.json else sys.stdout
    try:
        st = open_stream(args.host, args.port, args.mount, args.user, password, args.ntrip_version, keep=False,
                         save_dir=run_dir if args.save else None)
    except (OSError, ValueError, TimeoutError) as e:
        print(f"ntrip_client: {args.host}:{args.port}: {e}", file=sys.stderr)
        return EXIT_FAILED
    password = None
    (run_dir / "reply-header.txt").write_bytes(st.reply.head)
    if not args.json:
        print_head(st.request, st.reply, out)
    if st.refused:
        d = st.to_json(args.min_frames)
        (run_dir / "summary.json").write_text(json.dumps(d, indent=1) + "\n", encoding="utf-8")
        if args.json:
            print(json.dumps(d, indent=1))
        print(f"ntrip_client: {refusal_text(st)}", file=sys.stderr)
        return EXIT_REFUSED
    stop = StopFlag()
    say = None if args.json else (lambda text: print(f"[client] {text}", file=out, flush=True))
    if say:
        say(f"{args.host}:{args.port}/{args.mount} NTRIP {args.ntrip_version}.0"
            + (", chunked" if st.chunked else "") + f"; logging to {run_dir}; Ctrl-C to stop")
    read_for(st, args.duration, stop=stop.event, say=say)
    (run_dir / "reply-header.txt").write_bytes(st.reply.head)   # with the blank line after ICY 200 OK, once seen
    d = st.to_json(args.min_frames)
    (run_dir / "summary.json").write_text(json.dumps(d, indent=1) + "\n", encoding="utf-8")
    p = d["problems"]
    if args.json:
        print(json.dumps(d, indent=1))
    else:
        s = d["summary"]
        if st.reply.kind == "icy":
            say("ICY 200 OK " + ("with a blank line after it" if st.blank_line else "without a blank line after it"))
        say(f"end: {st.end} after {s['seconds']:.1f} s")
        print(f"frames {s['frames']} ({s['frames_per_s'] or 0:.1f}/s), {human_bytes(s['bytes'])}, {s['epochs']} "
              f"epochs; CRC failures {s['crc_failures']}; bytes outside frames {s['bytes_outside_frames']}; "
              f"cut frame at the end {s['partial_tail']} B", file=out)
        print("types: " + (", ".join(f"{k} x{v}" for k, v in s["types"].items()) or "none"), file=out)
        if st.chunked:
            print(f"chunks: {s['chunks']}, framing {'clean' if not s['chunk_error'] else s['chunk_error']}, "
                  + ("each held whole frames" if not s["chunks_split"] else
                     f"{s['chunks_split']} ended inside a frame (first: {s['first_split']})")
                  + (", the caster's last chunk came" if s["last_chunk"] else ""), file=out)
        print(("FAIL: " + "; ".join(p)) if p else "PASS", file=out)
    return EXIT_FAILED if p else EXIT_OK


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], epilog=__doc__.split("\n\n", 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", required=True, help="caster host name or address (no default)")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT, help=f"caster port (default {DEFAULT_PORT})")
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--mount", help="mountpoint to read (without it: the source table)")
    g.add_argument("--sourcetable", action="store_true", help="read the source table (GET /)")
    ap.add_argument("--user", default=DEFAULT_USER, help=f"username for Basic authentication (default {DEFAULT_USER})")
    pg = ap.add_mutually_exclusive_group()
    pg.add_argument("--password-env", metavar="VAR", help="take the password from this environment variable")
    pg.add_argument("--password-prompt", action="store_true", help="ask for the password")
    ap.add_argument("--ntrip-version", type=int, choices=(1, 2), default=1, help="1 (default) or 2")
    ap.add_argument("--duration", type=float, default=60.0, help="seconds to read (default 60, 0 = until Ctrl-C "
                                                                  "or the caster closes)")
    ap.add_argument("--min-frames", type=int, default=1, help="frames needed for a pass (default 1)")
    ap.add_argument("--save", action="store_true", help="save the stream (stream-NNNN.bin + .idx) in the run directory")
    ap.add_argument("--json", action="store_true", help="print the result as JSON (progress lines left out)")
    add_log_args(ap)
    args = ap.parse_args()
    if not 1 <= args.port <= 65535:
        ap.error("--port must be 1-65535")
    if args.duration < 0 or args.min_frames < 0:
        ap.error("--duration and --min-frames must not be negative")
    table = args.sourcetable or not args.mount
    password = None
    if not table:
        if args.password_env:
            password = os.environ.get(args.password_env)
            if password is None:
                print(f"ntrip_client: ${args.password_env} is not set", file=sys.stderr)
                return EXIT_REFUSED
        elif args.password_prompt:
            password = getpass.getpass(f"password for {args.user}: ")
    run_dir = make_run_dir(args.log_dir, "client", args.label)
    write_command_line(run_dir)
    if table:
        return run_sourcetable(args, run_dir)
    rc = run_stream(args, password, run_dir)
    password = None
    return rc


if __name__ == "__main__":
    sys.exit(main())
