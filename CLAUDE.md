# HIDway

Remote physical keyboard/mouse: remote Windows PC → Tailscale (UDP) → Raspberry Pi relay → UART (Pi GPIO or USB serial) → Pico / Pico 2 acting as a standard USB HID keyboard + mouse → target PC. The target PC runs no extra software or drivers. Full design: `docs/ARCHITECTURE.md`.

## Permanent rules (never relax — security and professionalism come first)

This repository is **PUBLIC**. Everything committed is world-readable forever.
These rules are standing policy for the whole project:

1. **Never commit anything personal or environment-specific.** No real IPs
   (Tailscale `100.x`, public IPs, LAN `192.168.x`/`10.x`), hostnames,
   usernames, email addresses, SSH keys/certs, tokens, or real runtime configs.
   Use placeholders only (e.g. `100.100.100.100`, `<pi-tailscale-ip>`).
2. **Real configs and secrets stay out of git.** `hidway.ini`, `hidwayd.conf`,
   `*.key`, `*.pem`, `*.crt`, `.env`, `secrets/` and `build/` are gitignored.
   Only `*.example` templates with placeholders are committed.
3. **Scan before every commit/push.** Grep the staged content for IPs,
   hostnames, emails and key material. If in doubt, do not commit.
4. **Commits use the GitHub noreply identity**, never a real email.
5. **Honest device, no evasion.** The firmware identifies itself truthfully and
   never imitates another vendor's VID/PID/strings. No detection-evasion,
   hiding, macros, automation or assisted-input features — ever.
6. **Fail-safe is non-negotiable.** Every path must end in "all keys and
   buttons released" on any loss, timeout or crash. No queues in the input
   path: keep only the latest complete state at every hop.
7. **Keep it presentable.** README, docs and code stay professional and clear;
   assume a reviewer is reading the repo cold.

## Layout
- `common/`: plain C11 shared by firmware, relay and client; no SDK/OS deps. Unit-tested on the host.
- `firmware/`: Pico SDK + TinyUSB, separate CMake project (ARM cross toolchain). `HIDWAY_T0=ON` builds the Phase-0 BOOTSEL test mode.
- `relay/`: `hidwayd`, plain C + Makefile (no cmake needed on the Pi).
  `relay/deploy/`: Pi install, systemd units, udev rule, `hidway-update`
  (release deployment with rollback) and `hidway-status`. See `docs/RELAY.md`.
- `client/`: Win32 GUI, built with the host CMake build on Windows.
- `tests/`: host tests for `common/`, run with ctest.
- `tools/build.ps1`: builds firmware and tests (uses VS vcvars, `~/.pico-sdk`).
- `tools/setup/`: `Setup.cmd` / `hidway-setup.ps1`, the Windows installer
  (per-PC checklist: HIDway, Tailscale, Moonlight, Sunshine via winget).
- `tools/ci/`: firmware-for-every-board and release-packaging scripts, used by
  `.github/workflows/` (`ci.yml` on every push/PR, `release.yml` on tags).

## Releases
The Pi deploys release tags `vX.Y.Z` only (annotated, on `main`); pushes to
`main` never reach it. Tag only a commit whose `make check` passes. Branches
`backup/*` are safety snapshots: never delete or rewrite them.
Each tag also gets a GitHub release built by `release.yml` (Windows zip with
`Setup.cmd`, firmware `.uf2` per board, `SHA256SUMS.txt`); it refuses
lightweight tags and tags not on `main`.

## Build
- Host tests + client (Windows): `powershell -ExecutionPolicy Bypass -File tools\build.ps1`
- Firmware output: `build\firmware\hidway_fw.uf2`
- Relay on the Pi: `cd relay && make && make check`, or install as a service
  with `sudo relay/deploy/install.sh`

## Status
See `docs/CHECKLIST.md` (the single source of truth for what is done and next).
