# Monocypher (vendored)

- Upstream: https://github.com/LoupVaillant/Monocypher
- Version: 4.0.2 (files taken unmodified from the `4.0.2` tag: `src/monocypher.c`,
  `src/monocypher.h`, `LICENCE.md`)
- Licence: dual BSD-2-Clause / CC0-1.0 (see `LICENCE.md`)
- Used for: `crypto_aead_lock` / `crypto_aead_unlock` (XChaCha20-Poly1305) and
  `crypto_wipe`, through `common/hidway_crypto.c`.

Monocypher was independently audited (Cure53, 2020). The host test suite
checks this copy against the AEAD_XChaCha20_Poly1305 test vector from
draft-irtf-cfrg-xchacha-03, Appendix A.3.1. Do not edit these files; update by
replacing them with a newer upstream release and re-running the tests.
