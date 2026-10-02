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

## Todo

- [ ] **10. Pico UART receiver + HID output wired to the link** (replaces the
  T0 input source).
- [ ] **11. `hidwayd` serial output + release-on-timeout** (stubs are marked in
  `relay/hidwayd.c`).
- [ ] **12. Status/RTT in the client UI over the real path** (loss, RTT, relay
  counters) — plumbing exists; surface it.
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
