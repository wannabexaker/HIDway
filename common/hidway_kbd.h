/*
 * HIDway - keyboard state helpers shared by firmware, relay and client.
 *
 * Key state travels as a bitmap of HID Keyboard/Keypad page usages
 * (0x00..0xA7); modifiers (0xE0..0xE7) travel separately as one byte.
 */
#ifndef HIDWAY_KBD_H
#define HIDWAY_KBD_H

#include <stdbool.h>
#include <stdint.h>

#define HIDWAY_KEY_USAGE_MIN    0x04u /* 0x00..0x03 are reserved/error codes */
#define HIDWAY_KEY_USAGE_MAX    0xA7u
#define HIDWAY_KEY_BITMAP_BITS  (HIDWAY_KEY_USAGE_MAX + 1u)
#define HIDWAY_KEY_BITMAP_BYTES ((HIDWAY_KEY_BITMAP_BITS + 7u) / 8u)
#define HIDWAY_BOOT_KEYS        6

static inline bool hidway_bitmap_test(const uint8_t *bm, uint8_t usage)
{
    return usage < HIDWAY_KEY_BITMAP_BITS && ((bm[usage >> 3] >> (usage & 7u)) & 1u);
}

static inline void hidway_bitmap_set(uint8_t *bm, uint8_t usage)
{
    if (usage < HIDWAY_KEY_BITMAP_BITS)
        bm[usage >> 3] |= (uint8_t)(1u << (usage & 7u));
}

static inline void hidway_bitmap_clear(uint8_t *bm, uint8_t usage)
{
    if (usage < HIDWAY_KEY_BITMAP_BITS)
        bm[usage >> 3] &= (uint8_t)~(1u << (usage & 7u));
}

/*
 * 6-key rollover slots for the boot keyboard report.
 *
 * A key that already occupies a slot keeps it while held, so pressing a
 * 7th key never drops one that is already reported (no phantom release of
 * e.g. a held movement key). Extra keys wait until a slot frees up.
 * Occupied slots are always packed at the front of the array.
 */
typedef struct {
    uint8_t slots[HIDWAY_BOOT_KEYS];
} hidway_kro6_t;

void hidway_kro6_reset(hidway_kro6_t *k);

/* Reconcile slots with the bitmap. Returns true if the slots changed. */
bool hidway_kro6_update(hidway_kro6_t *k, const uint8_t bitmap[HIDWAY_KEY_BITMAP_BYTES]);

#endif /* HIDWAY_KBD_H */
