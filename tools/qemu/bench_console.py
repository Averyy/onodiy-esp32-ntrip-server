#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
# /// script
# requires-python = ">=3.10"
# dependencies = []
# ///
"""The bench console in QEMU: the bench build's command line answers on UART0, beside the log.

    uv run tools/qemu/bench_console.py [--out DIR] [--timeout S]

Needs the ESP-IDF environment and the bench overlay's QEMU build in
build-qemu-bench/ (tools/qemu/run.sh --overlay sdkconfig.bench
--build-only). Standard library only.

The command line reads the console UART through the driver the log tap
installs at boot, and the log keeps writing through that same driver. A
build cannot show whether both still work, so this runs the image:

  1. listens on a free local port and starts tools/qemu/run.sh --overlay
     sdkconfig.bench --headless with --console connect:<port> (UART1 not
     connected: a station without a receiver), so it sees every console
     byte from the first boot and can type on the console;
  2. waits for the bench console's start line ("bench console on the UART0
     console") and its prompt;
  3. types commands: `help` lists `help` and every command bench.c
     registers (read from its source; the receiver link's hooks only when
     the build has them); `ftimes status` answers "not running";
     `ftimes summary` prints the summary line in its documented form for a
     recording without a frame ("...; arrival to read: none; N frames
     without an interrupt time; longest reader gap ...") and the
     "interrupt times since boot" line after it; an unknown command
     answers "Unrecognized command";
  4. the log still reaches the console after the command line started (a
     log line at least LATER_MS after the start line);
  5. the web server answers; with the first admin password set, the web log
     (GET /api/logs?format=text) has the start line, and /api/health counts
     no console drop;
  6. replies stay whole while another task logs: `logflood` makes a task
     above the command line's log a line every FLOOD_MS ms for FLOOD_S s,
     and meanwhile `status`, `race_result`, `heapfrag free`, a second
     `logflood` (refused), `ftimes status`, `ftimes summary` (with the link
     hooks) and `help` are typed round after round. Every console line with
     BENCH in it must be one of the replies in its whole form (WHOLE), none
     with a log line's time and tag inside it; `help` still lists every
     command; every flood line arrives whole, once; flood lines did arrive
     while the replies were printed; and /api/health counts no console drop;
  7. throughout: no "UART driver already installed" or "uart driver error"
     line, no console note about dropped lines ("[diag:"), no panic and one
     boot only.

QEMU always stops: on a normal end, Ctrl-C, SIGTERM or SIGHUP, and run.sh
gets --seconds as a backstop in case this script is killed outright.

Everything lands in --out: the console log (console.log, written by QEMU),
what was typed and when (typed.log), the HTTP answers and summary.txt.
Exit status 0 when every check passed, 1 otherwise.
"""

from __future__ import annotations

import argparse
import http.client
import json
import os
import re
import secrets
import signal
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.dont_write_bytecode = True  # no __pycache__ next to smoke.py
sys.path.insert(0, str(Path(__file__).resolve().parent))

import smoke  # noqa: E402  (by path, above: its HTTP helper and console markers)

BUILD = ROOT / "build-qemu-bench"
BENCH_C = ROOT / "components" / "bench" / "bench.c"
START_LINE = "bench console on the UART0 console"
PROMPT = "bench> "
# What the console must never show: a second driver on the console UART, and
# the log writer's driver calls failing after one was deleted.
DRIVER_ERRORS = ("UART driver already installed", "uart driver error")
LATER_MS = 5000              # a log line this long after the start line shows the log still flows
# An ESP log line's time, anywhere in a console line: a log line can follow the
# prompt on the same line ("bench> W (1861) bench: ...").
STAMP = re.compile(r"[EWIDV] \((\d+)\) [A-Za-z0-9_.-]+: ")
BACKSTOP_MARGIN_S = 60
SUMMARY = re.compile(
    r"BENCH ftimes \d+ s: (\d+) records, (\d+) frames, \d+ passes with data, \d+ stalls"
    r"; read to published: none; arrival to read: none; (\d+) frames without an interrupt time"
    r"; longest reader gap with data waiting \d+ us")
SINCE_BOOT = re.compile(r"BENCH ftimes interrupt times since boot: \d+ frames with one, \d+ without, \d+ resyncs, "
                        r"\d+ mismatches, \d+ pairings dropped by a known cause")
# The log flood (`logflood`): log lines from a task above the command line's, while commands are typed.
FLOOD_S = 15
FLOOD_MS = 2
FLOOD_MARGIN_S = 5           # no round starts in the flood's last seconds (a round took under 1 s in QEMU on a Mac)
FLOOD_TEXT = "a log line from another task while the command line answers"
FLOOD_LINE = re.compile(r"I \(\d+\) logflood: line (\d+) of (\d+): " + re.escape(FLOOD_TEXT) + r"$", re.M)
FLOOD_DONE = re.compile(r"I \(\d+\) logflood: done: (\d+) lines")
# Every BENCH line the commands typed during the flood print, in its whole form (the whole console line).
WHOLE = [
    re.compile(r"BENCH logflood: \d+ log lines, one every \d+ ms, from another task in the background \(commands "
               r"keep working meanwhile; the last line says done\)"),
    re.compile(r"BENCH REFUSED: a log flood runs"),
    re.compile(r"BENCH state \d+ \([a-z_]+\), receiver .+, journal (?:none|open|unknown) \(state \d+ [a-z0-9-]+, "
               r"phase \d+ [a-z0-9-]+, \d+ transfer\(s\)\), flashing (?:built in|not built in)"),
    re.compile(r"BENCH progress: phase \d+ \([a-z_]+\), \d+/\d+ bytes, \d+/\d+ frames, \d+ retries; last: outcome "
               r"\d+ \([a-z_]+\), rule \d+, reason .+"),
    re.compile(r"BENCH no stop-key-from-boot run this boot"),
    re.compile(r"BENCH heapfrag free: \d+ block\(s\) released"),
    re.compile(r"BENCH heap now: internal free \d+ B, largest internal block \d+ B"),
    re.compile(r"BENCH ftimes status: not running, \d+ records"),
    # Without a frame: nothing after the reader gap but the clock's note when SNTP has not set the time.
    re.compile(SUMMARY.pattern + r"(?:; epoch delay: no clock \(the system time is not set by SNTP\))?"),
    SINCE_BOOT,
]
# Typed during the flood, each with what its reply ends with (the link hooks' commands only when built in).
FLOOD_COMMANDS = [
    ("status", "BENCH progress:", False),
    ("race_result", "BENCH no stop-key-from-boot", False),
    ("heapfrag free", "BENCH heap now:", False),
    (f"logflood {FLOOD_S} {FLOOD_MS}", "BENCH REFUSED", False),
    ("ftimes status", "BENCH ftimes status:", True),
    ("ftimes summary", "interrupt times since boot", True),
]


def split_bench_lines(text: str) -> tuple[list[str], list[str], list[str]]:
    """The console lines that carry BENCH; those with a log line inside (a log line's time and tag after the
    reply's start); those not in a whole form of WHOLE."""
    lines = [x.rstrip("\r") for x in text.split("\n") if "BENCH" in x]
    inside = [x for x in lines if STAMP.search(x)]
    cut = [x for x in lines if not any(w.fullmatch(x) for w in WHOLE)]
    return lines, inside, cut


def registered_commands(source: str, hooks: bool) -> list[str]:
    """The commands bench_start registers (reg("name", ...)), without the receiver link's hooks unless `hooks`."""
    body = source[source.index("esp_err_t bench_start(void)"):]
    if not hooks:
        body = re.sub(r"#if CONFIG_GNSS_LINK_BENCH_HOOKS\n.*?#endif", "", body, flags=re.DOTALL)
    return re.findall(r'\breg\("([A-Za-z0-9_]+)"', body)


def expected_commands(hooks: bool) -> list[str]:
    """What `help` must list: help itself and every command bench.c registers."""
    return list(dict.fromkeys(["help"] + registered_commands(BENCH_C.read_text(encoding="utf-8"), hooks)))


def listed_commands(reply: str) -> set[str]:
    """The command names `help` printed: each entry starts at the line start with the name and a space."""
    return {m.group(1) for m in re.finditer(r"(?m)^([A-Za-z0-9_]+) ", reply.replace("\r", ""))}


def sdkconfig_on(build: Path, name: str) -> bool:
    try:
        return f"#define {name} 1" in (build / "config" / "sdkconfig.h").read_text(encoding="utf-8")
    except FileNotFoundError:
        return False


class ConsoleTap:
    """The host end of UART0: everything the firmware prints, and what is typed."""

    def __init__(self, conn: socket.socket, out: Path, t0: float):
        self.conn = conn
        self.t0 = t0
        self.rx = bytearray()
        self.lock = threading.Lock()
        self.typed = open(out / "typed.log", "w", encoding="utf-8")
        self.thread = threading.Thread(target=self._read, name="console", daemon=True)
        self.thread.start()

    def _read(self) -> None:
        while True:
            try:
                data = self.conn.recv(4096)
            except OSError:
                break
            if not data:
                break
            with self.lock:
                self.rx += data

    def text(self) -> str:
        with self.lock:
            return self.rx.decode("utf-8", errors="replace")

    def size(self) -> int:
        with self.lock:
            return len(self.rx)

    def type(self, line: str) -> None:
        self.typed.write(f"{time.monotonic() - self.t0:9.3f} {line!r}\n")
        self.typed.flush()
        self.conn.sendall(line.encode() + b"\r")

    def close(self) -> None:
        try:
            self.conn.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        self.conn.close()
        self.thread.join(timeout=2)
        self.typed.close()


class BenchConsole:
    def __init__(self, args: argparse.Namespace):
        self.args = args
        self.out = Path(args.out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.console = self.out / "console.log"
        self.results: list[tuple[bool, str]] = []
        self.proc: subprocess.Popen | None = None
        self.tap: ConsoleTap | None = None
        self.cookie: str | None = None
        self.http_port = 0
        self.t0 = time.monotonic()

    def check(self, ok: bool, text: str) -> bool:
        self.results.append((ok, text))
        print(("PASS " if ok else "FAIL ") + text, flush=True)
        return ok

    def elapsed(self) -> float:
        return time.monotonic() - self.t0

    def alive(self) -> bool:
        return self.proc is not None and self.proc.poll() is None

    def start(self) -> bool:
        listener = socket.socket()
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]
        self.http_port = smoke.free_port()
        backstop = 3 * self.args.timeout + BACKSTOP_MARGIN_S
        cmd = [str(smoke.RUN_SH), "--no-build", "--overlay", "sdkconfig.bench", "--headless", str(self.console),
               "--console", f"connect:{port}", "--uart1", "none", "--http-port", str(self.http_port),
               "--seconds", str(backstop)]
        print("starting: " + " ".join(cmd), flush=True)
        self.console.unlink(missing_ok=True)
        self.t0 = time.monotonic()
        self.proc = subprocess.Popen(cmd, stdout=open(self.out / "run.log", "w"), stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL, start_new_session=True)
        listener.settimeout(1)
        conn = None
        try:
            while conn is None and self.elapsed() < 60 and self.alive():
                try:
                    conn, _ = listener.accept()
                except socket.timeout:
                    pass
        finally:
            listener.close()
        if not self.check(conn is not None, "QEMU connected the console (UART0) to the host"):
            return False
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.tap = ConsoleTap(conn, self.out, self.t0)
        return True

    def stop(self) -> None:
        if self.alive():
            try:
                os.killpg(self.proc.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                os.killpg(self.proc.pid, signal.SIGKILL)
                self.proc.wait(timeout=5)
        if self.tap is not None:
            self.tap.close()
            self.tap = None

    def wait_text(self, start: int, pred, seconds: float) -> str:
        """Console text from offset `start` once pred(text) holds, or what came within `seconds`."""
        deadline = time.monotonic() + seconds
        text = ""
        while time.monotonic() < deadline and self.alive():
            text = self.tap.text()[start:]
            if pred(text):
                return text
            time.sleep(0.1)
        return self.tap.text()[start:]

    def wait_start(self) -> bool:
        # The command line's task may print its prompt before or after the start line.
        text = self.wait_text(0, lambda t: START_LINE in t and PROMPT in t or "bench console not started" in t,
                              self.args.timeout)
        if "bench console not started" in text:
            line = next(x for x in text.splitlines() if "bench console not started" in x)
            return self.check(False, f"bench console: {line.strip()[:160]}")
        ok = START_LINE in text and PROMPT in text
        return self.check(ok, f"the bench console started and shows its prompt after {self.elapsed():.1f} s" if ok
                          else f"no bench console start line and prompt within {self.args.timeout} s")

    def command(self, line: str, pred, seconds: float = 20.0) -> str:
        """Type `line` at the prompt; the console text after it once pred(text) holds and the next prompt is
        there (the only prompt in that text: the one before it was printed before typing)."""
        start = self.tap.size()
        self.tap.type(line)
        return self.wait_text(start, lambda t: pred(t) and PROMPT in t, seconds)

    def command_checks(self) -> None:
        hooks = sdkconfig_on(BUILD, "CONFIG_GNSS_LINK_BENCH_HOOKS")
        expected = expected_commands(hooks)
        reply = self.command("help", lambda t: set(expected) <= listed_commands(t))
        (self.out / "help.txt").write_text(reply, encoding="utf-8")
        missing = [c for c in expected if c not in listed_commands(reply)]
        self.check(not missing, f"help lists all {len(expected)} commands" +
                   (f" (link hooks {'on' if hooks else 'off'})" if not missing else f"; missing: {', '.join(missing)}"))

        reply = self.command("ftimes status", lambda t: "BENCH ftimes status:" in t)
        self.check("BENCH ftimes status: not running, 0 records" in reply,
                   "ftimes status: " + (next((x.strip() for x in reply.splitlines() if "BENCH ftimes status" in x),
                                             "no answer")))

        reply = self.command("ftimes summary", lambda t: SINCE_BOOT.search(t) is not None)
        (self.out / "ftimes-summary.txt").write_text(reply, encoding="utf-8")
        m = SUMMARY.search(reply)
        self.check(m is not None and int(m.group(2)) == 0,
                   "ftimes summary without a frame: \"arrival to read: none\", then the frames without an interrupt "
                   f"time{f' ({m.group(3)})' if m else ' (line not in that form)'}, then the longest reader gap")
        self.check(SINCE_BOOT.search(reply) is not None and (m is None or SINCE_BOOT.search(reply).start() > m.end()),
                   "ftimes summary: the \"interrupt times since boot\" line follows the summary line")

        reply = self.command("nosuchcommand", lambda t: "Unrecognized command" in t)
        self.check("Unrecognized command" in reply, "an unknown command answers \"Unrecognized command\"")

    def flood_checks(self) -> None:
        """Replies stay whole while another task logs: a log flood from a task above the command line's, commands
        typed meanwhile, and every BENCH line on the console in its whole form, none with a log line inside."""
        hooks = sdkconfig_on(BUILD, "CONFIG_GNSS_LINK_BENCH_HOOKS")
        start = self.tap.size()
        reply = self.command(f"logflood {FLOOD_S} {FLOOD_MS}", lambda t: "BENCH " in t)
        if not self.check("BENCH logflood:" in reply, "logflood started: " + next(
                (x.strip()[:160] for x in reply.splitlines() if "BENCH" in x), "no answer")):
            return
        commands = [(c, end) for c, end, needs_hooks in FLOOD_COMMANDS if hooks or not needs_hooks]
        expected = expected_commands(hooks)
        windows: list[tuple[int, int]] = []
        unanswered: list[str] = []
        help_missing: set[str] = set()
        rounds = 0
        # Rounds start only while the flood surely runs (it must refuse a second one), then its end is awaited.
        until = time.monotonic() + FLOOD_S - FLOOD_MARGIN_S
        while time.monotonic() < until and self.alive() and not FLOOD_DONE.search(self.tap.text()[start:]):
            for line, end in commands + [("help", None)]:
                at = self.tap.size()
                if end is None:
                    got = self.command(line, lambda t: set(expected) <= listed_commands(t))
                    help_missing |= set(expected) - listed_commands(got)
                else:
                    got = self.command(line, lambda t, e=end: e in t)
                if PROMPT not in got:
                    unanswered.append(line)
                windows.append((at, self.tap.size()))
            rounds += 1
        text = self.wait_text(start, lambda t: FLOOD_DONE.search(t) is not None, FLOOD_S + 60)
        (self.out / "flood.txt").write_text(text, encoding="utf-8")
        done = FLOOD_DONE.search(text)
        total = int(done.group(1)) if done else None
        numbers = [int(m.group(1)) for m in FLOOD_LINE.finditer(text.replace("\r", ""))]
        self.check(done is not None and len(numbers) == total and set(numbers) == set(range(1, total + 1)),
                   f"the flood's log lines all reached the console whole, once each ({len(set(numbers))} of "
                   f"{total}, {len(numbers)} in all)")
        refused = text.count("BENCH REFUSED: a log flood runs")
        self.check(rounds > 0 and refused == rounds,
                   f"a second flood is refused while one runs ({refused} refusal(s) in {rounds} round(s))")
        full = self.tap.text()
        during = sum(len(FLOOD_LINE.findall(full[a:b].replace("\r", ""))) for a, b in windows)
        self.check(rounds > 0 and during > 0 and not unanswered,
                   f"{rounds} round(s) of {len(commands) + 1} commands typed during the flood, all answered, "
                   f"with {during} flood line(s) arriving while they were answered"
                   + (f"; no answer to: {', '.join(unanswered[:4])}" if unanswered else ""))
        lines, inside, cut = split_bench_lines(text)
        self.check(bool(lines) and not inside,
                   f"{len(lines)} BENCH line(s) during the flood, none with a log line inside"
                   + (f" ({len(inside)}: {inside[0][:160]!r})" if inside else ""))
        self.check(bool(lines) and not cut,
                   f"every BENCH line during the flood in its whole form"
                   + (f" ({len(cut)} not: {cut[0][:160]!r})" if cut else ""))
        self.check(not help_missing, "help during the flood lists every command"
                   + (f" (missing: {', '.join(sorted(help_missing))})" if help_missing else ""))
        if self.cookie:
            try:
                status, _, body = smoke.request(self.http_port, "GET", "/api/health", cookie=self.cookie, timeout=30)
                log = (json.loads(body).get("log") or {}) if status == 200 else {}
                self.check(status == 200 and log.get("console_dropped") == 0,
                           f"after the flood /api/health log.console_dropped {log.get('console_dropped')} of "
                           f"{log.get('lines')} lines")
            except (OSError, http.client.HTTPException, ValueError) as e:
                self.check(False, f"/api/health after the flood: {type(e).__name__}: {e}")

    def log_flows(self) -> None:
        """A log line well after the start line: the log writer still reaches the console."""
        text = self.tap.text()
        m = re.search(r"[EWIDV] \((\d+)\) bench: " + re.escape(START_LINE), text)
        if not self.check(m is not None, "the start line is an ESP log line with its time"):
            return
        t_start = int(m.group(1))

        def later(t: str) -> int | None:
            for s in STAMP.finditer(t):
                if int(s.group(1)) >= t_start + LATER_MS:
                    return int(s.group(1))
            return None

        text = self.wait_text(m.end(), lambda t: later(t) is not None, self.args.timeout)
        t_later = later(text)
        self.check(t_later is not None,
                   f"the log still reaches the console after the command line started (a line at {t_later} ms, "
                   f"start line at {t_start} ms)" if t_later is not None else
                   f"no log line on the console {LATER_MS} ms or more after the start line ({t_start} ms)")

    def web_checks(self) -> None:
        deadline = time.monotonic() + self.args.timeout
        status = None
        while time.monotonic() < deadline and self.alive():
            try:
                status = smoke.request(self.http_port, "GET", "/", timeout=10)[0]
                if status == 200:
                    break
            except (OSError, http.client.HTTPException):
                pass
            time.sleep(1)
        if not self.check(status == 200, f"GET / answers (HTTP {status})"):
            return
        try:
            password = "bench-" + secrets.token_hex(8)
            status, hdrs, _ = smoke.request(self.http_port, "POST", "/api/setup/admin", {"password": password},
                                            timeout=60)
            cookie = hdrs.get("set-cookie", "").split(";", 1)[0]
            if not self.check(status == 200 and cookie.startswith("sid="),
                              f"POST /api/setup/admin set the first password (HTTP {status})"):
                return
            self.cookie = cookie
            status, _, body = smoke.request(self.http_port, "GET", "/api/logs?format=text", cookie=cookie, timeout=30)
            (self.out / "api-logs.txt").write_bytes(body)
            log = body.decode("utf-8", errors="replace")
            bad = [x.strip()[:160] for x in log.splitlines() if any(e in x for e in DRIVER_ERRORS)]
            self.check(status == 200 and START_LINE in log and not bad,
                       f"web log (HTTP {status}): the start line {'is' if START_LINE in log else 'is NOT'} there, "
                       f"{len(bad)} console driver error line(s)" + (f" ({bad[0]})" if bad else ""))
            status, _, body = smoke.request(self.http_port, "GET", "/api/health", cookie=cookie, timeout=30)
            (self.out / "api-health.json").write_bytes(body)
            log = (json.loads(body).get("log") or {}) if status == 200 else {}
            self.check(status == 200 and log.get("console_dropped") == 0,
                       f"/api/health log.console_dropped {log.get('console_dropped')} of {log.get('lines')} lines")
        except (OSError, http.client.HTTPException, ValueError) as e:
            self.check(False, f"web API: {type(e).__name__}: {e}")

    def final_checks(self) -> None:
        alive = self.alive()
        text = self.tap.text() if self.tap is not None else ""
        self.stop()
        self.check(alive, f"QEMU still running after {self.elapsed():.0f} s")
        try:
            logged = self.console.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            logged = ""
        self.check(START_LINE in logged, "QEMU's console log (console.log) has what the firmware printed")
        for marker in DRIVER_ERRORS:
            line = next((x.strip()[:160] for x in text.splitlines() if marker in x), None)
            self.check(line is None, f"no \"{marker}\" on the console" + (f": {line}" if line else ""))
        notes = [x.strip()[:160] for x in text.splitlines() if smoke.CONSOLE_NOTE in x]
        self.check(not notes, f"no dropped console lines ({notes[0] if notes else 'no [diag: note'})")
        fatal = next((x.strip()[:160] for x in text.splitlines() if any(f in x for f in smoke.FATAL)), None)
        self.check(fatal is None, f"no panic on the console ({fatal or 'none'})")
        boots = text.count(smoke.BOOT_MARK)
        self.check(boots == 1, f"{boots} boot(s): no restart during the run")

    def run(self) -> int:
        try:
            if self.start():
                started = self.wait_start()
                if started:
                    self.command_checks()
                    self.log_flows()
                self.web_checks()   # also without a command line: the web log shows what the console could not
                if started:
                    self.flood_checks()   # after the web log check: the flood overwrites the log ring
            self.final_checks()
        finally:
            self.stop()
        failed = [t for ok, t in self.results if not ok]
        summary = "\n".join(("PASS " if ok else "FAIL ") + t for ok, t in self.results)
        (self.out / "summary.txt").write_text(summary + "\n", encoding="utf-8")
        print(f"\n{len(self.results) - len(failed)} passed, {len(failed)} failed; logs in {self.out}")
        return 1 if failed else 0


def _exit_on_signal(signum, frame) -> None:
    # SystemExit unwinds through BenchConsole.run's `finally`, which stops QEMU.
    raise SystemExit(128 + signum)


def main() -> int:
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, _exit_on_signal)
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--out", default=str(BUILD / "bench-console"), help="output directory")
    ap.add_argument("--timeout", type=int, default=150,
                    help="seconds for each wait: the start line, a later log line, the web server")
    args = ap.parse_args()
    if not os.environ.get("IDF_PATH"):
        print("bench_console.py: run . $IDF_PATH/export.sh first (tools/qemu/run.sh needs it)", file=sys.stderr)
        return 2
    if not sdkconfig_on(BUILD, "CONFIG_BENCH_CONSOLE"):
        print(f"bench_console.py: {BUILD.name}/ holds no bench build: "
              "tools/qemu/run.sh --overlay sdkconfig.bench --build-only", file=sys.stderr)
        return 2
    return BenchConsole(args).run()


if __name__ == "__main__":
    sys.exit(main())
