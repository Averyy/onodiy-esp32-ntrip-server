# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""tools/release_notes.py, and CHANGELOG.md's section for the tag CI builds (so a tag without
notes fails here, minutes in, rather than at the release job's end)."""

from __future__ import annotations

import importlib.util
import os
import re
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[3]


def load():
    spec = importlib.util.spec_from_file_location("release_notes_under_test", ROOT / "tools" / "release_notes.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


rn = load()

TEXT = """# Changelog

Intro.

## v0.2.0

- Two.

- Still two.

## v0.1.0

One.
## v0.0.9

"""


def test_section_up_to_the_next_heading_trimmed():
    assert rn.section(TEXT, "v0.2.0") == "- Two.\n\n- Still two."
    assert rn.section(TEXT, "v0.1.0") == "One."


def test_missing_and_empty_sections():
    assert rn.section(TEXT, "v0.3.0") is None
    assert rn.section(TEXT, "v0.0.9") == ""
    assert rn.section(TEXT, "v0.2") is None   # a whole heading, not a prefix


def test_unwrap_joins_wrapped_lines_only():
    text = ("The first\nrelease.\n\n- One item\n  wrapped.\n- Two\n1. Three\n   more.\n\n"
            "```\nkeep\nthese\n```\n### Head\nNext\nline.\n> quote\n| a |\n| b |")
    assert rn.unwrap(text) == ("The first release.\n\n- One item wrapped.\n- Two\n1. Three more.\n\n"
                               "```\nkeep\nthese\n```\n### Head\nNext line.\n> quote\n| a |\n| b |")


def test_main_exit_status(tmp_path, capsys):
    f = tmp_path / "CHANGELOG.md"
    f.write_text(TEXT, encoding="utf-8")
    assert rn.main(["v0.1.0", "--changelog", str(f)]) == 0
    assert capsys.readouterr().out == "One.\n"
    assert rn.main(["v0.3.0", "--changelog", str(f)]) == 1
    assert "no '## v0.3.0' section" in capsys.readouterr().err
    assert rn.main(["v0.0.9", "--changelog", str(f)]) == 1


def test_changelog_headings_are_release_tags_newest_first():
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    tags = re.findall(r"^## (.+)$", text, re.MULTILINE)
    assert tags, "CHANGELOG.md has no release sections"
    bad = [t for t in tags if not re.fullmatch(r"v\d+\.\d+\.\d+(-[0-9A-Za-z.-]+)?", t)]
    assert not bad, f"headings that are not release tags: {bad}"
    nums = [tuple(int(x) for x in re.match(r"v(\d+)\.(\d+)\.(\d+)", t).groups()) for t in tags]
    assert nums == sorted(nums, reverse=True), f"not newest first: {tags}"
    assert all(rn.section(text, t) for t in tags), "an empty section"


@pytest.mark.skipif(not (os.environ.get("GITHUB_REF_TYPE") == "tag"
                         and os.environ.get("GITHUB_REF_NAME", "").startswith("v")),
                    reason="not a CI run for a release tag")
def test_the_tag_ci_builds_has_notes():
    tag = os.environ["GITHUB_REF_NAME"]
    assert rn.section((ROOT / "CHANGELOG.md").read_text(encoding="utf-8"), tag), \
        f"CHANGELOG.md has no '## {tag}' section: the release job would fail at the end"
