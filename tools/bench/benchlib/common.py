# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Log directories, timestamps, rotating files and Ctrl-C handling."""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import re
import signal
import sys
import threading
from pathlib import Path

DEFAULT_LOG_BASE = "bench-logs"


def utc_now() -> _dt.datetime:
    return _dt.datetime.now(_dt.timezone.utc)


def iso(t: float | None = None) -> str:
    """UTC ISO-8601 with microseconds, e.g. 2026-10-10T14:05:01.123456Z."""
    d = utc_now() if t is None else _dt.datetime.fromtimestamp(t, _dt.timezone.utc)
    return d.strftime("%Y-%m-%dT%H:%M:%S.%fZ")


def hms(t: float) -> str:
    """Local wall-clock time for console output."""
    return _dt.datetime.fromtimestamp(t).strftime("%H:%M:%S.%f")[:-3]


def add_log_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--log-dir",
        default=os.environ.get("BENCH_LOG_DIR", DEFAULT_LOG_BASE),
        help="base directory for run logs (default: $BENCH_LOG_DIR or ./bench-logs); "
        "each run writes to a new <base>/<timestamp>-<tool>[-<label>]/ directory",
    )
    parser.add_argument("--label", default="", help="short tag appended to this run's log directory name")


def make_run_dir(base: str | os.PathLike, tool: str, label: str = "") -> Path:
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    name = f"{stamp}-{tool}"
    if label:
        name += "-" + re.sub(r"[^A-Za-z0-9._-]+", "_", label)[:40]
    path = Path(base) / name
    n = 1
    while path.exists():
        n += 1
        path = Path(base) / f"{name}.{n}"
    path.mkdir(parents=True)
    return path


def write_command_line(run_dir: Path) -> None:
    (run_dir / "command.txt").write_text(" ".join(sys.argv) + "\n", encoding="utf-8")


class RotatingFile:
    """Append-only file that starts a new part when it reaches `limit` bytes.

    Parts are named <stem>-0000<suffix>, <stem>-0001<suffix>, ... so sorting
    the names gives stream order."""

    def __init__(self, directory: Path, stem: str, suffix: str, limit: int, binary: bool):
        self.directory = directory
        self.stem = stem
        self.suffix = suffix
        self.limit = max(limit, 1024)
        self.binary = binary
        self.part = -1
        self.size = 0
        self.total = 0
        self.fh = None
        self._open_next()

    def _open_next(self) -> None:
        if self.fh:
            self.fh.close()
        self.part += 1
        path = self.directory / f"{self.stem}-{self.part:04d}{self.suffix}"
        self.fh = open(path, "ab" if self.binary else "a", encoding=None if self.binary else "utf-8",
                       newline=None if self.binary else "\n")
        self.size = 0

    @property
    def path(self) -> Path:
        return self.directory / f"{self.stem}-{self.part:04d}{self.suffix}"

    def write(self, data) -> None:
        n = len(data) if self.binary else len(data.encode("utf-8"))
        if self.size and self.size + n > self.limit:
            self._open_next()
        self.fh.write(data)
        self.size += n
        self.total += n

    def flush(self) -> None:
        if self.fh:
            self.fh.flush()

    def close(self) -> None:
        if self.fh:
            self.fh.close()
            self.fh = None


class StopFlag:
    """Ctrl-C / SIGTERM set an event instead of raising in a random place.

    A second Ctrl-C raises KeyboardInterrupt as usual, so a stuck run can
    still be killed."""

    def __init__(self) -> None:
        self.event = threading.Event()
        self._count = 0
        signal.signal(signal.SIGINT, self._handler)
        try:
            signal.signal(signal.SIGTERM, self._handler)
        except (ValueError, OSError):
            pass

    def _handler(self, signum, frame) -> None:
        self._count += 1
        if self._count > 1:
            raise KeyboardInterrupt
        self.event.set()

    def is_set(self) -> bool:
        return self.event.is_set()

    def clear(self) -> None:
        """Re-arm after a Ctrl-C that ended one phase, so clean-up steps can still run."""
        self.event.clear()
        self._count = 0

    def wait(self, timeout: float) -> bool:
        return self.event.wait(timeout)


def human_bytes(n: float) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if abs(n) < 1024 or unit == "GB":
            return f"{n:.0f} {unit}" if unit == "B" else f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} GB"
