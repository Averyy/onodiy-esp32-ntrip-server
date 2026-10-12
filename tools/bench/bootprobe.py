#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Can the PC reach the receiver's boot menu over the receiver's own USB port (COM2)?

ACTIVE TOOL: it restarts the receiver. It refuses to run without
--i-understand and a typed "yes".

Before the station firmware ever drives the receiver's bootloader over COM1,
the PC must be able to reach that bootloader over COM2 (the board's
"UM982 UART2" USB-C port): that is the way back if a receiver firmware
update over COM1 goes wrong. This tool checks exactly that, and nothing more:

  1. VERSIONA at the application baud (read-only): the application answers;
     its firmware build is recorded. Nothing else is sent if it does not.
  2. The soft restart "\\r\\nreset\\r\\nreset\\r\\n", one write, at the
     application baud.
  3. The bootloader's stop key "T@", in bursts at --baud (460800 by default;
     the bootloader is expected to follow the baud of the stop key; 8 bytes
     per burst at 115200, 32 at 460800, 64 at 921600), each burst written
     once the previous one has left the port, until the prompt "boot>"
     arrives. Limits: 30 s after the restart; earlier, when data arrives 8 s
     or more after the restart without the prompt, or when the
     application's restart banner ($devicename) shows up instead.
  4. Nothing is sent while the bootloader's text (banner, settings, menu)
     comes in, until the port has been quiet for 200 ms. Then one "\\r\\n"
     clears any stop-key bytes left in the menu's input line; a fresh prompt
     is expected within 10 s (a bootloader that prints its menu only after
     a key press prints it now). All of it is saved verbatim: boot-menu.txt
     and loader-rx.bin.
  5. Menu item 6 ("6\\r\\n", soft reset cpu), which restarts the
     application; "resetting the cpu" is expected within 10 s. Sent once
     more only if the prompt comes back without it. Sent only when the
     printed menu (all of it received so far, the text after the "\\r\\n"
     included) numbers the soft reset as 6. A menu that names it on another
     number, or no readable line naming it at all (the menu arrived at
     another baud, or words it differently): the tool stops, sends nothing
     more and asks for a power-cycle.
  6. Back at the application baud: VERSIONA every 2 s until it answers (up
     to 60 s), and the build is compared with step 1. Without the restart
     message, the tool first listens (nothing sent) for the application's
     banner or NMEA; if nothing comes, it asks the bootloader once more
     ("\\r\\n", then "6\\r\\n" only at a fresh prompt).

Never sent: any other menu item (no download, no settings submenu), data
frames, erase or write commands, FRESET, SAVECONFIG. Every write passes the
allow-list in allowed_tx(); transcript.log records every byte in and out
with its UTC time and the port baud.

When the prompt does not come within the limits, the tool stops streaming,
says what to do, and checks that the application is back: it listens first,
then sends VERSIONA (the prompt was never seen, so the receiver is not
known to be at its menu); a prompt in the answer means the bootloader did
stop, and it gets "6\\r\\n" once.

Exit status: 0 prompt reached and the application is back with the same
build; 1 no prompt, the application is back; 2 the application did not come
back, or the menu could not be confirmed and the tool stopped at it
(power-cycle the receiver); 3 not started; 4 the application came back with
another firmware than before (the receiver did not come back as it was).

Examples:
  uv run bootprobe.py --port auto --i-understand
  uv run bootprobe.py --port /dev/cu.wchusbserial1120 --i-understand --baud 115200
"""

from __future__ import annotations

import argparse
import json
import re
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path

from benchlib.common import add_log_args, iso, make_run_dir, write_command_line
from benchlib.serialio import find_port, open_serial
from benchlib.unicore import UnicoreSession, parse_versiona

# The receiver side as documented for this module family. PROVISIONAL, none of
# it seen on this board yet (this tool is how it gets seen): that a soft
# restart over COM2 reaches the bootloader at all, that the bootloader
# follows the stop key's baud, the prompt and message texts, and the menu
# numbering (item 6 = soft reset cpu).
RESET_CMD = b"\r\nreset\r\nreset\r\n"
STOP_KEY = b"T@"
EXIT_CMD = b"6\r\n"
LINE_END = b"\r\n"
VERSION_QUERY = b"VERSIONA\r\n"
PROMPT = b"boot>"
RESETTING = b"resetting the cpu"
APP_BANNER = b"$devicename"
LAST_MENU_LINE = b"efuse from uart"
NMEA_STARTS = (b"$GPGGA", b"$GNGGA", b"$GPRMC", b"$GNRMC", b"$GPGSV", b"$GLGSV", b"$GAGSV", b"$GBGSV")
# Bootloader settings lines worth showing in the summary when they appear.
EVIDENCE = (b"uart baudrate:", b"Timeout:", b"Default action:")
BAUDS = (9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600)

EXIT_OK = 0           # prompt reached, application back with the same build
EXIT_NO_PROMPT = 1    # no prompt within the limits; application back
EXIT_APP_MISSING = 2  # the application did not come back (or the tool stopped at an unconfirmed menu): power-cycle
EXIT_REFUSED = 3      # not started (no confirmation, no port, no answer before the restart)
EXIT_OTHER_BUILD = 4  # the application came back with another firmware than before


def allowed_tx(data: bytes) -> bool:
    """The only byte strings this tool ever writes."""
    if data in (VERSION_QUERY, RESET_CMD, EXIT_CMD, LINE_END):
        return True
    return len(data) >= 2 and len(data) % 2 == 0 and data == STOP_KEY * (len(data) // 2)


def menu_item6(text: bytes) -> bool | None:
    """Does the printed menu list item 6 as the soft reset? None when no menu line names it at all.

    A menu that names the soft reset on a line without "6" is a different
    numbering: then "6" is never sent."""
    lines = [ln for ln in re.split(rb"[\r\n]+", text) if re.search(rb"soft\s*reset", ln, re.I)]
    if not lines:
        return None
    return any(re.search(rb"(?<![0-9])6(?![0-9])", ln) for ln in lines)


def burst_len(baud: int) -> int:
    """Stop-key burst: 8 bytes at 115200, 32 at 460800, 64 at 921600 (even: whole "T@" pairs)."""
    return max(2, 2 * round(baud / 28800))


class RefusedWrite(Exception):
    pass


class TracedPort:
    """A serial port whose every byte in and out goes to the transcript, with an allow-list on writes.

    Offers what UnicoreSession uses (in_waiting, read, write, flush), so the
    VERSIONA queries go through the same transcript. `rx` keeps every byte
    received, in order, for the marker searches."""

    def __init__(self, ser, transcript: Path):
        self.ser = ser
        self.fh = open(transcript, "a", encoding="utf-8")
        self.t0 = time.monotonic()
        self.lock = threading.Lock()
        self.rx = bytearray()
        self.tx_log: list[tuple[int, bytes]] = []

    def _line(self, direction: str, text: str) -> None:
        with self.lock:
            self.fh.write(f"{iso()} t+{time.monotonic() - self.t0:.3f}s {self.ser.baudrate:>7} {direction} {text}\n")
            self.fh.flush()

    def _bytes(self, direction: str, data: bytes) -> None:
        printable = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in data)
        self._line(direction, f"{len(data):5d} {data.hex(' ')} |{printable}|")

    def note(self, text: str) -> None:
        self._line("NOTE", text)

    @property
    def baudrate(self) -> int:
        return self.ser.baudrate

    @baudrate.setter
    def baudrate(self, baud: int) -> None:
        if baud != self.ser.baudrate:
            self._line("BAUD", f"{self.ser.baudrate} -> {baud}")
            self.ser.baudrate = baud

    @property
    def in_waiting(self) -> int:
        return self.ser.in_waiting

    def read(self, n: int = 1) -> bytes:
        data = self.ser.read(n)
        if data:
            with self.lock:
                self.rx += data
            self._bytes("RX", data)
        return data

    def write(self, data: bytes) -> int:
        data = bytes(data)
        if not allowed_tx(data):
            self._bytes("REFUSED", data)
            raise RefusedWrite(f"refusing to send {data!r}: not on this tool's allow-list")
        self._bytes("TX", data)
        self.tx_log.append((self.ser.baudrate, data))
        return self.ser.write(data)

    def flush(self) -> None:
        self.ser.flush()  # returns once the bytes have left the port

    def received(self, start: int = 0) -> bytes:
        with self.lock:
            return bytes(self.rx[start:])

    def mark(self) -> int:
        with self.lock:
            return len(self.rx)

    def close(self) -> None:
        with self.lock:
            self.fh.close()


@dataclass
class Limits:
    """Timings. PROVISIONAL: chosen from documented behaviour, to be checked against the bench capture."""

    entry_s: float = 30.0          # overall, from the restart write leaving the port
    late_data_s: float = 8.001     # data at or after this, still without the prompt: give up
    menu_quiet_s: float = 0.2      # after the prompt: collect until the port is quiet this long
    menu_max_s: float = 3.0
    prompt_wait_s: float = 10.0    # a fresh prompt after "\r\n"
    exit_wait_s: float = 10.0      # "resetting the cpu" after "6"
    app_listen_s: float = 20.0     # listen-only for the application's banner or NMEA
    app_version_s: float = 60.0    # VERSIONA retries
    version_every_s: float = 2.0
    burst_gap_s: float = 0.005


@dataclass
class Result:
    code: int = EXIT_REFUSED
    entry: str = ""                # prompt | app-banner | late-data | timeout
    prompt_after_s: float | None = None
    before: dict | None = None
    after: dict | None = None
    resetting_seen: bool = False
    exit_sends: int = 0
    evidence: list[str] = field(default_factory=list)


class BootProbe:
    def __init__(self, port: TracedPort, run_dir: Path, app_baud: int, boot_baud: int,
                 limits: Limits | None = None, echo=print):
        self.port = port
        self.run_dir = run_dir
        self.app_baud = app_baud
        self.boot_baud = boot_baud
        self.lim = limits or Limits()
        self.echo = echo
        self.reset_mark: int | None = None  # rx offset at the restart
        self.res = Result()

    # ---- low level

    def say(self, text: str) -> None:
        self.port.note(text)
        self.echo(text)

    def pump(self) -> bytes:
        n = self.port.in_waiting
        return self.port.read(n if n else 1)  # blocks for at most the port timeout when empty

    def drain(self) -> bytes:
        """What has already arrived, without waiting (the stop-key cadence must not stall on a read)."""
        n = self.port.in_waiting
        return self.port.read(n) if n else b""

    def send(self, data: bytes) -> None:
        self.port.write(data)
        self.port.flush()

    def wait_for(self, seconds: float, markers: tuple[bytes, ...], start: int) -> bytes | None:
        """Read for up to `seconds`; return the first of `markers` received after offset `start`."""
        end = time.monotonic() + seconds
        while True:
            got = self.port.received(start)
            for m in markers:
                if m in got:
                    return m
            if time.monotonic() >= end:
                return None
            self.pump()

    def loader_text(self) -> bytes:
        return self.port.received(self.reset_mark or 0)

    # ---- the application

    def versiona(self, tag: str, seconds: float, stop_at_prompt: bool = False) -> dict | None:
        """VERSIONA until a CRC-valid answer or `seconds` pass (benchlib's session, same transcript).

        stop_at_prompt: give up as soon as the bootloader's prompt shows up
        (never another VERSIONA at a menu)."""
        sub = self.run_dir / tag
        sub.mkdir(exist_ok=True)
        start = self.port.mark()
        sess = UnicoreSession(self.port, sub, echo=lambda s: None)
        try:
            end = time.monotonic() + seconds
            while True:
                t = time.monotonic()
                reply = sess.command("VERSIONA", ack_timeout=min(self.lim.version_every_s, 3.0), quiet=0.5,
                                     max_data=3.0)
                for item in reply.data:
                    if item.check == "crc32":
                        return parse_versiona(item.text)
                if stop_at_prompt and PROMPT in self.port.received(start):
                    return {"prompt": True}
                if time.monotonic() >= end:
                    return None
                time.sleep(max(0.0, self.lim.version_every_s - (time.monotonic() - t)))
        finally:
            sess.close()

    def app_heard(self, seconds: float) -> bool:
        """Listen only (nothing sent) at the application baud for its banner or an NMEA sentence."""
        self.port.baudrate = self.app_baud
        return self.wait_for(seconds, (APP_BANNER,) + NMEA_STARTS, self.port.mark()) is not None

    # ---- the steps

    def check_app_first(self) -> bool:
        self.port.baudrate = self.app_baud
        self.say(f"1. VERSIONA at {self.app_baud} (read-only)")
        b = self.versiona("app-before", 4.0)
        self.res.before = b
        if not b or "model" not in b:
            self.say(f"   no CRC-valid VERSIONA answer at {self.app_baud}: nothing else was sent.")
            self.say("   What to do: check the port and baud with `uv run um982.py --port <port>` "
                     "(or --find-baud); start this tool again once the receiver answers.")
            return False
        self.say(f"   {b['model']}, firmware {b['firmware']} (build {b['build']})")
        return True

    def enter(self) -> str:
        """Restart, then the stop key until the prompt. Returns how the entry ended."""
        while self.port.in_waiting:
            self.pump()  # whatever the application still had queued: logged, not used
        self.say(f"2. restart: {RESET_CMD!r} at {self.app_baud}")
        self.reset_mark = self.port.mark()
        self.send(RESET_CMD)
        t0 = time.monotonic()
        self.port.baudrate = self.boot_baud
        while self.port.in_waiting:
            self.pump()  # the application's reply to the restart: logged, not needed
        start = self.port.mark()
        burst = STOP_KEY * (burst_len(self.boot_baud) // 2)
        self.say(f"3. stop key {STOP_KEY!r} x{len(burst) // 2} per burst at {self.boot_baud}, "
                 f"until {PROMPT!r} (at most {self.lim.entry_s:g} s)")
        while True:
            if time.monotonic() - t0 >= self.lim.entry_s:
                return "timeout"
            self.send(burst)  # returns once the burst has left the port
            time.sleep(self.lim.burst_gap_s)
            data = self.drain()
            seen = self.port.received(start)
            if PROMPT in seen:
                self.res.prompt_after_s = time.monotonic() - t0
                return "prompt"
            if APP_BANNER in seen:
                return "app-banner"
            if data and time.monotonic() - t0 >= self.lim.late_data_s:
                return "late-data"

    def capture_menu(self) -> None:
        """After the prompt: send nothing, collect until the port has been quiet a moment."""
        end = time.monotonic() + self.lim.menu_max_s
        quiet_since = time.monotonic()
        while time.monotonic() < end and time.monotonic() - quiet_since < self.lim.menu_quiet_s:
            if self.pump():
                quiet_since = time.monotonic()
        text = self.loader_text()
        (self.run_dir / "boot-menu.txt").write_bytes(text)
        evidence = []  # from everything received so far (this runs again after the "\r\n")
        for marker in EVIDENCE:
            for line in text.splitlines():
                if marker in line:
                    evidence.append(line.decode("latin-1").strip())
        if LAST_MENU_LINE in text:
            evidence.append(f"last menu line present ({LAST_MENU_LINE.decode()})")
        self.res.evidence = evidence

    def leave(self) -> bool:
        """Clear the menu's input line, then menu item 6; wait for the CPU restart message.

        "6" goes out only when the menu as printed (everything received so
        far, the answer to the "\\r\\n" included) numbers the soft reset as 6.
        False otherwise: nothing more is sent."""
        mark = self.port.mark()
        self.say(f"4. {LINE_END!r} at {self.boot_baud}: clears stop-key bytes left in the menu's input line")
        self.send(LINE_END)
        if self.wait_for(self.lim.prompt_wait_s, (PROMPT,), mark):
            self.say(f"   fresh {PROMPT!r}")
            self.capture_menu()  # whatever the key brought (a menu printed only on a key press): saved too
        else:
            self.say(f"   no fresh {PROMPT!r} within {self.lim.prompt_wait_s:g} s")
        item6 = menu_item6(self.loader_text())
        if item6 is not True:
            self.say("   the printed menu " + ("numbers the soft reset differently" if item6 is False else
                                               "has no readable line naming the soft reset")
                     + f": {EXIT_CMD!r} is not sent, nothing more is (boot-menu.txt has what was received)")
            return False
        for attempt in (1, 2):
            mark = self.port.mark()
            self.say(f"5. menu item 6 {EXIT_CMD!r} at {self.boot_baud} (soft reset cpu)"
                     + (": again, the prompt came back without the restart message" if attempt == 2 else ""))
            self.send(EXIT_CMD)
            self.res.exit_sends += 1
            if self.wait_for(self.lim.exit_wait_s, (RESETTING,), mark):
                self.res.resetting_seen = True
                self.say(f"   {RESETTING.decode()!r} seen")
                return True
            if attempt == 1 and PROMPT in self.port.received(mark):
                continue
            self.say(f"   no {RESETTING.decode()!r} within {self.lim.exit_wait_s:g} s")
            return True
        return True

    def app_after_exit(self) -> dict | None:
        if self.res.resetting_seen:
            self.port.baudrate = self.app_baud
            self.say(f"6. VERSIONA at {self.app_baud} (the bootloader is restarting the CPU)")
            return self.versiona("app-after", self.lim.app_version_s)
        self.say(f"6. listening at {self.app_baud} for the application's banner or NMEA "
                 f"(up to {self.lim.app_listen_s:g} s, nothing sent)")
        if self.app_heard(self.lim.app_listen_s):
            self.say(f"   heard: VERSIONA at {self.app_baud}")
            return self.versiona("app-after", self.lim.app_version_s)
        # Still in the bootloader? Ask once at its baud; "6" only at a fresh prompt.
        self.port.baudrate = self.boot_baud
        mark = self.port.mark()
        self.say(f"   nothing heard: {LINE_END!r} at {self.boot_baud}, looking for {PROMPT!r}")
        self.send(LINE_END)
        if not self.wait_for(self.lim.prompt_wait_s, (PROMPT,), mark):
            return None
        mark = self.port.mark()
        self.say(f"   {PROMPT!r}: {EXIT_CMD!r} once more")
        self.send(EXIT_CMD)
        self.res.exit_sends += 1
        if not self.wait_for(self.lim.exit_wait_s, (RESETTING,), mark):
            return None
        self.res.resetting_seen = True
        self.port.baudrate = self.app_baud
        return self.versiona("app-after", self.lim.app_version_s)

    def app_after_no_prompt(self) -> dict | None:
        self.port.baudrate = self.app_baud
        if not (self.res.entry == "app-banner" and self.boot_baud == self.app_baud):
            self.say(f"6. listening at {self.app_baud} for the application's banner or NMEA "
                     f"(up to {self.lim.app_listen_s:g} s, nothing sent)")
            self.app_heard(self.lim.app_listen_s)
        # The prompt was never seen, so the receiver is not known to be at its menu: VERSIONA
        # may be sent; a prompt in the answer means the bootloader did stop after all.
        self.say(f"   VERSIONA at {self.app_baud}")
        v = self.versiona("app-after", self.lim.app_version_s, stop_at_prompt=True)
        if v is None or "prompt" not in v:
            return v
        mark = self.port.mark()
        self.say(f"   {PROMPT!r} in the answer at {self.app_baud}: the bootloader stopped there; "
                 f"{EXIT_CMD!r} once")
        self.send(EXIT_CMD)
        self.res.exit_sends += 1
        if not self.wait_for(self.lim.exit_wait_s, (RESETTING,), mark):
            return None
        self.res.resetting_seen = True
        return self.versiona("app-after-exit", self.lim.app_version_s)

    def run(self) -> Result:
        res = self.res
        if not self.check_app_first():
            res.code = EXIT_REFUSED
            return res
        try:
            res.entry = self.enter()
            if res.entry == "prompt":
                self.say(f"   {PROMPT!r} after {res.prompt_after_s:.2f} s; capturing the bootloader's text "
                         "(nothing sent)")
                self.capture_menu()
                if self.leave():
                    res.after = self.app_after_exit()
            else:
                reasons = {
                    "timeout": f"no {PROMPT!r} within {self.lim.entry_s:g} s of the restart",
                    "late-data": f"data arriving {self.lim.late_data_s:g} s or more after the restart "
                                 f"without {PROMPT!r}",
                    "app-banner": f"the application restarted ({APP_BANNER.decode()} seen) instead of stopping",
                }
                self.say(f"   stopped: {reasons[res.entry]}. No more stop keys are sent.")
                (self.run_dir / "boot-menu.txt").write_bytes(self.loader_text())
                res.after = self.app_after_no_prompt()
        finally:
            (self.run_dir / "loader-rx.bin").write_bytes(self.loader_text())
        if not res.after or "model" not in res.after:
            res.code = EXIT_APP_MISSING
            self.say("   the application did not answer.")
            self.say("   What to do: power-cycle the receiver (unplug the board: both USB-C cables), then run "
                     "`uv run um982.py --port <port>` to confirm it answers. If it does not, the receiver needs "
                     "the vendor's recovery; do not try any receiver update over COM1.")
            return res
        a = res.after
        same = res.before is not None and a.get("firmware") == res.before.get("firmware")
        self.say(f"   {a['model']}, firmware {a['firmware']} (build {a['build']})"
                 + ("" if same else f"; before it was {res.before.get('firmware') if res.before else '?'}"))
        if not same:
            res.code = EXIT_OTHER_BUILD
            self.say("   What to do: the receiver came back with another firmware than it had. Stop here: note "
                     "transcript.log and boot-menu.txt, check the receiver with `uv run um982.py --port <port>`, and do "
                     "not go on to the next check on the bench until it runs the build it had.")
            return res
        if res.entry == "prompt":
            res.code = EXIT_OK
        else:
            res.code = EXIT_NO_PROMPT
            self.say("   What to do: read transcript.log for bootloader text at another baud. Try once more with "
                     "--baud 115200 (the bootloader may follow the stop key's baud). If neither reaches the "
                     "prompt, the PC recovery channel over COM2 is NOT proven: do not update the receiver over "
                     "COM1 from the station.")
        return res


def describe(app_baud: int, boot_baud: int, port: str) -> str:
    return "\n".join([
        f"bootprobe sends, on {port}:",
        f"  1. VERSIONA at {app_baud} (read-only); nothing more if it gets no answer",
        f"  2. {RESET_CMD!r} at {app_baud}: the receiver restarts. Settings it has not saved are lost,",
        "     a survey-in starts over, and a running station sees the receiver vanish for a while",
        f"  3. {STOP_KEY!r} bursts at {boot_baud} for up to 30 s, until the boot menu prompt {PROMPT!r}",
        f"  4. {LINE_END!r} (clears the menu's input line), then {EXIT_CMD!r} (menu item 6, soft reset cpu)",
        "     only if the printed menu numbers the soft reset as 6: the application restarts",
        f"  5. VERSIONA at {app_baud} until the application answers; it must be the build it had",
        "It never selects any other menu item (no download, no settings), never sends data frames,",
        "erase or write commands, FRESET or SAVECONFIG. Use a receiver you can power-cycle by hand.",
    ])


def ask_tty(prompt: str) -> str:
    if not sys.stdin.isatty():
        return ""
    try:
        return input(prompt)
    except (EOFError, KeyboardInterrupt):
        return ""


def main(argv: list[str] | None = None, ask=ask_tty, opener=open_serial, limits: Limits | None = None) -> int:
    ap = argparse.ArgumentParser(
        description=__doc__.split("\n\n")[0],
        epilog=__doc__.split("\n\n", 1)[1],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--port", default="auto", help="UM982 COM2 port, or 'auto' (CH340 bridge by VID/PID; default)")
    ap.add_argument("--baud", type=int, default=460800, choices=BAUDS, metavar="BAUD",
                    help="baud for the stop key and the boot menu (default 460800)")
    ap.add_argument("--app-baud", type=int, default=115200, choices=BAUDS, metavar="BAUD",
                    help="the application's COM2 baud (default 115200, the factory rate)")
    ap.add_argument("--i-understand", action="store_true",
                    help="required: this restarts the receiver and stops it at its boot menu")
    add_log_args(ap)
    args = ap.parse_args(argv)

    text = describe(args.app_baud, args.baud, args.port)
    print(text)
    if not args.i_understand:
        print("\nNot started: add --i-understand to run it (you will still be asked to type yes).")
        return EXIT_REFUSED
    if ask("\nType 'yes' to start (anything else cancels): ").strip().lower() != "yes":
        print("Not confirmed: nothing sent.")
        return EXIT_REFUSED

    port_name = args.port
    if port_name == "auto":
        dev = find_port("um982-com2")
        if not dev:
            print("--port auto: no single CH340 (UM982 COM2) bridge found by VID/PID; run `uv run ports.py`")
            return EXIT_REFUSED
        port_name = dev

    run_dir = make_run_dir(args.log_dir, "bootprobe", args.label)
    write_command_line(run_dir)
    lines: list[str] = []

    def echo(s: str) -> None:
        lines.append(s)
        print(s, flush=True)

    echo(f"bootprobe on {port_name}: application at {args.app_baud}, boot menu at {args.baud}; log: {run_dir}")
    try:
        ser = opener(port_name, args.app_baud, timeout=0.05)
    except OSError as e:  # serial.SerialException is an OSError
        echo(f"cannot open {port_name}: {e}")
        return EXIT_REFUSED
    port = TracedPort(ser, run_dir / "transcript.log")
    probe = BootProbe(port, run_dir, args.app_baud, args.baud, limits=limits, echo=echo)
    try:
        res = probe.run()
    finally:
        port.close()
        ser.close()
    if res.evidence:
        echo("bootloader text of note: " + "; ".join(res.evidence))
    before = (res.before or {}).get("firmware", "?")
    after = (res.after or {}).get("firmware", "?")
    verdict = {EXIT_OK: "PASS: boot menu reached over this port, and the application is back with the same build",
               EXIT_NO_PROMPT: "FAIL: no boot menu (the application is back)",
               EXIT_APP_MISSING: "FAIL: the application did not come back",
               EXIT_OTHER_BUILD: f"FAIL: the application came back with another firmware: {after}, was {before}",
               EXIT_REFUSED: "not run"}[res.code]
    echo(verdict)
    (run_dir / "summary.txt").write_text("\n".join(lines) + "\n", encoding="utf-8")
    summary = {"port": port_name, "app_baud": args.app_baud, "boot_baud": args.baud, "code": res.code,
               "entry": res.entry, "prompt_after_s": res.prompt_after_s, "before": res.before, "after": res.after,
               "resetting_seen": res.resetting_seen, "exit_sends": res.exit_sends, "evidence": res.evidence,
               "sent": [{"baud": b, "bytes": d.decode("latin-1")} for b, d in port.tx_log]}
    (run_dir / "summary.json").write_text(json.dumps(summary, indent=2, default=str) + "\n", encoding="utf-8")
    echo(f"log: {run_dir}  (transcript.log: every byte in and out; boot-menu.txt: the bootloader's text)")
    return res.code


if __name__ == "__main__":
    sys.exit(main())
