/*
 * HIDway serial link framing (Raspberry Pi <-> Pico).
 *
 * One frame = COBS( payload + CRC16 ) + 0x00 delimiter. The payload is the
 * full current state (same philosophy as the UDP protocol: latest state wins,
 * never events), so a corrupted frame is simply dropped and the next one
 * restores the state.
 */
#ifndef HIDWAY_SERIAL_H
#define HIDWAY_SERIAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cobs.h"
#include "hidway_kbd.h"

enum {
    HIDWAY_SER_STATE = 1,
    HIDWAY_SER_RELEASE = 2,
};

typedef struct {
    uint8_t type;
    uint16_t seq;
    uint8_t mods;
    uint8_t keys[HIDWAY_KEY_BITMAP_BYTES];
    uint8_t buttons;
    int32_t x, y;        /* cumulative mouse counters */
    int16_t wheel, pan;  /* cumulative wheel / AC Pan */
} hidway_serial_state_t;

#define HIDWAY_SER_PAYLOAD_LEN (1 + 2 + 1 + HIDWAY_KEY_BITMAP_BYTES + 1 + 8 + 4 + 2) /* +crc = 40 */
#define HIDWAY_SER_FRAME_MAX   (HIDWAY_COBS_MAX(HIDWAY_SER_PAYLOAD_LEN) + 1)         /* +delimiter */

/* Build a framed message (COBS + trailing 0x00) into `out`.
 * Returns the framed length, or 0 if `cap` is too small. */
size_t hidway_serial_build(const hidway_serial_state_t *s, uint8_t *out, size_t cap);

/* Incremental receiver: feed bytes as they arrive. Returns true and fills
 * *out when a complete, CRC-valid frame ends at this byte (a 0x00). */
typedef struct {
    uint8_t buf[HIDWAY_SER_FRAME_MAX];
    size_t len;
    bool overflow;
} hidway_deframer_t;

void hidway_deframer_reset(hidway_deframer_t *d);
bool hidway_deframer_push(hidway_deframer_t *d, uint8_t byte, hidway_serial_state_t *out);

#endif /* HIDWAY_SERIAL_H */
