# Security

## Threat model

HIDway injects real keyboard and mouse input into a target PC. Anyone who can
send accepted packets to the relay can type and click on that PC. The design
limits that to a single authenticated peer:

- **Transport:** input travels over Tailscale (WireGuard) — authenticated,
  encrypted, with replay protection. The HID endpoint is never exposed to the
  public internet.
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

## Reporting a vulnerability

Please open a private report via GitHub Security Advisories on this
repository, or open an issue describing the problem without sensitive details.
Do not include personal IP addresses, hostnames or keys in public issues.
