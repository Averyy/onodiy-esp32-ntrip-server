#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# CI's host-test job (.github/workflows/build.yml, job host-tests) on this
# machine: the image that job pins and the commands of its steps, both read
# from build.yml, so this script never drifts from CI (tools/ci/common.sh,
# tools/ci/ci_job.py). The host tests then build with Linux gcc and glibc and
# run there, which finds what a macOS clang build does not: gcc's own -Werror
# diagnostics (_FORTIFY_SOURCE format checks among them), GNU ld's archive
# order, and the test task's real stack size (macOS runs FreeRTOS tasks of the
# linux target on larger thread stacks).
#
# The container gets a copy of the working tree (tracked files and untracked
# ones git does not ignore) and builds inside itself: nothing in the checkout
# is written. Needs Docker, and network access for the image and the managed
# components.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/ci/host_tests_linux.sh [options]

Runs the host-tests job of .github/workflows/build.yml in its pinned
espressif/idf image on a copy of the working tree, then lists every compiler
warning or error in the project's own sources.

  --platform P      linux/amd64 (default: CI's runners) or linux/arm64 (native
                    on Apple silicon and faster, but char is unsigned there
                    and CI never builds it)
  --keep-going      when the build fails, carry on (ninja -k 0) to list every
                    error instead of stopping at the first, as CI does
  --allow-warnings  pass with compiler warnings in the project's own sources
                    (CI does; without this they fail the run, since IDF keeps
                    some, such as unused functions, warnings under -Werror)
  -h, --help        this text

Exit status 0 when the job passes (and no warnings, unless --allow-warnings).
EOF
}

CI_SCRIPT=host_tests_linux.sh
# shellcheck source=tools/ci/common.sh
. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

platform=linux/amd64
CI_KEEP_GOING_DIR=""
CI_ALLOW_WARNINGS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --platform) [ $# -ge 2 ] || ci_die "--platform needs a value"; platform="$2"; shift ;;
        --keep-going) CI_KEEP_GOING_DIR=test/host/build ;;
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
ci_check_host

CI_WITH_GIT=0 CI_KEEP_GOING_DIR="$CI_KEEP_GOING_DIR" CI_ALLOW_WARNINGS="$CI_ALLOW_WARNINGS" \
    ci_run_job host-tests "$platform" ""
