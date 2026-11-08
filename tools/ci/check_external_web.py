#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
"""Check resolved component directories AND dependency edges, not link output."""
import json
import os
from pathlib import Path
import sys

SUPPORTED = {"web_shell", "settings", "diag", "lifecycle", "network", "ledger", "tcp_diag"}
FORBIDDEN = {"web", "station", "ntrip_server", "local_caster", "rtcm_hub", "monitor",
             "onocoy_report", "health", "um98x_ctl", "um98x_flash", "gnss_link", "gnss_obs",
             "gnss_types", "gnss_framing", "board", "leds", "fw_update", "pkg_store"}


def validate(description, repo):
    components = description["build_component_info"]
    root = Path(repo).resolve() / "components"
    took = set()
    for name, info in components.items():
        directory = Path(info.get("dir") or "/").resolve()
        if directory.parent.is_dir() and os.path.samefile(directory.parent, root):
            took.add(directory.name)
            if name != directory.name:
                raise ValueError(f"component alias {name}: {directory}")
        # IDF records public/private requirements and managed dependencies here.
        # Search all nested values so optional/indirect edges cannot hide a service.
        def strings(value):
            if isinstance(value, str):
                yield value
            elif isinstance(value, dict):
                for k, v in value.items():
                    yield k
                    yield from strings(v)
            elif isinstance(value, list):
                for v in value:
                    yield from strings(v)
        for value in [name, *strings(info)]:
            dep = value.removeprefix("__idf_").removeprefix("idf::")
            if dep in FORBIDDEN or Path(value).name in FORBIDDEN:
                raise ValueError(f"forbidden component/dependency {value} in {name}")
    if took != SUPPORTED:
        raise ValueError(f"repository closure {sorted(took)}, expected {sorted(SUPPORTED)}")
    return sorted(took)


if __name__ == "__main__":
    try:
        print("external web closure:", " ".join(validate(json.loads(Path(sys.argv[1]).read_text()), sys.argv[2])))
    except (ValueError, KeyError) as error:
        sys.exit(str(error))
