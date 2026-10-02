# HIDway

Remote physical keyboard/mouse: remote Windows PC → Tailscale (UDP) → Raspberry Pi 4 relay → UART → Pico W acting as a standard USB HID keyboard + mouse → gaming PC. The gaming PC runs no extra software or drivers. Full design: `docs/ARCHITECTURE.md`.

## Hard rules
- Pure 1:1 relay of human input. No macros, automation, scripted input or recoil/aim features.
- The device identifies itself honestly (strings "HIDway", own VID/PID). Never copy another vendor's VID/PID/strings and never add detection-evasion or hiding mechanisms.
- Fail-safe everywhere: any lost link, timeout or crash must end in "all keys and buttons released".
- No queues in the input path: every hop keeps only the latest state.

## Layout
- `common/`: plain C11 shared by firmware, relay and client; no SDK or OS dependencies. Unit-tested on the host.
- `firmware/`: Pico SDK + TinyUSB, separate CMake project (ARM cross toolchain). `HIDWAY_T0=ON` builds the Phase-0 BOOTSEL test mode.
- `tests/`: host tests for `common/`, run with ctest.
- `tools/build.ps1`: builds firmware and tests (uses VS vcvars, `~/.pico-sdk`).

## Build
- All: `powershell -ExecutionPolicy Bypass -File tools\build.ps1`
- Firmware output: `build\firmware\hidway_fw.uf2`

## Status
Phase 0: waiting for the T0 result (does AION 2 accept the Pico HID?). Do not start Phase 1 (UART link, relay, client) until T0 passes.
