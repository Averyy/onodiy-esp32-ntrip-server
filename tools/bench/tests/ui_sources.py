# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Resolve UI sources with the production composition loader, in manifest order."""
import importlib.util
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "components/web/ui/product.json"
_spec = importlib.util.spec_from_file_location("ui_bundle", ROOT / "components/web_shell/tools/ui_bundle.py")
bundler = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bundler)
COMPOSITION = bundler.load_composition(MANIFEST)
MODULES = list(bundler.module_sources(COMPOSITION))
HARNESS_MODULES = json.dumps([
    dict(path=str(path) if path else None, source=source if path is None else None)
    for module, (_, path, source) in zip(COMPOSITION["modules"], MODULES)
    if not module.get("entrypoint")
])
MODULE_FILES = [path for _, path, _ in MODULES if path]
CSS = next(asset["path"] for asset in COMPOSITION["assets"] if asset["url"] == "/app.css")


def module_path(filename):
    return next(path for path in MODULE_FILES if path.name == filename)
