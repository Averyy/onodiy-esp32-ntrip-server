#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Build the local BKG NtripCaster test peer image (see README.md here).
#   tools/bench/docker/bkg-caster/build.sh [extra docker build options]
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
image=${BKG_CASTER_IMAGE:-onodiy-bench-bkg-caster}
command -v docker >/dev/null || { echo "build.sh: docker not found" >&2; exit 1; }
docker build -t "$image" "$@" "$here"
echo "built image $image"
