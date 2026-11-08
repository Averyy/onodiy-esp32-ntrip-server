#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
# Copy outside the checkout; build both layouts and optionally QEMU Ethernet.
# Usage: tools/ci/external_web.sh [out-directory] [--qemu]
set -euo pipefail
repo=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
out=${TMPDIR:-/tmp}/onodiy-external-web
qemu=0
for arg in "$@"; do
    if [ "$arg" = --qemu ]; then qemu=1; else out=$arg; fi
done
die() { echo "external_web.sh: $*" >&2; exit 1; }
[ -n "${IDF_PATH:-}" ] && command -v idf.py >/dev/null || die "source ESP-IDF 6.1 export.sh first"
command -v uv >/dev/null || die "uv is required"
mkdir -p "$out"
out=$(cd "$out" && pwd -P)
case "$out/" in "$repo/"*) die "output must be outside the repository" ;; esac
app="$out/app"
mkdir -p "$out/logs"
rm -rf "$app"
cp -R "$repo/examples/external-web" "$app"
variants=(16MB 8MB)
if [ "$qemu" = 1 ]; then variants+=(16MB-qemu); fi
for variant in "${variants[@]}"; do
    layout=${variant%%-*}
    emulated=OFF
    if [ "$variant" = 16MB-qemu ]; then emulated=ON; fi
    b="$out/build-$variant"
    rm -rf "$b"
    args=(-C "$app" -B "$b" -D SDKCONFIG="$b/sdkconfig" -D STATION_DIR="$repo"
          -D STATION_FLASH="$layout" -D EXTERNAL_WEB_QEMU="$emulated")
    idf.py "${args[@]}" set-target esp32s3 2>&1 | tee "$out/logs/set-target-$variant.log"
    idf.py "${args[@]}" build 2>&1 | tee "$out/logs/build-$variant.log"
    if grep -Ei "unknown kconfig symbol|but is not visible|attempt to assign.*undefined symbol" \
        "$out/logs/set-target-$variant.log" "$out/logs/build-$variant.log"; then
        die "$variant: unknown or ignored Kconfig setting"
    fi
    grep -q "^CONFIG_ESPTOOLPY_FLASHSIZE_$layout=y" "$b/sdkconfig" || die "$variant: incorrect flash layout"
    uv run --no-project python "$repo/tools/ci/check_external_web.py" "$b/project_description.json" "$repo"
    echo "external web ($variant): built and component closure checked"
done
