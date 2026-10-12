#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""A scripted NTRIP caster on this machine, for testing the station's caster client.

Answers every NTRIP source (NTRIP 1.0 "SOURCE", NTRIP 2.0 "POST") with a
reply row you choose, one row per connection from a sequence, and records
what the source sends: every RTCM frame checked (CRC, bytes outside frames,
a partial frame at the end), optionally saved, optionally relayed to GET
clients. Nothing here talks to a real caster: it listens on 127.0.0.1 by
default and only answers.

  serve    run the caster (Ctrl-C to stop)
  rows     print every reply row and its exact bytes
  report   the connection table of a serve run directory

Reply rows (`rows` prints the bytes): success lines that start a session
(icy_ok, ok, http_200, http_1_0_200), HTTP errors, sourcetable, the
"ERROR - ..." lines, plain-text and HTML refusals, misbehaving casters
(trickle, no_reply, close_no_newline, close_silent, overlong, garbage) and
the Onocoy termination reasons (onocoy_*). The Onocoy rows are sent by
default in the one form seen from that caster so far, a plain-text line
"Error <n> <reason> [onocoy naomi v2.5.95-fake]" and then a close;
--onocoy-form plain_line sends the reason alone, http_401 an HTTP 401 with
the reason as its body. The numbers <n> are made up (the real ones are not
known); a client should match the reason text.

--sequence SPEC: comma-separated entries, one per source connection, the
last one repeating. Entry: ROW[:kick=S][:text=ROW][:record]. kick=S (an
accepting row only) ends the session S seconds after the reply, first
sending the line of the text row when given (e.g. onocoy_reconnected);
record saves that session even without --record.

Examples:
  uv run fake_caster.py serve --port 2101                       # ICY 200 OK to everyone
  uv run fake_caster.py serve --port 2101 --reply bad_password
  uv run fake_caster.py serve --port 2101 --mount BENCH --password bench-pass-1 --record --relay
  uv run fake_caster.py serve --port 2102 --sequence "icy_ok:kick=5:text=onocoy_reconnected,onocoy_in_use"
  uv run fake_caster.py rows
  uv run fake_caster.py report bench-logs/<serve run>

Writes to a new run directory (see --log-dir):
  connections.jsonl   one JSON line per connection when it ends: unix and
                      monotonic times, peer, role, request line (password
                      masked), agent, NTRIP version, mountpoint, user,
                      reply row and bytes, bytes in, frames, who closed
  caster.log          timestamped events
  record-NNN.rtcm3    with --record (or :record): connection NNN's RTCM
  record-NNN.json     its check: frames, CRC failures, bytes outside
                      frames, partial tail, types, epochs
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from benchlib.common import StopFlag, add_log_args, hms, iso, make_run_dir, write_command_line
from benchlib.ntrip import LAST_CHUNK, Dechunker, FrameRecord, Request, chunk, parse_request

SERVER = "NTRIP fake-caster/1.0"
NAOMI_TAG = "[onocoy naomi v2.5.95-fake]"
REQUEST_TIMEOUT_S = 10.0
REQUEST_MAX = 8192
HOLD_MAX_S = 600.0  # trickle / no_reply give up after this
DRAIN_S = 2.0  # after a refusal: read what the client still sends before closing
TRICKLE = b"zqxj"
DEFAULT_MOUNT = "BENCH"
ONOCOY_FORMS = ("error_line", "plain_line", "http_401")

# Onocoy's console termination reasons. The numbers are made up.
ONOCOY_REASONS = {
    "onocoy_login_blocked": (21, "Too many unsuccessful logins - temporarily blocking further connections"),
    "onocoy_in_use": (22, "Credentials already in use by another stream"),
    "onocoy_parallel": (23, "Maximum Number of parallel connections reached for this credential"),
    "onocoy_reconnected": (24, "Device reconnected"),
    "onocoy_duplicate": (25, "Duplicate of another stream"),
    "onocoy_credential_banned": (26, "Credential is banned"),
    "onocoy_auth_failed": (27, "Authentication failed"),
    "onocoy_reauth_failed": (28, "Reauthentication failed"),
    "onocoy_invalid_headers": (29, "Invalid request in headers"),
}


@dataclass(frozen=True)
class Row:
    name: str
    kind: str  # accept, close, trickle, silent, close_silent
    data: bytes  # sent at once (trickle: the bytes sent one per second, repeated)
    note: str
    line: bytes = b""  # the line this row sends inside a session (kick text)


HTTP_REASONS = {200: "OK", 400: "Bad Request", 401: "Unauthorized", 403: "Forbidden", 404: "Not Found",
                409: "Conflict", 418: "I'm a teapot", 431: "Request Header Fields Too Large",
                500: "Internal Server Error", 501: "Not Implemented", 503: "Service Unavailable"}


def http_reply(code: int, body: bytes = b"", chunks: list[bytes] | None = None, version: str = "1.1",
               ntrip2: bool = True) -> bytes:
    head = f"HTTP/{version} {code} {HTTP_REASONS[code]}\r\n"
    if ntrip2:
        head += "Ntrip-Version: Ntrip/2.0\r\n"
    head += f"Server: {SERVER}\r\n"
    if chunks is not None:
        head += "Content-Type: text/html\r\nTransfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
        return head.encode() + b"".join(chunk(c) for c in chunks) + LAST_CHUNK
    if code == 200:
        return (head + "\r\n").encode()
    head += f"Content-Type: text/plain\r\nContent-Length: {len(body)}\r\nConnection: close\r\n\r\n"
    return head.encode() + body


def sourcetable(mount: str) -> bytes:
    body = (f"STR;{mount};{mount};RTCM 3.3;1005(10),1077(1),1087(1),1097(1),1127(1),1230(10);2;"
            f"GPS+GLO+GAL+BDS;FAKE;XXX;0.00;0.00;0;0;fake-caster;none;N;N;0;\r\n"
            f"ENDSOURCETABLE\r\n").encode()
    return (f"SOURCETABLE 200 OK\r\nServer: {SERVER}\r\nContent-Type: text/plain\r\n"
            f"Content-Length: {len(body)}\r\n\r\n").encode() + body


def onocoy_row(name: str, form: str) -> Row:
    num, text = ONOCOY_REASONS[name]
    error_line = f"Error {num} {text} {NAOMI_TAG}\r\n".encode()
    plain = f"{text}\r\n".encode()
    if form == "error_line":
        data, line = error_line, error_line
    elif form == "plain_line":
        data, line = plain, plain
    elif form == "http_401":
        data, line = http_reply(401, plain), plain  # inside a session only a line makes sense
    else:
        raise ValueError(f"unknown Onocoy form {form!r} ({', '.join(ONOCOY_FORMS)})")
    return Row(name, "close", data, f"Onocoy: {text} ({form})", line)


def build_rows(onocoy_form: str = "error_line", mount: str = DEFAULT_MOUNT) -> dict[str, Row]:
    banned_html = b"<html><body><h1>The NTRIP Caster has banned your IP</h1></body></html>\r\n"
    rows = [
        Row("icy_ok", "accept", b"ICY 200 OK\r\n", "NTRIP 1.0 success; the session is recorded"),
        Row("ok", "accept", b"OK\r\n", "bare OK (some 1.0 casters); the session is recorded"),
        Row("http_200", "accept", http_reply(200), "NTRIP 2.0 success; chunked or raw body recorded"),
        Row("http_1_0_200", "accept", b"HTTP/1.0 200 OK\r\n\r\n", "HTTP/1.0 200 without headers"),
        Row("http_400", "close", http_reply(400, b"Bad Request\r\n"), "HTTP 400"),
        Row("http_401", "close", http_reply(401, b"Unauthorized\r\n"), "HTTP 401 (credentials)"),
        Row("http_403", "close", http_reply(403, b"Forbidden\r\n"), "HTTP 403"),
        Row("http_403_banned", "close", http_reply(403, b"Your IP address is banned\r\n"),
            "HTTP 403 whose body says banned"),
        Row("http_403_banned_chunked", "close",
            http_reply(403, chunks=[b"<html><body>You are ban", b"ned from this caster</body></html>\r\n"]),
            "HTTP 403, chunked body with 'banned' split across two chunks"),
        Row("http_404", "close", http_reply(404, b"Not Found\r\n"), "HTTP 404"),
        Row("http_409", "close", http_reply(409, b"Mountpoint in use\r\n"), "HTTP 409 (mountpoint in use)"),
        Row("http_431", "close", http_reply(431, b"Request Header Fields Too Large\r\n"), "HTTP 431"),
        Row("http_500", "close", http_reply(500, b"Internal Server Error\r\n"), "HTTP 500"),
        Row("http_501", "close", http_reply(501, b"Not Implemented\r\n"), "HTTP 501"),
        Row("http_503", "close", http_reply(503, b"Service Unavailable\r\n"), "HTTP 503"),
        Row("http_418", "close", http_reply(418, b"I'm a teapot\r\n"), "an HTTP code no caster uses"),
        Row("sourcetable", "close", sourcetable(mount), "the sourcetable (the caster took the request for a client)"),
        Row("bad_password", "close", b"ERROR - Bad Password\r\n", "NTRIP 1.0 wrong password"),
        Row("mount_taken", "close", b"ERROR - Mount Point Taken or Invalid\r\n", "NTRIP 1.0 mountpoint in use"),
        Row("already_connected", "close", b"ERROR - Already Connected\r\n", "NTRIP 1.0 source already connected"),
        Row("bad_mountpoint", "close", b"ERROR - Bad Mountpoint\r\n", "NTRIP 1.0 unknown mountpoint"),
        Row("missing_mountpoint", "close", b"ERROR - Missing Mountpoint\r\n", "NTRIP 1.0 no mountpoint"),
        Row("too_many_sources", "close", b"ERROR - Too many sources\r\n", "caster full"),
        Row("error_other", "close", b"ERROR - Unexpected request\r\n", "another ERROR line"),
        Row("naomi_error", "close", f"Error 11 Unknown - epoll rd hup {NAOMI_TAG}\r\n".encode(),
            "the plain-text error form seen from Onocoy's caster"),
        Row("not_authorized", "close", b"Not authorized (no NTRIP source)\r\n", "agent refused"),
        Row("html_table", "close",
            b"<html><head><title>NTRIP caster</title></head><body><table><tr><td>Mountpoint</td>"
            b"<td>Status</td></tr></table></body></html>\r\n", "an HTML page (a web server, not a caster)"),
        Row("html_banned", "close", banned_html, "HTML page saying the caster banned this IP"),
        Row("line_banned", "close", b"Your IP address is banned\r\n", "plain line containing 'banned'"),
        Row("line_sandbox", "close", b"Source moved to the sandbox\r\n", "plain line containing 'sandbox'"),
        Row("trickle", "trickle", TRICKLE, "1 nonsense byte per second, no newline (until the client gives up)"),
        Row("no_reply", "silent", b"", "accepts the request and holds the socket silent"),
        Row("close_no_newline", "close", b"ICY 200 OK", "a reply without its line end, then close"),
        Row("close_silent", "close_silent", b"", "closes without a byte"),
        Row("overlong", "close", (b"overlong-" * 123)[:1100], "1100 bytes without a newline, then close"),
        Row("garbage", "close", b"\x00\xff\x13\x37\xfe\x80garbage\x81\x00\r\n", "binary garbage line"),
    ]
    rows += [onocoy_row(name, onocoy_form) for name in ONOCOY_REASONS]
    return {r.name: r for r in rows}


ROW_NAMES = tuple(build_rows())


@dataclass
class Entry:
    row: str
    kick: float | None = None
    text: str | None = None
    record: bool = False

    @property
    def spec(self) -> str:
        s = self.row
        if self.kick is not None:
            s += f":kick={self.kick:g}"
        if self.text:
            s += f":text={self.text}"
        if self.record:
            s += ":record"
        return s


def parse_sequence(spec: str) -> list[Entry]:
    rows = build_rows()
    out = []
    for raw in spec.split(","):
        raw = raw.strip()
        if not raw:
            raise ValueError("empty entry in the sequence")
        parts = raw.split(":")
        e = Entry(parts[0])
        if e.row not in rows:
            raise ValueError(f"unknown row {e.row!r} (`fake_caster.py rows` lists them)")
        for opt in parts[1:]:
            if opt == "record":
                e.record = True
            elif opt.startswith("kick="):
                try:
                    e.kick = float(opt[5:])
                except ValueError:
                    raise ValueError(f"{raw}: kick needs seconds") from None
                if e.kick <= 0:
                    raise ValueError(f"{raw}: kick must be > 0")
            elif opt.startswith("text="):
                e.text = opt[5:]
                if e.text not in rows:
                    raise ValueError(f"{raw}: unknown text row {e.text!r}")
                if not (rows[e.text].line or rows[e.text].data):
                    raise ValueError(f"{raw}: row {e.text} sends no line")
            else:
                raise ValueError(f"{raw}: unknown option {opt!r} (kick=S, text=ROW, record)")
        if e.kick is not None and rows[e.row].kind != "accept":
            raise ValueError(f"{raw}: kick needs an accepting row (icy_ok, ok, http_200, http_1_0_200)")
        if e.text and e.kick is None:
            raise ValueError(f"{raw}: text= goes with kick=")
        out.append(e)
    return out


@dataclass
class Conn:
    """One connection's record (the connections.jsonl line)."""

    n: int
    peer: str
    start_unix: float
    start_mono: float
    role: str = "unknown"
    request_line: str = ""
    request_headers: dict = field(default_factory=dict)
    request_complete: bool = False
    agent: str = ""
    ntrip_version: int | None = None
    mount: str = ""
    user: str | None = None
    entry: str = ""
    row: str = ""
    refused_by: str = ""  # mount / password check, before the sequence
    reply: bytes = b""
    text_sent: bytes = b""
    kick_s: float | None = None
    chunked: bool = False
    chunk_error: str | None = None
    chunked_end: bool = False
    bytes_in: int = 0
    record: dict | None = None
    record_file: str | None = None
    bytes_out: int = 0  # relay clients
    frames_out: int = 0
    closed_by: str = ""
    end_unix: float | None = None
    end_mono: float | None = None

    def to_json(self) -> dict:
        d = {
            "n": self.n, "peer": self.peer, "start_unix": round(self.start_unix, 6),
            "start_mono": round(self.start_mono, 6),
            "end_unix": round(self.end_unix, 6) if self.end_unix is not None else None,
            "end_mono": round(self.end_mono, 6) if self.end_mono is not None else None,
            "duration_s": round(self.end_mono - self.start_mono, 3) if self.end_mono is not None else None,
            "role": self.role, "request_line": self.request_line, "request_headers": self.request_headers,
            "request_complete": self.request_complete, "agent": self.agent, "ntrip_version": self.ntrip_version,
            "mount": self.mount, "user": self.user, "entry": self.entry, "row": self.row,
            "refused_by": self.refused_by or None,
            "reply": self.reply.decode("latin-1"), "text_sent": self.text_sent.decode("latin-1") or None,
            "kick_s": self.kick_s, "chunked": self.chunked, "chunk_error": self.chunk_error,
            "chunked_end": self.chunked_end, "bytes_in": self.bytes_in, "closed_by": self.closed_by,
            "record_file": self.record_file,
        }
        rec = self.record or {}
        for k in ("frames_ok", "crc_failures", "bytes_outside_frames", "partial_tail", "types", "epochs",
                  "first_frame_unix", "last_frame_unix", "frames_span_s"):
            d[k] = rec.get(k)
        if self.role == "client":
            d["bytes_out"] = self.bytes_out
            d["frames_out"] = self.frames_out
        return d


class _Client:
    def __init__(self, sock: socket.socket, v2: bool, conn: Conn):
        self.sock = sock
        self.v2 = v2
        self.conn = conn
        self.lock = threading.Lock()
        self.dead = threading.Event()


class FakeCaster:
    """The caster, in a background thread per connection. Also used in-process
    by the tests and tools/qemu/e2e.py."""

    def __init__(self, port: int = 0, host: str = "127.0.0.1", sequence: list[Entry] | None = None,
                 onocoy_form: str = "error_line", mount: str | None = None, password: str | None = None,
                 record: bool = False, relay: bool = False, run_dir: Path | None = None, say=print,
                 hold_max_s: float = HOLD_MAX_S):
        if onocoy_form not in ONOCOY_FORMS:
            raise ValueError(f"unknown Onocoy form {onocoy_form!r}")
        self.rows = build_rows(onocoy_form, mount or DEFAULT_MOUNT)
        self.sequence = sequence or [Entry("icy_ok")]
        self.seq_pos = 0
        self.mount = mount
        self.password = password
        self.record_all = record
        self.relay = relay
        self.run_dir = run_dir
        self.say = say
        self.hold_max_s = hold_max_s
        self.lock = threading.RLock()
        self.stop_event = threading.Event()
        self.conns: list[Conn] = []
        self.done: list[Conn] = []
        self.clients: list[_Client] = []
        self.current_source = 0
        self.records: dict[int, FrameRecord] = {}
        self.threads: list[threading.Thread] = []
        self.changed = threading.Condition(self.lock)
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.sock.listen(16)
        self.sock.settimeout(0.2)
        self.host, self.port = self.sock.getsockname()[:2]
        self.log_file = open(run_dir / "caster.log", "a", encoding="utf-8") if run_dir else None
        self.jsonl = open(run_dir / "connections.jsonl", "a", encoding="utf-8") if run_dir else None
        self.accept_thread = threading.Thread(target=self._accept_loop, name="caster", daemon=True)

    # -- control ----------------------------------------------------------------

    def start(self) -> FakeCaster:
        self.accept_thread.start()
        return self

    def stop(self) -> None:
        self.stop_event.set()
        self.accept_thread.join(timeout=5)
        try:
            self.sock.close()
        except OSError:
            pass
        for t in list(self.threads):
            t.join(timeout=5)
        with self.lock:
            if self.log_file:
                self.log_file.close()
                self.log_file = None
            if self.jsonl:
                self.jsonl.close()
                self.jsonl = None

    def set_sequence(self, sequence: list[Entry] | str) -> None:
        """Replace the sequence; the next source connection gets its first entry."""
        if isinstance(sequence, str):
            sequence = parse_sequence(sequence)
        with self.lock:
            self.sequence = sequence
            self.seq_pos = 0

    def finished(self) -> list[dict]:
        """Ended connections, in the order they ended (as in connections.jsonl)."""
        with self.lock:
            return [c.to_json() for c in self.done]

    def all_connections(self) -> list[dict]:
        """Every connection so far (ongoing ones without end times), in start order."""
        with self.lock:
            return [c.to_json() for c in self.conns]

    def record_of(self, n: int) -> FrameRecord | None:
        with self.lock:
            return self.records.get(n)

    def wait_for(self, predicate, timeout: float) -> bool:
        """Wait until predicate(self) is true (checked whenever a connection starts or ends)."""
        deadline = time.monotonic() + timeout
        with self.changed:
            while not predicate(self):
                left = deadline - time.monotonic()
                if left <= 0:
                    return False
                self.changed.wait(min(left, 0.5))
        return True

    # -- internals ----------------------------------------------------------------

    def _log(self, text: str, echo: bool = True) -> None:
        line = f"{iso()} {text}"
        with self.lock:
            if self.log_file:
                self.log_file.write(line + "\n")
                self.log_file.flush()
        if echo:
            self.say(f"{hms(time.time())} {text}")

    def _accept_loop(self) -> None:
        self._log(f"listening on {self.host}:{self.port}; sequence "
                  f"{','.join(e.spec for e in self.sequence)}"
                  + (f"; mountpoint {self.mount}" if self.mount else "")
                  + ("; password checked" if self.password else "")
                  + ("; recording" if self.record_all else "") + ("; relay on" if self.relay else ""))
        n = 0
        while not self.stop_event.is_set():
            try:
                sock, peer = self.sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            n += 1
            conn = Conn(n, f"{peer[0]}:{peer[1]}", time.time(), time.monotonic())
            with self.changed:
                self.conns.append(conn)
                self.changed.notify_all()
            t = threading.Thread(target=self._handle, args=(sock, conn), name=f"caster-{n}", daemon=True)
            self.threads = [x for x in self.threads if x.is_alive()] + [t]
            t.start()

    def _next_entry(self) -> Entry:
        with self.lock:
            e = self.sequence[min(self.seq_pos, len(self.sequence) - 1)]
            self.seq_pos += 1
            return e

    def _read_request(self, sock: socket.socket, conn: Conn) -> tuple[Request | None, bytes]:
        buf = bytearray()
        deadline = time.monotonic() + REQUEST_TIMEOUT_S
        while b"\r\n\r\n" not in buf and len(buf) < REQUEST_MAX:
            left = deadline - time.monotonic()
            if left <= 0 or self.stop_event.is_set():
                break
            sock.settimeout(min(left, 0.2))
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                conn.closed_by = "client (reset)"
                break
            if not data:
                conn.closed_by = "client"
                break
            buf += data
        end = buf.find(b"\r\n\r\n")
        head, rest = (bytes(buf[:end]), bytes(buf[end + 4 :])) if end >= 0 else (bytes(buf), b"")
        if not head.strip():
            return None, b""
        req = parse_request(head)
        req.complete = end >= 0
        return req, rest

    def _handle(self, sock: socket.socket, conn: Conn) -> None:
        try:
            req, rest = self._read_request(sock, conn)
            if req is None:
                if not conn.closed_by:
                    conn.closed_by = "caster (no request)"
                self._log(f"#{conn.n} {conn.peer}: no request ({conn.closed_by})")
                return
            conn.role = req.role
            conn.request_line = req.masked_line
            conn.request_headers = req.masked_headers()
            conn.request_complete = req.complete
            conn.agent = req.agent
            conn.ntrip_version = req.ntrip_version
            conn.mount = req.mount
            conn.user = req.user
            conn.chunked = req.chunked
            if req.role == "client":
                self._client(sock, conn, req)
            else:
                self._source(sock, conn, req, rest)
        except OSError as e:
            if not conn.closed_by:
                conn.closed_by = f"client (error: {e.__class__.__name__})"
        finally:
            try:
                sock.close()
            except OSError:
                pass
            conn.end_unix = time.time()
            conn.end_mono = time.monotonic()
            line = json.dumps(conn.to_json())
            with self.changed:
                self.done.append(conn)
                if self.jsonl:
                    self.jsonl.write(line + "\n")
                    self.jsonl.flush()
                self.changed.notify_all()
            self._log(f"#{conn.n} ended after {conn.end_mono - conn.start_mono:.1f} s: closed by {conn.closed_by}; "
                      f"{conn.bytes_in} bytes in" + (f", {conn.record['frames_ok']} frames ok, "
                                                       f"{conn.record['crc_failures']} CRC failures"
                                                       if conn.record else ""))

    # -- sources ------------------------------------------------------------------

    def _source(self, sock: socket.socket, conn: Conn, req: Request, rest: bytes) -> None:
        v2 = req.method == "POST"
        entry = None
        if self.mount is not None and req.mount != self.mount:
            row, conn.refused_by = self.rows["http_404" if v2 else "bad_mountpoint"], "mountpoint"
        elif self.password is not None and req.password != self.password:
            row, conn.refused_by = self.rows["http_401" if v2 else "bad_password"], "password"
        else:
            entry = self._next_entry()
            row = self.rows[entry.row]
            conn.entry = entry.spec
        conn.row = row.name
        self._log(f"#{conn.n} {conn.peer} {req.masked_line!r} agent {req.agent!r}"
                  + (f" user {req.user!r}" if req.user is not None else "")
                  + f" -> {row.name}" + (f" (refused: {conn.refused_by})" if conn.refused_by else "")
                  + (f" [{entry.spec}]" if entry and entry.spec != row.name else ""))
        if row.kind == "accept":
            self._session(sock, conn, req, row, entry, rest)
        else:
            self._refuse(sock, conn, row)

    def _refuse(self, sock: socket.socket, conn: Conn, row: Row) -> None:
        if row.kind == "close_silent":
            conn.closed_by = "caster"
            return
        if row.kind in ("trickle", "silent"):
            self._hold(sock, conn, row)
            return
        sock.sendall(row.data)
        conn.reply = row.data
        self._drain_and_close(sock, conn)

    def _drain_and_close(self, sock: socket.socket, conn: Conn) -> None:
        """FIN after the reply, then read until the client closes (at most
        DRAIN_S), so closing never resets a reply the client has not read."""
        try:
            sock.shutdown(socket.SHUT_WR)
        except OSError:
            pass
        deadline = time.monotonic() + DRAIN_S
        while time.monotonic() < deadline:
            sock.settimeout(max(0.01, deadline - time.monotonic()))
            try:
                data = sock.recv(4096)
            except (socket.timeout, OSError):
                break
            if not data:
                break
            conn.bytes_in += len(data)
        conn.closed_by = "caster"

    def _hold(self, sock: socket.socket, conn: Conn, row: Row) -> None:
        sent = bytearray()
        start = time.monotonic()
        next_byte = start
        i = 0
        while not self.stop_event.is_set():
            now = time.monotonic()
            if now - start >= self.hold_max_s:
                conn.closed_by = "caster (hold limit)"
                break
            if row.kind == "trickle" and now >= next_byte:
                b = row.data[i % len(row.data) : i % len(row.data) + 1]
                try:
                    sock.sendall(b)
                except OSError:
                    conn.closed_by = "client (reset)"
                    break
                sent += b
                i += 1
                next_byte += 1.0
            sock.settimeout(max(0.01, min(0.2, next_byte - time.monotonic())) if row.kind == "trickle" else 0.2)
            try:
                data = sock.recv(4096)
            except socket.timeout:
                continue
            except OSError:
                conn.closed_by = "client (reset)"
                break
            if not data:
                conn.closed_by = "client"
                break
            conn.bytes_in += len(data)
        else:
            conn.closed_by = "caster (stopped)"
        conn.reply = bytes(sent)

    def _session(self, sock: socket.socket, conn: Conn, req: Request, row: Row, entry: Entry,
                 rest: bytes) -> None:
        sock.sendall(row.data)
        conn.reply = row.data
        reply_at = time.monotonic()
        recording = self.record_all or entry.record
        rec = FrameRecord(keep_frames=True)
        with self.changed:
            self.records[conn.n] = rec
            self.current_source = conn.n
        out = None
        if recording and self.run_dir:
            conn.record_file = f"record-{conn.n:03d}.rtcm3"
            out = open(self.run_dir / conn.record_file, "wb")
        dechunk = None
        if req.chunked:
            dechunk = Dechunker()
        if entry.kick is not None:
            conn.kick_s = entry.kick
        kick_at = reply_at + entry.kick if entry.kick is not None else None

        def take(data: bytes) -> None:
            conn.bytes_in += len(data)
            body = dechunk.feed(data) if dechunk else data
            if out and body:
                out.write(body)
                out.flush()  # readable while the session runs (tools/qemu/e2e.py)
            if body:
                frames = rec.feed(body, time.time())
                if frames and self.relay and self.current_source == conn.n:
                    self._relay(frames)

        try:
            if rest:
                take(rest)
            while True:
                if self.stop_event.is_set():
                    conn.closed_by = "caster (stopped)"
                    break
                now = time.monotonic()
                if kick_at is not None and now >= kick_at:
                    if entry.text:
                        line = self.rows[entry.text].line or self.rows[entry.text].data
                        try:
                            sock.sendall(line)
                            conn.text_sent = line
                        except OSError:
                            pass
                    self._log(f"#{conn.n} kicked after {entry.kick:g} s" + (f" with {entry.text}" if entry.text else ""))
                    try:
                        sock.shutdown(socket.SHUT_RDWR)
                    except OSError:
                        pass
                    conn.closed_by = "caster (kick)"
                    break
                sock.settimeout(0.2 if kick_at is None else max(0.01, min(0.2, kick_at - now)))
                try:
                    data = sock.recv(65536)
                except socket.timeout:
                    continue
                except OSError:
                    conn.closed_by = "client (reset)"
                    break
                if not data:
                    conn.closed_by = "client"
                    break
                take(data)
        finally:
            if dechunk:
                conn.chunk_error = dechunk.error
                conn.chunked_end = dechunk.done
            conn.record = rec.finish()
            if out:
                out.close()
                info = dict(conn.record, connection=conn.n, entry=conn.entry, row=conn.row,
                            chunked=conn.chunked, chunk_error=conn.chunk_error, chunked_end=conn.chunked_end,
                            bytes_in=conn.bytes_in, closed_by=conn.closed_by)
                (self.run_dir / f"record-{conn.n:03d}.json").write_text(json.dumps(info, indent=1) + "\n",
                                                                        encoding="utf-8")

    # -- relay clients ------------------------------------------------------------

    def _client(self, sock: socket.socket, conn: Conn, req: Request) -> None:
        mount = self.mount or DEFAULT_MOUNT
        if not self.relay or req.mount != mount:
            row = self.rows["sourcetable"]
            conn.row = row.name
            self._log(f"#{conn.n} {conn.peer} client {req.masked_line!r} -> sourcetable")
            sock.sendall(row.data)
            conn.reply = row.data
            self._drain_and_close(sock, conn)
            return
        v2 = req.ntrip_version == 2
        reply = (b"HTTP/1.1 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nServer: " + SERVER.encode()
                 + b"\r\nContent-Type: gnss/data\r\nTransfer-Encoding: chunked\r\nCache-Control: no-store\r\n\r\n"
                 if v2 else b"ICY 200 OK\r\n")
        conn.row = "relay"
        sock.sendall(reply)
        conn.reply = reply
        client = _Client(sock, v2, conn)
        with self.lock:
            self.clients.append(client)
        self._log(f"#{conn.n} {conn.peer} relay client for /{mount} (NTRIP {'2.0' if v2 else '1.0'})")
        try:
            while not self.stop_event.is_set() and not client.dead.is_set():
                sock.settimeout(0.2)
                try:
                    data = sock.recv(4096)
                except socket.timeout:
                    continue
                except OSError:
                    conn.closed_by = "client (reset)"
                    break
                if not data:
                    conn.closed_by = "client"
                    break
                conn.bytes_in += len(data)
            else:
                conn.closed_by = "caster (stopped)" if self.stop_event.is_set() else "caster (send failed)"
        finally:
            with self.lock:
                if client in self.clients:
                    self.clients.remove(client)
            if v2 and not client.dead.is_set():
                try:
                    with client.lock:
                        sock.sendall(LAST_CHUNK)
                except OSError:
                    pass

    def _relay(self, frames: list[bytes]) -> None:
        data = b"".join(frames)
        with self.lock:
            clients = list(self.clients)
        for c in clients:
            try:
                with c.lock:
                    c.sock.settimeout(5)
                    c.sock.sendall(chunk(data) if c.v2 else data)
                c.conn.bytes_out += len(data)
                c.conn.frames_out += len(frames)
            except OSError:
                c.dead.set()


# ---------------------------------------------------------------------------
# report


def load_connections(run_dir: Path) -> list[dict]:
    path = Path(run_dir) / "connections.jsonl"
    if not path.is_file():
        raise ValueError(f"{run_dir}: no connections.jsonl")
    out = []
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if line.strip():
            try:
                out.append(json.loads(line))
            except ValueError as e:
                raise ValueError(f"connections.jsonl line {n}: {e}") from e
    out.sort(key=lambda c: c["start_mono"])
    return out


def report(conns: list[dict]) -> str:
    head = (f"{'#':>4} {'time (UTC)':<12} {'gap_s':>8} {'role':<7} {'request':<34} {'agent':<30} {'ver':>3} "
            f"{'reply row':<26} {'in_bytes':>9} {'ok/bad':>9} {'tail':>5} {'closed by':<20} {'dur_s':>8}")
    lines = [head]
    prev_end: dict[str, float | None] = {}  # per role: sources and relay clients come and go apart
    for c in conns:
        pe = prev_end.get(c["role"], "first")
        if pe == "first":
            gap = "-"
        elif pe is None or pe > c["start_mono"]:
            gap = "open"  # the previous one had not ended yet
        else:
            gap = f"{c['start_mono'] - pe:.1f}"
        t = time.strftime("%H:%M:%S", time.gmtime(c["start_unix"])) + f".{int(c['start_unix'] % 1 * 1000):03d}"
        okbad = "-" if c.get("frames_ok") is None else f"{c['frames_ok']}/{c['crc_failures']}"
        tail = "-" if c.get("partial_tail") is None else str(c["partial_tail"])
        row = c.get("entry") or c.get("row") or "-"
        if c.get("refused_by"):
            row = f"{c['row']} ({c['refused_by']})"
        dur = "-" if c.get("duration_s") is None else f"{c['duration_s']:.1f}"
        lines.append(f"{c['n']:>4} {t:<12} {gap:>8} {c['role']:<7} {c['request_line'][:34]:<34} "
                     f"{(c.get('agent') or '-')[:30]:<30} {c.get('ntrip_version') or '-':>3} {row[:26]:<26} "
                     f"{c['bytes_in']:>9} {okbad:>9} {tail:>5} {c.get('closed_by', '')[:20]:<20} {dur:>8}")
        prev_end[c["role"]] = c.get("end_mono")
    sources = [c for c in conns if c["role"] == "source"]
    lines.append("")
    lines.append(f"{len(conns)} connection(s): {len(sources)} source, "
                 f"{sum(1 for c in conns if c['role'] == 'client')} client, "
                 f"{sum(1 for c in conns if c['role'] == 'unknown')} other; "
                 "gap = from the end of the previous connection of the same role (source, client) to the start "
                 "of this one ('open': that one was still open)")
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0], epilog=__doc__.split("\n\n", 1)[1],
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sp = sub.add_parser("serve", help="run the caster until Ctrl-C")
    sp.add_argument("--port", type=int, required=True, help="TCP port to listen on (0 = any free port)")
    sp.add_argument("--host", default="127.0.0.1", help="address to listen on (default 127.0.0.1)")
    g = sp.add_mutually_exclusive_group()
    g.add_argument("--reply", metavar="ROW", help="the reply row for every source (default icy_ok)")
    g.add_argument("--sequence", metavar="SPEC", help="ROW[:kick=S][:text=ROW][:record],... one per connection")
    sp.add_argument("--onocoy-form", choices=ONOCOY_FORMS, default="error_line",
                    help="how the onocoy_* rows are sent (default error_line)")
    sp.add_argument("--mount", help="accept only this mountpoint (others get bad_mountpoint / HTTP 404)")
    sp.add_argument("--password", help="accept only this password (others get bad_password / HTTP 401); "
                    "use a made-up one such as bench-pass-1")
    sp.add_argument("--record", action="store_true", help="save every accepted session (record-NNN.rtcm3/.json)")
    sp.add_argument("--relay", action="store_true", help="serve GET /<mount> clients the current source's frames")
    add_log_args(sp)
    rp = sub.add_parser("rows", help="print every reply row and its exact bytes")
    rp.add_argument("--onocoy-form", choices=ONOCOY_FORMS, default="error_line")
    rp.add_argument("--json", action="store_true", help="JSON: {row: {kind, bytes (latin-1), note}}")
    pp = sub.add_parser("report", help="the connection table of a serve run")
    pp.add_argument("run_dir")
    pp.add_argument("--json", action="store_true", help="print the connections as a JSON list")
    args = ap.parse_args()

    if args.cmd == "rows":
        rows = build_rows(args.onocoy_form)
        if args.json:
            print(json.dumps({r.name: {"kind": r.kind, "bytes": r.data.decode("latin-1"), "note": r.note}
                              for r in rows.values()}, indent=1))
        else:
            for r in rows.values():
                print(f"{r.name:<26} {r.kind:<12} {r.note}")
                print(f"{'':<26} {r.data!r}")
        return 0
    if args.cmd == "report":
        try:
            conns = load_connections(Path(args.run_dir))
        except ValueError as e:
            print(f"fake_caster: {e}", file=sys.stderr)
            return 2
        print(json.dumps(conns, indent=1) if args.json else report(conns))
        return 0

    try:
        seq = parse_sequence(args.sequence) if args.sequence else [Entry(args.reply or "icy_ok")]
        if not args.sequence and seq[0].row not in ROW_NAMES:
            raise ValueError(f"unknown row {seq[0].row!r} (`fake_caster.py rows` lists them)")
    except ValueError as e:
        ap.error(str(e))
    run_dir = make_run_dir(args.log_dir, "caster", args.label)
    write_command_line(run_dir)
    stop = StopFlag()
    try:
        caster = FakeCaster(args.port, args.host, seq, args.onocoy_form, args.mount, args.password, args.record,
                            args.relay, run_dir, say=lambda m: print(m, flush=True))
    except OSError as e:
        print(f"fake_caster: cannot listen on {args.host}:{args.port}: {e}", file=sys.stderr)
        return 2
    print(f"[caster] {args.host}:{caster.port}; logging to {run_dir}; Ctrl-C to stop", flush=True)
    caster.start()
    try:
        while not stop.is_set():
            stop.wait(0.5)
    finally:
        caster.stop()
        conns = load_connections(run_dir) if (run_dir / "connections.jsonl").is_file() else []
        text = report(conns) if conns else "no connections"
        (run_dir / "report.txt").write_text(text + "\n", encoding="utf-8")
        print(text)
        print(f"log: {run_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
