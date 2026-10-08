/*
 * HIDway end-to-end encryption (optional, pre-shared key).
 *
 * When a key is configured on both the client and the relay, every UDP
 * datagram travels as a sealed envelope, so any network in between (a VPN
 * provider, Cloudflare, a relay) sees only ciphertext:
 *
 *   'H' | 2 | SEALED | nonce[24] | E( ts_us[8] || inner packet ) | mac[16]
 *
 * E is XChaCha20-Poly1305 (Monocypher crypto_aead_lock) with the 3 header
 * bytes as associated data; the nonce is random per packet. `inner` is the
 * unchanged plaintext protocol packet (protocol.h). ts_us is the sender's
 * monotonic wall-clock time in microseconds; the receiver of input packets
 * (the relay) uses it with hidway_replay_accept() to reject replays.
 *
 * Without a key nothing changes: the plaintext protocol is used as before.
 * This file is pure C11 with no OS calls; callers supply the random nonce.
 */
#ifndef HIDWAY_CRYPTO_H
#define HIDWAY_CRYPTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HIDWAY_KEY_LEN    32
#define HIDWAY_NONCE_LEN  24
#define HIDWAY_MAC_LEN    16
#define HIDWAY_TS_LEN     8
#define HIDWAY_SEAL_HDR   3
#define HIDWAY_PROTO_VER_SEALED 2
#define HIDWAY_MSG_SEALED 0x10

/* Bytes added around an inner packet. */
#define HIDWAY_SEAL_OVERHEAD (HIDWAY_SEAL_HDR + HIDWAY_NONCE_LEN + HIDWAY_TS_LEN + HIDWAY_MAC_LEN)
/* Largest inner packet accepted (protocol packets are 51 bytes or less). */
#define HIDWAY_SEAL_MAX_INNER 64
#define HIDWAY_SEAL_MAX (HIDWAY_SEAL_MAX_INNER + HIDWAY_SEAL_OVERHEAD)

/* Seal `inner` into `out`. Returns the envelope length, or 0 if it does not
 * fit or the inner packet is too large. */
size_t hidway_seal(uint8_t *out, size_t cap, const uint8_t key[HIDWAY_KEY_LEN],
                   const uint8_t nonce[HIDWAY_NONCE_LEN], uint64_t ts_us,
                   const uint8_t *inner, size_t inner_len);

/* True if the datagram looks like a sealed envelope (header only). */
bool hidway_is_sealed(const uint8_t *in, size_t len);

/* Verify and decrypt. Returns the inner length copied to `out`, or 0 if the
 * envelope is malformed, forged, tampered with or sealed with another key. */
size_t hidway_open(uint8_t *out, size_t cap, const uint8_t key[HIDWAY_KEY_LEN],
                   const uint8_t *in, size_t len, uint64_t *ts_us);

/* Replay guard for the receiving side. A timestamp is accepted only if it is
 * strictly newer than every timestamp accepted before and within +/- window
 * of the local wall clock (which bounds replays after a restart). */
typedef struct {
    uint64_t last_ts_us;
} hidway_replay_t;

bool hidway_replay_accept(hidway_replay_t *r, uint64_t ts_us, uint64_t now_us, uint64_t window_us);

/* Key as 64 hex characters. Parse ignores surrounding whitespace. */
bool hidway_key_parse(const char *hex, uint8_t key[HIDWAY_KEY_LEN]);
void hidway_key_format(const uint8_t key[HIDWAY_KEY_LEN], char out[2 * HIDWAY_KEY_LEN + 1]);

/* Zero a secret in a way the compiler will not optimise away. */
void hidway_wipe(void *p, size_t n);

#endif /* HIDWAY_CRYPTO_H */
