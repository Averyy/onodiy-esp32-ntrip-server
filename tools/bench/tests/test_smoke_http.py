# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The HTTP reading of tools/qemu/smoke.py: each answer read to the end its framing gives
(Content-Length, the last chunk, the end of the connection), late answers waited for instead of
taken for missing or different ones, pipelined answers kept apart, and the response buffer's size
as the firmware declares it."""

from __future__ import annotations

import importlib.util
import socket
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]


def _load_smoke():
    spec = importlib.util.spec_from_file_location("smoke_under_test", ROOT / "tools" / "qemu" / "smoke.py")
    mod = importlib.util.module_from_spec(spec)
    before = sys.dont_write_bytecode
    sys.dont_write_bytecode = True
    try:
        spec.loader.exec_module(mod)
    finally:
        sys.dont_write_bytecode = before
    return mod


smoke = _load_smoke()


def _send_later(sock: socket.socket, parts: list[tuple[float, bytes]], close: bool = False) -> threading.Thread:
    def run():
        for delay, data in parts:
            time.sleep(delay)
            sock.sendall(data)
        if close:
            sock.shutdown(socket.SHUT_WR)
    t = threading.Thread(target=run, daemon=True)
    t.start()
    return t


def chunked(body: bytes, size: int) -> bytes:
    out = b""
    for i in range(0, len(body), size):
        part = body[i:i + size]
        out += f"{len(part):x}\r\n".encode() + part + b"\r\n"
    return out + b"0\r\n\r\n"


def test_chunked_then_length_on_one_connection_in_pieces():
    server, client = socket.socketpair()
    body = b'{"api":1,"x":"' + b"y" * 3000 + b'"}'
    wire = (b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nTransfer-Encoding: chunked\r\n\r\n"
            + chunked(body, 2048)
            + b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}")
    # Byte by byte at first, then the rest: the framing decides where each answer ends.
    _send_later(server, [(0, wire[i:i + 1]) for i in range(40)] + [(0.01, wire[40:])])
    rd = smoke.AnswerReader(client)
    a = rd.read(timeout=5)
    b = rd.read(timeout=5)
    assert (a.status, a.framing, a.complete, a.body) == (200, "chunked", True, body)
    assert a.headers["content-type"] == "application/json"
    assert (b.status, b.framing, b.complete, b.body) == (200, "length", True, b"{}")
    assert not rd.buf


def test_a_late_answer_is_waited_for():
    # As a frame the emulated MAC dropped makes it: the answer only after several seconds.
    server, client = socket.socketpair()
    _send_later(server, [(2.5, b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello")])
    a = smoke.AnswerReader(client).read(timeout=10)
    assert a.complete and a.body == b"hello" and a.seconds >= 2.4


def test_no_answer_and_a_cut_answer_are_incomplete():
    server, client = socket.socketpair()
    a = smoke.AnswerReader(client).read(timeout=0.3)
    assert a.status == 0 and not a.complete and a.framing == ""
    server2, client2 = socket.socketpair()
    _send_later(server2, [(0, b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel")], close=True)
    a = smoke.AnswerReader(client2).read(timeout=5)
    assert a.status == 200 and a.framing == "chunked" and not a.complete


def test_to_the_end_of_the_connection_and_the_close_after_it():
    server, client = socket.socketpair()
    _send_later(server, [(0, b"HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nline one\nline two\n")], close=True)
    rd = smoke.AnswerReader(client)
    a = rd.read(timeout=5)
    assert (a.framing, a.complete, a.body) == ("close", True, b"line one\nline two\n")
    closed = rd.wait_close(1.0)
    assert closed is not None and closed < 0.5


def test_a_connection_left_open_is_not_closed():
    server, client = socket.socketpair()
    _send_later(server, [(0, b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n")])
    rd = smoke.AnswerReader(client)
    assert rd.read(timeout=5).complete
    assert rd.wait_close(0.3) is None
    server.close()
    assert rd.wait_close(2.0) is not None


def test_no_body_for_switching_protocols():
    server, client = socket.socketpair()
    _send_later(server, [(0, b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n\r\n")])
    a = smoke.AnswerReader(client).read(timeout=2)
    assert a.status == 101 and a.complete and a.body == b""


def test_response_buffer_size_from_the_firmware():
    n = smoke.response_buffer_size()
    assert n is not None and n >= 1024
