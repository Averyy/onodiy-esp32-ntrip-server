#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# Build the station firmware for Espressif's QEMU (sdkconfig.defaults plus the
# sdkconfig.qemu overlay, into build-qemu/) and run it. See tools/qemu/README.md.
#
# Needs the ESP-IDF v6.1 environment (. $IDF_PATH/export.sh) and qemu-xtensa
# (python $IDF_PATH/tools/idf_tools.py install qemu-xtensa).
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/qemu/run.sh [options]

Builds the QEMU development image into build-qemu/ and runs it with the web
server forwarded to the host and UART1 (the receiver link) on a host socket.
Every run starts from a freshly written flash image: empty NVS (no admin
password, default settings).

  --no-build          run the image build-qemu/ already holds
  --build-only        build, then stop (no QEMU)
  --headless LOG      no terminal: the console (UART0) is written to LOG and
                      QEMU runs until this script gets Ctrl-C/SIGTERM or
                      --seconds is up (QEMU is always stopped on the way out)
  --seconds N         stop QEMU after N seconds (with --headless)
  --http-port N       host port forwarded to the web server's port 80
                      (default 8080; the UI is at http://localhost:N/)
  --uart1 MODE        where UART1 goes (default listen:5556):
                        listen[:PORT]   QEMU listens on 127.0.0.1:PORT; connect any time
                                        (bytes sent while nothing is connected are lost,
                                        like a wire that is not plugged in)
                        connect:PORT    QEMU connects to a program already listening on
                                        127.0.0.1:PORT (retried every second), so that
                                        program sees every byte from the first boot
                        pty             a pseudo-terminal; QEMU prints its path
                        file:PATH       only what the firmware sends, written to PATH
                        none            not connected (a board without a receiver)
  -h, --help          this text

Interactive runs put the console on this terminal: Ctrl-A X quits QEMU,
Ctrl-A C switches to the QEMU monitor.
EOF
}

die() {
    echo "run.sh: $*" >&2
    exit 1
}

build=1
run=1
headless=""
seconds=""
http_port=8080
uart1="listen:5556"
while [ $# -gt 0 ]; do
    case "$1" in
        --no-build) build=0 ;;
        --build-only) run=0 ;;
        --headless) [ $# -ge 2 ] || die "--headless needs a log file"; headless="$2"; shift ;;
        --seconds) [ $# -ge 2 ] || die "--seconds needs a number"; seconds="$2"; shift ;;
        --http-port) [ $# -ge 2 ] || die "--http-port needs a port"; http_port="$2"; shift ;;
        --uart1) [ $# -ge 2 ] || die "--uart1 needs a mode"; uart1="$2"; shift ;;
        -h|--help) usage; exit 0 ;;
        *) usage >&2; die "unknown option $1" ;;
    esac
    shift
done
[[ "$http_port" =~ ^[0-9]+$ ]] && [ "$http_port" -ge 1 ] && [ "$http_port" -le 65535 ] || die "bad --http-port"
[ -z "$seconds" ] || [[ "$seconds" =~ ^[0-9]+$ ]] || die "bad --seconds"
[ -z "$seconds" ] || [ -n "$headless" ] || die "--seconds needs --headless"
[ "$build" = 1 ] || [ "$run" = 1 ] || die "--no-build and --build-only together do nothing"

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
bdir="$root/build-qemu"

[ -n "${IDF_PATH:-}" ] && command -v idf.py >/dev/null ||
    die "no ESP-IDF environment: run . \$IDF_PATH/export.sh (ESP-IDF v6.1) first"
# IDF's own Python (esptool, idf_py_actions): a `uv run` or venv python may come first in PATH.
idf_python="${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/python}"
[ -n "$idf_python" ] && [ -x "$idf_python" ] || idf_python=$(command -v python3 || command -v python) ||
    die "no Python found"
command -v qemu-system-xtensa >/dev/null ||
    die "qemu-system-xtensa not found: python \$IDF_PATH/tools/idf_tools.py install qemu-xtensa, then . \$IDF_PATH/export.sh again"

# UART1: the second -serial is UART1 in Espressif's esp32s3 machine (UART0 is
# the first), whatever pins the firmware routes it to.
case "$uart1" in
    listen) uart1_dev=(-chardev socket,id=uart1,host=127.0.0.1,port=5556,server=on,wait=off); uart1_text="tcp 127.0.0.1:5556 (QEMU listens)" ;;
    listen:*) p=${uart1#listen:}; [[ "$p" =~ ^[0-9]+$ ]] || die "bad --uart1 $uart1"
        uart1_dev=(-chardev "socket,id=uart1,host=127.0.0.1,port=$p,server=on,wait=off"); uart1_text="tcp 127.0.0.1:$p (QEMU listens)" ;;
    connect:*) p=${uart1#connect:}; [[ "$p" =~ ^[0-9]+$ ]] || die "bad --uart1 $uart1"
        uart1_dev=(-chardev "socket,id=uart1,host=127.0.0.1,port=$p,server=off,reconnect-ms=1000"); uart1_text="tcp 127.0.0.1:$p (QEMU connects)" ;;
    pty) uart1_dev=(-chardev pty,id=uart1); uart1_text="a pseudo-terminal (path printed by QEMU below)" ;;
    file:?*) f=${uart1#file:}; uart1_dev=(-chardev "file,id=uart1,path=$f"); uart1_text="file $f (firmware to host only)" ;;
    none) uart1_dev=(-chardev null,id=uart1); uart1_text="not connected" ;;
    *) die "bad --uart1 $uart1 (listen[:PORT], connect:PORT, pty, file:PATH, none)" ;;
esac

if [ "$build" = 1 ]; then
    # The overlay applies to a fresh configuration only: start one again when
    # either defaults file is newer than build-qemu/sdkconfig.
    if [ -f "$bdir/sdkconfig" ] &&
       { [ "$root/sdkconfig.defaults" -nt "$bdir/sdkconfig" ] || [ "$root/sdkconfig.qemu" -nt "$bdir/sdkconfig" ]; }; then
        echo "run.sh: sdkconfig.defaults or sdkconfig.qemu changed: configuring build-qemu again"
        rm -f "$bdir/sdkconfig"
    fi
    (cd "$root" && "$idf_python" "$IDF_PATH/tools/idf.py" -B "$bdir" -D SDKCONFIG="$bdir/sdkconfig" \
        -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.qemu" build)
fi
[ -f "$bdir/flash_args" ] || die "no build in build-qemu/: run without --no-build"
grep -q '^#define CONFIG_NETWORK_QEMU_ETHERNET 1' "$bdir/config/sdkconfig.h" ||
    die "build-qemu/ is not a QEMU build (CONFIG_NETWORK_QEMU_ETHERNET is off): delete build-qemu/ and run again"
[ "$run" = 1 ] || exit 0

# Flash image like idf.py qemu makes it, written fresh every run, and the
# eFuse image with IDF's QEMU defaults (written once).
flash_size=$(sed -n 's/^CONFIG_ESPTOOLPY_FLASHSIZE="\(.*\)"$/\1/p' "$bdir/sdkconfig")
[ -n "$flash_size" ] || die "no CONFIG_ESPTOOLPY_FLASHSIZE in build-qemu/sdkconfig"
(cd "$bdir" && "$idf_python" -m esptool --chip=esp32s3 merge-bin --output=qemu_flash.bin \
    --pad-to-size="$flash_size" @flash_args >/dev/null)
if [ ! -f "$bdir/qemu_efuse.bin" ]; then
    "$idf_python" - "$bdir/qemu_efuse.bin" <<'EOF'
import os, sys
sys.path.insert(0, os.path.join(os.environ["IDF_PATH"], "tools"))
from idf_py_actions.qemu_ext import QEMU_TARGETS
with open(sys.argv[1], "wb") as f:
    f.write(QEMU_TARGETS["esp32s3"].default_efuse)
EOF
fi

# -m 8M: the board's PSRAM. idf.py qemu passes -m 32M, which leaves no MMU
# address space for the partition table mapping (NVS unusable, crash loop).
# -accel tcg,thread=single: both CPU cores on one host thread. With QEMU's
# default (one host thread per core) this QEMU build crashed on Linux within
# seconds of boot in 5 of 13 runs (host heap corruption: glibc "malloc():
# unsorted double linked list corrupted", or SIGSEGV inside a device register
# read); with one thread, in none of 13 (README).
args=(
    -M esp32s3 -m 8M -accel tcg,thread=single
    -drive "file=$bdir/qemu_flash.bin,if=mtd,format=raw"
    -drive "file=$bdir/qemu_efuse.bin,if=none,format=raw,id=efuse"
    -global driver=nvram.esp32s3.efuse,property=drive,value=efuse
    -global driver=timer.esp32s3.timg,property=wdt_disable,value=true
    -nic "user,model=open_eth,hostfwd=tcp:127.0.0.1:$http_port-:80"
)

echo "Web UI:  http://localhost:$http_port/  (empty NVS: set the admin password on the first page)"
echo "UART1:   $uart1_text"

if [ -z "$headless" ]; then
    echo "Console: this terminal (Ctrl-A X quits QEMU)"
    args+=(-nographic -serial mon:stdio "${uart1_dev[@]}" -serial chardev:uart1)
    exec qemu-system-xtensa "${args[@]}"
fi

# Headless: QEMU in the background, always stopped when this script ends.
qpid=""
stop_qemu() {
    if [ -n "$qpid" ] && kill -0 "$qpid" 2>/dev/null; then
        kill "$qpid" 2>/dev/null || true
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$qpid" 2>/dev/null || break
            sleep 0.2
        done
        kill -9 "$qpid" 2>/dev/null || true
    fi
}
trap stop_qemu EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo "Console: $headless"
args+=(-display none -monitor none -serial "file:$headless" "${uart1_dev[@]}" -serial chardev:uart1)
qemu-system-xtensa "${args[@]}" </dev/null &
qpid=$!
echo "QEMU pid $qpid"
if [ -n "$seconds" ]; then
    for ((i = 0; i < seconds; i++)); do
        kill -0 "$qpid" 2>/dev/null || break
        sleep 1
    done
    kill -0 "$qpid" 2>/dev/null && exit 0   # still running: stopped on the way out
fi
rc=0
wait "$qpid" || rc=$?
qpid=""
echo "run.sh: QEMU exited by itself (status $rc)" >&2
exit "$rc"
