#include "hidway_kbd.h"

#include <string.h>

void hidway_kro6_reset(hidway_kro6_t *k)
{
    memset(k->slots, 0, sizeof k->slots);
}

static bool kro6_contains(const hidway_kro6_t *k, uint8_t usage)
{
    for (int i = 0; i < HIDWAY_BOOT_KEYS; i++)
        if (k->slots[i] == usage)
            return true;
    return false;
}

bool hidway_kro6_update(hidway_kro6_t *k, const uint8_t bitmap[HIDWAY_KEY_BITMAP_BYTES])
{
    bool changed = false;
    int used = 0;

    /* Drop released keys, keeping the remaining ones packed and in order. */
    for (int i = 0; i < HIDWAY_BOOT_KEYS; i++) {
        uint8_t u = k->slots[i];
        if (u == 0)
            continue;
        if (hidway_bitmap_test(bitmap, u)) {
            k->slots[used++] = u;
        } else {
            changed = true;
        }
    }
    for (int i = used; i < HIDWAY_BOOT_KEYS; i++)
        k->slots[i] = 0;

    /* Fill free slots with newly held keys, lowest usage first. */
    for (unsigned u = HIDWAY_KEY_USAGE_MIN; u <= HIDWAY_KEY_USAGE_MAX && used < HIDWAY_BOOT_KEYS; u++) {
        if (!hidway_bitmap_test(bitmap, (uint8_t)u) || kro6_contains(k, (uint8_t)u))
            continue;
        k->slots[used++] = (uint8_t)u;
        changed = true;
    }

    return changed;
}
