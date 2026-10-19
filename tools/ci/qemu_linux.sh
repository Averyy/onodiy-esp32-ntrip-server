#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# CI's QEMU jobs (.github/workflows/build.yml: qemu-smoke, the smoke test and
# the end-to-end run; qemu-bench-console, the bench overlay's command line) on
# this machine: the image each job pins, started with the job's container
# options (qemu-smoke bounds TCP send buffers for e2e.py's stuck rover), and
# the steps it runs, all read from build.yml (tools/ci/common.sh,
# tools/ci/ci_job.py), so this script never drifts from CI. A QEMU check that
# passes on a Mac can fail in CI's Linux container (another QEMU build of the
# same release, another host network stack, other timing); this runs it there
# before a push.
#
# The container gets a copy of the working tree with the repository's history
# (the jobs' checkout fetches it: git describe names the build) and builds
# inside itself: nothing in the checkout is written. What a job uploads (its
# build log and the checks' output directories) is copied to --out at the end,
# with steps.log, every step's output. Needs Docker, and network access for
# the image, the managed components and uv's release.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/ci/qemu_linux.sh [options]

Runs CI's QEMU jobs of .github/workflows/build.yml in their pinned
espressif/idf image on a copy of the working tree, one after the other, and
lists every compiler warning or error in the project's own sources.

  --job J           smoke (qemu-smoke: build, smoke.py, e2e.py), bench-console
                    (qemu-bench-console: bench overlay, bench_console.py) or
                    all (default: both)
  --platform P      linux/amd64 (default: CI's runners; emulated on Apple
                    silicon, so slower) or linux/arm64 (native there; CI never
                    runs it)
  --cpus N          CPUs for the container (default 2: GitHub's standard
                    Linux runner for a private repository has 2, for a
                    public one 4; 0 = all Docker has). The QEMU checks are
                    timing-sensitive, so CI's CPU count is the default
  --out DIR         where each job's uploads land, in DIR/<job>/ (default
                    build-ci/; emptied first)
  --allow-warnings  pass with compiler warnings in the project's own sources
  -h, --help        this text

Exit status 0 when every job run passes (and no warnings, unless
--allow-warnings).
EOF
}

CI_SCRIPT=qemu_linux.sh
# shellcheck source=tools/ci/common.sh
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

platform=linux/amd64
which=all
out="$ci_root/build-ci"
cpus=2
CI_ALLOW_WARNINGS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --job) [ $# -ge 2 ] || ci_die "--job needs a value"; which="$2"; shift ;;
        --platform) [ $# -ge 2 ] || ci_die "--platform needs a value"; platform="$2"; shift ;;
        --cpus) [ $# -ge 2 ] || ci_die "--cpus needs a number"; cpus="$2"; shift ;;
        --out) [ $# -ge 2 ] || ci_die "--out needs a directory"; out="$2"; shift ;;
        --allow-warnings) CI_ALLOW_WARNINGS=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; ci_die "unknown option $1" ;;
    esac
    shift
done
case "$platform" in
    linux/amd64|linux/arm64) ;;
    *) ci_die "bad --platform $platform (linux/amd64 or linux/arm64)" ;;
esac
[[ "$cpus" =~ ^[0-9]+$ ]] || ci_die "bad --cpus $cpus (a whole number; 0 = no limit)"
case "$which" in
    smoke) jobs=(qemu-smoke) ;;
    bench-console) jobs=(qemu-bench-console) ;;
    all) jobs=(qemu-smoke qemu-bench-console) ;;
    *) ci_die "bad --job $which (smoke, bench-console or all)" ;;
esac
ci_check_host
mkdir -p "$out"
out=$(cd "$out" && pwd)

failed=()
for job in "${jobs[@]}"; do
    if ! CI_WITH_GIT=1 CI_KEEP_GOING_DIR="" CI_ALLOW_WARNINGS="$CI_ALLOW_WARNINGS" CI_CPUS="$cpus" \
         ci_run_job "$job" "$platform" "$out/$job"; then
        failed+=("$job")
    fi
done
if [ "${#failed[@]}" -gt 0 ]; then
    echo "qemu_linux.sh: FAILED: ${failed[*]} (logs in $out)"
    exit 1
fi
echo "qemu_linux.sh: passed: ${jobs[*]} (logs in $out)"
