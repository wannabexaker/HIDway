/* CRC-16/CCITT-FALSE: poly 0x1021, init 0xFFFF, no reflection, xorout 0. */
#ifndef HIDWAY_CRC16_H
#define HIDWAY_CRC16_H

#include <stddef.h>
#include <stdint.h>

uint16_t hidway_crc16(const uint8_t *data, size_t len);

#endif /* HIDWAY_CRC16_H */
