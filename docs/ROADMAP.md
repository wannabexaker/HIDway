# HIDway roadmap

The design rationale behind these phases is in [ARCHITECTURE.md](ARCHITECTURE.md).
Live progress is tracked in [CHECKLIST.md](CHECKLIST.md).

### Phase 0 — Go / No-Go
Prove the target accepts input from the Pico as a real USB HID device before
building anything else. `HIDWAY_T0=ON` firmware: while BOOTSEL is held, it
holds a key and moves the mouse. If the target rejects it, the project stops.

### Phase 1 — Transport + input, on the LAN
Shared protocol, Windows client (Raw Input + arm/disarm UI), UDP over
Tailscale, and the `hidwayd` relay. Serial link from the Pi to the Pico, with a
link-timeout release on the Pico.

### Phase 2 — Internet hardening
Direct (non-relayed) path where the network allows it, status/RTT feedback,
auto-release on link loss, edge-case redundancy, Tailscale ACL + source
allow-list, rate limiting.

### Phase 3 — Completeness
NKRO keyboard, horizontal wheel, host LED feedback, soft-detach / physical arm
switch, remote flashing of the Pico.

### Phase 4 — Stable 1.0
Soak testing, latency report, config validation, full docs, security review.
