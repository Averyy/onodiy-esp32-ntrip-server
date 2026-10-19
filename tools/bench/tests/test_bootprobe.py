# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""bootprobe.py against a scripted receiver over a socketpair (no hardware, no network).

The host side is a pyserial stand-in over one end of a socketpair; the other
end is a small receiver model. Every write carries the baud it was sent at,
so a side listening at another baud gets unreadable bytes, as on a real
line. The receiver's text is built from the documented strings: the command
acknowledgement and #VERSIONA formats, the restart banner, the boot menu's
item names, the "boot>" prompt and the "resetting the cpu" message. Their
exact layout on a real receiver is what bootprobe records on the bench.
"""

from __future__ import annotations

import select
import socket
import struct
import threading
import time

import pytest

import bootprobe
from benchlib.stream import crc32_unicore
from builders import unicore_line

LOADER_BAUDS = (115200, 460800, 921600)
MENU = [
    "0 Load OS", "1 Load FW only", "2 Download from uart to flash", "3 Download from uart and run",
    "4 Set boot config", "5 Menu cmd", "6 Soft reset cpu", "7 Dump efuse", "8 Update factory image",
    "9 Download from spi to flash", "10 Program efuse from uart",
]
FAST = bootprobe.Limits(listen_s=0.3, entry_s=3.0, late_data_s=0.8, menu_quiet_s=0.1, menu_max_s=1.0,
                        prompt_wait_s=1.0, exit_wait_s=1.0, app_listen_s=0.6, app_version_s=4.0, version_every_s=0.3,
                        burst_gap_s=0.005)


def _send_frame(sock: socket.socket, baud: int, data: bytes) -> None:
    sock.sendall(struct.pack(">IH", baud, len(data)) + data)


class _Frames:
    """Reads (baud, bytes) frames from a socket."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = bytearray()

    def poll(self, wait: float) -> list[tuple[int, bytes]]:
        r, _, _ = select.select([self.sock], [], [], wait)
        if r:
            try:
                data = self.sock.recv(65536)
            except OSError:
                data = b""
            if not data:
                raise EOFError
            self.buf += data
        out = []
        while len(self.buf) >= 6:
            baud, n = struct.unpack(">IH", self.buf[:6])
            if len(self.buf) < 6 + n:
                break
            out.append((baud, bytes(self.buf[6:6 + n])))
            del self.buf[:6 + n]
        return out


class SocketSerial:
    """The host end: the part of pyserial the tools use."""

    def __init__(self, sock: socket.socket, baud: int, timeout: float):
        self.sock = sock
        self.frames = _Frames(sock)
        self.baudrate = baud
        self.timeout = timeout
        self.rx = bytearray()
        self.closed = False

    def _take(self, wait: float) -> None:
        try:
            for baud, data in self.frames.poll(wait):
                # Sent at another baud: what arrives is unreadable.
                self.rx += data if baud == self.baudrate else b"\xff" * len(data)
        except EOFError:
            pass

    @property
    def in_waiting(self) -> int:
        self._take(0)
        return len(self.rx)

    def read(self, n: int = 1) -> bytes:
        if not self.rx:
            self._take(self.timeout)
        out = bytes(self.rx[:n])
        del self.rx[:n]
        return out

    def write(self, data: bytes) -> int:
        _send_frame(self.sock, self.baudrate, bytes(data))
        return len(data)

    def flush(self) -> None:
        pass

    def close(self) -> None:
        self.closed = True


def versiona_line(build: int) -> str:
    """A made-up version log in the manual's layout (made-up P/N, serial, chip ID and date)."""
    body = (f'"UM982","R4.10Build{build}","HRPT00-S10C-P","1234567890123-XX00A0000000001",'
            f'"0011223344556677","2025/01/02"')
    line = f"#VERSIONA,80,GPS,FINE,2400,100000000,{build},0,18,12;{body}"
    return f"{line}*{crc32_unicore(line[1:].encode()):08x}"


class Receiver(threading.Thread):
    """A receiver on COM2: application at `app_baud`; after a restart, a bootloader that stops at
    the stop key (at a baud it can follow) within `window` seconds, else the application restarts."""

    def __init__(self, sock: socket.socket, app_baud: int = 115200, build: int = 18975, window: float = 1.0,
                 stops: bool = True, answers: bool = True, chatter: bool = False, menu: list[str] = MENU,
                 menu_unreadable: bool = False, menu_on_key: bool = False, build_after: int | None = None,
                 at_loader: bool = False):
        super().__init__(daemon=True)
        self.sock = sock
        self.frames = _Frames(sock)
        self.app_baud = app_baud
        self.build = build
        self.window = window
        self.stops = stops
        self.answers = answers
        self.chatter = chatter          # unreadable bytes after the restart, never a prompt
        self.menu = menu
        self.menu_unreadable = menu_unreadable  # banner and menu at another baud; only the prompt is readable
        self.menu_on_key = menu_on_key          # the menu is printed only after a key press (an empty line)
        self.menu_shown = False
        self.build_after = build_after          # the application that comes back after "6" (None: the same)
        self.state = "app"
        self.loader_baud = None
        self.restart_at = 0.0
        self.line = bytearray()
        self.app_lines: list[str] = []
        self.loader_input = bytearray()
        self.loader_lines: list[bytes] = []
        self.received: list[tuple[str, int, bytes]] = []
        self.done = threading.Event()
        # Already at its loader (a failed or interrupted update): it shows its prompt again and again at 460800.
        self.at_loader = at_loader
        if at_loader:
            self.state = "loader"
            self.loader_baud = 460800

    def say(self, baud: int, text: str | bytes) -> None:
        _send_frame(self.sock, baud, text.encode("latin-1") if isinstance(text, str) else text)

    def app_restarted(self) -> None:
        self.state = "app"
        self.line.clear()
        self.say(self.app_baud, unicore_line("devicename,COM2") + "\r\n")

    def on_app_line(self, text: str) -> None:
        self.app_lines.append(text)
        cmd = text.strip().upper()
        if cmd == "VERSIONA" and self.answers:
            self.say(self.app_baud, unicore_line("command,VERSIONA,response: OK") + "\r\n")
            self.say(self.app_baud, versiona_line(self.build) + "\r\n")
        elif cmd == "RESET" and self.state == "app":
            self.say(self.app_baud, unicore_line("command,reset,response: OK") + "\r\n")
            self.state = "restarting"
            self.restart_at = time.monotonic()

    def on_loader_bytes(self, data: bytes) -> None:
        for b in data:
            if b in (0x0D, 0x0A):
                if b == 0x0A and not self.line:
                    continue
                line = bytes(self.line)
                self.line.clear()
                self.loader_lines.append(line)
                if line == b"6":
                    self.say(self.loader_baud, "resetting the cpu...\r\n")
                    time.sleep(0.05)
                    if self.build_after is not None:
                        self.build = self.build_after
                    self.app_restarted()
                    return
                if not line and self.menu_on_key and not self.menu_shown:
                    self.menu_shown = True
                    self.say(self.loader_baud, "\r\n" + "".join(f"  {m}\r\n" for m in self.menu) + "boot>")
                    continue
                self.say(self.loader_baud, ("\r\n" if not line else "Invalid input\r\n") + "boot>")
            else:
                self.line.append(b)

    def run(self) -> None:
        last_prompt = 0.0
        try:
            while not self.done.is_set():
                if self.at_loader and self.state == "loader" and time.monotonic() - last_prompt > 0.15:
                    last_prompt = time.monotonic()
                    self.say(self.loader_baud, "\r\n  6 Soft reset cpu\r\nboot>")
                if self.state == "restarting" and time.monotonic() - self.restart_at > self.window:
                    self.app_restarted()
                if self.state == "restarting" and self.chatter:
                    self.say(921600, b"\x55\xaa")  # never readable at the host's bauds here
                for baud, data in self.frames.poll(0.02):
                    self.received.append((self.state, baud, data))
                    if self.state == "app":
                        if baud != self.app_baud:
                            continue  # unreadable at this baud
                        for b in data:
                            if b in (0x0D, 0x0A):
                                if self.line:
                                    self.on_app_line(self.line.decode("latin-1"))
                                self.line.clear()
                            else:
                                self.line.append(b)
                    elif self.state == "restarting":
                        if self.stops and baud in LOADER_BAUDS and b"T@" in data:
                            self.state = "loader"
                            self.loader_baud = baud
                            self.line.clear()
                            banner = "\r\nuart baudrate: 460800\r\nTimeout: 2s\r\nDefault action: Load OS\r\n"
                            menu = "".join(f"  {m}\r\n" for m in self.menu)
                            if self.menu_unreadable:
                                # Printed before the bootloader followed the stop key's baud.
                                self.say(921600 if baud != 921600 else 115200, banner + menu)
                                self.say(baud, "boot>")
                            elif self.menu_on_key:
                                self.say(baud, banner + "boot>")
                            else:
                                self.say(baud, banner + menu + "boot>")
                    elif self.state == "loader":
                        if baud == self.loader_baud:
                            self.loader_input += data
                            self.on_loader_bytes(data)
        except (EOFError, OSError):
            pass


@pytest.fixture
def rig(tmp_path, monkeypatch):
    made = {}

    def build(**kw):
        host, dev = socket.socketpair()
        rx = Receiver(dev, **kw)
        rx.start()
        made["rx"] = rx
        made["sockets"] = (host, dev)

        def opener(port, baud, timeout=0.05):
            made["ser"] = SocketSerial(host, baud, timeout)
            return made["ser"]

        return rx, opener

    yield build, made
    if "rx" in made:
        made["rx"].done.set()
        for s in made["sockets"]:
            s.close()


def run_probe(tmp_path, opener, *extra, answer="yes"):
    argv = ["--port", "/dev/fake-com2", "--i-understand", "--log-dir", str(tmp_path / "logs"), *extra]
    code = bootprobe.main(argv, ask=lambda prompt: answer, opener=opener, limits=FAST)
    run_dirs = list((tmp_path / "logs").glob("*-bootprobe*"))
    return code, (run_dirs[0] if run_dirs else None)


def assert_only_allowed(rx: Receiver):
    sent = b"".join(d for _, _, d in rx.received)
    for bad in (b"2\r", b"SAVECONFIG", b"FRESET", b"\x02", b"\x04", b"q\r\n"):
        assert bad not in sent, bad
    # At the menu, the only lines ever entered: the stop-key leftovers (cleared), empty, and "6".
    for line in rx.loader_lines:
        assert line in (b"", b"6") or set(line) <= set(b"T@"), line


def test_allow_list():
    for ok in (bootprobe.RESET_CMD, bootprobe.EXIT_CMD, b"\r\n", b"VERSIONA\r\n", b"T@" * 16, b"T@"):
        assert bootprobe.allowed_tx(ok), ok
    for bad in (b"2\r\n", b"\r\n2\r\n", b"q\r\n", b"SAVECONFIG\r\n", b"FRESET\r\n", b"T@2", b"T", b"@T",
                b"\x02\x01\xfe", b"\x04", b"6\r\n6\r\n6\r\n", b"reset\r\n", b"CONFIG COM2 460800\r\n", b""):
        assert not bootprobe.allowed_tx(bad), bad


def test_burst_lengths():
    assert [bootprobe.burst_len(b) for b in (115200, 230400, 460800, 921600)] == [8, 16, 32, 64]


def test_traced_port_refuses_and_logs(tmp_path):
    host, dev = socket.socketpair()
    try:
        port = bootprobe.TracedPort(SocketSerial(host, 115200, 0.05), tmp_path / "t.log")
        port.write(b"VERSIONA\r\n")
        with pytest.raises(bootprobe.RefusedWrite):
            port.write(b"\r\n2\r\n")
        port.close()
        log = (tmp_path / "t.log").read_text()
        assert " 115200 TX " in log and "|VERSIONA..|" in log and " REFUSED " in log
        assert [d for _, d in port.tx_log] == [b"VERSIONA\r\n"]
    finally:
        host.close()
        dev.close()


def test_refuses_without_flag(tmp_path):
    opened = []
    code = bootprobe.main(["--port", "/dev/fake", "--log-dir", str(tmp_path)], ask=lambda p: "yes",
                          opener=lambda *a, **k: opened.append(a))
    assert code == bootprobe.EXIT_REFUSED and not opened and not list(tmp_path.iterdir())


def test_refuses_without_typed_yes(tmp_path):
    opened = []
    for answer in ("", "y", "no", "YES please"):
        code = bootprobe.main(["--port", "/dev/fake", "--i-understand", "--log-dir", str(tmp_path)],
                              ask=lambda p, a=answer: a, opener=lambda *a, **k: opened.append(a))
        assert code == bootprobe.EXIT_REFUSED
    assert not opened


def test_menu_reached_and_application_back(tmp_path, rig):
    build, made = rig
    rx, opener = build(window=1.0)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_OK
    assert rx.state == "app" and rx.loader_baud == 460800
    menu = (run_dir / "boot-menu.txt").read_bytes()
    assert b"boot>" in menu and b"6 Soft reset cpu" in menu and b"10 Program efuse from uart" in menu
    assert b"Timeout: 2s" in (run_dir / "loader-rx.bin").read_bytes()
    summary = (run_dir / "summary.txt").read_text()
    assert "PASS" in summary and "Timeout: 2s" in summary
    sent = made["ser"]
    log = (run_dir / "transcript.log").read_text()
    assert " BAUD 115200 -> 460800" in log and " BAUD 460800 -> 115200" in log
    tx = [line for line in log.splitlines() if " TX " in line]
    assert tx[0].split()[2] == "115200" and "|VERSIONA..|" in tx[0]
    assert "|..reset..reset..|" in tx[1] and tx[1].split()[2] == "115200"
    assert all(" 460800 TX    32 " in line for line in tx[2:] if "T@T@" in line)
    assert any("|6..|" in line and " 460800 " in line for line in tx)
    assert "|VERSIONA..|" in tx[-1] and tx[-1].split()[2] == "115200"
    assert rx.loader_lines[-1] == b"6" and rx.loader_lines.count(b"6") == 1
    assert_only_allowed(rx)
    assert sent.closed


def test_menu_at_115200(tmp_path, rig):
    build, _ = rig
    rx, opener = build(window=1.0)
    code, run_dir = run_probe(tmp_path, opener, "--baud", "115200")
    assert code == bootprobe.EXIT_OK and rx.loader_baud == 115200
    assert all(" 115200 TX     8 " in line for line in (run_dir / "transcript.log").read_text().splitlines()
               if "T@T@" in line)
    assert_only_allowed(rx)


def test_no_menu_application_restarts(tmp_path, rig):
    build, _ = rig
    rx, opener = build(stops=False, window=0.3)
    t = time.monotonic()
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_NO_PROMPT
    assert time.monotonic() - t < 20
    summary = (run_dir / "summary.txt").read_text()
    assert "stopped:" in summary and "NOT proven" in summary and "FAIL: no boot menu" in summary
    assert rx.state == "app" and rx.loader_baud is None
    assert not rx.loader_lines and b"6\r\n" not in b"".join(d for _, _, d in rx.received)
    assert_only_allowed(rx)


def test_no_menu_application_banner_at_same_baud(tmp_path, rig):
    build, _ = rig
    rx, opener = build(stops=False, window=0.3, app_baud=115200)
    code, run_dir = run_probe(tmp_path, opener, "--baud", "115200")
    assert code == bootprobe.EXIT_NO_PROMPT
    assert "$devicename seen" in (run_dir / "summary.txt").read_text()


def test_unreadable_data_stops_early(tmp_path, rig):
    build, _ = rig
    rx, opener = build(stops=False, window=60, chatter=True)
    t = time.monotonic()
    code, run_dir = run_probe(tmp_path, opener)
    took = time.monotonic() - t
    assert "data arriving" in (run_dir / "summary.txt").read_text()
    assert code == bootprobe.EXIT_APP_MISSING  # this receiver never comes back by itself
    assert took < 15
    assert "power-cycle the receiver" in (run_dir / "summary.txt").read_text()
    assert_only_allowed(rx)


def test_silent_receiver_gets_only_versiona(tmp_path, rig):
    build, _ = rig
    rx, opener = build(answers=False)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_REFUSED
    sent = b"".join(d for _, _, d in rx.received)
    assert sent and set(sent.split(b"\r\n")) <= {b"VERSIONA", b""}
    assert "nothing else was sent" in (run_dir / "summary.txt").read_text()


def test_menu_item6():
    assert bootprobe.menu_item6(b"  5 Menu cmd\r\n  6 Soft reset cpu\r\n") is True
    assert bootprobe.menu_item6(b"[6] soft reset CPU\r\nboot>") is True
    assert bootprobe.menu_item6(b"  7 Soft reset cpu\r\n  6 Dump efuse\r\n") is False
    assert bootprobe.menu_item6(b"  16 Soft reset cpu\r\n") is False
    assert bootprobe.menu_item6(b"\xff\xffboot>") is None


def test_unreadable_menu_gets_no_6(tmp_path, rig):
    # The prompt is readable but the menu above it is not (it came at another baud): item 6 is not confirmed.
    build, _ = rig
    rx, opener = build(window=1.0, menu_unreadable=True)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_APP_MISSING
    assert rx.state == "loader" and b"6" not in rx.loader_lines
    assert not any(d == b"6\r\n" for _, _, d in rx.received)
    summary = (run_dir / "summary.txt").read_text()
    assert "no readable line naming the soft reset" in summary and "power-cycle the receiver" in summary
    assert_only_allowed(rx)


def test_reworded_menu_gets_no_6(tmp_path, rig):
    # Another bootloader version: item 6 is something else and the restart is worded differently.
    build, _ = rig
    other = [m.replace("6 Soft reset cpu", "6 Update factory image").replace("8 Update factory image", "8 Restart cpu")
             for m in MENU]
    rx, opener = build(window=1.0, menu=other)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_APP_MISSING
    assert b"6" not in rx.loader_lines
    assert "no readable line naming the soft reset" in (run_dir / "summary.txt").read_text()
    assert_only_allowed(rx)


def test_menu_printed_on_key_press(tmp_path, rig):
    # Only "boot>" at the stop; the menu comes with the answer to the "\r\n": checked then, 6 sent once.
    build, _ = rig
    rx, opener = build(window=1.0, menu_on_key=True)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_OK
    assert rx.loader_lines.count(b"6") == 1 and rx.state == "app"
    assert b"6 Soft reset cpu" in (run_dir / "boot-menu.txt").read_bytes()
    assert_only_allowed(rx)


def test_application_back_with_another_build_fails(tmp_path, rig):
    build, _ = rig
    rx, opener = build(window=1.0, build_after=17000)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_OTHER_BUILD
    summary = (run_dir / "summary.txt").read_text()
    assert "FAIL: the application came back with another firmware: R4.10Build17000, was R4.10Build18975" in summary
    assert "PASS" not in summary
    assert_only_allowed(rx)


def test_stop_key_bursts_follow_each_other_closely(tmp_path, rig):
    # Each burst once the previous one has left the port, plus about 5 ms: a read must never hold one back.
    build, _ = rig
    rx, opener = build(stops=False, window=1.5)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_NO_PROMPT
    times = []
    for line in (run_dir / "transcript.log").read_text().splitlines():
        if " TX " in line and "|T@T@" in line:
            times.append(float(line.split()[1][2:-1]))  # "t+12.345s"
    assert len(times) > 20
    gaps = sorted(b - a for a, b in zip(times, times[1:]))
    assert gaps[len(gaps) // 2] < 0.010, gaps[len(gaps) // 2]


def test_other_numbering_gets_no_6(tmp_path, rig):
    build, _ = rig
    shifted = [m.replace("6 Soft reset cpu", "7 Soft reset cpu").replace("7 Dump efuse", "6 Dump efuse") for m in MENU]
    rx, opener = build(window=1.0, menu=shifted)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_APP_MISSING
    assert rx.state == "loader" and b"6" not in rx.loader_lines
    assert not any(d == b"6\r\n" for _, _, d in rx.received)
    summary = (run_dir / "summary.txt").read_text()
    assert "numbers the soft reset differently" in summary and "power-cycle the receiver" in summary


def test_receiver_already_at_its_loader_gets_nothing(tmp_path, rig):
    # A failed or interrupted update left the receiver at its loader: the listen window hears it, nothing is sent.
    build, made = rig
    rx, opener = build(at_loader=True)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_REFUSED
    assert not rx.received, rx.received   # not one byte written
    assert made["ser"].closed
    summary = (run_dir / "summary.txt").read_text()
    assert "loader is heard at 460800" in summary and "Nothing was sent" in summary
    assert "power-cycle the receiver" in summary and "not run: the receiver sits at its loader" in summary
    tx = [line for line in (run_dir / "transcript.log").read_text().splitlines() if " TX " in line]
    assert not tx


def test_loader_heard_at_the_application_baud_too(tmp_path, rig):
    # --baud 115200: one listen window at that baud, the loader heard there.
    build, _ = rig
    rx, opener = build(at_loader=True)
    rx.loader_baud = 115200
    code, run_dir = run_probe(tmp_path, opener, "--baud", "115200")
    assert code == bootprobe.EXIT_REFUSED and not rx.received
    assert "loader is heard at 115200" in (run_dir / "summary.txt").read_text()


def test_failure_advice_listens_first_and_never_names_the_baud_search(tmp_path, rig):
    build, _ = rig
    # No VERSIONA answer: nothing else sent, the safe sequence.
    rx, opener = build(answers=False)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_REFUSED
    summary = (run_dir / "summary.txt").read_text()
    assert "--find-baud" not in summary and "find-baud" not in summary
    assert "capture.py --receiver /dev/fake-com2 --receiver-baud 460800 --duration 20" in summary
    assert "same with --receiver-baud 115200" in summary
    assert "run neither um982.py nor bootprobe.py until the application is heard" in summary
    assert "power-cycle" in summary and "sends 6" not in summary


def test_application_missing_advice_listens_first(tmp_path, rig):
    # The menu cannot be confirmed: stopped at the loader, the application missing: power-cycle, then listen first.
    build, _ = rig
    rx, opener = build(window=1.0, menu_unreadable=True)
    code, run_dir = run_probe(tmp_path, opener)
    assert code == bootprobe.EXIT_APP_MISSING
    summary = (run_dir / "summary.txt").read_text()
    assert "find-baud" not in summary
    assert "First power-cycle the receiver" in summary and "listen only: `uv run capture.py" in summary
