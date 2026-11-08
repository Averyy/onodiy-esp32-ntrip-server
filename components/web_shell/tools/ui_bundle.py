#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Validate, bundle and embed an explicitly selected browser composition.

Module sources use shared: paths relative to web_shell/ui or product: paths
relative to --product-root. The ordered manifest is also consumed by the UI
harnesses through load_composition()/module_sources(). No directory scanning.
"""

import argparse
import gzip
import hashlib
import html.parser
import json
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

SHARED_UI = pathlib.Path(__file__).resolve().parents[1] / "ui"

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


def check_js(src, name, comments=None):
    """Return the identifiers seen; raise CheckError on a structural error.

    comments: a list that gets the (start, end) of every comment."""
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
            j = n if j < 0 else j
            if comments is not None:
                comments.append((i, j))
            i = j
            continue
        if c == "/" and nxt == "*":
            j = src.find("*/", i + 2)
            if j < 0:
                err("unterminated comment")
                break
            line += src.count("\n", i, j)
            if comments is not None:
                comments.append((i, j + 2))
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


def strip_comments(src, spans):
    """src without the comments at spans (check_js): a comment alone on its
    lines goes with them, one beside code becomes a space."""
    out, at = [], 0
    for a, b in spans:
        start = src.rfind("\n", 0, a) + 1
        end = src.find("\n", b)
        end = len(src) if end < 0 else end
        if not src[start:a].strip() and not src[b:end].strip():
            out.append(src[at:start])
            at = end + 1
        else:
            out.append(src[at:a] + " ")
            at = b
    out.append(src[at:])
    return "".join(out)


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


def c_string(value):
    """C string containing the exact UTF-8 bytes, including non-BMP branding."""
    return '"' + "".join(chr(byte) if 32 <= byte < 127 and byte not in (34, 92)
                         else f"\\{byte:03o}" for byte in value.encode("utf-8")) + '"'


def _json(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        raise CheckError(f"{path}: {e}") from e


def resolve_source(source, product_root):
    kind, sep, relative = source.partition(":")
    root = {"shared": SHARED_UI, "product": product_root}.get(kind)
    if not sep or root is None or not relative:
        raise CheckError(f"invalid source: {source}")
    path = (root / relative).resolve()
    if not path.is_relative_to(root.resolve()) or not path.is_file():
        raise CheckError(f"missing or escaping source: {source}")
    return path


def load_composition(manifest, product_root=None, asset_manifest=None):
    manifest = pathlib.Path(manifest).resolve()
    root = pathlib.Path(product_root).resolve() if product_root else manifest.parent
    product = _json(manifest)
    modules, names, paths = [], set(), set()
    for module in product["modules"]:
        name = module["name"]
        if not re.fullmatch(r"[a-z][a-z0-9_-]*", name) or name in names:
            raise CheckError(f"invalid or duplicate module name: {name}")
        missing = set(module.get("requires", [])) - names
        if missing:
            raise CheckError(f"{name}: dependencies must precede it: {sorted(missing)}")
        source = module["source"]
        path = None if source == "generated:product" else resolve_source(source, root)
        identity = path if path else source
        if identity in paths:
            raise CheckError(f"duplicate module source: {source}")
        if path and path.suffix != ".js":
            raise CheckError(f"{name}: module must be JavaScript")
        modules.append(dict(module, path=path))
        paths.add(identity)
        names.add(name)
    if not modules or modules[0]["source"] != "generated:product":
        raise CheckError("first module must be generated:product metadata")
    entrypoints = [i for i, module in enumerate(modules) if module.get("entrypoint")]
    if entrypoints and entrypoints != [len(modules) - 1]:
        raise CheckError("entrypoint must be unique and last")
    metadata = product["metadata"]
    for key in ("title", "default_brand", "product_noun", "project_name", "browser_storage_prefix"):
        if not isinstance(metadata.get(key), str) or not metadata[key] or any(ord(c) < 32 for c in metadata[key]):
            raise CheckError(f"invalid product metadata: {key}")
    asset_manifest = pathlib.Path(asset_manifest).resolve() if asset_manifest else root / product["assets"]
    assets, urls = [], set()
    for asset in _json(asset_manifest)["assets"]:
        if asset.get("if_module") and asset["if_module"] not in names:
            continue
        url = asset["url"]
        if (not re.fullmatch(r"/[A-Za-z0-9_./-]+", url) or "//" in url or
                any(part in (".", "..") for part in url.split("/")) or url in urls or
                url in ("/api", "/ws") or url.startswith(("/api/", "/ws/"))):
            raise CheckError(f"invalid or duplicate asset URL: {url}")
        mime = asset["type"]
        if not isinstance(mime, str) or not mime or any(ord(c) < 32 or ord(c) > 126 for c in mime):
            raise CheckError(f"invalid asset MIME type: {url}")
        path = None if asset["source"] == "generated:bundle" else resolve_source(asset["source"], root)
        assets.append(dict(asset, path=path))
        urls.add(url)
    if not {"/index.html", "/app.js", "/app.css", "/favicon.svg"} <= urls:
        raise CheckError("missing canonical shell assets")
    if sum(a["source"] == "generated:bundle" for a in assets) != 1 or not any(
            a["url"] == "/app.js" and a["source"] == "generated:bundle" for a in assets):
        raise CheckError("/app.js must be the generated bundle")
    inputs = [manifest, asset_manifest] + [m["path"] for m in modules if m["path"]]
    inputs += [a["path"] for a in assets if a["path"]]
    for asset in assets:
        if asset.get("hash_manifest"):
            pinned = resolve_source(asset["hash_manifest"], root)
            inputs.append(pinned)
            expected = _json(pinned)["files"][asset["path"].name]
            if hashlib.sha256(asset["path"].read_bytes()).hexdigest() != expected:
                raise CheckError(f"{asset['url']}: differs from the pinned vendor distribution")
    stylesheets = [a["url"] for a in assets if a.get("stylesheet")]
    return dict(metadata=metadata, modules=modules, assets=assets, inputs=list(dict.fromkeys(inputs)), stylesheets=stylesheets)


def module_sources(composition, include_entrypoint=True):
    """Ordered (name, path, source) triples, including declarative metadata."""
    for module in composition["modules"]:
        if not include_entrypoint and module.get("entrypoint"):
            continue
        path = module["path"]
        source = path.read_text(encoding="utf-8") if path else (
            "const PRODUCT = " + json.dumps(composition["metadata"], ensure_ascii=True) + ";\n")
        yield module["name"], path, source


def render_index(text, composition):
    metadata = composition["metadata"]
    text = text.replace("{{title}}", html.escape(metadata["title"], quote=True))
    text = text.replace("{{brand}}", html.escape(metadata["default_brand"], quote=True))
    links = "\n".join('<link rel="stylesheet" href="' + html.escape(url.lstrip("/"), quote=True) + '">' for url in composition["stylesheets"])
    return text.replace("{{stylesheets}}", links)


def build(composition, max_gz, max_project_gz=160 * 1024):
    served = {a["url"] for a in composition["assets"]}
    errors, sources = [], {}
    for asset in composition["assets"]:
        if asset["path"] is None:
            continue
        raw = asset["path"].read_bytes()
        if asset["url"] == "/index.html":
            raw = render_index(raw.decode("utf-8"), composition).encode("utf-8")
        sources[asset["url"]] = raw

    ids = None  # None: the page failed its own check, so ids are not cross-checked
    for path, check in (("/index.html", "html"), ("/app.css", "css"), ("/favicon.svg", "svg")):
        try:
            if check == "html":
                ids = check_html(sources[path].decode("utf-8"), path.lstrip("/"), served)
            elif check == "css":
                check_css(sources[path].decode("utf-8"), path.lstrip("/"))
            else:
                ET.fromstring(sources[path])
        except (CheckError, ET.ParseError) as e:
            errors.append(str(e) if isinstance(e, CheckError) else f"{path.lstrip('/')}: {e}")

    parts = []
    js_ok = True
    for name, f, text in module_sources(composition):
        spans = []
        try:
            check_js(text, str(f) if f else name, spans)
            text = strip_comments(text, spans)
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
    sources["/app.js"] = bundle.encode("utf-8")
    # Vendor JS is hash-checked above; project scripts and styles retain the
    # same structural/CSP checks as the canonical shell.
    for asset in composition["assets"]:
        if asset.get("hash_manifest") or asset["url"] in ("/app.js", "/app.css", "/index.html", "/favicon.svg"):
            continue
        raw = sources[asset["url"]]
        mime = asset["type"].split(";")[0]
        if mime in ("text/javascript", "application/javascript"):
            check_js(raw.decode("utf-8"), asset["url"])
        elif mime == "text/html":
            check_html(raw.decode("utf-8"), asset["url"], served)
        elif mime == "text/css":
            check_css(raw.decode("utf-8"), asset["url"])
        elif mime == "image/svg+xml":
            ET.fromstring(raw)

    out = []
    for asset in composition["assets"]:
        path, ctype = asset["url"], asset["type"]
        raw = sources[path]
        gz = gzip.compress(raw, compresslevel=9, mtime=0)
        out.append((path, ctype, raw, gz))
    total = sum(len(gz) for _, _, _, gz in out)
    own_total = sum(len(row[3]) for asset, row in zip(composition['assets'], out) if not asset.get('vendor', False))
    if own_total > max_project_gz:
        raise CheckError(f"Project UI is {own_total} bytes gzipped, over the {max_project_gz}-byte project budget")
    if total > max_gz:
        raise CheckError(f"UI is {total} bytes gzipped, over the {max_gz}-byte budget")
    return out, total


def write_c(path, assets, total, prefix, metadata):
    digest = hashlib.sha256(b"".join(gz for _, _, _, gz in assets)).hexdigest()[:16]
    lines = [
        "/*",
        " * SPDX-FileCopyrightText: 2026 Avery L.",
        " * SPDX-License-Identifier: MIT",
        " *",
        " * Generated by components/web_shell/tools/ui_bundle.py.",
        " * Do not edit; the build regenerates it.",
        " */",
        '#include "web_shell_assets.h"',
        "",
    ]
    for index, (p, _, raw, gz) in enumerate(assets):
        lines.append(f"/* {p}: {len(raw)} bytes, {len(gz)} gzipped */")
        lines.append(f"static const uint8_t s_asset_{index}[{len(gz)}] = {{")
        for k in range(0, len(gz), 16):
            lines.append("    " + ", ".join(f"0x{b:02x}" for b in gz[k:k + 16]) + ",")
        lines.append("};")
        lines.append("")
    lines.append(f"static const web_asset_t {prefix}_table[] = {{")
    for index, (p, ctype, _, gz) in enumerate(assets):
        lines.append(f'    {{ {c_string(p)}, {c_string(ctype)}, s_asset_{index}, sizeof(s_asset_{index}) }},')
    lines.append("};")
    lines.append(f'const web_shell_assets_t {prefix}_assets = {{ {prefix}_table, sizeof({prefix}_table) / sizeof({prefix}_table[0]), "{digest}", {total}u }};')
    lines.append("")
    path.write_text("\n".join(lines), encoding="utf-8")
    header = [SPDX_HEADER, "#pragma once", '#include "web_shell_assets.h"',
              f"extern const web_shell_assets_t {prefix}_assets;"]
    for key in ("title", "default_brand", "product_noun", "project_name", "browser_storage_prefix"):
        header.append(f"#define {prefix.upper()}_{key.upper()} " + c_string(metadata[key]))
    path.with_suffix(".h").write_text("\n".join(header) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--ui", type=pathlib.Path, help="compatibility: product root containing product.json")
    ap.add_argument("--manifest", type=pathlib.Path, help="ordered product module manifest")
    ap.add_argument("--product-root", type=pathlib.Path)
    ap.add_argument("--asset-manifest", type=pathlib.Path)
    ap.add_argument("--symbol-prefix", default="station_ui")
    ap.add_argument("--max-project-gz", type=int, default=160 * 1024)
    ap.add_argument("--list-inputs", action="store_true")
    ap.add_argument("--sources-json", action="store_true", help="ordered modules for browser test harnesses")
    ap.add_argument("--c-out", type=pathlib.Path, help="generated C file")
    ap.add_argument("--out-dir", type=pathlib.Path, help="also write the uncompressed bundle here")
    ap.add_argument("--max-gz", type=int, default=640 * 1024, help="budget for the compressed total, including selected vendors")
    args = ap.parse_args()
    try:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.symbol_prefix):
            raise CheckError("invalid C symbol prefix")
        manifest = args.manifest or (args.ui / "product.json" if args.ui else None)
        if manifest is None:
            raise CheckError("--manifest is required")
        composition = load_composition(manifest, args.product_root or args.ui, args.asset_manifest)
        if args.list_inputs:
            print("\n".join(str(p) for p in composition["inputs"]))
            return 0
        if args.sources_json:
            print(json.dumps([dict(name=n, path=str(p) if p else None, source=s) for n, p, s in module_sources(composition, False)]))
            return 0
        assets, total = build(composition, args.max_gz, args.max_project_gz)
    except (CheckError, OSError, ValueError, KeyError, TypeError, ET.ParseError) as e:
        print(f"web UI check failed:\n{e}", file=sys.stderr)
        return 1
    if args.c_out:
        args.c_out.parent.mkdir(parents=True, exist_ok=True)
        write_c(args.c_out, assets, total, args.symbol_prefix, composition["metadata"])
    if args.out_dir:
        args.out_dir.mkdir(parents=True, exist_ok=True)
        for p, _, raw, _ in assets:
            target = args.out_dir / p.lstrip("/")
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(raw)
    sizes = ", ".join(f"{p.lstrip('/')} {len(gz)}" for p, _, _, gz in assets)
    print(f"web UI: {total} bytes gzipped ({sizes})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
