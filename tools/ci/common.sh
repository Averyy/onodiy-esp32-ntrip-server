# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Sourced by tools/ci/host_tests_linux.sh and tools/ci/qemu_linux.sh: one job
# of .github/workflows/build.yml in the image that job pins (started with the
# container options the job gives, as the runner starts it), on a copy of the
# working tree, its steps read from build.yml and run there by
# tools/ci/ci_job.py, so neither script drifts from CI. The checkout is never
# written; what the job uploads is copied out at the end. Needs Docker and git,
# and network access for the image, the managed components and (for jobs that
# set up uv) uv's release.

ci_die() {
    echo "${CI_SCRIPT:-ci}: $*" >&2
    exit 1
}

ci_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
ci_workflow="$ci_root/.github/workflows/build.yml"
ci_options=()   # ci_container_options

ci_check_host() {
    [ -f "$ci_workflow" ] || ci_die "no $ci_workflow"
    command -v docker >/dev/null || ci_die "docker not found"
    command -v git >/dev/null || ci_die "git not found"
}

# The image of job $1 (the one place it is pinned): its container line, or the
# image line under it when the container is a mapping (image, options).
ci_image() {
    local image
    image=$(awk -v job="$1:" '
        /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { injob = ($1 == job); block = 0; next }
        !injob || /^[[:space:]]*(#|$)/ { next }
        { indent = match($0, /[^ ]/) - 1 }
        block && indent <= block_indent { block = 0 }
        $1 == "container:" {
            if (NF >= 2 && $2 !~ /^#/) { print $2; exit }
            block = 1; block_indent = indent; next
        }
        block && $1 == "image:" { print $2; exit }' "$ci_workflow")
    [[ "$image" =~ ^espressif/idf@sha256:[0-9a-f]{64}$ ]] ||
        ci_die "no espressif/idf@sha256 image pinned in build.yml's $1 job (found '${image}')"
    printf '%s\n' "$image"
}

# The working tree as a checkout would have it (tracked files and untracked
# ones git does not ignore; no build directories, sdkconfig or
# managed_components), NUL-separated. With $1 = git, the repository itself
# too (.git), for a job whose checkout fetches the history (git describe).
ci_files() {
    git -C "$ci_root" ls-files -z --cached --others --exclude-standard --deduplicate |
        while IFS= read -r -d '' f; do
            if [ -e "$ci_root/$f" ] || [ -L "$ci_root/$f" ]; then
                printf '%s\0' "$f"
            fi
        done
    if [ "${1:-}" = git ]; then
        printf '%s\0' .git
    fi
}

# Runs in the container (bash -c). The steps come from build.yml, read by
# ci_job.py with the image's ESP-IDF Python (which has PyYAML), and run from
# the image's own environment, as a runner starts each step. Then every
# compiler warning or error in the project's own sources is listed (IDF's are
# not ours to fix). (read, not $(cat <<...): macOS bash 3.2 misparses quotes in
# a here-document inside $().)
IFS= read -r -d '' ci_inner <<'INNER' || true
set -euo pipefail
mkdir -p /work /artifacts
tar -xf - -C /work --no-same-owner --warning=no-unknown-keyword
cd /work
py=$(ls "$IDF_TOOLS_PATH"/python_env/*/bin/python 2>/dev/null | head -n 1)
[ -x "$py" ] || { echo "no ESP-IDF Python environment in the image" >&2; exit 1; }
rc=0
"$py" tools/ci/ci_job.py .github/workflows/build.yml "$CI_JOB" --log /work/.steps.log --artifacts /artifacts || rc=$?

if [ "$rc" != 0 ] && [ -n "$KEEP_GOING_DIR" ] && [ -f "/work/$KEEP_GOING_DIR/build.ninja" ] &&
   grep -q '^ninja: build stopped' /work/.steps.log; then
    echo "=== the build failed: continuing with ninja -k 0 for the full list"
    (cd "/work/$KEEP_GOING_DIR" && ninja -k 0 all) 2>&1 | tee -a /work/.steps.log || true
fi

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
cp /work/.steps.log /artifacts/steps.log 2>/dev/null || true
[ "$rc" = 0 ] && echo "=== $CI_JOB: passed" || echo "=== $CI_JOB: FAILED"
exit "$rc"
INNER

# Runs in the job's image (bash -c) before the job: ci_job.py --container
# prints the image and the container's options as docker arguments (split as
# the runner splits them), read with the image's ESP-IDF Python, so the host
# needs no Python of its own and the job is checked before the long run.
IFS= read -r -d '' ci_container_inner <<'INNER' || true
set -euo pipefail
d=$(mktemp -d)
tar -xf - -C "$d" --no-same-owner --warning=no-unknown-keyword
py=$(ls "$IDF_TOOLS_PATH"/python_env/*/bin/python 2>/dev/null | head -n 1)
[ -x "$py" ] || { echo "no ESP-IDF Python environment in the image" >&2; exit 1; }
"$py" "$d/tools/ci/ci_job.py" "$d/.github/workflows/build.yml" "$CI_JOB" --container
INNER

# The container options of job $1 in image $2 on platform $3, as docker run
# arguments, into the array ci_options. The image ci_job.py reads must be the
# one ci_image read.
ci_container_options() {
    local job="$1" image="$2" platform="$3" file rc=0 a
    local got=()
    file=$(mktemp)
    COPYFILE_DISABLE=1 tar -C "$ci_root" -cf - tools/ci/ci_job.py .github/workflows/build.yml |
        docker run -i --rm --platform "$platform" --entrypoint bash -e CI_JOB="$job" \
            "$image" -c "$ci_container_inner" > "$file" || rc=$?
    while IFS= read -r -d '' a; do
        got+=("$a")
    done < "$file"
    rm -f "$file"
    [ "$rc" = 0 ] || ci_die "job $job: ci_job.py --container failed in $image (exit $rc)"
    [ "${got[0]:-}" = "$image" ] ||
        ci_die "job $job: ci_job.py reads the image as '${got[0]:-}', this script as '$image'"
    ci_options=()
    if [ "${#got[@]}" -gt 1 ]; then
        ci_options=("${got[@]:1}")
    fi
}

# Run job $1 on platform $2; what it uploads (and steps.log, every step's
# output) lands in $3 (emptied first; "" = not kept). Options from the
# environment: CI_WITH_GIT=1 copies .git too, CI_KEEP_GOING_DIR names the
# build directory to finish with ninja -k 0 after a failed build,
# CI_ALLOW_WARNINGS=1 passes a job with compiler warnings in our sources,
# CI_CPUS=N gives the container N CPUs (0 or unset: all Docker has).
# The container is removed on every way out (Ctrl-C too).
ci_run_job() {
    local job="$1" platform="$2" out="$3" image name rc=0
    local limits=() cpus_text=""
    if [ -n "${CI_CPUS:-}" ] && [ "${CI_CPUS}" != 0 ]; then
        limits=(--cpus "$CI_CPUS")
        cpus_text=", $CI_CPUS CPUs"
    fi
    image=$(ci_image "$job")
    ci_container_options "$job" "$image" "$platform"
    if [ "${#ci_options[@]}" -gt 0 ]; then
        cpus_text="$cpus_text, options$(printf ' %q' "${ci_options[@]}")"
    fi
    name="ci-$job-$$-$RANDOM"
    echo "${CI_SCRIPT:-ci}: job $job in $image ($platform$cpus_text)"
    # shellcheck disable=SC2064  # the name is fixed now
    trap "docker rm -f '$name' >/dev/null 2>&1 || true" EXIT
    trap 'exit 130' INT
    trap 'exit 143' TERM
    ci_files "$([ "${CI_WITH_GIT:-0}" = 1 ] && echo git)" |
        COPYFILE_DISABLE=1 tar -C "$ci_root" -cf - --null -T - |
        docker run -i --init --name "$name" --platform "$platform" ${limits[@]+"${limits[@]}"} \
            ${ci_options[@]+"${ci_options[@]}"} --entrypoint bash \
            -e CI_JOB="$job" -e KEEP_GOING_DIR="${CI_KEEP_GOING_DIR:-}" \
            -e ALLOW_WARNINGS="${CI_ALLOW_WARNINGS:-0}" \
            "$image" -c "$ci_inner" || rc=$?
    if [ -n "$out" ]; then
        rm -rf "$out"
        mkdir -p "$out"
        if docker cp "$name:/artifacts/." "$out" >/dev/null 2>&1; then
            echo "${CI_SCRIPT:-ci}: job $job: logs and uploads in $out"
        else
            echo "${CI_SCRIPT:-ci}: job $job: nothing to copy out of the container" >&2
        fi
    fi
    docker rm -f "$name" >/dev/null 2>&1 || true
    trap - EXIT INT TERM
    return "$rc"
}
