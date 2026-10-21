# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""ntrip_client.py against a small caster in this process that answers with
the station's replies byte for byte (benchlib.ntrip station_*): NTRIP 1.0
with and without the blank line after ICY 200 OK, 2.0 chunked, 2.0 over
HTTP/1.0 raw, the source table (1.0 and 2.0), 401, 403, 429, 503; a
corrupted frame and a frame split across chunks fail the checks; the
password never reaches the output or the run directory. Also the reply
builders against the documented bytes, parse_sourcetable and the chunk
boundaries of the Dechunker."""

from __future__ import annotations

import base64
import calendar
import json
import os
import socket
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path

import pytest

from benchlib import rtcm
from benchlib.ntrip import (
    LAST_CHUNK, STR_FIELDS, V1, V2_CHUNKED, V2_RAW, Dechunker, chunk, imf_date, parse_sourcetable, reply_flavour,
    station_error, station_sourcetable, station_str, station_stream_head, str_field,
)
from builders import arp_payload, msm_frame
from ntrip_client import fetch_sourcetable, open_stream, read_for

BENCH = Path(__file__).resolve().parents[1]
MOUNT = "ROVER"
USER = "rover"
PASSWORD = "test-pw-not-secret-1"   # made up for these tests
VER = "v0.9.0-test"
CELLS = {(3, 2): 40.0, (9, 2): 43.0}
EPOCHS = 8
STR_LINE = station_str(MOUNT, "onodiy-test", "1005(1),1077(1),1087(1),1127(1)", 2, "GPS+GLO+BDS", "onodiy", 13600)


def epochs(n: int = EPOCHS) -> list[list[bytes]]:
    """n epochs of three MSMs; the first opens with a 1005."""
    out = []
    for k in range(n):
        e = [msm_frame(1077, 1, 1000 * k, True, CELLS), msm_frame(1087, 1, 1000 * k, True, CELLS),
             msm_frame(1127, 1, 1000 * k, False, CELLS)]
        if k == 0:
            e.insert(0, rtcm.build_frame(arp_payload(1, 1113194.9, -4842853.7, 3985496.0)))
        out.append(e)
    return out


def frame_count(eps: list[list[bytes]]) -> int:
    return sum(len(e) for e in eps)


class MiniCaster:
    """A local caster that decides as the station does (bad request or browser, source table, throttle, login,
    full, stream) and answers with benchlib.ntrip's station replies. One chunk (or raw block) per epoch."""

    def __init__(self, eps=None, *, icy_blank=True, full=False, throttle_s=0, terminate=True, split_frame=False,
                 corrupt=None, date=None, stream_head=None, table_raw=None, reset_at_end=False, gap_s=0.01):
        self.eps = eps if eps is not None else epochs()
        self.icy_blank, self.full, self.throttle_s = icy_blank, full, throttle_s
        self.terminate, self.split_frame, self.corrupt = terminate, split_frame, corrupt
        self.date, self.stream_head, self.table_raw = date, stream_head, table_raw
        self.reset_at_end, self.gap_s = reset_at_end, gap_s
        self.requests: list[bytes] = []
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(8)
        self.port = self.srv.getsockname()[1]
        self.stopped = threading.Event()
        self.thread = threading.Thread(target=self._accept, daemon=True)

    def __enter__(self) -> MiniCaster:
        self.thread.start()
        return self

    def __exit__(self, *exc) -> None:
        self.stopped.set()
        self.srv.close()
        self.thread.join(timeout=5)

    def _accept(self) -> None:
        while not self.stopped.is_set():
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    def _serve(self, c: socket.socket) -> None:
        with c:
            c.settimeout(5)
            head = b""
            try:
                while b"\r\n\r\n" not in head and b"\n\n" not in head and len(head) < 1024:
                    d = c.recv(1024)
                    if not d:
                        return
                    head += d
            except OSError:
                return
            self.requests.append(head)
            lines = head.decode("latin-1").replace("\r\n", "\n").split("\n")
            parts = lines[0].split()
            hdrs = {}
            for line in lines[1:]:
                if ":" in line:
                    k, v = line.split(":", 1)
                    hdrs[k.strip().lower()] = v.strip()
            version = parts[2] if len(parts) > 2 else "HTTP/1.0"
            flavour = reply_flavour(hdrs.get("ntrip-version", "").lower() == "ntrip/2.0", version == "HTTP/1.1")
            if not parts or parts[0] != "GET" or version not in ("HTTP/1.0", "HTTP/1.1"):
                return self._close(c, station_error(400, flavour, VER, self.date))
            if "origin" in hdrs or any(k.startswith("sec-fetch-") for k in hdrs):
                return self._close(c, station_error(403, flavour, VER, self.date))
            target = parts[1]
            if target.startswith("http://"):
                target = "/" + target[7:].split("/", 1)[-1] if "/" in target[7:] else "/"
            mount = target[1:].split("?", 1)[0]
            if not mount or mount.lower() != MOUNT.lower():
                reply = self.table_raw or station_sourcetable(flavour, VER, [STR_LINE], self.date)
                return self._close(c, reply)
            if self.throttle_s:
                return self._close(c, station_error(429, flavour, VER, self.date, retry_s=self.throttle_s))
            want = "Basic " + base64.b64encode(f"{USER}:{PASSWORD}".encode()).decode()
            if hdrs.get("authorization") != want:
                return self._close(c, station_error(401, flavour, VER, self.date, mount=MOUNT))
            if self.full:
                return self._close(c, station_error(503, flavour, VER, self.date))
            self._stream(c, flavour)

    def _close(self, c: socket.socket, reply: bytes) -> None:
        c.sendall(reply)
        c.shutdown(socket.SHUT_WR)
        end = time.monotonic() + 2
        try:
            while time.monotonic() < end and c.recv(1024):
                pass
        except OSError:
            pass

    def _stream(self, c: socket.socket, flavour: str) -> None:
        head = self.stream_head or station_stream_head(flavour, VER, self.date)
        if flavour == V1 and not self.icy_blank and self.stream_head is None:
            head = b"ICY 200 OK\r\n"
        try:
            c.sendall(head)
            n = 0
            for i, e in enumerate(self.eps):
                frames = list(e)
                for j, f in enumerate(frames):
                    if self.corrupt == n:
                        frames[j] = f[:8] + bytes([f[8] ^ 0x40]) + f[9:]
                    n += 1
                data = b"".join(frames)
                if flavour != V2_CHUNKED:
                    c.sendall(data)
                elif self.split_frame and i == 1:
                    k = len(frames[0]) + 5   # 5 bytes into the epoch's second frame
                    c.sendall(chunk(data[:k]) + chunk(data[k:]))
                else:
                    c.sendall(chunk(data))
                time.sleep(self.gap_s)
            if self.reset_at_end:
                c.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
                return
            if flavour == V2_CHUNKED and self.terminate:
                c.sendall(LAST_CHUNK)
            c.shutdown(socket.SHUT_WR)
            c.settimeout(2)
            while c.recv(1024):
                pass
        except OSError:
            pass


def run_client(port: int, *args: str, env: dict | None = None, log_dir: Path) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, "ntrip_client.py", "--host", "127.0.0.1", "--port", str(port),
                           "--log-dir", str(log_dir), *args], cwd=BENCH, capture_output=True, text=True,
                          env=env or dict(os.environ), timeout=60)


def with_pw(password: str = PASSWORD) -> dict:
    return dict(os.environ, ROVER_PW=password)


def run_dir(log_dir: Path) -> Path:
    runs = sorted(log_dir.iterdir())
    assert len(runs) == 1, runs
    return runs[0]


# -- the station's replies, as documented ------------------------------------------------------------


def test_reply_builders_match_the_documented_bytes():
    date = imf_date(calendar.timegm((2026, 10, 19, 4, 30, 0)))
    assert date == "Mon, 19 Oct 2026 04:30:00 GMT"
    assert station_stream_head(V1, "v1.2.3", date) == b"ICY 200 OK\r\n\r\n"
    assert station_stream_head(V2_CHUNKED, "v1.2.3", date) == (
        b"HTTP/1.1 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nServer: NTRIP onodiy/v1.2.3\r\n"
        b"Date: Mon, 19 Oct 2026 04:30:00 GMT\r\nCache-Control: no-store, no-cache, max-age=0\r\n"
        b"Pragma: no-cache\r\nConnection: close\r\nContent-Type: gnss/data\r\nTransfer-Encoding: chunked\r\n\r\n")
    assert station_stream_head(V2_RAW, "v1.2.3") == (
        b"HTTP/1.0 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nServer: NTRIP onodiy/v1.2.3\r\n"
        b"Cache-Control: no-store, no-cache, max-age=0\r\nPragma: no-cache\r\nConnection: close\r\n"
        b"Content-Type: gnss/data\r\n\r\n")
    body = STR_LINE + "\r\nENDSOURCETABLE\r\n"
    assert station_sourcetable(V1, "v1.2.3", [STR_LINE], date) == (
        f"SOURCETABLE 200 OK\r\nServer: NTRIP onodiy/v1.2.3\r\nContent-Type: text/plain\r\n"
        f"Content-Length: {len(body)}\r\n\r\n{body}").encode()
    assert station_sourcetable(V2_RAW, "v1.2.3", [STR_LINE]) == (
        f"HTTP/1.0 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nServer: NTRIP onodiy/v1.2.3\r\nConnection: close\r\n"
        f"Content-Type: gnss/sourcetable\r\nContent-Length: {len(body)}\r\n\r\n{body}").encode()
    assert station_error(401, V1, "v1.2.3", mount="ROVER") == (
        b'HTTP/1.0 401 Unauthorized\r\nServer: NTRIP onodiy/v1.2.3\r\nWWW-Authenticate: Basic realm="/ROVER"\r\n'
        b"Connection: close\r\nContent-Type: text/plain\r\nContent-Length: 39\r\n\r\n"
        b"Wrong or missing username or password\r\n")
    assert station_error(429, V2_CHUNKED, "v1.2.3", date, retry_s=8).endswith(
        b"Retry-After: 8\r\nConnection: close\r\nContent-Type: text/plain\r\nContent-Length: 41\r\n\r\n"
        b"Too many wrong logins, try again in 8 s\r\n")
    assert station_error(429, V2_CHUNKED, "v1.2.3", date, retry_s=8).startswith(
        b"HTTP/1.1 429 Too Many Requests\r\nNtrip-Version: Ntrip/2.0\r\nServer: NTRIP onodiy/v1.2.3\r\nDate: ")
    assert station_error(503, V2_RAW, "v1.2.3").startswith(b"HTTP/1.0 503 Service Unavailable\r\nNtrip-Version")
    assert station_error(503, V2_RAW, "v1.2.3").endswith(b"\r\n\r\nAt most 4 rovers at a time\r\n")
    assert station_error(403, V1, "v").endswith(b"Content-Length: 22\r\n\r\nNot for web browsers\r\n")
    assert station_error(400, V1, "v").endswith(b"Content-Length: 13\r\n\r\nBad request\r\n")
    assert [reply_flavour(False, True), reply_flavour(True, True), reply_flavour(True, False)] == [V1, V2_CHUNKED,
                                                                                                    V2_RAW]


def test_str_record_and_parse_sourcetable():
    example = station_str("onodiy", "onodiy", "1005(1),1033(1),1077(1),1087(1),1097(1),1117(1),1127(1),1137(1),"
                          "1230(1)", 2, "GPS+GLO+GAL+BDS+QZS+IRN", "UM982", 13600)
    assert example == ("STR;onodiy;onodiy;RTCM 3.3;1005(1),1033(1),1077(1),1087(1),1097(1),1117(1),1127(1),1137(1),"
                       "1230(1);2;GPS+GLO+GAL+BDS+QZS+IRN;onodiy;;0.00;0.00;0;0;UM982;none;B;N;13600;")
    assert example.count(";") == 18
    assert str_field("UM;982\x01éx") == "UM_982x"
    text = ("CAS;caster.example;2101;X;Y;0;DEU;50.0;8.0;0.0.0.0;0;\r\nNET;N;X;B;N;none;none;none;\r\n"
            f"{example}\r\nSTR;SHORT;x;RTCM 3.3\r\nSTR;LONG;{';' * 16}a;b\r\nENDSOURCETABLE\r\nSTR;AFTER;\r\n")
    recs = parse_sourcetable(text)
    assert [r["mountpoint"] for r in recs] == ["onodiy", "SHORT", "LONG"]
    r = recs[0]
    assert r["field_count"] == 19 and tuple(k for k in r if k != "field_count") == STR_FIELDS
    assert (r["format"], r["carrier"], r["nav_system"], r["country"], r["latitude"], r["longitude"],
            r["generator"], r["authentication"], r["fee"], r["bitrate"], r["misc"]) == (
        "RTCM 3.3", "2", "GPS+GLO+GAL+BDS+QZS+IRN", "", "0.00", "0.00", "UM982", "B", "N", "13600", "")
    assert recs[1]["field_count"] == 4 and "misc" not in recs[1]
    assert recs[2]["field_count"] == 20 and recs[2]["misc"] == "a;b"


def test_dechunker_records_where_each_chunk_ended():
    d = Dechunker()
    wire = chunk(b"abc") + chunk(b"defgh") + chunk(b"i") + LAST_CHUNK
    out = b"".join(d.feed(wire[i : i + 2]) for i in range(0, len(wire), 2))
    assert out == b"abcdefghi" and d.done and d.error is None
    assert d.chunks == 3 and d.chunk_boundaries == [3, 8, 9] and d.out == 9
    bad = Dechunker()
    bad.feed(b"3\r\nabcXX")
    assert bad.error and bad.chunk_boundaries == []


# -- the client against the caster -----------------------------------------------------------------


@pytest.mark.parametrize("blank", [True, False])
def test_ntrip_1_with_and_without_the_blank_line(tmp_path, blank):
    with MiniCaster(icy_blank=blank) as mc:
        out = run_client(mc.port, "--mount", MOUNT, "--password-env", "ROVER_PW", "--duration", "10",
                         env=with_pw(), log_dir=tmp_path)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "< ICY 200 OK" in out.stdout and "PASS" in out.stdout
    assert ("with a blank line" if blank else "without a blank line") in out.stdout
    assert "the caster closed the stream" in out.stdout
    d = json.loads((run_dir(tmp_path) / "summary.json").read_text())
    s = d["summary"]
    assert s["frames"] == frame_count(epochs()) and s["crc_failures"] == 0 and s["bytes_outside_frames"] == 0
    assert s["blank_line"] is blank and not s["chunked"]
    head = (run_dir(tmp_path) / "reply-header.txt").read_bytes()
    assert head == (b"ICY 200 OK\r\n\r\n" if blank else b"ICY 200 OK\r\n")
    req = mc.requests[0].decode()
    assert req.startswith(f"GET /{MOUNT} HTTP/1.0\r\n") and "Ntrip-Version" not in req
    assert "User-Agent: NTRIP onodiy-ntrip-client/1.0\r\n" in req


def test_icy_followed_by_header_lines():
    with MiniCaster(stream_head=b"ICY 200 OK\r\nServer: other/1\r\n\r\n") as mc:
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, PASSWORD, 1)
        s = read_for(st, 10)
    assert st.blank_line and st.reply.header("Server") == "other/1"
    assert st.reply.head == b"ICY 200 OK\r\nServer: other/1\r\n\r\n"
    assert s["frames"] == frame_count(epochs()) and s["bytes_outside_frames"] == 0 and st.first_byte == 0xD3


def test_ntrip_2_chunked(tmp_path):
    with MiniCaster() as mc:
        out = run_client(mc.port, "--mount", MOUNT, "--password-env", "ROVER_PW", "--ntrip-version", "2",
                         "--duration", "10", env=with_pw(), log_dir=tmp_path)
    assert out.returncode == 0, out.stdout + out.stderr
    assert "< HTTP/1.1 200 OK" in out.stdout and "< Transfer-Encoding: chunked" in out.stdout
    assert f"chunks: {EPOCHS}, framing clean, each held whole frames, the caster's last chunk came" in out.stdout
    assert "the caster closed the stream after the last chunk" in out.stdout
    req = mc.requests[0].decode()
    assert req.startswith(f"GET /{MOUNT} HTTP/1.1\r\nHost: 127.0.0.1:{mc.port}\r\nNtrip-Version: Ntrip/2.0\r\n")
    s = json.loads((run_dir(tmp_path) / "summary.json").read_text())["summary"]
    assert s["chunks"] == EPOCHS and s["chunks_split"] == 0 and s["last_chunk"] and s["eof"]
    assert s["frames"] == frame_count(epochs())


def test_ntrip_2_over_http_1_0_is_raw_and_the_head_byte_exact():
    date = imf_date(time.time())
    with MiniCaster(date=date) as mc:
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, PASSWORD, 2, http_version="1.0")
        s = read_for(st, 10)
    assert mc.requests[0].startswith(f"GET /{MOUNT} HTTP/1.0\r\n".encode())
    assert st.reply.head == station_stream_head(V2_RAW, VER, date)
    assert not st.chunked and st.dechunk is None and st.eof and not st.reset
    assert s["frames"] == frame_count(epochs()) and s["bytes_outside_frames"] == 0
    assert bytes(st.data) == b"".join(f for e in epochs() for f in e)
    assert [f for _, f in st.frames()] == [f for e in epochs() for f in e]
    assert all(t is not None for t, _ in st.frames())


@pytest.mark.parametrize("case", ["wrong", "none", "full", "throttled", "unknown_mount"])
def test_refusals_exit_2_with_the_code_and_reason(tmp_path, case):
    kw = {"full": True} if case == "full" else {"throttle_s": 7} if case == "throttled" else {}
    args = ["--mount", "NOPE" if case == "unknown_mount" else MOUNT, "--ntrip-version", "2"]
    if case != "none":
        args += ["--password-env", "ROVER_PW"]
    env = with_pw("wrong-password" if case == "wrong" else PASSWORD)
    with MiniCaster(**kw) as mc:
        out = run_client(mc.port, *args, env=env, log_dir=tmp_path)
    assert out.returncode == 2, out.stdout + out.stderr
    want = {"wrong": '401 Unauthorized (Basic realm="/ROVER")', "none": "401 Unauthorized",
            "full": "503 Service Unavailable", "throttled": "429 Too Many Requests (Retry-After: 7 s)",
            "unknown_mount": "the caster sent its source table instead: it has no mountpoint 'NOPE'"}[case]
    assert f"refused: {want}" in out.stderr
    if case == "none":
        assert "authorization" not in mc.requests[0].decode().lower()
    d = json.loads((run_dir(tmp_path) / "summary.json").read_text())
    assert d["summary"]["refused"] is True


def test_refusals_through_the_api_are_read_whole():
    with MiniCaster() as mc:
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, PASSWORD, 1, extra_headers={"Origin": "http://x"})
        assert st.refused and st.reply.code == 403 and st.body_eof
        assert st.reply.head + st.body == station_error(403, V1, VER)
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, "wrong-password", 2)
        assert st.reply.head + st.body == station_error(401, V2_CHUNKED, VER, mount=MOUNT)
        assert read_for(st, 5)["refused"] is True


@pytest.mark.parametrize("version", [1, 2])
def test_sourcetable(tmp_path, version):
    with MiniCaster() as mc:
        out = run_client(mc.port, "--sourcetable", "--ntrip-version", str(version), log_dir=tmp_path)
        t = fetch_sourcetable("127.0.0.1", mc.port, version)
        t2 = fetch_sourcetable("127.0.0.1", mc.port, version, mount="NOPE")
    assert out.returncode == 0, out.stdout + out.stderr
    first = "SOURCETABLE 200 OK" if version == 1 else "HTTP/1.1 200 OK"
    assert f"< {first}" in out.stdout and "STR 1 of 1 (19 fields):" in out.stdout
    assert "  mountpoint      ROVER" in out.stdout and "  authentication  B" in out.stdout
    assert "PASS: ENDSOURCETABLE at the end, 1 STR record(s) of 19 fields" in out.stdout
    flavour = V1 if version == 1 else V2_CHUNKED
    assert t.reply.head + t.body == station_sourcetable(flavour, VER, [STR_LINE])
    assert t.eof and t.ended and not t.problems() and t.records[0]["mountpoint"] == MOUNT
    assert t2.reply.head + t2.body == t.reply.head + t.body
    assert (run_dir(tmp_path) / "sourcetable.txt").read_bytes() == t.body
    assert "authorization" not in mc.requests[0].decode().lower()


def test_a_table_without_its_end_fails(tmp_path):
    body = b"STR;ROVER;x;RTCM 3.3\r\n"
    raw = b"SOURCETABLE 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body
    with MiniCaster(table_raw=raw) as mc:
        out = run_client(mc.port, "--sourcetable", log_dir=tmp_path)
    assert out.returncode == 1
    assert "does not end with ENDSOURCETABLE" in out.stdout and "no STR record of 19 fields" in out.stdout


def test_a_corrupted_frame_fails(tmp_path):
    with MiniCaster(corrupt=5) as mc:
        out = run_client(mc.port, "--mount", MOUNT, "--password-env", "ROVER_PW", "--duration", "10",
                         env=with_pw(), log_dir=tmp_path)
    assert out.returncode == 1, out.stdout + out.stderr
    assert "FAIL: 1 CRC failures" in out.stdout


def test_a_frame_split_across_chunks_is_reported(tmp_path):
    with MiniCaster(split_frame=True) as mc:
        out = run_client(mc.port, "--mount", MOUNT, "--password-env", "ROVER_PW", "--ntrip-version", "2",
                         "--duration", "10", env=with_pw(), log_dir=tmp_path)
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, PASSWORD, 2)
        s = read_for(st, 10)
    assert out.returncode == 1, out.stdout + out.stderr
    assert "1 chunk(s) did not hold whole frames (first: chunk 2 ended 5 bytes into a frame" in out.stdout
    # The frames themselves are whole and valid: only the chunking is wrong.
    assert s["frames"] == frame_count(epochs()) and s["crc_failures"] == 0 and s["chunks"] == EPOCHS + 1
    assert st.chunks_split == 1 and st.problems() == [f"1 chunk(s) did not hold whole frames (first: {st.first_split})"]


def test_min_frames(tmp_path):
    with MiniCaster(eps=epochs(2)) as mc:
        out = run_client(mc.port, "--mount", MOUNT, "--password-env", "ROVER_PW", "--duration", "10",
                         "--min-frames", "50", env=with_pw(), log_dir=tmp_path)
    assert out.returncode == 1 and "7 frames (at least 50 wanted)" in out.stdout


def test_save_and_json_and_the_password_never_appears(tmp_path):
    secret_b64 = base64.b64encode(f"{USER}:{PASSWORD}".encode())
    with MiniCaster() as mc:
        out = run_client(mc.port, "--mount", MOUNT, "--password-env", "ROVER_PW", "--ntrip-version", "2",
                         "--duration", "10", "--save", "--json", env=with_pw(), log_dir=tmp_path)
    assert out.returncode == 0, out.stdout + out.stderr
    assert mc.requests[0].decode().count(f"Authorization: Basic {secret_b64.decode()}\r\n") == 1
    d = json.loads(out.stdout)
    assert d["problems"] == [] and d["summary"]["frames"] == frame_count(epochs())
    assert all(not x.lower().startswith("authorization") for x in d["request"])
    rd = run_dir(tmp_path)
    data = (rd / "stream-0000.bin").read_bytes()
    assert data == b"".join(f for e in epochs() for f in e)
    idx = [line.split(",") for line in (rd / "stream-0000.idx").read_text().split()]
    assert sum(int(n) for _, _, n in idx) == len(data)
    files = [p for p in rd.rglob("*") if p.is_file()]
    assert {p.name for p in files} >= {"command.txt", "reply-header.txt", "summary.json", "stream-0000.bin"}
    for blob in (out.stdout.encode(), out.stderr.encode(), *(p.read_bytes() for p in files)):
        assert PASSWORD.encode() not in blob and secret_b64 not in blob


def test_end_reasons_last_chunk_and_reset():
    with MiniCaster(terminate=True) as mc:
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, PASSWORD, 2)
        read_for(st, 10)
    assert st.last_chunk and st.eof and st.end == "the caster closed the stream after the last chunk"
    assert st.summary()["partial_tail"] == 0
    with MiniCaster(reset_at_end=True) as mc:
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, PASSWORD, 1)
        read_for(st, 10)
    assert st.reset and not st.eof and st.end == "the caster reset the connection"
    stop = threading.Event()
    with MiniCaster(eps=epochs(200), gap_s=0.05) as mc:
        st = open_stream("127.0.0.1", mc.port, MOUNT, USER, PASSWORD, 2)
        t = threading.Thread(target=read_for, args=(st, None), kwargs={"stop": stop})
        t.start()
        time.sleep(0.5)
        live = st.summary()
        stop.set()
        t.join(timeout=5)
    assert 0 < live["frames"] < frame_count(epochs(200)) and live["bytes_outside_frames"] == 0
    assert st.end == "stopped" and not st.last_chunk


def test_help_and_imports_with_the_standard_library_only():
    out = subprocess.run([sys.executable, "ntrip_client.py", "--help"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 0 and "--sourcetable" in out.stdout and "--password-env" in out.stdout
    # tools/qemu/e2e.py imports it by path, with no third-party package installed.
    code = ("import sys; sys.path.insert(0, '.'); import ntrip_client; "
            "bad = [m for m in ('serial', 'httpx', 'pytest') if m in sys.modules]; assert not bad, bad")
    out = subprocess.run([sys.executable, "-I", "-c", code], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 0, out.stderr


def test_host_is_required_and_a_missing_password_variable_is_named(tmp_path):
    out = subprocess.run([sys.executable, "ntrip_client.py", "--mount", MOUNT], cwd=BENCH, capture_output=True,
                         text=True)
    assert out.returncode == 2 and "--host" in out.stderr
    env = dict(os.environ)
    env.pop("ROVER_PW_UNSET", None)
    out = run_client(1, "--mount", MOUNT, "--password-env", "ROVER_PW_UNSET", env=env, log_dir=tmp_path)
    assert out.returncode == 2 and "$ROVER_PW_UNSET is not set" in out.stderr
