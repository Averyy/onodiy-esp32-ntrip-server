# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The README's Layout table names every benchlib module, so a new one is never left out."""

from __future__ import annotations

import re
from pathlib import Path

BENCH = Path(__file__).resolve().parents[1]


def layout_table(text: str) -> str:
    """The rows of the table under '## Layout' (up to the first line that is not a row)."""
    m = re.search(r"^## Layout\n+((?:\|.*\n)+)", text, re.MULTILINE)
    assert m, "README.md has no Layout table"
    return m.group(1)


def test_layout_names_every_benchlib_module():
    table = layout_table((BENCH / "README.md").read_text(encoding="utf-8"))
    modules = sorted(p.name for p in (BENCH / "benchlib").glob("*.py") if p.name != "__init__.py")
    assert modules, "no benchlib modules found"
    missing = [m for m in modules if f"`benchlib/{m}`" not in table]
    assert not missing, f"Layout table in tools/bench/README.md lacks: {', '.join(missing)}"


def test_layout_names_every_test_with_its_own_rule():
    """Tests that need something special (Node.js, Docker, a script's options) are named in the tests/ row."""
    table = layout_table((BENCH / "README.md").read_text(encoding="utf-8"))
    for name in ("test_ui_text.py", "test_ui_casters.py", "test_docker_caster.py", "test_docker_run_script.py",
                 "test_readme_layout.py"):
        assert f"`{name}`" in table, name
