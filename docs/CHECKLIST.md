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
- [!] **12d. Relay redeploy for PROBE** — `hidwayd` with PROBE support is
  committed but not yet running on the Pi (SSH to the Pi was timing out over
  the relayed Tailscale path). Until then the disarmed client shows
  "no reply from relay"; armed mode works. Once item 16 is installed on the
  Pi (one `install.sh` run), this and every later relay change deploy
  automatically.

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
  *Not yet run on the Pi (needs systemd + udev there).*
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

## Blocked on hardware / decision

- [!] **7. T0 on-device test (GO/NO-GO)** — plug the Pico into the target,
  confirm it is accepted as a real HID keyboard+mouse (and in the target app).
  Needs: the Pico in hand. This gates everything else.
- [!] **8. Serial link Pi ↔ Pico** — framing (COBS+CRC) from `hidwayd` to the
  Pico, and the Pico applying state with a link-timeout release. Needs: a
  Raspberry Pi Debug Probe (≈€12) or equivalent 3.3 V USB-serial.
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

- [!] **8b. On-hardware serial test** — flash the link firmware
  (`-DHIDWAY_T0=OFF`), run `hidwayd --serial /dev/serial/by-id/...`, confirm the
  target receives input end to end. Needs the Debug Probe + Pico.
- [ ] **13. Local input handling for gaming** — so input goes to the target
  only. (Design-sensitive; revisit carefully.)
- [ ] **14. Completeness** — NKRO, horizontal wheel, host LED feedback,
  soft-detach / physical arm switch, remote Pico flashing.
- [ ] **15. Stable 1.0** — soak test, latency report, config validation,
  security review.

## Hardware

- Raspberry Pi Pico W — **have**.
- Raspberry Pi Debug Probe (or 3.3 V USB-serial) — **to get**.
- Good micro-USB **data** cable; connect the Pico to a rear USB port on the
  target.
