#!/usr/bin/env bash
# Assemble the release files from the CI build outputs:
#   OUT_DIR/HIDway-<version>-windows.zip   client, probe, Setup.cmd, firmware
#   OUT_DIR/hidway_fw_<board>.uf2          the firmware files on their own
#   OUT_DIR/SHA256SUMS.txt
#
#   tools/ci/package.sh VERSION WINDOWS_DIR FIRMWARE_DIR OUT_DIR
#
# WINDOWS_DIR holds hidway-client.exe and hidway-probe.exe, FIRMWARE_DIR the
# hidway_fw_*.uf2 files. Needs zip and sha256sum.
set -euo pipefail

if (($# != 4)); then
    echo "usage: package.sh VERSION WINDOWS_DIR FIRMWARE_DIR OUT_DIR" >&2
    exit 2
fi
version=$1
win=$2
fw=$3
root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
mkdir -p "$4"
out=$(cd "$4" && pwd)

name=HIDway-$version
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
stage=$work/$name
mkdir -p "$stage/firmware"

cp "$win/hidway-client.exe" "$win/hidway-probe.exe" "$stage/"
cp "$root/tools/setup/Setup.cmd" "$root/tools/setup/hidway-setup.ps1" \
    "$root/client/hidway.ini.example" "$root/LICENSE" "$root/README.md" "$stage/"
shopt -s nullglob
uf2=("$fw"/hidway_fw_*.uf2)
((${#uf2[@]})) || { echo "no firmware in $fw" >&2; exit 1; }
cp "${uf2[@]}" "$stage/firmware/"
printf '%s\r\n' "$version" >"$stage/VERSION"

(cd "$work" && zip -qrX "$out/$name-windows.zip" "$name")
cp "${uf2[@]}" "$out/"
(cd "$out" && sha256sum -- "$name-windows.zip" hidway_fw_*.uf2 >SHA256SUMS.txt)
cat "$out/SHA256SUMS.txt"
