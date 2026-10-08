#include "hidway_crypto.h"

#include <string.h>

#include "vendor/monocypher/monocypher.h"

#define MAGIC 0x48 /* 'H' */

static void wr64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t rd64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

size_t hidway_seal(uint8_t *out, size_t cap, const uint8_t key[HIDWAY_KEY_LEN],
                   const uint8_t nonce[HIDWAY_NONCE_LEN], uint64_t ts_us,
                   const uint8_t *inner, size_t inner_len)
{
    size_t body = HIDWAY_TS_LEN + inner_len;
    size_t total = HIDWAY_SEAL_HDR + HIDWAY_NONCE_LEN + body + HIDWAY_MAC_LEN;
    if (inner_len > HIDWAY_SEAL_MAX_INNER || cap < total)
        return 0;

    uint8_t plain[HIDWAY_TS_LEN + HIDWAY_SEAL_MAX_INNER];
    wr64(plain, ts_us);
    memcpy(plain + HIDWAY_TS_LEN, inner, inner_len);

    out[0] = MAGIC;
    out[1] = HIDWAY_PROTO_VER_SEALED;
    out[2] = HIDWAY_MSG_SEALED;
    memcpy(out + HIDWAY_SEAL_HDR, nonce, HIDWAY_NONCE_LEN);
    uint8_t *ct = out + HIDWAY_SEAL_HDR + HIDWAY_NONCE_LEN;
    crypto_aead_lock(ct, ct + body, key, nonce, out, HIDWAY_SEAL_HDR, plain, body);

    crypto_wipe(plain, sizeof plain);
    return total;
}

bool hidway_is_sealed(const uint8_t *in, size_t len)
{
    return len >= HIDWAY_SEAL_OVERHEAD && in[0] == MAGIC && in[1] == HIDWAY_PROTO_VER_SEALED &&
           in[2] == HIDWAY_MSG_SEALED;
}

size_t hidway_open(uint8_t *out, size_t cap, const uint8_t key[HIDWAY_KEY_LEN],
                   const uint8_t *in, size_t len, uint64_t *ts_us)
{
    if (!hidway_is_sealed(in, len))
        return 0;
    size_t body = len - HIDWAY_SEAL_HDR - HIDWAY_NONCE_LEN - HIDWAY_MAC_LEN;
    if (body < HIDWAY_TS_LEN || body - HIDWAY_TS_LEN > HIDWAY_SEAL_MAX_INNER)
        return 0;
    size_t inner_len = body - HIDWAY_TS_LEN;
    if (cap < inner_len)
        return 0;

    const uint8_t *nonce = in + HIDWAY_SEAL_HDR;
    const uint8_t *ct = nonce + HIDWAY_NONCE_LEN;
    const uint8_t *mac = ct + body;
    uint8_t plain[HIDWAY_TS_LEN + HIDWAY_SEAL_MAX_INNER];
    if (crypto_aead_unlock(plain, mac, key, nonce, in, HIDWAY_SEAL_HDR, ct, body) != 0)
        return 0; /* forged, tampered or wrong key */

    if (ts_us)
        *ts_us = rd64(plain);
    memcpy(out, plain + HIDWAY_TS_LEN, inner_len);
    crypto_wipe(plain, sizeof plain);
    return inner_len;
}

bool hidway_replay_accept(hidway_replay_t *r, uint64_t ts_us, uint64_t now_us, uint64_t window_us)
{
    uint64_t skew = ts_us > now_us ? ts_us - now_us : now_us - ts_us;
    if (skew > window_us || ts_us <= r->last_ts_us)
        return false;
    r->last_ts_us = ts_us;
    return true;
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool hidway_key_parse(const char *hex, uint8_t key[HIDWAY_KEY_LEN])
{
    if (!hex)
        return false;
    while (*hex == ' ' || *hex == '\t')
        hex++;
    uint8_t tmp[HIDWAY_KEY_LEN];
    for (int i = 0; i < HIDWAY_KEY_LEN; i++) {
        int hi = hexval(hex[2 * i]), lo = hi < 0 ? -1 : hexval(hex[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            crypto_wipe(tmp, sizeof tmp);
            return false;
        }
        tmp[i] = (uint8_t)(hi << 4 | lo);
    }
    const char *rest = hex + 2 * HIDWAY_KEY_LEN;
    while (*rest == ' ' || *rest == '\t' || *rest == '\r' || *rest == '\n')
        rest++;
    if (*rest) { /* trailing junk: not exactly one key */
        crypto_wipe(tmp, sizeof tmp);
        return false;
    }
    memcpy(key, tmp, HIDWAY_KEY_LEN);
    crypto_wipe(tmp, sizeof tmp);
    return true;
}

void hidway_key_format(const uint8_t key[HIDWAY_KEY_LEN], char out[2 * HIDWAY_KEY_LEN + 1])
{
    static const char digits[] = "0123456789abcdef";
    for (int i = 0; i < HIDWAY_KEY_LEN; i++) {
        out[2 * i] = digits[key[i] >> 4];
        out[2 * i + 1] = digits[key[i] & 15];
    }
    out[2 * HIDWAY_KEY_LEN] = 0;
}

void hidway_wipe(void *p, size_t n)
{
    crypto_wipe(p, n);
}
