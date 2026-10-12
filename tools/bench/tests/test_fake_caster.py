# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""fake_caster.py: every reply row on the wire, recording and checking
sessions (NTRIP 1.0 and 2.0 chunked), sequences, kicks, the mountpoint and
password checks, the relay and the connection report. Fake credentials only."""

from __future__ import annotations

import json
import socket
import subprocess
import sys
import time
from pathlib import Path

import pytest

from benchlib.ntrip import Dechunker, basic_auth, chunk, parse_request
from builders import arp_payload, msm_frame
from benchlib.rtcm import build_frame
from fake_caster import (
    ONOCOY_REASONS, ROW_NAMES, Entry, FakeCaster, build_rows, load_connections, parse_sequence, report,
)

BENCH = Path(__file__).resolve().parents[1]
PASSWORD = "bench-pass-1"
AGENT = "NTRIP bench-test/1.0"


def v1_request(password: str = PASSWORD, mount: str = "BENCH") -> bytes:
    return f"SOURCE {password} /{mount}\r\nSource-Agent: {AGENT}\r\n\r\n".encode()


def v2_request(mount: str = "BENCH", user: str = "testuser01", password: str = PASSWORD, chunked: bool = True) -> bytes:
    lines = [f"POST /{mount} HTTP/1.1", "Host: 127.0.0.1", "Ntrip-Version: Ntrip/2.0", f"User-Agent: {AGENT}",
             f"Authorization: {basic_auth(user, password)}", "Content-Type: gnss/data"]
    if chunked:
        lines.append("Transfer-Encoding: chunked")
    return ("\r\n".join(lines) + "\r\n\r\n").encode()


def frames() -> list[bytes]:
    cells = {(3, 2): 41.0, (3, 15): 38.5, (7, 2): 44.0, (7, 15): 40.0}
    out = []
    for k in range(4):
        out.append(msm_frame(1077, 9, 100000 + k * 1000, True, cells))
        out.append(msm_frame(1097, 9, 100000 + k * 1000, False, {(4, 2): 42.0}))
    out.append(build_frame(arp_payload(9, 1113194.9, -4842853.7, 3985496.0)))
    return out


def recv_all(sock: socket.socket, timeout: float = 5.0, limit: int = 1 << 20) -> bytes:
    sock.settimeout(timeout)
    buf = bytearray()
    while len(buf) < limit:
        try:
            data = sock.recv(65536)
        except socket.timeout:
            break
        except ConnectionResetError:
            break
        if not data:
            break
        buf += data
    return bytes(buf)


def recv_n(sock: socket.socket, n: int, timeout: float = 5.0) -> bytes:
    sock.settimeout(timeout)
    buf = bytearray()
    while len(buf) < n:
        data = sock.recv(n - len(buf))
        if not data:
            break
        buf += data
    return bytes(buf)


@pytest.fixture
def caster_factory(tmp_path):
    made = []

    def make(**kw) -> FakeCaster:
        run = tmp_path / f"run{len(made)}"
        run.mkdir()
        kw.setdefault("say", lambda m: None)
        c = FakeCaster(run_dir=run, **kw).start()
        made.append(c)
        return c

    yield make
    for c in made:
        c.stop()


def finished(c: FakeCaster, n: int, timeout: float = 10.0) -> list[dict]:
    assert c.wait_for(lambda x: len(x.done) >= n, timeout), f"{len(c.done)} of {n} connections ended"
    return sorted(c.finished(), key=lambda d: d["n"])


# -- rows ---------------------------------------------------------------------------


def test_every_row_is_named_in_the_contract():
    want = {"icy_ok", "ok", "http_200", "http_1_0_200", "http_400", "http_401", "http_403", "http_403_banned",
            "http_403_banned_chunked", "http_404", "http_409", "http_431", "http_500", "http_501", "http_503",
            "http_418", "sourcetable", "bad_password", "mount_taken", "already_connected", "bad_mountpoint",
            "missing_mountpoint", "too_many_sources", "error_other", "naomi_error", "not_authorized", "html_table",
            "html_banned", "line_banned", "line_sandbox", "trickle", "no_reply", "close_no_newline", "close_silent",
            "overlong", "garbage", "onocoy_login_blocked", "onocoy_in_use", "onocoy_parallel", "onocoy_reconnected",
            "onocoy_duplicate", "onocoy_credential_banned", "onocoy_auth_failed", "onocoy_reauth_failed",
            "onocoy_invalid_headers"}
    assert set(ROW_NAMES) == want
    rows = build_rows()
    assert rows["naomi_error"].data == b"Error 11 Unknown - epoll rd hup [onocoy naomi v2.5.95-fake]\r\n"
    assert b"The NTRIP Caster has banned your IP" in rows["html_banned"].data
    assert len(rows["overlong"].data) == 1100 and b"\n" not in rows["overlong"].data
    body = rows["http_403_banned_chunked"].data.split(b"\r\n\r\n", 1)[1]
    d = Dechunker()
    assert b"banned" in d.feed(body) and d.done and d.chunks == 2 and b"banned" not in body


@pytest.mark.parametrize("form", ["error_line", "plain_line", "http_401", "no_line_end"])
def test_onocoy_rows_in_each_form(form):
    rows = build_rows(form)
    for name, (num, text) in ONOCOY_REASONS.items():
        data = rows[name].data
        if form == "error_line":
            assert data == f"Error {num} {text} [onocoy naomi v2.5.95-fake]\r\n".encode()
        elif form == "no_line_end":
            assert data == f"Error {num} {text} [onocoy naomi v2.5.95-fake]".encode() and rows[name].kind == "close"
        elif form == "plain_line":
            assert data == f"{text}\r\n".encode()
        else:
            assert data.startswith(b"HTTP/1.1 401 Unauthorized\r\n") and data.endswith(f"{text}\r\n".encode())
        assert text.encode() in rows[name].line


@pytest.mark.parametrize("name", ROW_NAMES)
def test_each_row_on_the_wire_equals_the_table_and_the_log(caster_factory, name):
    row = build_rows()[name]
    c = caster_factory(sequence=[Entry(name)], hold_max_s=30)
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        if row.kind == "accept":
            got = recv_n(s, len(row.data))
            time.sleep(0.2)
        elif row.kind == "trickle":
            t0 = time.monotonic()
            got = recv_n(s, 3, timeout=6)
            assert time.monotonic() - t0 >= 1.8  # one byte per second
        elif row.kind == "silent":
            got = recv_all(s, timeout=1.0)
        else:
            got = recv_all(s, timeout=5.0)
    log = finished(c, 1)[0]
    if row.kind == "trickle":
        assert got == row.data[:3] and b"\n" not in got
        assert log["reply"].encode("latin-1").startswith(got)
    else:
        assert got == row.data
        assert log["reply"].encode("latin-1") == row.data
    assert log["row"] == name and log["role"] == "source" and log["request_line"] == "SOURCE *** /BENCH"
    assert log["agent"] == AGENT and log["ntrip_version"] == 1
    if row.kind in ("close", "close_silent"):
        assert log["closed_by"] == "caster"


def test_rows_cli_prints_every_row():
    out = subprocess.run([sys.executable, "fake_caster.py", "rows", "--json"], cwd=BENCH, capture_output=True,
                         text=True)
    assert out.returncode == 0, out.stderr
    table = json.loads(out.stdout)
    rows = build_rows()
    assert list(table) == list(rows)
    for name, r in rows.items():
        assert table[name]["bytes"].encode("latin-1") == r.data
    out = subprocess.run([sys.executable, "fake_caster.py", "rows"], cwd=BENCH, capture_output=True, text=True)
    assert out.returncode == 0 and "onocoy_in_use" in out.stdout


# -- sessions -----------------------------------------------------------------------


def test_v1_source_session_is_recorded_whole(caster_factory, tmp_path):
    c = caster_factory(record=True)
    sent = frames()
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        assert recv_n(s, 12) == b"ICY 200 OK\r\n"
        for f in sent:
            s.sendall(f)
            time.sleep(0.01)
    log = finished(c, 1)[0]
    run = c.run_dir
    assert (run / "record-001.rtcm3").read_bytes() == b"".join(sent)
    rec = json.loads((run / "record-001.json").read_text())
    assert rec["frames_ok"] == len(sent) and rec["crc_failures"] == 0
    assert rec["bytes_outside_frames"] == 0 and rec["partial_tail"] == 0
    assert rec["types"] == {"1005": 1, "1077": 4, "1097": 4} and rec["epochs"] == 4
    assert log["frames_ok"] == len(sent) and log["closed_by"] == "client" and log["record_file"] == "record-001.rtcm3"
    assert log["bytes_in"] == len(b"".join(sent))
    assert [f for _t, f in c.record_of(1).frames] == sent


def test_v2_chunked_source_session_is_dechunked_and_recorded_whole(caster_factory):
    c = caster_factory(sequence=[Entry("http_200", record=True)])
    sent = b"".join(frames())
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v2_request())
        reply = build_rows()["http_200"].data
        assert recv_n(s, len(reply)) == reply
        # Chunks of 37 bytes cut frames anywhere.
        for i in range(0, len(sent), 37):
            s.sendall(chunk(sent[i : i + 37]))
        s.sendall(b"0\r\n\r\n")
    log = finished(c, 1)[0]
    assert (c.run_dir / "record-001.rtcm3").read_bytes() == sent
    assert log["chunked"] and log["chunked_end"] and log["chunk_error"] is None
    assert log["frames_ok"] == len(frames()) and log["partial_tail"] == 0 and log["bytes_outside_frames"] == 0
    assert log["ntrip_version"] == 2 and log["user"] == "testuser01"
    assert "Basic ***" == log["request_headers"]["authorization"]


def test_v2_raw_body_is_recorded_too(caster_factory):
    c = caster_factory(sequence=[Entry("http_200")], record=True)
    sent = b"".join(frames())
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v2_request(chunked=False) + sent[:50])  # data right behind the header
        s.sendall(sent[50:])
        time.sleep(0.2)
    log = finished(c, 1)[0]
    assert (c.run_dir / "record-001.rtcm3").read_bytes() == sent
    assert log["frames_ok"] == len(frames()) and not log["chunked"]


def test_truncated_stream_reports_the_partial_tail(caster_factory):
    c = caster_factory(record=True)
    sent = frames()
    cut = sent[1][: len(sent[1]) // 2]
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        recv_n(s, 12)
        s.sendall(sent[0] + cut)
        time.sleep(0.2)
    log = finished(c, 1)[0]
    assert log["frames_ok"] == 1 and log["partial_tail"] == len(cut) and log["bytes_outside_frames"] == 0


def test_junk_between_frames_is_reported_as_bytes_outside_frames(caster_factory):
    c = caster_factory(record=True)
    sent = frames()
    junk = b"\x00\x01junk\xff"
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        recv_n(s, 12)
        s.sendall(sent[0] + junk + sent[1] + b"$GPGGA,noise\r\n" + sent[2])
        time.sleep(0.2)
    log = finished(c, 1)[0]
    assert log["frames_ok"] == 3 and log["bytes_outside_frames"] == len(junk) + len(b"$GPGGA,noise\r\n")
    assert log["partial_tail"] == 0


def test_bad_crc_frame_is_counted(caster_factory):
    c = caster_factory(record=True)
    good = frames()[0]
    bad = bytearray(good)
    bad[10] ^= 0x55
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        recv_n(s, 12)
        s.sendall(bytes(bad) + good)
        time.sleep(0.2)
    log = finished(c, 1)[0]
    assert log["frames_ok"] == 1 and log["crc_failures"] == 1 and log["bytes_outside_frames"] == len(bad)


# -- sequences, kicks, checks ----------------------------------------------------


def test_sequence_last_entry_repeats(caster_factory):
    c = caster_factory(sequence=parse_sequence("bad_password,mount_taken"))
    got = []
    for _ in range(3):
        with socket.create_connection((c.host, c.port)) as s:
            s.sendall(v1_request())
            got.append(recv_all(s))
    assert got == [b"ERROR - Bad Password\r\n", b"ERROR - Mount Point Taken or Invalid\r\n",
                   b"ERROR - Mount Point Taken or Invalid\r\n"]
    assert [d["row"] for d in finished(c, 3)] == ["bad_password", "mount_taken", "mount_taken"]


def test_set_sequence_starts_from_its_first_entry(caster_factory):
    c = caster_factory(sequence=parse_sequence("bad_password"))
    c.set_sequence("too_many_sources,ok")
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        assert recv_all(s) == b"ERROR - Too many sources\r\n"


def test_kick_sends_the_text_line_then_closes_after_the_delay(caster_factory):
    c = caster_factory(sequence=parse_sequence("icy_ok:kick=1:text=onocoy_reconnected"))
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        assert recv_n(s, 12) == b"ICY 200 OK\r\n"
        t_reply = time.monotonic()
        s.sendall(frames()[0])
        rest = recv_all(s, timeout=5)
        took = time.monotonic() - t_reply
    want = build_rows()["onocoy_reconnected"].line
    assert rest == want == b"Error 24 Device reconnected [onocoy naomi v2.5.95-fake]\r\n"
    assert 0.9 <= took <= 1.6
    log = finished(c, 1)[0]
    assert log["closed_by"] == "caster (kick)" and log["kick_s"] == 1 and log["text_sent"].encode() == want
    assert log["entry"] == "icy_ok:kick=1:text=onocoy_reconnected" and log["frames_ok"] == 1


def test_sequence_spec_errors():
    for bad in ("nope", "bad_password:kick=1", "icy_ok:text=onocoy_in_use", "icy_ok:kick=0", "icy_ok:kick=x",
                "icy_ok:frob", "icy_ok,,ok"):
        with pytest.raises(ValueError):
            parse_sequence(bad)
    e = parse_sequence("icy_ok:kick=2.5:text=onocoy_in_use:record")[0]
    assert (e.row, e.kick, e.text, e.record) == ("icy_ok", 2.5, "onocoy_in_use", True)


def test_password_check_refuses_a_wrong_one_with_bad_password(caster_factory):
    c = caster_factory(password=PASSWORD, mount="BENCH")
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request(password="wrong-pass-9"))
        assert recv_all(s) == b"ERROR - Bad Password\r\n"
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request())
        assert recv_n(s, 12) == b"ICY 200 OK\r\n"
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v1_request(mount="OTHER"))
        assert recv_all(s) == b"ERROR - Bad Mountpoint\r\n"
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(v2_request(password="wrong-pass-9"))
        assert recv_all(s).startswith(b"HTTP/1.1 401 ")
    logs = finished(c, 4)
    assert [d["refused_by"] for d in logs] == ["password", None, "mountpoint", "password"]
    for f in c.run_dir.iterdir():
        text = f.read_bytes()
        assert b"wrong-pass-9" not in text and PASSWORD.encode() not in text


def test_relay_serves_the_sources_frames_to_v1_and_v2_clients(caster_factory):
    c = caster_factory(relay=True, mount="BENCH")
    sent = frames()
    with socket.create_connection((c.host, c.port)) as v1, socket.create_connection((c.host, c.port)) as v2:
        v1.sendall(b"GET /BENCH HTTP/1.0\r\nUser-Agent: NTRIP test-client/1.0\r\n\r\n")
        v2.sendall(b"GET /BENCH HTTP/1.1\r\nHost: x\r\nNtrip-Version: Ntrip/2.0\r\nUser-Agent: NTRIP t/1\r\n\r\n")
        assert recv_n(v1, 12) == b"ICY 200 OK\r\n"
        head = bytearray()
        while b"\r\n\r\n" not in head:
            head += recv_n(v2, 1)
        assert head.startswith(b"HTTP/1.1 200 OK\r\n") and b"chunked" in head
        assert c.wait_for(lambda x: len(x.clients) == 2, 5)
        with socket.create_connection((c.host, c.port)) as src:
            src.sendall(v1_request())
            recv_n(src, 12)
            for f in sent:
                src.sendall(f)
                time.sleep(0.01)
            want = b"".join(sent)
            assert recv_n(v1, len(want)) == want
            d = Dechunker()
            got = bytearray()
            v2.settimeout(5)
            while len(got) < len(want):
                got += d.feed(v2.recv(65536))
            assert bytes(got) == want
    logs = finished(c, 3)
    clients = [x for x in logs if x["role"] == "client"]
    assert len(clients) == 2 and all(x["frames_out"] == len(sent) for x in clients)


def test_get_without_relay_gets_the_sourcetable(caster_factory):
    c = caster_factory()
    with socket.create_connection((c.host, c.port)) as s:
        s.sendall(b"GET / HTTP/1.0\r\nUser-Agent: NTRIP t/1\r\n\r\n")
        assert recv_all(s).startswith(b"SOURCETABLE 200 OK\r\n")


# -- report -------------------------------------------------------------------------


def test_report_prints_the_gaps_between_connections(caster_factory):
    c = caster_factory(sequence=parse_sequence("bad_password"))
    for _ in range(3):
        with socket.create_connection((c.host, c.port)) as s:
            s.sendall(v1_request())
            recv_all(s)
        time.sleep(0.5)
    finished(c, 3)
    conns = load_connections(c.run_dir)
    assert len(conns) == 3
    for line in (c.run_dir / "connections.jsonl").read_text().splitlines():
        d = json.loads(line)
        assert d["start_unix"] > 1.7e9 and d["start_mono"] <= d["end_mono"]
    gaps = [b["start_mono"] - a["end_mono"] for a, b in zip(conns, conns[1:])]
    text = report(conns)
    for g in gaps:
        assert 0.4 <= g <= 1.0
        assert f"{g:.1f}" in text
    assert "SOURCE *** /BENCH" in text and "bad_password" in text
    out = subprocess.run([sys.executable, "fake_caster.py", "report", str(c.run_dir)], cwd=BENCH,
                         capture_output=True, text=True)
    assert out.returncode == 0 and "gap" in out.stdout and PASSWORD not in out.stdout


def test_request_parsing_masks_the_password():
    r = parse_request(b"SOURCE fakepass123 /testuser01\r\nSource-Agent: NTRIP x/1\r\n")
    assert (r.role, r.mount, r.password, r.ntrip_version) == ("source", "testuser01", "fakepass123", 1)
    assert r.masked_line == "SOURCE *** /testuser01" and "fakepass123" not in repr(r)
    r = parse_request(v2_request(password="fakepass123")[:-4])
    assert (r.role, r.user, r.password, r.ntrip_version, r.chunked) == ("source", "testuser01", "fakepass123", 2, True)
    assert "fakepass123" not in json.dumps(r.masked_headers())


def test_serve_cli_help():
    out = subprocess.run([sys.executable, "fake_caster.py", "serve", "--help"], cwd=BENCH, capture_output=True,
                         text=True)
    assert out.returncode == 0 and "--sequence" in out.stdout and "--onocoy-form" in out.stdout
