# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""capture.py's log files reach the disk while it runs (no hardware).

The sinks are checked directly, and a whole capture runs against two
scripted ports while a second thread reads the files from the disk, as
`tail -f` would."""

from __future__ import annotations

import signal
import sys
import threading
import time

import pytest

import capture
from benchlib.common import FLUSH_EVERY_S, RotatingFile


def on_disk(path) -> bytes:
    """What another process sees of the file now."""
    with open(path, "rb") as f:
        return f.read()


def test_rotating_file_flush_pushes_what_was_written(tmp_path):
    f = RotatingFile(tmp_path, "x", ".log", 1 << 20, binary=False)
    f.write("one line\n")
    assert on_disk(f.path) == b""            # buffered: a small write alone does not reach the disk
    f.flush()
    assert on_disk(f.path) == b"one line\n"
    assert not f.dirty
    f.flush()                                # nothing new: nothing to do
    f.close()


def test_console_sink_flushes_on_an_event_and_on_request(tmp_path):
    sink = capture.ConsoleSink(tmp_path, 1 << 20, echo=False, raw=True, elf=None)
    t = time.time()
    sink.data(b"I (100) main: a quiet line\r\n", t)
    assert on_disk(tmp_path / "console-0000.log") == b""
    sink.flush()
    assert b"a quiet line" in on_disk(tmp_path / "console-0000.log")
    assert on_disk(tmp_path / "console-0000.bin") == b"I (100) main: a quiet line\r\n"
    # An event: the event line and the console lines before it are on the disk at once.
    sink.data(b"I (200) main: just before\r\nGuru Meditation Error: Core  0 panic'ed (LoadProhibited)\r\n", t)
    log = on_disk(tmp_path / "console-0000.log")
    assert b"just before" in log and b"Guru Meditation" in log
    assert b"[panic] Guru Meditation" in on_disk(tmp_path / "events.log")
    # An ESP-IDF error line counts as an event too (not written to events.log).
    sink.data(b"E (300) net: lost\r\n", t)
    assert b"E (300) net: lost" in on_disk(tmp_path / "console-0000.log")
    sink.close()


def test_receiver_sink_flush(tmp_path):
    sink = capture.ReceiverSink(tmp_path, 1 << 20, echo="none")
    line = b"$GNGGA,000001.00,,,,,0,00,9999.0,,,,,,*7A\r\n"
    sink.data(line, time.time())
    for name in ("receiver-0000.bin", "receiver-0000.idx", "receiver-0000.log"):
        assert on_disk(tmp_path / name) == b""
    sink.flush()
    assert on_disk(tmp_path / "receiver-0000.bin") == line
    assert on_disk(tmp_path / "receiver-0000.idx").endswith(f",0,{len(line)}\n".encode())
    assert b"GNGGA" in on_disk(tmp_path / "receiver-0000.log")
    # A restarting receiver's answer is named as such in the log.
    sink.data(b"The Board is resetting,do not response any command!*39\r\n", time.time())
    sink.flush()
    assert b"TXT[restarting] The Board is resetting" in on_disk(tmp_path / "receiver-0000.log")
    sink.close()


class TalkingPort:
    """A port that sends one numbered line every `every` seconds, and when."""

    def __init__(self, fmt: str, every: float = 0.05) -> None:
        self.fmt = fmt
        self.every = every
        self.start = time.monotonic()
        self.sent: list[float] = []   # monotonic time of each line handed out
        self.lock = threading.Lock()

    def _due(self) -> bytes:
        now = time.monotonic()
        out = b""
        with self.lock:
            while self.start + len(self.sent) * self.every <= now:
                out += self.fmt.format(n=len(self.sent)).encode()
                self.sent.append(now)
        return out

    @property
    def in_waiting(self) -> int:
        return 0

    def read(self, n: int = 1) -> bytes:
        data = self._due()
        if not data:
            time.sleep(0.01)
        return data

    def close(self) -> None:
        pass


@pytest.fixture
def keep_signal_handlers():
    saved = {s: signal.getsignal(s) for s in (signal.SIGINT, signal.SIGTERM)}
    yield
    for s, h in saved.items():
        signal.signal(s, h)


def test_a_running_capture_is_on_the_disk_within_about_a_second(tmp_path, monkeypatch, keep_signal_handlers):
    ports = {
        "/dev/fake-console": TalkingPort("I ({n}) test: console line {n}\r\n"),
        "/dev/fake-com2": TalkingPort("$GNGGA,{n:06d}.00,,,,,0,00,9999.0,,,,,,*7A\r\n"),
    }
    monkeypatch.setattr(capture, "open_serial", lambda device, baud, timeout=0.05: ports[device])
    monkeypatch.setattr(sys, "argv", ["capture.py", "--console", "/dev/fake-console", "--receiver", "/dev/fake-com2",
                                      "--duration", "3.5", "--status-every", "0", "--quiet", "--show", "none",
                                      "--log-dir", str(tmp_path)])
    seen: dict[str, tuple[int, int]] = {}

    def look() -> None:
        time.sleep(2.6)   # the capture still runs (no status lines: only the once-a-second flush)
        run = next(tmp_path.iterdir())
        checked = time.monotonic()
        for port, name in (("/dev/fake-console", "console-0000.log"), ("/dev/fake-com2", "receiver-0000.log")):
            lines = on_disk(run / name).count(b"\n")
            with ports[port].lock:
                old = sum(1 for t in ports[port].sent if t <= checked - FLUSH_EVERY_S - 0.5)
            seen[name] = (lines, old)

    watcher = threading.Thread(target=look)
    watcher.start()
    assert capture.main() == 0
    watcher.join()
    for name, (lines, old) in seen.items():
        # Everything older than the flush period (and the loop's 0.2 s tick, with room) was on the disk.
        assert old > 10 and lines >= old, f"{name}: {lines} line(s) on the disk, {old} sent more than 1.5 s before"
