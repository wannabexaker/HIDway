# HIDway checklist

Status of each task. Nothing is deleted; completed items keep their evidence.
Last updated 2026-10-25.
`[x]` done · `[~]` partial · `[ ]` todo · `[!]` blocked on hardware/decision.

## Done

- [x] **1. Repo bootstrap** — git, `.gitignore`, layout, project `CLAUDE.md`.
- [x] **2. Shared logic (`common/`)** — 6KRO key slots, motion accumulator,
  scan-code→HID map, wire protocol. Host unit tests pass (ctest).
- [x] **3. Phase-0 firmware (T0)** — Pico (W) composite HID (boot keyboard +
  16-bit mouse, 1 kHz), honest identity, watchdog, status LED. Builds to
  `hidway_fw.uf2`. *Ran on hardware 2026-10-24: see 7.*
- [x] **4. Client preview UI** — Win32 arm/disarm window, Raw Input capture,
  live display. Does not block local input.
- [x] **5. Step 1 — UDP transport over Tailscale** — client sender + `hidwayd`
  relay. Validated live: 300/300 packets, 0 loss, gap detection exact,
  link-timeout release fired, RTT p50 ~94 ms over the DERP (relayed) path.
- [x] **6. Public repo hygiene** — scrubbed of personal data, placeholders
  only, README/SECURITY/LICENSE/ROADMAP, noreply commit identity.
- [x] **10/11. Serial link code, both ends** — `common/` COBS+CRC16 framing
  (host unit-tested); `hidwayd` serial output + release-on-timeout (`--serial`,
  builds on the Pi); Pico `link.c` UART receiver with catch-up and
  link-timeout release (firmware builds). *Tested on hardware 2026-10-24: see 8b.*
- [x] **12. Client UI** — health header (RTT/loss/pps/relay-serial/armed-since),
  recent-input history (memory only), in-app settings + save to hidway.ini,
  panic hotkey + auto-disarm-on-link-loss, always starts disarmed.
- [x] **12b. Client UX** — tray icon (state colour, tooltip, menu,
  minimize-to-tray), pre-arm reachability (PROBE), link-loss alert, hotkey
  conflict warning, app icon, remembered window position.
- [x] **12c. Client UI rebuild** — per-monitor DPI aware, anti-aliased
  card/tile/chip design, dark scrollbar, readable hover/focus states,
  `--preview`/`--shot` for verified screenshots.
- [x] **12d. Relay redeploy for PROBE** — resolved by item 16: the Pi runs
  release `v0.1.0`, which includes PROBE support (checked 2026-10-24 with
  `hidway-status`).

- [x] **16. Relay as a managed service** — `hidwayd`: serial reopen with
  retry (probe unplug/replug), RELEASE on every new serial connection,
  latest-only serial writes (`TIOCOUTQ`) with partial-write resync, 2000 pps
  cap, `--version`, live `--state-file`; event-driven loop (idle wakeups
  60 per 3 s -> 0). `relay/deploy/`: `install.sh` / `uninstall.sh`, hardened
  and resource-bounded `hidwayd.service`; `hidway-update` deploys release
  tags `vX.Y.Z` only (daily check = one `git ls-remote`; build as an
  unprivileged user; unit tests gate; per-commit releases; atomic switch;
  never restarts during a live session; auto-rollback on a failed health
  check; held releases; pruning); service files only via `--apply-system`;
  `hidway-status` (OK/WARN/DOWN + exit code) and a status line at SSH login;
  udev rule `/dev/hidway-serial`; backups under `/var/backups/hidway`.
  Docs: `docs/RELAY.md`. Verified in a sandbox: unit tests incl. resync cases
  (ctest + `make check`), serial reconnect/timeout/shutdown 13/13 (pty), rate
  cap exact, install/release/rollback/status/uninstall 43/43 (fake root,
  local git server, systemctl stand-in), shellcheck clean.
  Installed on the Pi: `hidwayd` running release `v0.1.0` under systemd, daily
  release check active, `hidway-status` reports only "serial device missing"
  (expected until the Debug Probe is connected) — checked 2026-10-24.
- [x] **17. Optional end-to-end encryption** (branch `feature/e2e-encryption`,
  backup of the previous `main` at `backup/main-pre-encryption-2026-10-08`) —
  `common/hidway_crypto`: sealed envelope with XChaCha20-Poly1305 (vendored
  Monocypher 4.0.2, unmodified), random 24-byte nonce, timestamp-based replay
  guard (strictly newer, ±2 min); client `key =` in hidway.ini, relay
  `--key-file` / `--gen-key`; with a key plaintext is refused, without one
  behaviour is unchanged; client E2E badge; `hidway-probe` tool. Verified:
  host tests incl. the IETF XChaCha20-Poly1305 test vector (MSVC and gcc,
  0 warnings); end-to-end against the Linux relay in a container: correct key
  40/40 replies, plaintext and wrong key rejected, replay of an identical
  packet rejected, key-less relay still serves plaintext clients.
  *On the Pi since v0.2.0; not switched on (no key configured).*
- [x] **7. T0 on-device test — GO** (2026-10-24) — T0 firmware built for a
  Pico 2 (RP2350, `-DPICO_BOARD=pico2`) enumerated on Windows as "HID Keyboard
  Device" + "HID-compliant mouse" with no driver install; holding BOOTSEL typed
  `wwww` in Notepad, and in the target game the character walked forward and
  the camera turned. The HID path is proven.

## Blocked on hardware / decision

- [x] **8. Serial link Pi ↔ Pico** — framing (COBS+CRC) from `hidwayd` to the
  Pico, and the Pico applying state with a link-timeout release. Chosen link:
  the Pi 4's GPIO UART3 wired straight to the Pico (no adapter). Release
  `v0.1.1` (udev rule + sandbox allow for `ttyAMA`) deployed on the Pi;
  `dtoverlay uart3` loaded at runtime (no reboot, backup of the boot config
  under `/var/backups/hidway/`); `hidway-status` OK with `/dev/hidway-serial
  -> ttyAMA3` connected (2026-10-24). Link firmware built for the Pico 2.
  `dtoverlay=uart3` added to `config.txt` (takes over at the next boot; boot
  config backed up first).
- [x] ~~**9. Lower latency (direct path)**~~ — SKIPPED 2026-10-25: resolved
  by item 22 (direct Tailscale path without any router change). Original: decide from the Nova router whether
  the line is public (→ one UDP port-forward to the Pi gives a direct Tailscale
  path, ~15–35 ms) or CGNAT (→ stay on relay / other options). Needs: Nova WAN
  IP check.
- [x] ~~**18. Low-latency path via Cloudflare Zero Trust (option 4)**~~ —
  SKIPPED 2026-10-25: dropped for now; the direct Tailscale path (item 22,
  p50 37 ms from mobile data) makes it unnecessary. Original: WARP
  client on the remote PC (split tunnel: the relay only) → Cloudflare Tunnel
  (`cloudflared` on the Pi) → `hidwayd` with E2E encryption on (item 17), so
  Cloudflare sees only ciphertext. Needs: Zero Trust onboarding in the
  Cloudflare dashboard (user action: team name, Free plan), a private-network
  route to the Pi, relay bind/allow adjusted for traffic arriving via
  `cloudflared`, then `hidway-probe` to compare RTT with the DERP path. Risk to
  test: WARP and Tailscale coexisting on the same PC.

## Todo

- [x] **8b. On-hardware serial test — end to end works** (2026-10-24) — link
  firmware (`-DPICO_BOARD=pico2 -DHIDWAY_T0=OFF`) on the Pico 2, wired to the
  Pi 4 UART3 over ~2 m; client → Tailscale (direct LAN path, probe 20/20, p50
  4.5 ms) → relay v0.1.1 → Pico 2 → target: typing and mouse movement arrive
  on the target. Open issue: input also acts on the local PC (item 13).
- [x] **13. Input goes to the target only while armed** (2026-10-25) — capture
  badge: always-on-top, holds the focus and the hidden, confined cursor while
  armed; low-level keyboard hook on its own thread, installed on arm and
  removed on disarm, so system shortcuts (Alt+Tab, Win, Ctrl+Shift+Esc, Alt+F4)
  reach the target; Ctrl+Alt+Del / Win+L stay local and disarm; Ctrl+Alt+End
  sends Ctrl+Alt+Del. Auto-disarm on focus loss (names the program that took
  it), lock/logoff, display change, sleep, a missing hook, link loss. Mouse
  Raw Input only while armed and only to the badge; injected input never
  relayed; disarm reasons always listed. Disarmed, the badge is a small "Arm"
  pill that never takes the focus (works over a full-screen AnyDesk window),
  can be dragged and remembers its place; tray toggle. Also: RTT read on
  arrival (was overstated by up to 400 ms while disarmed). Verified by the
  user: typing, Ctrl, Alt+Tab reach the target only; click-to-arm works.
- [x] **19. Status LEDs on the Pico** (2026-10-25) — red idle / blinking on a
  link problem (stream stopped without RELEASE, corrupt frames, hang reboot),
  green armed, yellow flash per mouse click, blue flash per key press; dark
  when the target is off or asleep. Pins 21–25 (blue, yellow, GND, green, red).
  Deframer counts dropped frames (unit-tested). Verified on the breadboard.
- [x] **20. Video with Sunshine + Moonlight** (2026-10-25) — Sunshine on the
  target with keyboard/mouse/gamepad input disabled; Moonlight here in
  borderless-windowed mode with "optimize mouse for remote desktop", so the
  badge can be clicked over it and the stream keeps running while HIDway holds
  the focus. User: "much faster than AnyDesk". Lesson: a stream session
  started before Sunshine's input was disabled keeps the old setting (stop it
  and start again); two Moonlight instances overwrite each other's settings.
- [x] **21. Key as a mouse side button** (2026-10-25) — `mouse4_key` /
  `mouse5_key` in `hidway.ini`: while armed the key is not sent and the target
  sees mouse button 4/5 held for as long as the key is held (1:1). New key
  names: grave, capslock, rctrl, ralt, apps, scrolllock, pause. Badge: larger,
  red dot when disarmed, green when armed. Verified by the user (Mouse 4).
- [x] **22. Remote play from outside (direct path)** — DONE 2026-10-25, with
  no router changes: from a phone hotspot Tailscale went direct to both the Pi
  and the target PC (RTT avg ~40 ms, `hidway-probe` 60/60 p50 37 ms vs 94 ms
  over DERP before). Port forwards turned out unnecessary (and with an
  "easy" home NAT Tailscale would not advertise them anyway); the fallback if
  a network ever forces DERP is a Tailscale Peer Relay on the Pi. Moonlight
  reaches the target by its tailnet address; on mobile data the stream needs
  ~10 Mbps (20 Mbps congested the link and the control channel timed out).
  The target PC's Tailscale uses a fixed UDP port (later switched to "run
  unattended" so it is up after Wake-on-LAN, item 25). SSH to the target PC: key-only, LAN + tailnet
  only (laptop alias `gamingpc`). Original analysis, measured 2026-10-25:
  home has a public dynamic IPv4 (no CGNAT), endpoint-independent NAT, no
  UPnP/NAT-PMP, no IPv6; the Pi sits behind the ISP router only, the target
  PC and this PC behind a second router (double NAT); the target PC is in the
  tailnet but offline. Without a direct path the stream would ride DERP
  (throughput-limited, not fit for ~20 Mbps video). Plan: target PC Tailscale
  always on ("run unattended") with a fixed UDP port; on the ISP router UDP
  41641 -> Pi and a second UDP port -> the second router; on the second router
  that port -> target PC plus a static lease; Moonlight adds the target by its
  tailnet address; test on a phone hotspot (`tailscale ping` = direct,
  `hidway-probe`, Moonlight stats). Nothing but Tailscale's encrypted UDP is
  exposed. Limit: networks that block UDP cannot carry the video.
- [x] **23. Audio over Moonlight** (2026-10-25) — no sound at first: the
  target's default output is a Voicemeeter virtual device, so with
  Moonlight's "mute host PC speakers" Sunshine captured its own virtual sink
  while the game kept playing into Voicemeeter. Fix: turn that Moonlight
  option off, so Sunshine captures the default device. Verified by the user.
- [x] **26. Security review of the branch** (2026-10-25) — crypto envelope,
  relay mode enforcement, client capture/hook, config parsing, firmware link,
  photos: no vulnerability at confidence >= 8. Defense-in-depth idea for
  later: persist the relay's replay timestamp (or reject packets older than
  the relay's start) so a restart cannot reopen a 2-minute replay window.
- [~] **24. Guided setup for new users** — Windows installer for the client,
  then a setup wizard: (1) install on this PC, (2) find the Raspberry Pi on the
  network and install the relay over SSH, filling in the addresses, (3) "plug
  the Pico in holding BOOTSEL" and flash the right firmware for the detected
  board (Pico / Pico 2, W or not), (4) end-to-end check. Publish prebuilt
  client and firmware files with each release.
  Done (2026-10-25): `install.sh` takes every setting on the command line
  (`--bind auto --allow --uart gpio --wake-mac --wake-to`, `--configure`),
  41 fake-root tests; `tools/setup/Setup.cmd`: per-PC checklist of HIDway,
  Tailscale, Moonlight, Sunshine, each with its own official installer
  (winget), `-Update`, `-WhatIf`, recommended Moonlight settings, optional
  Sunshine input off. Verified: dry runs on both PCs, a real install in a
  clean Windows Sandbox (no winget: official pages offered, client starts).
  CI (2026-10-25): `.github/workflows/ci.yml` builds and tests everything on
  each push (gcc + MSVC with warnings as errors, relay `make check`,
  install.sh tests, shellcheck, firmware for Pico / Pico W / Pico 2 / Pico 2 W,
  release files); first run on GitHub all green, no annotations.
  `release.yml` publishes the Windows zip, the .uf2 files and SHA256SUMS.txt
  for an annotated `vX.Y.Z` tag on main. First run: v0.3.0 (2026-10-25)
  published; downloaded copy passes `sha256sum -c`, Windows scripts CRLF, no
  personal data or build paths in any file. Pi updated to v0.3.0 with
  `--apply-system`; status OK, PC on, laptop probe 10/10 (p50 4.4 ms).
  Next: the setup wizard in the client.
- [x] **25. Wake-on-LAN** (2026-10-25) — `hidwayd --wake MAC [--wake-to
  IPv4[:PORT]]` (magic packet, SO_BROADCAST, sent 3 times; MAC/address
  validated) and `hidway-wake`, which reads `HIDWAY_WAKE_MAC` /
  `HIDWAY_WAKE_TO` from `hidwayd.conf` without executing it. Target behind a
  second router: UDP 9 forwarded from the Pi only plus a static ARP entry
  (docs/RELAY.md). Firmware: power-on by PCI-E allowed, ErP off.
  Verified: from a full shutdown the PC switched itself on, Tailscale came up
  unattended, the user logged in through Moonlight. Tests: packet bytes,
  config parsing, bad input, fake-root install, shellcheck clean.
  Next: target power state (on/asleep/off) from the Pico over its TX line and
  a "Wake" button in the client.
- [x] **28. Target state and Wake in the client** (2026-10-25) — DONE: the
  user pressed WAKE in the client with the target shut down and it switched
  on (2026-10-25). The Pico
  reports the target over its TX line (new serial frame `HIDWAY_SER_TARGET`:
  on / asleep / off from USB mount/suspend; on change and every 3 s); the
  relay reads it (serial now read-write), expires it after 10 s silence
  (unknown), publishes it in status flags (`HIDWAY_STATUS_*`, backward
  compatible), the state file and `hidway-status`; new client message
  `HIDWAY_MSG_WAKE` (allowed peer only, sealed with encryption, only the
  configured packet, max once per 3 s; config from `HIDWAY_WAKE_*` in
  hidwayd.conf). Client: "PC" tile (On / Asleep / Off / Off? / Waking) with a
  WAKE button and a tray item; armed time moved into the header pill;
  history notes target changes; `hidway-probe` prints the target. Tests:
  unit (frame round trip, corrupt/out-of-range, WAKE decode), Linux
  end-to-end with a pty Pico (reports, staleness, wake burst, rate limit),
  shellcheck, previews. Released as v0.2.2 and deployed; Pico 2 flashed.
  Verified live: the relay and the client show the target "on" (after
  fixing the Pico TX -> Pi RX wire). Remaining: press WAKE in the client with
  the target shut down (the same packet as `hidway-wake`, already verified).
- [x] **27. Updater: no false "service files changed"** (2026-10-25) — only
  the files `--apply-system` installs count, not examples or docs;
  `.gitattributes` keeps the Linux scripts LF in Windows checkouts.
- [x] **29. Relay: no replay across a restart** — DONE 2026-10-25 · `639d652`.
  With encryption on, nothing sealed before the relay's start is accepted
  (closes the item-26 idea). Unit test; in Linux the previous relay answered
  a packet sealed 3 s before its start, the new one rejects it and logs why,
  normal traffic unaffected.
- [x] **30. Firmware defaults to the relay link** — DONE 2026-10-25 ·
  `639d652`. `HIDWAY_T0` default OFF (a plain build was the test mode);
  `tools/build.ps1` passes it on every run (`-T0` for the test mode).
  Verified: default build has `HIDWAY_T0=0`; local cache switched to OFF.
- [x] **31. One version everywhere** — DONE 2026-10-25 · `639d652`. From the
  release tag (`git describe`), regenerated on every build: exe properties of
  client and probe (were fixed at 0.1.0), next to the title in the client,
  `hidway-probe --version`, firmware binary info (`picotool info`). Verified
  with MSVC (warnings as errors), a client screenshot and `picotool info`.
- [x] **32. Relay fail-safe tests, automated** — DONE 2026-10-26 · `8c08ad4`.
  `make -C relay failsafe`: the real `hidwayd` with a fake client and a fake
  Pico (pty); 16 checks: RELEASE on a new serial connection, on client loss
  (251 ms for a 250 ms timeout), on explicit release, after the serial line
  returns, on relay stop; stale packets and other senders ignored. In CI on
  every push. Stable over repeated runs; five deliberate breaks of the relay
  each caught by the matching check. Hardware part (Pico timeout, USB
  unplug, real network) still to do with the user.
- [ ] **14. Completeness** — NKRO, horizontal wheel, host LED feedback,
  soft-detach / physical arm switch, remote Pico flashing.
- [ ] **15. Stable 1.0** — soak test, latency report, config validation,
  security review.

## Next — before installers (MSI / Setup.exe), signing, Linux, macOS

Proposed order; numbers are given when work starts. Rule (user, 2026-10-25):
nothing ships as an installer before hardening and serious testing.

1. **Security hardening**
   - Pi: SSH key-only and no root login — done 2026-10-25 (drop-in
     `00-hardening.conf`; verified: password login refused, key login works).
     Automatic security updates — done 2026-10-25 (`unattended-upgrades`,
     security only, no automatic reboot; all updates applied). Rebooted for
     the new kernel (6.18.50): the GPIO UART came up from `config.txt` on its
     own (first proof; so far it was loaded at runtime), no failed units,
     `hidway-status` OK with the target on, laptop probe 10/10 (p50 4.3 ms).
   - Tailscale ACL — not now (user decision 2026-10-26): one user, own
     devices only; the relay already accepts the player PC alone (Tailscale
     authenticates source addresses) and SSH is key-only on the Pi and the
     target, so the rules would mostly restrict the phone, at the cost of
     editing them for every new device or tool. Needed as soon as other
     people's devices join the tailnet; a draft is kept locally.
   - Decide: end-to-end encryption on by default?
2. **Device correctness**
   - Honest USB identity: VID 0xCAFE is a development placeholder; request a
     PID under the Raspberry Pi VID 0x2E8A (raspberrypi/usb-pid).
   - Completeness (item 14), at least NKRO and host Caps/Num Lock feedback.
3. **Serious testing** (item 15) — relay part automated (item 32); on the
   hardware, with the user: fail-safe with a key held (kill the client,
   cut the network, stop the relay, unplug the UART, reboot the Pi, reset the
   Pico, Tailscale down: all released within 250 ms); packet loss and jitter;
   login screen / UAC / Ctrl+Alt+Del; BIOS; measured latency (bench mode, not
   written yet) and a latency report; 8-hour soak; clear errors for a bad
   `hidway.ini` / `hidwayd.conf`.
4. **Professional image**
   - README screenshot is from 2026-10-06 (before the badge and the PC tile).
   - Docs: ARCHITECTURE is the original plan in Greek, names a specific game
     and its anti-cheat, and still describes the Debug Probe link; rewrite in
     English, game-neutral, as built. ROADMAP is stale, no CHANGELOG.
   - Code: `client/src/main.c` is 2,700 lines; split into modules (also the
     base for Linux/macOS clients).
   - Hardware: soldered board in an enclosure with the 1 kΩ resistors, photos.
   - Final `/security-review`, `/code-review`, `/repo-hygiene`.
5. **Setup wizard in the client** (rest of item 24).
6. Then: MSI (WiX) + `HIDway-Setup.exe` (Inno Setup) → code signing (SignPath
   Foundation, free for open source) → Linux client → macOS client.

## Waiting on the user

- Pi SSH / updates and the Tailscale ACL are system settings: the user
  applies them (commands provided).
- USB PID request is filed from the owner's GitHub account (text prepared).
- Permanent hardware build.

## Log

- 2026-10-25 — reconciled against git and the live Pi: items 3, 10/11 and 17
  no longer "not on hardware / not on the Pi"; 9 and 18 SKIPPED (resolved by
  22); 22 notes "run unattended". Added Next (hardening and testing before
  installers) and Waiting.
- 2026-10-25 — 28 DONE (user: WAKE switched the shut-down PC on). 29, 30, 31
  DONE (`639d652`). Pi SSH key-only and automatic updates: commands given to
  the user.
- 2026-10-26 — Pi hardened and rebooted (kernel 6.18.50, UART up from
  config.txt). 32 DONE (`8c08ad4`). Hardware fail-safe session started (a
  test sender held F24 through the relay) but the key recorder did not start
  on the target; postponed by the user in favour of a day of real use.
  Readiness checked for that: target services (Tailscale unattended,
  Sunshine, sshd) automatic and running, no sleep or hibernate, no reboot
  pending, Wake-on-LAN on; direct Tailscale paths to the Pi and the target;
  relay v0.3.0 OK, probe 10/10. The Pi stays on v0.3.0 until after that day;
  29-32 go into the next release then.

## Hardware

- Raspberry Pi Pico 2 — **have**; the HID device (T0 passed on it), on a
  breadboard with 4 status LEDs, wired to the Pi 4 GPIO UART3.
- 1 kΩ series resistors on the two UART lines — recommended before a
  permanent (soldered) build.
- Raspberry Pi Pico (RP2040) — **have**; can serve as the Pi↔Pico serial
  link with the official `debugprobe` firmware instead of buying a Debug Probe.
- Raspberry Pi Pico 2 W — **have**; spare.
- Good micro-USB **data** cable; connect the Pico to a rear USB port on the
  target.
