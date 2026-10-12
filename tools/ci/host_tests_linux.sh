#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# CI's host-test job (.github/workflows/build.yml, job host-tests) on this
# machine: the image that job pins and the commands of its steps, both read
# from build.yml, so this script never drifts from CI. The host tests then
# build with Linux gcc and glibc and run there, which finds what a macOS
# clang build does not: gcc's own -Werror diagnostics (_FORTIFY_SOURCE
# format checks among them), GNU ld's archive order, and the test task's real
# stack size (macOS runs FreeRTOS tasks of the linux target on larger thread
# stacks).
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

die() {
    echo "host_tests_linux.sh: $*" >&2
    exit 1
}

platform=linux/amd64
keep_going=0
allow_warnings=0
while [ $# -gt 0 ]; do
    case "$1" in
        --platform) [ $# -ge 2 ] || die "--platform needs a value"; platform="$2"; shift ;;
        --keep-going) keep_going=1 ;;
        --allow-warnings) allow_warnings=1 ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option $1" ;;
    esac
    shift
done
case "$platform" in
    linux/amd64|linux/arm64) ;;
    *) die "bad --platform $platform (linux/amd64 or linux/arm64)" ;;
esac

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
workflow="$root/.github/workflows/build.yml"
[ -f "$workflow" ] || die "no $workflow"
command -v docker >/dev/null || die "docker not found"
command -v git >/dev/null || die "git not found"

# The image: the container line of the host-tests job (the one place it is pinned).
image=$(awk '/^  [A-Za-z0-9_-]+:[[:space:]]*$/ { injob = ($1 == "host-tests:") }
             injob && $1 == "container:" { print $2; exit }' "$workflow")
[[ "$image" =~ ^espressif/idf@sha256:[0-9a-f]{64}$ ]] ||
    die "no espressif/idf@sha256 image pinned in build.yml's host-tests job (found '${image}')"

# The working tree as a checkout would have it, without the files a checkout
# lacks (ignored ones: build directories, sdkconfig, managed_components).
files() {
    git -C "$root" ls-files -z --cached --others --exclude-standard --deduplicate |
        while IFS= read -r -d '' f; do
            if [ -e "$root/$f" ] || [ -L "$root/$f" ]; then
                printf '%s\0' "$f"
            fi
        done
}

# Runs in the container. The steps are taken from build.yml there, with the
# image's own Python and PyYAML, and run as a job step runs them: each in its
# working directory with `bash --noprofile --norc -eo pipefail`. (read, not
# $(cat <<...): macOS bash 3.2 misparses quotes in a here-document inside $().)
IFS= read -r -d '' inner <<'INNER' || true
set -euo pipefail
mkdir -p /work /steps
tar -xf - -C /work --no-same-owner --warning=no-unknown-keyword
cd /work
( . "$IDF_PATH/export.sh" >/dev/null 2>&1
  python - .github/workflows/build.yml /steps <<'PY'
import os, shlex, sys
import yaml

job = yaml.safe_load(open(sys.argv[1]))["jobs"]["host-tests"]
out = sys.argv[2]
if "defaults" in job:
    sys.exit("build.yml: host-tests has job defaults, which this script does not apply")
n = 0
for st in job["steps"]:
    if "run" not in st:
        continue  # actions/checkout: the copy of the working tree stands in for it
    name = st.get("name", "run")
    if st.get("shell", "bash") != "bash":
        sys.exit(f"build.yml: step '{name}' uses shell {st['shell']}, not bash")
    env = dict(job.get("env") or {})
    env.update(st.get("env") or {})
    for k, v in env.items():
        if "${{" in str(v):
            sys.exit(f"build.yml: step '{name}' sets {k} from an expression, which this script cannot evaluate")
    n += 1
    with open(f"{out}/{n}.sh", "w") as f:
        f.write(st["run"])
    with open(f"{out}/{n}.env", "w") as f:
        f.writelines(f"export {k}={shlex.quote(str(v))}\n" for k, v in env.items())
    with open(f"{out}/{n}.meta", "w") as f:
        f.write(f"{st.get('working-directory', '.')}\n{name}\n")
if n == 0:
    sys.exit("build.yml: host-tests has no run steps")
PY
)

rc=0
failed_dir=""
for script in $(ls /steps/*.sh | sort -t/ -k3 -n); do
    meta="${script%.sh}.meta"
    dir=$(sed -n 1p "$meta")
    name=$(sed -n 2p "$meta")
    echo "=== step: $name (in $dir)"
    if ! (cd "/work/$dir" && . "${script%.sh}.env" &&
          GITHUB_WORKSPACE=/work CI=true bash --noprofile --norc -eo pipefail "$script") 2>&1 |
         tee -a /work/.steps.log; then
        rc=1
        failed_dir="/work/$dir"
        break
    fi
done

if [ "$rc" != 0 ] && [ "$KEEP_GOING" = 1 ] && [ -f "$failed_dir/build/build.ninja" ] &&
   grep -q '^ninja: build stopped' /work/.steps.log; then
    echo "=== the build failed: continuing with ninja -k 0 for the full list"
    (cd "$failed_dir/build" && ninja -k 0 all) 2>&1 | tee -a /work/.steps.log || true
fi

# Diagnostics in the project's own sources (IDF's are not ours to fix).
grep -E '^/work/[^:]+:[0-9]+(:[0-9]+)?: (warning|error): ' /work/.steps.log |
    grep -v '^/work/[^:]*/managed_components/' | sort -u > /work/.diag || true
nw=$(grep -c ': warning: ' /work/.diag || true)
ne=$(grep -c ': error: ' /work/.diag || true)
echo "=== compiler diagnostics in the project's sources: $ne error(s), $nw warning(s)"
sed 's#^/work/##' /work/.diag
if [ "$rc" = 0 ] && [ "$nw" != 0 ] && [ "$ALLOW_WARNINGS" != 1 ]; then
    echo "=== the job passed, but with warnings (--allow-warnings accepts them)"
    rc=1
fi
[ "$rc" = 0 ] && echo "=== host-tests: passed" || echo "=== host-tests: FAILED"
exit "$rc"
INNER

echo "host_tests_linux.sh: $image ($platform)"
files | COPYFILE_DISABLE=1 tar -C "$root" -cf - --null -T - |
    docker run --rm -i --init --platform "$platform" --entrypoint bash \
        -e KEEP_GOING="$keep_going" -e ALLOW_WARNINGS="$allow_warnings" \
        "$image" -c "$inner"
