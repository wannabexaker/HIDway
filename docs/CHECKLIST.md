# HIDway checklist

Status of each task. Nothing is deleted; completed items keep their evidence.
`[x]` done · `[~]` partial · `[ ]` todo · `[!]` blocked on hardware/decision.

## Done

- [x] **1. Repo bootstrap** — git, `.gitignore`, layout, project `CLAUDE.md`.
- [x] **2. Shared logic (`common/`)** — 6KRO key slots, motion accumulator,
  scan-code→HID map, wire protocol. Host unit tests pass (ctest).
- [x] **3. Phase-0 firmware (T0)** — Pico (W) composite HID (boot keyboard +
  16-bit mouse, 1 kHz), honest identity, watchdog, status LED. Builds to
  `hidway_fw.uf2`. *Not yet run on hardware.*
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
  link-timeout release (firmware builds). *Not yet tested on real hardware.*
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
  *Not yet on the Pi.*
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
- [!] **9. Lower latency (direct path)** — decide from the Nova router whether
  the line is public (→ one UDP port-forward to the Pi gives a direct Tailscale
  path, ~15–35 ms) or CGNAT (→ stay on relay / other options). Needs: Nova WAN
  IP check.
- [!] **18. Low-latency path via Cloudflare Zero Trust (option 4)** — WARP
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
- [ ] **14. Completeness** — NKRO, horizontal wheel, host LED feedback,
  soft-detach / physical arm switch, remote Pico flashing.
- [ ] **15. Stable 1.0** — soak test, latency report, config validation,
  security review.

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
