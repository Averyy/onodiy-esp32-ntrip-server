# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The pure checks of tools/qemu/bench_console.py: which commands `help` must list (read from
components/bench/bench.c), how a `help` reply is read, the `ftimes summary` line's form for a
recording without a frame, and how the log flood check tells a whole reply line from one with a log
line inside it or cut short."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BENCH_C = ROOT / "components" / "bench" / "bench.c"


def _load():
    sys.path.insert(0, str(ROOT / "tools" / "qemu"))
    spec = importlib.util.spec_from_file_location("bench_console", ROOT / "tools" / "qemu" / "bench_console.py")
    mod = importlib.util.module_from_spec(spec)
    before = sys.dont_write_bytecode
    try:
        spec.loader.exec_module(mod)  # sets dont_write_bytecode for its own import (smoke.py)
    finally:
        sys.dont_write_bytecode = before
    return mod


bc = _load()


def test_commands_from_the_source_with_and_without_the_link_hooks():
    src = BENCH_C.read_text(encoding="utf-8")
    full = bc.registered_commands(src, hooks=True)
    plain = bc.registered_commands(src, hooks=False)
    for name in ("listen", "version", "flash", "status", "capture", "heapfrag", "linkrestart"):
        assert name in full and name in plain
    hooks = {"stall", "rawstall", "ftimes", "promptdet", "promptwatch", "uartclose"}
    assert hooks <= set(full)
    assert not hooks & set(plain)
    assert len(full) == len(set(full)), "a command registered twice"
    # Only bench_start's registrations: nothing before it counts.
    assert bc.registered_commands('reg("early", "x", f);\nesp_err_t bench_start(void)\n{\n    reg("late", "y", g);\n',
                                  hooks=True) == ["late"]


def test_help_reply_names_each_entry_at_the_line_start():
    reply = ("help\r\nhelp  [<string>] [-v <0|1>]\r\n  Print the summary of all registered commands if no arguments are\r\n"
             "  given.\r\n\r\nlisten \r\n  Listen only on the receiver's COM1: listen <baud> <seconds>\r\n\r\n"
             "bench> I (2014) network: online: 10.0.2.15\r\nftimes \r\n  Record frame and drain-pass times\r\n"
             "  in the background: ftimes status \r\n\r\nbench> ")
    names = bc.listed_commands(reply)
    assert {"help", "listen", "ftimes"} <= names
    # Help text and a log line after the prompt are no entries; a wrapped help line is indented.
    assert "Print" not in names and "in" not in names and "I" not in names


def test_summary_form_without_a_frame():
    line = ("BENCH ftimes 0 s: 0 records, 0 frames, 0 passes with data, 0 stalls; read to published: none; "
            "arrival to read: none; 0 frames without an interrupt time; longest reader gap with data waiting 0 us; "
            "epoch delay: no clock (the system time is not set by SNTP)")
    m = bc.SUMMARY.search(line)
    assert m and m.group(2) == "0" and m.group(3) == "0"
    # Every frame inexact: the count follows "none" (it is never left out).
    inexact = line.replace("0 records, 0 frames", "900 records, 812 frames").replace("0 frames without", "812 frames without")
    assert bc.SUMMARY.search(inexact).group(3) == "812"
    # "none" straight before the reader gap (the count missing) is not the firmware's form.
    assert bc.SUMMARY.search(line.replace("; 0 frames without an interrupt time", "")) is None
    assert bc.SINCE_BOOT.search("BENCH ftimes interrupt times since boot: 0 frames with one, 0 without, 1 resyncs, "
                                "0 mismatches, 0 pairings dropped by a known cause")


def test_help_reply_one_line_per_command():
    # The bench console's own help: one line per command, its name first.
    reply = ("help\r\nhelp         The commands, or one of them: help [command]\r\n"
             "listen       Listen only on the receiver's COM1: listen <baud> <seconds>\r\n"
             "bench> I (2014) network: online: 10.0.2.15\r\nlinkrestart  The ladder's link restart now\r\nbench> ")
    assert {"help", "listen", "linkrestart"} <= bc.listed_commands(reply)
    assert "I" not in bc.listed_commands(reply) and "The" not in bc.listed_commands(reply)
    assert bc.expected_commands(hooks=False)[0] == "help"
    assert len(bc.expected_commands(hooks=True)) == len(set(bc.expected_commands(hooks=True)))


STATUS = ("BENCH state 7 (no_receiver), receiver none, journal none (state 0 none, phase 0 none, 0 transfer(s)), "
          "flashing built in")
PROGRESS = ("BENCH progress: phase 0 (idle), 0/0 bytes, 0/0 frames, 0 retries; last: outcome 0 (none), rule 0, "
            "reason none")


def test_flood_reply_forms_are_whole():
    lines = [
        STATUS,
        STATUS.replace("receiver none", "receiver UM982 R4.10Build18975").replace("journal none (state 0 none",
                                                                                  "journal open (state 9 needs-user"),
        PROGRESS,
        "BENCH logflood: 7500 log lines, one every 2 ms, from another task in the background (commands keep working "
        "meanwhile; the last line says done)",
        "BENCH REFUSED: a log flood runs",
        "BENCH no stop-key-from-boot run this boot",
        "BENCH heapfrag free: 0 block(s) released",
        "BENCH heap now: internal free 123456 B, largest internal block 65536 B",
        "BENCH ftimes status: not running, 0 records",
        "BENCH ftimes 0 s: 0 records, 0 frames, 0 passes with data, 0 stalls; read to published: none; arrival to read: "
        "none; 0 frames without an interrupt time; longest reader gap with data waiting 0 us",
        "BENCH ftimes interrupt times since boot: 0 frames with one, 0 without, 0 resyncs, 0 mismatches, 0 pairings "
        "dropped by a known cause",
    ]
    text = "\r\n".join(lines + ["I (10692) logflood: line 57 of 7500: " + bc.FLOOD_TEXT, "bench> "]) + "\r\n"
    found, inside, cut = bc.split_bench_lines(text)
    assert len(found) == len(lines) and not inside and not cut


def test_a_log_line_inside_a_reply_is_found():
    flood = "I (10692) logflood: line 57 of 7500: " + bc.FLOOD_TEXT
    # As seen on the bench before the fix: the reply's first part, a log line, then the rest on the next line.
    split = STATUS[:60] + flood + "\r\n" + STATUS[60:] + "\r\n"
    found, inside, cut = bc.split_bench_lines(split)
    assert inside == [STATUS[:60] + flood] and cut == [STATUS[:60] + flood]
    # A reply cut short (its rest went elsewhere) is not in a whole form, though no log line is inside it.
    found, inside, cut = bc.split_bench_lines("BENCH ftimes status: not run\r\n")
    assert not inside and cut == ["BENCH ftimes status: not run"]
    # The flood's own lines are counted whole only when they end where the text ends.
    assert bc.FLOOD_LINE.search(flood + "\n") and not bc.FLOOD_LINE.search(flood[:-3] + "\n")
