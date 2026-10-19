#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""A release's highlights: the section of CHANGELOG.md for its tag.

    python3 tools/release_notes.py TAG [--changelog FILE]

Prints the lines under "## TAG" up to the next "## " heading, without the blank lines
around them, each paragraph and list item on one line: GitHub shows every line break of
a release's notes, so CHANGELOG.md's wrapped lines would break mid-sentence there.
Exits 1, naming the problem, when the section is missing or empty, so a tag without
notes fails before its release is made (CI's release job puts this at the top of the
notes; tools/bench/tests/test_release_notes.py checks a tag's section early).
Standard library only: the release job runs it with the runner's python3."""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

CHANGELOG = Path(__file__).resolve().parents[1] / "CHANGELOG.md"


def section(text: str, tag: str) -> str | None:
    """The text under "## <tag>" (trimmed), "" for an empty section, None without one."""
    lines = text.splitlines()
    for i, line in enumerate(lines):
        if line.strip() == f"## {tag}":
            body = []
            for rest in lines[i + 1:]:
                if rest.startswith("## "):
                    break
                body.append(rest)
            return "\n".join(body).strip("\n")
    return None


def unwrap(text: str) -> str:
    """Wrapped lines joined: a line that starts no list item, heading, quote, table row or code
    fence goes onto the line before it (code blocks and blank lines stay as they are)."""
    out: list[str] = []
    fenced = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("```"):
            fenced = not fenced
            out.append(line)
            continue
        starts = fenced or not stripped or re.match(r"([-*+]|\d+[.)])\s|#|>|\|", stripped)
        prev = out[-1] if out else ""
        if starts or not prev.strip() or prev.lstrip().startswith(("```", "#", "|")):
            out.append(line)
        else:
            out[-1] = f"{prev.rstrip()} {stripped}"
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("tag")
    ap.add_argument("--changelog", type=Path, default=CHANGELOG)
    args = ap.parse_args(argv)
    body = section(args.changelog.read_text(encoding="utf-8"), args.tag)
    if body is None:
        print(f"{args.changelog.name} has no '## {args.tag}' section: add the release's highlights there",
              file=sys.stderr)
        return 1
    if not body.strip():
        print(f"{args.changelog.name}'s '## {args.tag}' section is empty", file=sys.stderr)
        return 1
    print(unwrap(body))
    return 0


if __name__ == "__main__":
    sys.exit(main())
