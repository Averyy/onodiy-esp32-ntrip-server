# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""um982.py against scripted ports (no hardware): it sends nothing to a
receiver whose loader it hears, and its advice after no answer is to listen
first, never a search over other bauds."""

from __future__ import annotations

import threading
import time

import um982


class ScriptedPort:
    """The part of pyserial um982.py uses. `chatter` is sent again and again while the port is open (a loader
    that shows its prompt); every write is recorded."""

    def __init__(self, baud: int, chatter: bytes = b"", every: float = 0.1, timeout: float = 0.05):
        self.baudrate = baud
        self.timeout = timeout
        self.chatter = chatter
        self.every = every
        self.rx = bytearray()
        self.written: list[bytes] = []
        self.lock = threading.Lock()
        self.closed = False
        self.next_at = 0.0

    def _tick(self) -> None:
        now = time.monotonic()
        if self.chatter and now >= self.next_at:
            self.next_at = now + self.every
            with self.lock:
                self.rx += self.chatter

    @property
    def in_waiting(self) -> int:
        self._tick()
        with self.lock:
            return len(self.rx)

    def read(self, n: int = 1) -> bytes:
        self._tick()
        with self.lock:
            if not self.rx:
                empty = True
            else:
                empty = False
                out = bytes(self.rx[:n])
                del self.rx[:n]
        if empty:
            time.sleep(self.timeout)
            return b""
        return out

    def write(self, data: bytes) -> int:
        self.written.append(bytes(data))
        return len(data)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True


LOADER = b"\r\n  5 Menu cmd\r\n  6 Soft reset cpu\r\nboot>"


def opener_for(ports: list, chatter_at: dict[int, bytes] | None = None):
    def opener(device, baud, timeout=0.05):
        p = ScriptedPort(baud, (chatter_at or {}).get(baud, b""), timeout=timeout)
        ports.append(p)
        return p
    return opener


def run(tmp_path, *argv, opener):
    return um982.main(["--port", "/dev/fake-com2", "--log-dir", str(tmp_path / "logs"), *argv], opener=opener)


def test_loader_text():
    assert um982.loader_heard(b"\xff\xffboot>")
    assert um982.loader_heard(b"  6 Soft reset cpu\r\n")
    assert um982.loader_heard(b"uart baudrate: 460800\r\n")
    assert not um982.loader_heard(b"$GNGGA,123519,4807.038,N*47\r\n#VERSIONA,79,GPS;...")
    assert not um982.loader_heard(b"")


def test_loader_in_the_listen_window_sends_nothing(tmp_path, capsys):
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--listen-first", "0.3", opener=opener_for(ports, {115200: LOADER}))
    assert code == um982.EXIT_LOADER == 3
    assert len(ports) == 1 and ports[0].closed
    assert ports[0].written == []   # not even the bare line end
    out = capsys.readouterr().out
    assert "loader was heard at 115200" in out and "Nothing was sent" in out and "power-cycle" in out
    assert "find-baud" not in out
    run_dir = next((tmp_path / "logs").glob("*-um982*"))
    assert "boot>" in (run_dir / "session-rx.bin").read_bytes().decode("latin-1")
    assert "loader was heard" in (run_dir / "summary.txt").read_text()


def test_send_is_refused_too_at_the_loader(tmp_path, capsys):
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "send", "VERSIONA", opener=opener_for(ports, {115200: LOADER}))
    assert code == um982.EXIT_LOADER
    assert ports[0].written == []


def test_no_versiona_answer_advises_listening_first(tmp_path, capsys, monkeypatch):
    monkeypatch.setattr(um982, "DAY_ONE", ["VERSIONA"])   # one query keeps the silent wait short
    ports: list[ScriptedPort] = []
    code = run(tmp_path, opener=opener_for(ports))
    assert code == um982.EXIT_OK
    sent = b"".join(ports[0].written)
    assert sent.replace(b"\r\n", b"") == b"VERSIONA" * 2   # the bare line end, then VERSIONA and its one retry
    out = capsys.readouterr().out
    assert "No answer to VERSIONA" in out
    assert "find-baud" not in out
    assert "capture.py --receiver /dev/fake-com2 --receiver-baud 460800 --duration 20" in out
    assert "never um982.py at that baud" in out and "still silent: stop" in out


def test_find_baud_stops_at_a_loader_heard_while_listening(capsys):
    ports: list[ScriptedPort] = []
    out = um982.Out()
    found = um982.find_baud("/dev/fake-com2", out, opener=opener_for(ports, {460800: LOADER}), listen_s=0.3)
    assert found == -460800
    # Listened at 115200, then heard the loader at 460800: no other baud opened, the ask phase never ran.
    assert [p.baudrate for p in ports] == [115200, 460800]
    assert all(p.written == [] for p in ports)
    assert any("loader" in line for line in out.lines)


def test_find_baud_asks_only_after_listening_everywhere(capsys):
    # A silent receiver: every baud listened to first, then VERSIONA at each (the search as it was).
    ports: list[ScriptedPort] = []
    out = um982.Out()
    found = um982.find_baud("/dev/fake-com2", out, opener=opener_for(ports), listen_s=0.05)
    assert found is None
    n = len(um982.FIND_BAUDS)
    assert len(ports) == 2 * n
    assert all(p.written == [] for p in ports[:n])
    assert all(p.written == [b"\r\nVERSIONA\r\n"] for p in ports[n:])


def test_main_with_find_baud_at_a_loader_exits_3(tmp_path, capsys, monkeypatch):
    real = um982.find_baud
    monkeypatch.setattr(um982, "find_baud", lambda dev, out, opener: real(dev, out, opener=opener, listen_s=0.2))
    ports: list[ScriptedPort] = []
    code = run(tmp_path, "--find-baud", opener=opener_for(ports, {115200: LOADER}))
    assert code == um982.EXIT_LOADER
    assert len(ports) == 1 and ports[0].written == []
    assert "loader was heard at 115200" in capsys.readouterr().out
