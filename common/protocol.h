/*
 * HIDway wire protocol - single source of truth for client, relay and Pico.
 *
 * Design: every input packet carries the COMPLETE current state, never
 * events. A lost or reordered packet is corrected by the next one, so a
 * missed key-up can never leave a key stuck. Mouse movement travels as
 * cumulative counters; the receiver applies the signed, wrap-safe difference
 * from the previous packet, so a dropped packet loses no net movement.
 *
 * All multi-byte fields are little-endian, encoded/decoded explicitly here so
 * the format is identical on x86, ARM and the RP2040 regardless of their
 * native endianness.
 */
#ifndef HIDWAY_PROTOCOL_H
#define HIDWAY_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hidway_kbd.h"

#define HIDWAY_PROTO_MAGIC 0x48 /* 'H' */
#define HIDWAY_PROTO_VER   1

enum {
    HIDWAY_MSG_STATE = 1,   /* client -> relay: full keyboard+mouse state */
    HIDWAY_MSG_RELEASE = 2, /* client -> relay: release everything now */
    HIDWAY_MSG_STATUS = 0x81 /* relay -> client: counters + RTT echo */
};

/* Full input state plus its header. */
typedef struct {
    uint8_t type;  /* HIDWAY_MSG_STATE or HIDWAY_MSG_RELEASE */
    uint8_t flags; /* reserved, 0 */
    uint32_t session_id;     /* random per client run; a change means reset */
    uint32_t seq;            /* strictly increasing within a session */
    uint32_t client_time_us; /* client clock, echoed back for RTT */

    uint8_t mods;                          /* modifier bits, 1<<(usage-0xE0) */
    uint8_t keys[HIDWAY_KEY_BITMAP_BYTES]; /* usage bitmap 0x00..0xA7 */
    uint8_t buttons;                       /* L,R,M,X1,X2 */
    int32_t x, y;                          /* cumulative mouse counters */
    int16_t wheel, pan;                    /* cumulative wheel / AC Pan */
} hidway_input_pkt_t;

#define HIDWAY_INPUT_PKT_LEN (4 + 12 + 1 + HIDWAY_KEY_BITMAP_BYTES + 1 + 8 + 4)
/* = 4 header + 12 ids + 1 mods + 21 keys + 1 buttons + 8 xy + 4 wheels = 51 */

typedef struct {
    uint32_t session_id;     /* echo of the client's session */
    uint32_t seq;            /* echo of the last applied seq */
    uint32_t client_time_us; /* echo for RTT = now - this */
    uint32_t frames_ok;      /* input packets accepted since start */
    uint16_t seq_gaps;       /* detected gaps in seq (loss/reorder estimate) */
    uint16_t flags;          /* bit0 usb_mounted, bit1 armed (future) */
    uint8_t leds;            /* host keyboard LEDs (future) */
} hidway_status_pkt_t;

#define HIDWAY_STATUS_PKT_LEN (4 + 16 + 2 + 2 + 1) /* = 25 */

/* ----------------------------------------------------------- LE helpers */

static inline void hidway_wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
static inline void hidway_wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
static inline uint16_t hidway_rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
static inline uint32_t hidway_rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------- input encode/decode */

static inline size_t hidway_input_encode(uint8_t out[HIDWAY_INPUT_PKT_LEN], const hidway_input_pkt_t *p)
{
    size_t i = 0;
    out[i++] = HIDWAY_PROTO_MAGIC;
    out[i++] = HIDWAY_PROTO_VER;
    out[i++] = p->type;
    out[i++] = p->flags;
    hidway_wr32(out + i, p->session_id); i += 4;
    hidway_wr32(out + i, p->seq);        i += 4;
    hidway_wr32(out + i, p->client_time_us); i += 4;
    out[i++] = p->mods;
    memcpy(out + i, p->keys, HIDWAY_KEY_BITMAP_BYTES); i += HIDWAY_KEY_BITMAP_BYTES;
    out[i++] = p->buttons;
    hidway_wr32(out + i, (uint32_t)p->x); i += 4;
    hidway_wr32(out + i, (uint32_t)p->y); i += 4;
    hidway_wr16(out + i, (uint16_t)p->wheel); i += 2;
    hidway_wr16(out + i, (uint16_t)p->pan);   i += 2;
    return i; /* HIDWAY_INPUT_PKT_LEN */
}

static inline bool hidway_input_decode(const uint8_t *in, size_t len, hidway_input_pkt_t *p)
{
    if (len != HIDWAY_INPUT_PKT_LEN || in[0] != HIDWAY_PROTO_MAGIC || in[1] != HIDWAY_PROTO_VER)
        return false;
    uint8_t type = in[2];
    if (type != HIDWAY_MSG_STATE && type != HIDWAY_MSG_RELEASE)
        return false;

    size_t i = 2;
    p->type = in[i++];
    p->flags = in[i++];
    p->session_id = hidway_rd32(in + i); i += 4;
    p->seq = hidway_rd32(in + i);        i += 4;
    p->client_time_us = hidway_rd32(in + i); i += 4;
    p->mods = in[i++];
    memcpy(p->keys, in + i, HIDWAY_KEY_BITMAP_BYTES); i += HIDWAY_KEY_BITMAP_BYTES;
    p->buttons = in[i++];
    p->x = (int32_t)hidway_rd32(in + i); i += 4;
    p->y = (int32_t)hidway_rd32(in + i); i += 4;
    p->wheel = (int16_t)hidway_rd16(in + i); i += 2;
    p->pan = (int16_t)hidway_rd16(in + i);   i += 2;

    if (p->type == HIDWAY_MSG_RELEASE) {
        /* Normalize so a RELEASE is unambiguously "nothing held". */
        p->mods = 0;
        memset(p->keys, 0, HIDWAY_KEY_BITMAP_BYTES);
        p->buttons = 0;
    }
    return true;
}

/* ------------------------------------------------------ status encode/decode */

static inline size_t hidway_status_encode(uint8_t out[HIDWAY_STATUS_PKT_LEN], const hidway_status_pkt_t *p)
{
    size_t i = 0;
    out[i++] = HIDWAY_PROTO_MAGIC;
    out[i++] = HIDWAY_PROTO_VER;
    out[i++] = HIDWAY_MSG_STATUS;
    out[i++] = 0;
    hidway_wr32(out + i, p->session_id); i += 4;
    hidway_wr32(out + i, p->seq);        i += 4;
    hidway_wr32(out + i, p->client_time_us); i += 4;
    hidway_wr32(out + i, p->frames_ok); i += 4;
    hidway_wr16(out + i, p->seq_gaps);  i += 2;
    hidway_wr16(out + i, p->flags);     i += 2;
    out[i++] = p->leds;
    return i; /* HIDWAY_STATUS_PKT_LEN */
}

static inline bool hidway_status_decode(const uint8_t *in, size_t len, hidway_status_pkt_t *p)
{
    if (len != HIDWAY_STATUS_PKT_LEN || in[0] != HIDWAY_PROTO_MAGIC ||
        in[1] != HIDWAY_PROTO_VER || in[2] != HIDWAY_MSG_STATUS)
        return false;
    size_t i = 4;
    p->session_id = hidway_rd32(in + i); i += 4;
    p->seq = hidway_rd32(in + i);        i += 4;
    p->client_time_us = hidway_rd32(in + i); i += 4;
    p->frames_ok = hidway_rd32(in + i); i += 4;
    p->seq_gaps = hidway_rd16(in + i);  i += 2;
    p->flags = hidway_rd16(in + i);     i += 2;
    p->leds = in[i++];
    return true;
}

#endif /* HIDWAY_PROTOCOL_H */
