# Security

## Threat model

HIDway injects real keyboard and mouse input into a target PC. Anyone who can
send accepted packets to the relay can type and click on that PC. The design
limits that to a single authenticated peer:

- **Transport:** input travels over Tailscale (WireGuard) — authenticated,
  encrypted, with replay protection. The HID endpoint is never exposed to the
  public internet.
- **End‑to‑end encryption (optional):** with a pre‑shared 32‑byte key on the
  client and the relay, every packet is sealed with XChaCha20‑Poly1305
  (Monocypher, vendored unmodified and checked against the IETF test vector).
  Packets carry a timestamp; the relay accepts only strictly newer timestamps
  within ±2 minutes of its clock, so captured traffic cannot be replayed. This
  keeps keystrokes confidential and unforgeable even on paths that terminate
  encryption in the middle (e.g. a Zero Trust proxy). With a key configured,
  plaintext is refused and neither end ever falls back to it.
- **Relay (`hidwayd`):** binds to the Tailscale interface only and accepts
  packets from a single allow‑listed peer address. Unknown sources are dropped.
- **Fail‑safe:** loss of the client or link releases all keys and buttons
  within a bounded time; a stuck key is not a reachable state.
- **Device:** the Pico enumerates as a standard HID device under its own
  identity. It contains no automation and no evasion logic.

### Residual risk

Whoever controls the relay host (root on the Raspberry Pi) can send input to
the target PC. This is inherent to any KVM. Protect the relay host: SSH keys
only, keep it patched, and keep the Tailscale ACL restricted to the intended
peer.

The end‑to‑end key is as sensitive as a password to the target PC: keep it
only in the client's `hidway.ini` and the relay's key file (mode 0640), never
in the repository, and rotate it if either machine is compromised. Status
replies to the client are authenticated but not replay‑checked; they only
affect what the client displays. Packet sizes and timing remain visible to
the network.

## Reporting a vulnerability

Please open a private report via GitHub Security Advisories on this
repository, or open an issue describing the problem without sensitive details.
Do not include personal IP addresses, hostnames or keys in public issues.
