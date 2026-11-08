#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Builds examples/external-receiver as another product would
# (docs/component-reuse.md): copied out of this repository, pointed at it
# only through STATION_DIR, for both station layouts (16 MB and 8 MB), with
# -Werror on its own code, and once more on 16 MB with each updater
# (STATION_UPDATER=ON, STATION_APP_UPDATE=ON). Then checks that each build took exactly the
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

base="board gnss_framing gnss_link gnss_obs gnss_types leds settings um98x_ctl"
with_updater="board diag gnss_framing gnss_link gnss_obs gnss_types leds pkg_store settings um98x_ctl um98x_flash"
with_app_update="board diag fw_update gnss_framing gnss_link gnss_obs gnss_types ledger leds lifecycle network pkg_store settings tcp_diag um98x_ctl"

# The two layouts, then the 16 MB one with each updater.
for variant in 16MB 8MB 16MB-updater 16MB-app-update; do
    layout=${variant%%-*}
    updater=OFF
    app_update=OFF
    supported=$base
    case "$variant" in
    *-updater)
        updater=ON
        supported=$with_updater
        ;;
    *-app-update)
        app_update=ON
        supported=$with_app_update
        ;;
    esac
    b="$out/build-$variant"
    rm -rf "$b"
    args=(-C "$app" -B "$b" -D SDKCONFIG="$b/sdkconfig" -D STATION_DIR="$repo" -D STATION_FLASH="$layout"
          -D STATION_UPDATER="$updater" -D STATION_APP_UPDATE="$app_update")
    idf.py "${args[@]}" set-target esp32s3 2>&1 | tee "$out/set-target-$variant.log"
    idf.py "${args[@]}" build 2>&1 | tee "$out/build-$variant.log"
    if grep -E "unknown kconfig symbol|but is not visible" "$out/set-target-$variant.log" "$out/build-$variant.log"; then
        die "$variant: a Kconfig symbol is unknown or ignored"
    fi
    grep -q "^CONFIG_ESPTOOLPY_FLASHSIZE_$layout=y" "$b/sdkconfig" || die "$variant: the flash size did not come from the layout"
    if grep -q "^CONFIG_GNSS_LINK_BENCH_HOOKS=y" "$b/sdkconfig"; then
        die "$variant: the link's bench hooks are on (they need the station's bench console)"
    fi
    # Every component the build took from this repository's components/, by
    # its directory (the app's own main may sit anywhere, also in the tree).
    took=$("$py" -I - "$b/project_description.json" "$repo/components" <<'PY'
import json
import os
import sys

desc, comps = sys.argv[1], sys.argv[2]
info = json.load(open(desc))["build_component_info"]
names = []
for name, c in info.items():
    # By file identity, not by spelling: a case-insensitive file system
    # (macOS) may give the same directory in another case.
    parent = os.path.dirname(os.path.normpath(c.get("dir") or "/"))
    if os.path.isdir(parent) and os.path.samefile(parent, comps):
        names.append(name)
print(" ".join(sorted(names)))
PY
)
    [ "$took" = "$supported" ] || die "$variant: components from this repository: '$took', want '$supported'"
    echo "external app ($variant): built with exactly: $took"
done
