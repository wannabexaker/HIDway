#include "serial.h"

#include <string.h>

#include "crc16.h"
#include "protocol.h" /* hidway_wr16/wr32/rd16/rd32 LE helpers */

static size_t encode_payload(const hidway_serial_state_t *s, uint8_t *p)
{
    size_t i = 0;
    p[i++] = s->type;
    hidway_wr16(p + i, s->seq); i += 2;
    p[i++] = s->mods;
    memcpy(p + i, s->keys, HIDWAY_KEY_BITMAP_BYTES); i += HIDWAY_KEY_BITMAP_BYTES;
    p[i++] = s->buttons;
    hidway_wr32(p + i, (uint32_t)s->x); i += 4;
    hidway_wr32(p + i, (uint32_t)s->y); i += 4;
    hidway_wr16(p + i, (uint16_t)s->wheel); i += 2;
    hidway_wr16(p + i, (uint16_t)s->pan);   i += 2;
    uint16_t crc = hidway_crc16(p, i);
    hidway_wr16(p + i, crc); i += 2;
    return i; /* HIDWAY_SER_PAYLOAD_LEN */
}

static bool decode_payload(const uint8_t *p, size_t len, hidway_serial_state_t *s)
{
    if (len != HIDWAY_SER_PAYLOAD_LEN)
        return false;
    size_t body = len - 2;
    if (hidway_crc16(p, body) != hidway_rd16(p + body))
        return false;

    size_t i = 0;
    s->type = p[i++];
    s->seq = hidway_rd16(p + i); i += 2;
    s->mods = p[i++];
    memcpy(s->keys, p + i, HIDWAY_KEY_BITMAP_BYTES); i += HIDWAY_KEY_BITMAP_BYTES;
    s->buttons = p[i++];
    s->x = (int32_t)hidway_rd32(p + i); i += 4;
    s->y = (int32_t)hidway_rd32(p + i); i += 4;
    s->wheel = (int16_t)hidway_rd16(p + i); i += 2;
    s->pan = (int16_t)hidway_rd16(p + i);   i += 2;

    if (s->type == HIDWAY_SER_RELEASE) {
        s->mods = 0;
        memset(s->keys, 0, HIDWAY_KEY_BITMAP_BYTES);
        s->buttons = 0;
    }
    return true;
}

size_t hidway_serial_build(const hidway_serial_state_t *s, uint8_t *out, size_t cap)
{
    uint8_t payload[HIDWAY_SER_PAYLOAD_LEN];
    size_t plen = encode_payload(s, payload);

    if (cap < HIDWAY_COBS_MAX(plen) + 1)
        return 0;
    size_t n = hidway_cobs_encode(payload, plen, out);
    out[n++] = 0x00; /* delimiter */
    return n;
}

void hidway_deframer_reset(hidway_deframer_t *d)
{
    d->len = 0;
    d->overflow = false;
}

bool hidway_deframer_push(hidway_deframer_t *d, uint8_t byte, hidway_serial_state_t *out)
{
    if (byte != 0x00) {
        if (d->len < sizeof d->buf)
            d->buf[d->len++] = byte;
        else
            d->overflow = true; /* frame too long: poison until the delimiter */
        return false;
    }

    /* Delimiter: try to decode the accumulated frame. */
    bool ok = false;
    if (!d->overflow && d->len > 0) {
        uint8_t payload[HIDWAY_SER_FRAME_MAX];
        size_t plen = hidway_cobs_decode(d->buf, d->len, payload);
        if (plen > 0)
            ok = decode_payload(payload, plen, out);
    }
    d->len = 0;
    d->overflow = false;
    return ok;
}
