#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# The framing component's sources, unchanged, built with the host compiler
# outside ESP-IDF and run on a recorded RTCM3 file, as another product's
# host tests do (docs/component-reuse.md).
#
#   tools/ci/external_host_framing.sh [<out dir>]
#
# Needs a C compiler (cc). The expected count is the fixture's frames_ok in
# test/fixtures/oracle/index.json (pyrtcm's decode).
set -euo pipefail

repo=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-${TMPDIR:-/tmp}/onodiy-external-app}
mkdir -p "$out"
fr="$repo/components/gnss_framing"
cc -std=c11 -Wall -Wextra -Werror -Wstrict-prototypes -I"$fr/include" \
    "$repo/examples/external-receiver/host/framing_check.c" "$fr/gnss_framing.c" "$fr/gnss_rtcm3.c" \
    "$fr/gnss_rtcm_scan.c" \
    -o "$out/framing_check"
"$out/framing_check" "$repo/test/fixtures/rtcm/centipede_LAX_30ep.rtcm3" 211
