#!/usr/bin/env bash
# Build the release firmware (relay link, HIDWAY_T0=OFF) for each board and
# copy it to OUT_DIR as hidway_fw_<board>.uf2.
#
#   PICO_SDK_PATH=<sdk> tools/ci/build-firmware.sh OUT_DIR [BOARD...]
#
# Boards default to pico pico_w pico2 pico2_w. Needs CMake, Ninja, the Arm
# GNU toolchain (arm-none-eabi-gcc) and a host C/C++ compiler (the SDK builds
# pioasm and picotool from source). HIDWAY_FW_BUILD sets the build directory.
set -euo pipefail

out=${1:?usage: build-firmware.sh OUT_DIR [BOARD...]}
shift
boards=("$@")
((${#boards[@]})) || boards=(pico pico_w pico2 pico2_w)

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
build=${HIDWAY_FW_BUILD:-$root/build/fw-release}
# one picotool build, shared by every board
export PICOTOOL_FETCH_FROM_GIT_PATH=${PICOTOOL_FETCH_FROM_GIT_PATH:-$build/picotool}

mkdir -p "$out"
for b in "${boards[@]}"; do
    echo "== $b"
    cmake -S "$root/firmware" -B "$build/$b" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DPICO_BOARD="$b" -DHIDWAY_T0=OFF
    cmake --build "$build/$b"
    cp "$build/$b/hidway_fw.uf2" "$out/hidway_fw_$b.uf2"
done
ls -l "$out"
