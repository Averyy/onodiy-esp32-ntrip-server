#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Check, bundle, compress and embed the browser UI (components/web/ui).

The build runs this for every change under ui/ (components/web/CMakeLists.txt):

  ui/index.html, ui/app.css, ui/favicon.svg   served as they are
  ui/js/*.js                                  joined in file-name order into
                                              app.js, inside one strict-mode
                                              function (no globals leak)

Each asset is gzip-compressed deterministically (no file name, mtime 0) and
written into one C file holding the asset table declared in
priv_include/web_assets.h. The build fails when a check fails or the
compressed total exceeds --max-gz.

The checks need nothing beyond the Python standard library (no Node.js):

  HTML  well-formed nesting, unique ids, the page skeleton (doctype, lang,
        charset, viewport), and what the server's Content-Security-Policy
        (own origin only) would block: inline scripts and styles, style and
        on* attributes, resources from other origins. Every src/href names a
        served asset.
  JS    a tokenizer pass over the bundle: strings, template literals, regular
        expressions and comments terminate; (), [] and {} balance. Refused
        identifiers: eval, Function, innerHTML, outerHTML, insertAdjacentHTML,
        document.write (CSP and HTML-injection sinks; the UI builds DOM
        nodes). getElementById('x') must name an id in index.html.
  CSS   comments and strings terminate, braces balance, no @import and no
        url() to another origin.
  SVG   well-formed XML.

  python3 ui_bundle.py --ui DIR --c-out FILE [--max-gz BYTES] [--out-dir DIR]

--out-dir also writes the uncompressed bundle (for looking at the UI with a
local development server).
"""

import argparse
import gzip
import hashlib
import html.parser
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

# Served path, source (None = the joined js/*.js), content type. The page
# itself is also served at "/" (web_asset_find).
ASSETS = (
    ("/index.html", "index.html", "text/html; charset=utf-8"),
    ("/app.js", None, "text/javascript; charset=utf-8"),
    ("/app.css", "app.css", "text/css; charset=utf-8"),
    ("/favicon.svg", "favicon.svg", "image/svg+xml"),
)

SPDX_HEADER = "/* SPDX-FileCopyrightText: 2026 Avery L.\n * SPDX-License-Identifier: MIT */\n"


class CheckError(Exception):
    pass


# --------------------------------------------------------------------------
# HTML

VOID = {"area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"}


class _HtmlCheck(html.parser.HTMLParser):
    def __init__(self, name, served):
        super().__init__(convert_charrefs=True)
        self.name = name
        self.served = served
        self.stack = []
        self.errors = []
        self.ids = set()
        self.seen = set()
        self.in_script = False
        self.doctype = False

    def err(self, msg):
        line, _ = self.getpos()
        self.errors.append(f"{self.name}:{line}: {msg}")

    def handle_decl(self, decl):
        if decl.lower() == "doctype html":
            self.doctype = True

    def handle_starttag(self, tag, attrs):
        self._attrs(tag, attrs)
        if tag == "script":
            self.in_script = True
        if tag not in VOID:
            self.stack.append((tag, self.getpos()[0]))

    def handle_startendtag(self, tag, attrs):
        self._attrs(tag, attrs)
        if tag not in VOID:
            self.err(f"<{tag}/> is not a void element")

    def handle_endtag(self, tag):
        if tag == "script":
            self.in_script = False
        if tag in VOID:
            self.err(f"end tag for void element </{tag}>")
            return
        if not self.stack:
            self.err(f"</{tag}> without an open element")
            return
        open_tag, line = self.stack.pop()
        if open_tag != tag:
            self.err(f"</{tag}> closes <{open_tag}> opened on line {line}")

    def handle_data(self, data):
        if self.in_script and data.strip():
            self.err("inline script (the Content-Security-Policy blocks it)")

    def _attrs(self, tag, attrs):
        self.seen.add(tag)
        names = [a for a, _ in attrs]
        if len(names) != len(set(names)):
            self.err(f"<{tag}> repeats an attribute")
        for attr, value in attrs:
            value = value or ""
            if attr.startswith("on"):
                self.err(f"inline handler {attr}= (the Content-Security-Policy blocks it)")
            if attr == "style":
                self.err("style attribute (the Content-Security-Policy blocks it)")
            if attr == "id":
                if value in self.ids:
                    self.err(f"duplicate id '{value}'")
                self.ids.add(value)
            if attr in ("src", "href") and not value.startswith("#"):
                if re.match(r"^([a-z][a-z0-9+.-]*:|//)", value, re.I):
                    self.err(f"{attr}='{value}' leaves the device's own origin")
                elif "/" + value.lstrip("./") not in self.served:
                    self.err(f"{attr}='{value}' is not a served asset")
        if tag == "style":
            self.err("<style> element (the Content-Security-Policy blocks it)")
        if tag == "script" and "src" not in names:
            self.err("<script> without src")


def check_html(text, name, served):
    p = _HtmlCheck(name, served)
    p.feed(text)
    p.close()
    if not p.doctype:
        p.errors.append(f"{name}: missing <!DOCTYPE html>")
    for tag, line in p.stack:
        p.errors.append(f"{name}:{line}: <{tag}> is never closed")
    if not re.search(r"<html[^>]*\blang=", text):
        p.errors.append(f"{name}: <html> has no lang attribute")
    if not re.search(r'<meta charset="utf-8">', text, re.I):
        p.errors.append(f"{name}: missing <meta charset=\"utf-8\">")
    if 'name="viewport"' not in text:
        p.errors.append(f"{name}: missing the viewport meta tag")
    if p.errors:
        raise CheckError("\n".join(p.errors))
    return p.ids


# --------------------------------------------------------------------------
# JavaScript (tokenizer-level check)

# After these keywords an expression starts, so "/" opens a regular expression.
_REGEX_AFTER_KW = {
    "return", "typeof", "instanceof", "in", "of", "new", "delete", "void", "throw", "case", "do", "else",
    "yield", "await",
}
_BANNED = {
    "eval": "eval (blocked by the Content-Security-Policy)",
    "Function": "the Function constructor (blocked by the Content-Security-Policy)",
    "innerHTML": "innerHTML (build DOM nodes instead)",
    "outerHTML": "outerHTML (build DOM nodes instead)",
    "insertAdjacentHTML": "insertAdjacentHTML (build DOM nodes instead)",
    "write": None,  # only as document.write, checked below
}
_PAIRS = {")": "(", "]": "[", "}": "{"}
_IDENT_START = re.compile(r"[A-Za-z_$]")
_IDENT = re.compile(r"[A-Za-z0-9_$]*")
_NUMBER = re.compile(r"(0[xX][0-9a-fA-F_]+|0[bB][01_]+|0[oO][0-7_]+|(\d[\d_]*)?\.?\d[\d_]*([eE][+-]?\d+)?n?)")


def check_js(src, name):
    """Return the identifiers seen; raise CheckError on a structural error."""
    errors = []
    stack = []  # (opener, line); "${" marks an expression inside a template literal
    i, n, line = 0, len(src), 1
    regex_ok = True  # "/" here starts a regular expression
    idents = []
    prev_ident = ""

    def err(msg, at_line=None):
        errors.append(f"{name}:{at_line or line}: {msg}")

    def scan_template(i, line):
        """Scan template text from i (after ` or the closing } of ${...}).

        Returns (index after the template or after "${", line, entered_expr)."""
        start_line = line
        while i < n:
            c = src[i]
            if c == "\\":
                i += 2
                continue
            if c == "\n":
                line += 1
            if c == "`":
                return i + 1, line, False
            if c == "$" and i + 1 < n and src[i + 1] == "{":
                return i + 2, line, True
            i += 1
        err("unterminated template literal", start_line)
        return n, line, False

    while i < n and len(errors) < 20:
        c = src[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r\f\v﻿":
            i += 1
            continue
        nxt = src[i + 1] if i + 1 < n else ""
        if c == "/" and nxt == "/":
            j = src.find("\n", i)
            i = n if j < 0 else j
            continue
        if c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            if j < 0:
                err("unterminated comment")
                break
            line += src.count("\n", i, j)
            i = j + 2
            continue
        if c in "'\"":
            j = i + 1
            while j < n and src[j] != c:
                if src[j] == "\\":
                    if j + 1 < n and src[j + 1] == "\n":
                        line += 1
                    j += 2
                    continue
                if src[j] == "\n":
                    err("line break inside a string")
                    break
                j += 1
            if j >= n:
                err("unterminated string")
                break
            i = j + 1
            regex_ok = False
            continue
        if c == "`":
            i, line, entered = scan_template(i + 1, line)
            if entered:
                stack.append(("${", line))
                regex_ok = True
            else:
                regex_ok = False
            continue
        if c == "/" and regex_ok:
            j, in_class = i + 1, False
            while j < n:
                d = src[j]
                if d == "\\":
                    j += 2
                    continue
                if d == "\n":
                    break
                if d == "[":
                    in_class = True
                elif d == "]":
                    in_class = False
                elif d == "/" and not in_class:
                    break
                j += 1
            if j >= n or src[j] != "/":
                err("unterminated regular expression")
                break
            j += 1
            while j < n and src[j].isalpha():
                j += 1
            i = j
            regex_ok = False
            continue
        if _IDENT_START.match(c):
            m = _IDENT.match(src, i + 1)
            word = src[i:m.end()]
            dotted = i > 0 and src[i - 1] == "."
            if word in _BANNED:
                if word == "write":
                    if dotted and prev_ident == "document":
                        err("document.write")
                elif word == "Function":
                    if not dotted:
                        err(_BANNED[word])
                else:
                    err(_BANNED[word])
            idents.append(word)
            prev_ident = word
            regex_ok = (not dotted) and word in _REGEX_AFTER_KW
            i = m.end()
            continue
        if c.isdigit() or (c == "." and nxt.isdigit()):
            m = _NUMBER.match(src, i)
            i = m.end() if m and m.end() > i else i + 1
            regex_ok = False
            continue
        if c in "([{":
            stack.append((c, line))
            regex_ok = True
            i += 1
            continue
        if c in ")]}":
            if not stack:
                err(f"'{c}' without an opener")
                break
            opener, open_line = stack.pop()
            if c == "}" and opener == "${":
                i, line, entered = scan_template(i + 1, line)
                if entered:
                    stack.append(("${", line))
                    regex_ok = True
                else:
                    regex_ok = False
                continue
            if _PAIRS[c] != opener:
                err(f"'{c}' closes '{opener}' opened on line {open_line}")
                break
            regex_ok = c == "}"
            i += 1
            continue
        # Operators and other punctuation: an expression may start after them,
        # except after postfix ++/-- (rare before a division; not handled).
        regex_ok = True
        i += 1

    for opener, open_line in stack:
        err(f"'{opener}' is never closed", open_line)
    if errors:
        raise CheckError("\n".join(errors))
    return idents


# --------------------------------------------------------------------------
# CSS


def check_css(text, name):
    errors = []
    depth, line, i, n = 0, 1, 0, len(text)
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
        elif c == "/" and text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                errors.append(f"{name}:{line}: unterminated comment")
                break
            line += text.count("\n", i, j)
            i = j + 2
            continue
        elif c in "'\"":
            j = text.find(c, i + 1)
            if j < 0 or "\n" in text[i:j]:
                errors.append(f"{name}:{line}: unterminated string")
                break
            i = j + 1
            continue
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth < 0:
                errors.append(f"{name}:{line}: '}}' without '{{'")
                depth = 0
        i += 1
    if depth:
        errors.append(f"{name}: {depth} unclosed '{{'")
    if re.search(r"@import\b", text):
        errors.append(f"{name}: @import (bundle the rules instead)")
    for m in re.finditer(r"url\(\s*['\"]?([^)'\"]*)", text):
        if re.match(r"^([a-z][a-z0-9+.-]*:|//)", m.group(1), re.I):
            errors.append(f"{name}: url({m.group(1)}) leaves the device's own origin")
    if errors:
        raise CheckError("\n".join(errors))


# --------------------------------------------------------------------------


def c_ident(path):
    return "s_" + re.sub(r"[^A-Za-z0-9]", "_", path.strip("/"))


def build(ui, max_gz):
    served = {p for p, _, _ in ASSETS}
    errors = []
    sources = {}
    for path, src, _ in ASSETS:
        if src is None:
            continue
        try:
            sources[path] = (ui / src).read_text(encoding="utf-8")
        except OSError as e:
            errors.append(f"{src}: {e}")
    js_files = sorted((ui / "js").glob("*.js"))
    if not js_files:
        errors.append("js/: no scripts")
    if errors:
        raise CheckError("\n".join(errors))

    ids = None  # None: the page failed its own check, so ids are not cross-checked
    for path, check in (("/index.html", "html"), ("/app.css", "css"), ("/favicon.svg", "svg")):
        try:
            if check == "html":
                ids = check_html(sources[path], path.lstrip("/"), served)
            elif check == "css":
                check_css(sources[path], path.lstrip("/"))
            else:
                ET.fromstring(sources[path])
        except (CheckError, ET.ParseError) as e:
            errors.append(str(e) if isinstance(e, CheckError) else f"{path.lstrip('/')}: {e}")

    parts = []
    js_ok = True
    for f in js_files:
        text = f.read_text(encoding="utf-8")
        try:
            check_js(text, "js/" + f.name)
        except CheckError as e:
            errors.append(str(e))
            js_ok = False
        parts.append(text.rstrip() + "\n")
    bundle = SPDX_HEADER + "(function () {\n'use strict';\n" + "\n".join(parts) + "})();\n"
    if js_ok:  # the served script as a whole, wrapper included
        try:
            check_js(bundle, "app.js")
        except CheckError as e:
            errors.append(str(e))
    if ids is not None:
        missing = sorted({m.group(1) for m in re.finditer(r"getElementById\(\s*'([^']+)'\s*\)", bundle)} - ids)
        for name in missing:
            errors.append(f"app.js: getElementById('{name}'): no such id in index.html")
    if errors:
        raise CheckError("\n".join(errors))
    sources["/app.js"] = bundle

    out = []
    for path, _, ctype in ASSETS:
        raw = sources[path].encode("utf-8")
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        out.append((path, ctype, raw, gz))
    total = sum(len(gz) for _, _, _, gz in out)
    if total > max_gz:
        raise CheckError(f"UI is {total} bytes gzipped, over the {max_gz}-byte budget")
    return out, total


def write_c(path, assets, total):
    digest = hashlib.sha256(b"".join(gz for _, _, _, gz in assets)).hexdigest()[:16]
    lines = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 Avery L.",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * Generated by components/web/tools/ui_bundle.py from components/web/ui.",
        " * Do not edit; the build regenerates it.",
        " */",
        '#include "web_assets.h"',
        "",
    ]
    for p, _, raw, gz in assets:
        lines.append(f"/* {p}: {len(raw)} bytes, {len(gz)} gzipped */")
        lines.append(f"static const uint8_t {c_ident(p)}[{len(gz)}] = {{")
        for k in range(0, len(gz), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in gz[k:k + 16]) + ",")
        lines.append("};")
        lines.append("")
    lines.append("const web_asset_t web_ui_assets[] = {")
    for p, ctype, _, gz in assets:
        lines.append(f'    {{ "{p}", "{ctype}", {c_ident(p)}, sizeof({c_ident(p)}) }},')
    lines.append("};")
    lines.append("const size_t web_ui_asset_count = sizeof(web_ui_assets) / sizeof(web_ui_assets[0]);")
    lines.append(f'const char web_ui_digest[] = "{digest}";')
    lines.append(f"const size_t web_ui_gz_total = {total}u;")
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ui", required=True, type=pathlib.Path, help="UI source directory")
    ap.add_argument("--c-out", type=pathlib.Path, help="generated C file")
    ap.add_argument("--out-dir", type=pathlib.Path, help="also write the uncompressed bundle here")
    ap.add_argument("--max-gz", type=int, default=150 * 1024, help="budget for the compressed total")
    args = ap.parse_args()
    try:
        assets, total = build(args.ui, args.max_gz)
    except CheckError as e:
        print(f"web UI check failed:\n{e}", file=sys.stderr)
        return 1
    if args.c_out:
        args.c_out.parent.mkdir(parents=True, exist_ok=True)
        write_c(args.c_out, assets, total)
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for p, _, raw, _ in assets:
            (args.out_dir / p.lstrip("/")).write_bytes(raw)
    sizes = ", ".join(f"{p.lstrip('/')} {len(gz)}" for p, _, _, gz in assets)
    print(f"web UI: {total} bytes gzipped ({sizes})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
