/*
 * Consistent Overhead Byte Stuffing (COBS).
 *
 * Removes 0x00 from the payload so 0x00 can be used as a frame delimiter on the
 * serial link. Encoding never emits a 0x00; the caller appends the 0x00
 * delimiter itself.
 */
#ifndef HIDWAY_COBS_H
#define HIDWAY_COBS_H

#include <stddef.h>
#include <stdint.h>

/* Max encoded size for a payload of `n` bytes (excluding the 0x00 delimiter). */
#define HIDWAY_COBS_MAX(n) ((n) + (n) / 254 + 1)

/* Encode `len` bytes into `out` (capacity >= HIDWAY_COBS_MAX(len)).
 * Returns the number of bytes written (no trailing 0x00). */
size_t hidway_cobs_encode(const uint8_t *in, size_t len, uint8_t *out);

/* Decode `len` COBS bytes (without the 0x00 delimiter) into `out`
 * (capacity >= len). Returns decoded length, or 0 on malformed input. */
size_t hidway_cobs_decode(const uint8_t *in, size_t len, uint8_t *out);

#endif /* HIDWAY_COBS_H */
