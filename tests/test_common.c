/* Host unit tests for common/ (built with the host compiler, run by ctest). */
#include <stdio.h>
#include <string.h>

#include "hidway_kbd.h"
#include "hidway_motion.h"

static int failures;

#define CHECK(cond)                                                    \
    do {                                                               \
        if (!(cond)) {                                                 \
            printf("%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                \
        }                                                              \
    } while (0)

#define KEY_A 0x04
#define KEY_B 0x05
#define KEY_C 0x06
#define KEY_D 0x07
#define KEY_E 0x08
#define KEY_F 0x09
#define KEY_W 0x1A

static int slot_count(const hidway_kro6_t *k)
{
    int n = 0;
    for (int i = 0; i < HIDWAY_BOOT_KEYS; i++)
        if (k->slots[i])
            n++;
    return n;
}

static int has_slot(const hidway_kro6_t *k, uint8_t usage)
{
    for (int i = 0; i < HIDWAY_BOOT_KEYS; i++)
        if (k->slots[i] == usage)
            return 1;
    return 0;
}

static void test_bitmap(void)
{
    uint8_t bm[HIDWAY_KEY_BITMAP_BYTES] = {0};
    CHECK(HIDWAY_KEY_BITMAP_BYTES == 21);
    hidway_bitmap_set(bm, KEY_W);
    hidway_bitmap_set(bm, HIDWAY_KEY_USAGE_MAX);
    CHECK(hidway_bitmap_test(bm, KEY_W));
    CHECK(hidway_bitmap_test(bm, HIDWAY_KEY_USAGE_MAX));
    CHECK(!hidway_bitmap_test(bm, KEY_A));
    hidway_bitmap_set(bm, 0xE0); /* out of range: modifiers travel separately */
    CHECK(!hidway_bitmap_test(bm, 0xE0));
    hidway_bitmap_clear(bm, KEY_W);
    CHECK(!hidway_bitmap_test(bm, KEY_W));
}

static void test_kro6_basic(void)
{
    hidway_kro6_t k;
    uint8_t bm[HIDWAY_KEY_BITMAP_BYTES] = {0};
    hidway_kro6_reset(&k);

    CHECK(!hidway_kro6_update(&k, bm));
    CHECK(slot_count(&k) == 0);

    hidway_bitmap_set(bm, KEY_W);
    CHECK(hidway_kro6_update(&k, bm));
    CHECK(k.slots[0] == KEY_W && slot_count(&k) == 1);
    CHECK(!hidway_kro6_update(&k, bm)); /* same state: no change */

    hidway_bitmap_clear(bm, KEY_W);
    CHECK(hidway_kro6_update(&k, bm));
    CHECK(slot_count(&k) == 0);
}

static void test_kro6_reserved_usages_ignored(void)
{
    hidway_kro6_t k;
    uint8_t bm[HIDWAY_KEY_BITMAP_BYTES] = {0};
    hidway_kro6_reset(&k);
    bm[0] = 0x0F; /* usages 0x00..0x03: none / error codes */
    CHECK(!hidway_kro6_update(&k, bm));
    CHECK(slot_count(&k) == 0);
}

static void test_kro6_rollover_keeps_held_keys(void)
{
    hidway_kro6_t k;
    uint8_t bm[HIDWAY_KEY_BITMAP_BYTES] = {0};
    hidway_kro6_reset(&k);

    /* W held first, then six more keys: W must never be dropped. */
    hidway_bitmap_set(bm, KEY_W);
    hidway_kro6_update(&k, bm);
    const uint8_t extra[] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F};
    for (size_t i = 0; i < sizeof extra; i++) {
        hidway_bitmap_set(bm, extra[i]);
        hidway_kro6_update(&k, bm);
        CHECK(has_slot(&k, KEY_W));
    }
    CHECK(slot_count(&k) == 6);
    CHECK(!has_slot(&k, KEY_F)); /* 7th key waits */

    /* Releasing one frees a slot for the waiting key; order stays packed. */
    hidway_bitmap_clear(bm, KEY_B);
    CHECK(hidway_kro6_update(&k, bm));
    CHECK(slot_count(&k) == 6);
    CHECK(has_slot(&k, KEY_F) && !has_slot(&k, KEY_B) && has_slot(&k, KEY_W));
    CHECK(k.slots[0] == KEY_W);

    /* Packing: no zero before a non-zero slot. */
    hidway_bitmap_clear(bm, KEY_A);
    hidway_bitmap_clear(bm, KEY_C);
    hidway_kro6_update(&k, bm);
    int seen_zero = 0, ok = 1;
    for (int i = 0; i < HIDWAY_BOOT_KEYS; i++) {
        if (k.slots[i] == 0)
            seen_zero = 1;
        else if (seen_zero)
            ok = 0;
    }
    CHECK(ok);
    CHECK(slot_count(&k) == 4);

    memset(bm, 0, sizeof bm);
    hidway_kro6_update(&k, bm);
    CHECK(slot_count(&k) == 0);
}

static void test_motion_chunking(void)
{
    hidway_motion_t m, c;
    hidway_motion_clear(&m);
    CHECK(!hidway_motion_pending(&m));

    hidway_motion_add(&m, 100000, -40000, 300, -2);
    CHECK(hidway_motion_pending(&m));

    hidway_motion_take(&m, 32767, 127, &c);
    CHECK(c.x == 32767 && c.y == -32767 && c.wheel == 127 && c.pan == -2);
    CHECK(m.x == 100000 - 32767 && m.y == -40000 + 32767 && m.wheel == 173 && m.pan == 0);

    /* Total motion is conserved across chunks. */
    long sum_x = c.x, sum_y = c.y, sum_w = c.wheel;
    while (hidway_motion_pending(&m)) {
        hidway_motion_take(&m, 32767, 127, &c);
        sum_x += c.x;
        sum_y += c.y;
        sum_w += c.wheel;
    }
    CHECK(sum_x == 100000 && sum_y == -40000 && sum_w == 300);
}

static void test_motion_give_back(void)
{
    hidway_motion_t m, c;
    hidway_motion_clear(&m);
    hidway_motion_add(&m, 10, -3, 1, 0);
    hidway_motion_take(&m, 127, 127, &c);
    CHECK(!hidway_motion_pending(&m));
    hidway_motion_give_back(&m, &c);
    CHECK(m.x == 10 && m.y == -3 && m.wheel == 1);
}

static void test_motion_saturation(void)
{
    hidway_motion_t m;
    hidway_motion_clear(&m);
    for (int i = 0; i < 1000; i++)
        hidway_motion_add(&m, 0x7FFFFFFF, -0x7FFFFFFF, 0, 0);
    CHECK(m.x == HIDWAY_MOTION_CAP && m.y == -HIDWAY_MOTION_CAP);
}

static void test_motion_wheel_limit_zero(void)
{
    hidway_motion_t m, c;
    hidway_motion_clear(&m);
    hidway_motion_add(&m, 5, 0, 3, 0);
    hidway_motion_take(&m, 127, 0, &c);
    CHECK(c.x == 5 && c.wheel == 0);
    CHECK(m.wheel == 3); /* caller must clear it explicitly (boot protocol) */
}

int main(void)
{
    test_bitmap();
    test_kro6_basic();
    test_kro6_reserved_usages_ignored();
    test_kro6_rollover_keeps_held_keys();
    test_motion_chunking();
    test_motion_give_back();
    test_motion_saturation();
    test_motion_wheel_limit_zero();

    if (failures) {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("all common tests passed\n");
    return 0;
}
