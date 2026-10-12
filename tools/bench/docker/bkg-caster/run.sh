#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Run the local BKG NtripCaster test peer (build it first with build.sh).
# By default the caster is published on 127.0.0.1 only: nothing outside this
# computer reaches it (QEMU's user network still does, as 10.0.2.2). --lan
# publishes it on every interface, for a station board on the same LAN.
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/bench/docker/bkg-caster/run.sh [--port N] [--name NAME] [--lan] [--print] [--detach | --stop]

  --port N     host port (default 2101)
  --name NAME  container name (default onocoydiy-bench-bkg-caster)
  --lan        publish on 0.0.0.0 (every interface) instead of 127.0.0.1, so a
               station board on your LAN reaches it; so does everyone else on
               the LAN: use the throwaway test passwords only
  --print      print the docker command and stop (runs nothing; no Docker needed)
  --detach     run in the background (default: in the foreground, Ctrl-C stops it)
  --stop       stop the container of that name
Sources: SOURCE bench-src /BENCH (NTRIP 1.0). Clients: GET /BENCH (open).
The caster's log: docker exec NAME tail -f /usr/local/ntripcaster/logs/ntripcaster.log
EOF
}

port=2101
name=onocoydiy-bench-bkg-caster
image=${BKG_CASTER_IMAGE:-onocoydiy-bench-bkg-caster}
mode=foreground
bind=127.0.0.1
print=0
while [ $# -gt 0 ]; do
    case "$1" in
        --port) [ $# -ge 2 ] || { usage >&2; exit 2; }; port=$2; shift ;;
        --name) [ $# -ge 2 ] || { usage >&2; exit 2; }; name=$2; shift ;;
        --lan) bind=0.0.0.0 ;;
        --print) print=1 ;;
        --detach) mode=detach ;;
        --stop) mode=stop ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; exit 2 ;;
    esac
    shift
done
[[ "$port" =~ ^[0-9]+$ ]] && [ "$port" -ge 1 ] && [ "$port" -le 65535 ] || { echo "run.sh: bad --port" >&2; exit 2; }

if [ "$mode" = stop ]; then
    args=(rm -f "$name")
elif [ "$mode" = detach ]; then
    args=(run -d --rm --name "$name" -p "$bind:$port:2101" "$image")
else
    args=(run --rm --name "$name" -p "$bind:$port:2101" "$image")
fi
if [ "$bind" != 127.0.0.1 ] && [ "$mode" != stop ]; then
    echo "run.sh: reachable from your LAN: use the throwaway test passwords only" >&2
fi
if [ "$print" = 1 ]; then
    printf 'docker'
    printf ' %q' "${args[@]}"
    printf '\n'
    exit 0
fi

command -v docker >/dev/null || { echo "run.sh: docker not found" >&2; exit 1; }
if [ "$mode" = stop ]; then
    docker "${args[@]}" >/dev/null 2>&1 && echo "stopped $name" || echo "no container $name"
    exit 0
fi
docker image inspect "$image" >/dev/null 2>&1 || { echo "run.sh: no image $image: run build.sh first" >&2; exit 1; }
echo "BKG NtripCaster on $bind:$port (container $name); mountpoint BENCH, source password bench-src"
if [ "$mode" = detach ]; then
    docker "${args[@]}"
else
    exec docker "${args[@]}"
fi
