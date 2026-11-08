#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Compatibility entry point for the shared UI bundler."""
import pathlib
import runpy

if __name__ == "__main__":
    runpy.run_path(str(pathlib.Path(__file__).resolve().parents[2] / "web_shell/tools/ui_bundle.py"), run_name="__main__")
