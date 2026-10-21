#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Avery L.
# SPDX-License-Identifier: MIT
#
# The files for installing a station build on each board, from a build
# directory (idf.py build): one app and one bootloader for every board, a
# partition table per flash size.
#
#   tools/ci/install_files.sh <build dir> <out dir>
#
# Writes to <out dir>:
#   partition-table-8mb.bin        partitions-8mb.csv (the GNS NTRIP-X)
#   onodiy-install-16mb.bin        one image for 0x0 on a 16 MB board (UM982 board,
#                                  UM980 board): bootloader, table, otadata, app
#   onodiy-install-um980-ntrx.bin  the same for the GNS NTRIP-X: the 8 MB table and
#                                  --flash-size 8MB (esptool writes it into the
#                                  bootloader's header and updates its digest)
#
# Fails when the app is larger than the smallest app slot of any table (the
# 2.5 MB of partitions-8mb.csv), or when a merged image's bootloader does not
# validate or names another flash size. Needs the ESP-IDF environment
# (esptool, gen_esp32part.py).
set -euo pipefail

die() {
    echo "install_files.sh: $*" >&2
    exit 1
}

[ $# -eq 2 ] || die "usage: install_files.sh <build dir> <out dir>"
bdir=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
[ -n "${IDF_PATH:-}" ] || die "no ESP-IDF environment: run . \$IDF_PATH/export.sh first"
py="${IDF_PYTHON_ENV_PATH:+$IDF_PYTHON_ENV_PATH/bin/python}"
[ -n "$py" ] && [ -x "$py" ] || py=$(command -v python3 || command -v python) || die "no Python found"

args="$bdir/flash_args"
[ -f "$args" ] || die "no $args: build first"
app="$bdir/onodiy-esp32-ntrip-server.bin"
[ -f "$app" ] || die "no $app"

# The app slot of partitions-8mb.csv, the smallest of any table.
slot=$(awk -F, '/^ota_0,/ {gsub(/ /, "", $5); print $5}' "$root/partitions-8mb.csv")
[ -n "$slot" ] || die "no ota_0 in partitions-8mb.csv"
size=$(wc -c < "$app" | tr -d ' ')
if [ "$size" -gt $((slot)) ]; then
    die "the app is $size bytes, larger than the $((slot)) bytes of an app slot on the 8 MB board (partitions-8mb.csv)"
fi
echo "app: $size bytes of $((slot)) ($((size * 100 / slot))% of the smallest slot)"

table_off=$(awk '$2 ~ /partition-table\.bin$/ {print $1}' "$args")
[ -n "$table_off" ] || die "no partition table line in $args"
"$py" "$IDF_PATH/components/partition_table/gen_esp32part.py" --quiet --flash-size 8MB --offset "$table_off" \
    "$root/partitions-8mb.csv" "$out/partition-table-8mb.bin"

# The build's flash_args again, for the 8 MB board: its table and its flash size.
sed -e "s/--flash-size [0-9A-Za-z]*/--flash-size 8MB/" \
    -e "s|^$table_off .*|$table_off $out/partition-table-8mb.bin|" "$args" > "$out/flash_args_8mb"
grep -q "^$table_off $out/partition-table-8mb.bin$" "$out/flash_args_8mb" || die "the 8 MB table did not replace the table in $args"
grep -q -- "--flash-size 8MB" "$out/flash_args_8mb" || die "no --flash-size in $args"

merge() {   # <args file> <output>
    (cd "$bdir" && "$py" -m esptool --chip esp32s3 merge-bin --output "$2" "@$1" >/dev/null)
}
merge "$args" "$out/onodiy-install-16mb.bin"
merge "$out/flash_args_8mb" "$out/onodiy-install-um980-ntrx.bin"
rm -f "$out/flash_args_8mb"

# Each image's bootloader: valid (checksum and digest) and for its flash size.
check() {   # <image> <flash size>
    local info
    info=$("$py" -m esptool --chip esp32s3 image-info "$1" 2>&1) || die "$(basename "$1"): bootloader does not validate: $info"
    grep -q "Flash size: $2" <<<"$info" || die "$(basename "$1"): bootloader flash size is not $2"
    grep -q "Checksum: .*(valid)" <<<"$info" || die "$(basename "$1"): bootloader checksum not valid"
    grep -q "Validation hash: .*(valid)" <<<"$info" || die "$(basename "$1"): bootloader validation hash not valid"
    echo "$(basename "$1"): $(wc -c < "$1" | tr -d ' ') bytes, bootloader for $2, checksum and hash valid"
}
check "$out/onodiy-install-16mb.bin" "$(sed -n 's/.*--flash-size \([0-9A-Za-z]*\).*/\1/p' "$args")"
check "$out/onodiy-install-um980-ntrx.bin" 8MB
echo "partition-table-8mb.bin: $(wc -c < "$out/partition-table-8mb.bin" | tr -d ' ') bytes"
