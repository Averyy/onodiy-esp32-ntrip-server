# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The pure checks of tools/qemu/bench_console.py: which commands `help` must list (read from
components/bench/bench.c), how a `help` reply is read, and the `ftimes summary` line's form for a
recording without a frame."""

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
