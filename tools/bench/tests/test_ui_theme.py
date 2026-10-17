# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""The web UI's colour tokens (components/web/ui/app.css): the dark theme the
same from the system setting and from the header's button, and the colour
pairs the pages draw at WCAG AA contrast (4.5:1 for text, 3:1 for the edges
of controls and for figures)."""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CSS = (ROOT / "components" / "web" / "ui" / "app.css").read_text()
NOCOMMENT = re.sub(r"/\*.*?\*/", "", CSS, flags=re.S)


def block(start: str) -> dict[str, str]:
    """The custom properties of the rule that opens with `start` (up to its closing brace)."""
    i = NOCOMMENT.index(start) + len(start)
    body = NOCOMMENT[i:NOCOMMENT.index("}", i)]
    return dict(re.findall(r"(--[\w-]+)\s*:\s*([^;]+);", body))


LIGHT = block(":root {")
DARK_SYSTEM = block('@media (prefers-color-scheme: dark) {\n  :root:not([data-theme="light"]) {')
DARK_BUTTON = block(':root[data-theme="dark"] {')


def hex_color(theme: dict[str, str], name: str) -> str:
    v = theme[name].strip()
    assert re.fullmatch(r"#[0-9a-f]{6}", v), f"{name}: {v}"
    return v


def luminance(h: str) -> float:
    def lin(c: int) -> float:
        s = c / 255
        return s / 12.92 if s <= 0.03928 else ((s + 0.055) / 1.055) ** 2.4
    r, g, b = (int(h[i:i + 2], 16) for i in (1, 3, 5))
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b)


def contrast(a: str, b: str) -> float:
    x, y = luminance(a), luminance(b)
    return (max(x, y) + 0.05) / (min(x, y) + 0.05)


def low_pairs(theme: dict[str, str], pairs, need: float) -> list[str]:
    out = []
    for fg, bg in pairs:
        r = contrast(hex_color(theme, fg), hex_color(theme, bg))
        if r < need:
            out.append(f"{fg} on {bg}: {r:.2f}")
    return out


SURFACES = ("--bg", "--surface", "--surface-2")
TONES = ("ok", "warn", "bad", "info", "accent", "off")


def test_the_dark_theme_is_the_same_from_the_system_and_from_the_button():
    assert DARK_SYSTEM == DARK_BUTTON
    assert set(DARK_SYSTEM) <= set(LIGHT)   # every dark token has its light value
    # Rules of the dark theme beyond the tokens (the placeholders) exist for both ways of choosing it.
    system = re.findall(r':root:not\(\[data-theme="light"\]\) ([^\s{][^{]*)\{([^}]*)\}', NOCOMMENT)
    button = re.findall(r':root\[data-theme="dark"\] ([^\s{][^{]*)\{([^}]*)\}', NOCOMMENT)
    assert system and sorted(system) == sorted(button)


def test_text_colours_reach_4_5_to_1_in_both_themes():
    text = ["--text", "--muted", "--accent", "--ok", "--warn", "--bad", "--info", "--off"]
    pairs = [(f, s) for f in text for s in SURFACES]
    pairs += [(f"--{t}", f"--{t}-soft") for t in TONES]                       # chips, notices
    pairs += [(f, f"--{t}-soft") for f in ("--text", "--muted") for t in TONES]
    pairs += [("--accent-text", "--accent"), ("--bad-text", "--bad")]      # filled buttons
    for name, theme in (("light", LIGHT), ("dark", DARK_SYSTEM)):
        assert low_pairs(theme, pairs, 4.5) == [], name


def test_dark_edges_and_figures_reach_3_to_1():
    d = DARK_SYSTEM
    behind = list(SURFACES) + [f"--{t}-soft" for t in TONES]
    # Fields, buttons, choices and chips (on a card, the page and inside a notice), the focus ring.
    assert low_pairs(d, [("--edge", b) for b in behind], 3) == []
    assert low_pairs(d, [("--focus", b) for b in behind], 3) == []
    # A bar: its track outlined in the edge colour, its fill against the track.
    assert "var(--edge)" in d["--bar-ring"]
    assert low_pairs(d, [(f"--{t}", "--off-soft") for t in ("ok", "warn", "bad", "accent")], 3) == []
    # A dialog outlined in the edge colour over its backdrop.
    assert "var(--edge)" in d["--dialog-ring"]
    # Constellation dots on a card; the sky's C/N0 dots on a card, their labels on each dot.
    sys_colours = [k for k in d if k.startswith("--c-")]
    assert len(sys_colours) == 7
    assert low_pairs(d, [(c, "--surface") for c in sys_colours], 3) == []
    assert low_pairs(d, [(f"--cnr-{i}", "--surface") for i in range(5)], 3) == []
    assert low_pairs(d, [(f"--cnr-ink-{i}", f"--cnr-{i}") for i in range(5)], 4.5) == []


def test_c_n0_steps_stay_an_even_ramp():
    """One hue from weak to strong with even steps between neighbours, in both themes."""
    for name, theme in (("light", LIGHT), ("dark", DARK_SYSTEM)):
        steps = [hex_color(theme, f"--cnr-{i}") for i in range(5)]
        ratios = [contrast(a, b) for a, b in zip(steps, steps[1:])]
        assert all(1.38 <= r <= 1.65 for r in ratios), (name, [round(r, 2) for r in ratios])
        assert low_pairs(theme, [(f"--cnr-ink-{i}", f"--cnr-{i}") for i in range(5)], 4.5) == [], name


def test_the_header_state_reads_in_both_themes():
    """The header's indicator (renderLive in 20-shell.js): its word in the text colour and the count of seconds
    without contact in the muted one, at 4.5:1 on the header and on its hover background; its dot, in the top card's
    colour (grey while the state is unknown), at 3:1 there in every tone."""
    def rule(sel: str) -> str:
        m = re.search(re.escape(sel) + r"\s*\{([^}]*)\}", NOCOMMENT)
        assert m, sel
        return m.group(1)
    assert "color: var(--text)" in rule(".live a")
    assert "background: var(--surface-2)" in rule(".live a:hover")
    assert "color: var(--muted)" in rule(".live .age")
    assert "background: var(--tone, var(--off))" in rule(".live .in::before")
    # Without contact the Status page's top card goes grey like the header (after its tone rules, which it overrides).
    assert "background: var(--off)" in rule("body.no-contact .hero .state::before")
    assert NOCOMMENT.index("body.no-contact .hero .state::before") > NOCOMMENT.index(".hero.info .state::before")
    behind = ("--surface", "--surface-2")
    for name, theme in (("light", LIGHT), ("dark", DARK_SYSTEM)):
        assert low_pairs(theme, [(f, b) for f in ("--text", "--muted") for b in behind], 4.5) == [], name
        assert low_pairs(theme, [(f"--{t}", b) for t in ("ok", "warn", "bad", "info", "off") for b in behind], 3) == [], name
