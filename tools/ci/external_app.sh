#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Builds examples/external-receiver as another product would
# (docs/component-reuse.md): copied out of this repository, pointed at it
# only through STATION_DIR, for both station layouts (16 MB and 8 MB), with
# -Werror on its own code. Then checks that the build took exactly the
# supported components from this repository and nothing else of it, and
# that no Kconfig symbol was unknown or ignored.
#
#   tools/ci/external_app.sh [<out dir>]     (ESP-IDF environment required)
#
# <out dir> (default: onodiy-external-app under $TMPDIR or /tmp, outside the
# repository) gets the copied app and one build directory and sdkconfig per
# layout.
set -euo pipefail

die() {
    echo "external_app.sh: $*" >&2
    exit 1
}

repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=${1:-${TMPDIR:-/tmp}/onodiy-external-app}
[ -n "${IDF_PATH:-}" ] || die "no ESP-IDF environment: run . \$IDF_PATH/export.sh first"
py="${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/python}"
[ -n "$py" ] && [ -x "$py" ] || py=$(command -v python3 || command -v python) || die "no Python found"

mkdir -p "$out"
out=$(cd "$out" && pwd)
app="$out/app"
rm -rf "$app"
cp -R "$repo/examples/external-receiver" "$app"

supported="board gnss_framing gnss_link gnss_obs gnss_types leds settings um98x_ctl"

for layout in 16MB 8MB; do
    b="$out/build-$layout"
    rm -rf "$b"
    args=(-C "$app" -B "$b" -D SDKCONFIG="$b/sdkconfig" -D STATION_DIR="$repo" -D STATION_FLASH="$layout")
    idf.py "${args[@]}" set-target esp32s3 2>&1 | tee "$out/set-target-$layout.log"
    idf.py "${args[@]}" build 2>&1 | tee "$out/build-$layout.log"
    if grep -E "unknown kconfig symbol|but is not visible" "$out/set-target-$layout.log" "$out/build-$layout.log"; then
        die "$layout: a Kconfig symbol is unknown or ignored"
    fi
    grep -q "^CONFIG_ESPTOOLPY_FLASHSIZE_$layout=y" "$b/sdkconfig" || die "$layout: the flash size did not come from the layout"
    if grep -q "^CONFIG_GNSS_LINK_BENCH_HOOKS=y" "$b/sdkconfig"; then
        die "$layout: the link's bench hooks are on (they need the station's bench console)"
    fi
    # Every component the build took from this repository's components/, by
    # its directory (the app's own main may sit anywhere, also in the tree).
    took=$("$py" -I - "$b/project_description.json" "$repo/components" <<'PY'
import json
import os
import sys

desc, comps = sys.argv[1], os.path.realpath(sys.argv[2])
info = json.load(open(desc))["build_component_info"]
names = []
for name, c in info.items():
    d = os.path.realpath(c.get("dir") or "")
    if d.startswith(comps + os.sep):
        names.append(name)
print(" ".join(sorted(names)))
PY
)
    [ "$took" = "$supported" ] || die "$layout: components from this repository: '$took', want '$supported'"
    echo "external app ($layout): built with exactly: $took"
done
