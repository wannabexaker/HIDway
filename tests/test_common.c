/* Host unit tests for common/ (built with the host compiler, run by ctest). */
#include <stdio.h>
#include <string.h>

#include "cobs.h"
#include "crc16.h"
#include "hidway_kbd.h"
#include "hidway_motion.h"
#include "keymap_sc2hid.h"
#include "protocol.h"
#include "serial.h"

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

static void test_keymap(void)
{
    char tmp[8];

    /* Scan code -> usage, layout-independent (position based). */
    CHECK(hidway_sc_to_usage(0x11, false) == 0x1A); /* W */
    CHECK(hidway_sc_to_usage(0x1E, false) == 0x04); /* A */
    CHECK(hidway_sc_to_usage(0x39, false) == 0x2C); /* Space */
    CHECK(hidway_sc_to_usage(0x1C, false) == 0x28); /* Enter */
    CHECK(hidway_sc_to_usage(0x58, false) == 0x45); /* F12 */
    CHECK(hidway_sc_to_usage(0x48, true) == 0x52);  /* Up (extended) */
    CHECK(hidway_sc_to_usage(0x1C, true) == 0x58);  /* Keypad Enter (extended) */

    /* Modifiers are reported separately, never as usages. */
    CHECK(hidway_sc_to_usage(0x1D, false) == 0);
    CHECK(hidway_sc_to_modifier(0x1D, false) == 0x01); /* LCtrl */
    CHECK(hidway_sc_to_modifier(0x2A, false) == 0x02); /* LShift */
    CHECK(hidway_sc_to_modifier(0x38, false) == 0x04); /* LAlt */
    CHECK(hidway_sc_to_modifier(0x1D, true) == 0x10);  /* RCtrl */
    CHECK(hidway_sc_to_modifier(0x38, true) == 0x40);  /* RAlt */
    CHECK(hidway_sc_to_modifier(0x5B, true) == 0x08);  /* LGui */

    /* Same code, extended flag changes meaning (RCtrl vs LCtrl). */
    CHECK(hidway_sc_to_modifier(0x1D, false) != hidway_sc_to_modifier(0x1D, true));

    /* Unmapped / out of range. */
    CHECK(hidway_sc_to_usage(0x00, false) == 0);
    CHECK(hidway_sc_to_usage(0xF0, false) == 0);
    CHECK(hidway_sc_to_modifier(0x11, false) == 0);

    /* Display names. */
    CHECK(strcmp(hidway_usage_name(0x1A, tmp), "W") == 0);
    CHECK(strcmp(hidway_usage_name(0x2C, tmp), "Space") == 0);
    CHECK(strcmp(hidway_usage_name(0x3A, tmp), "F1") == 0);
    CHECK(strcmp(hidway_usage_name(0x27, tmp), "0") == 0);
    CHECK(strcmp(hidway_usage_name(0x52, tmp), "Up") == 0);
}

static void test_protocol_input(void)
{
    CHECK(HIDWAY_INPUT_PKT_LEN == 51);

    hidway_input_pkt_t in = {0}, out;
    in.type = HIDWAY_MSG_STATE;
    in.session_id = 0xDEADBEEF;
    in.seq = 0x01020304;
    in.client_time_us = 123456789;
    in.mods = 0x05;
    hidway_bitmap_set(in.keys, 0x1A); /* W */
    hidway_bitmap_set(in.keys, 0xA7); /* highest usage */
    in.buttons = 0x13;
    in.x = -70000;
    in.y = 65000;
    in.wheel = -3;
    in.pan = 7;

    uint8_t buf[HIDWAY_INPUT_PKT_LEN];
    CHECK(hidway_input_encode(buf, &in) == HIDWAY_INPUT_PKT_LEN);
    CHECK(buf[0] == 'H' && buf[1] == HIDWAY_PROTO_VER);
    CHECK(hidway_input_decode(buf, sizeof buf, &out));
    CHECK(out.session_id == in.session_id && out.seq == in.seq);
    CHECK(out.client_time_us == in.client_time_us);
    CHECK(out.mods == in.mods && out.buttons == in.buttons);
    CHECK(memcmp(out.keys, in.keys, HIDWAY_KEY_BITMAP_BYTES) == 0);
    CHECK(out.x == -70000 && out.y == 65000); /* beyond int16: needs 32-bit */
    CHECK(out.wheel == -3 && out.pan == 7);

    /* RELEASE normalizes to nothing held regardless of payload. */
    uint8_t rel[HIDWAY_INPUT_PKT_LEN];
    in.type = HIDWAY_MSG_RELEASE;
    hidway_input_encode(rel, &in);
    CHECK(hidway_input_decode(rel, sizeof rel, &out));
    CHECK(out.mods == 0 && out.buttons == 0);
    for (size_t i = 0; i < HIDWAY_KEY_BITMAP_BYTES; i++)
        CHECK(out.keys[i] == 0);

    /* Rejections: wrong length, magic, version, type. */
    CHECK(!hidway_input_decode(buf, HIDWAY_INPUT_PKT_LEN - 1, &out));
    uint8_t bad[HIDWAY_INPUT_PKT_LEN];
    memcpy(bad, buf, sizeof bad);
    bad[0] = 'X';
    CHECK(!hidway_input_decode(bad, sizeof bad, &out));
    memcpy(bad, buf, sizeof bad);
    bad[1] = 99;
    CHECK(!hidway_input_decode(bad, sizeof bad, &out));
    memcpy(bad, buf, sizeof bad);
    bad[2] = 7;
    CHECK(!hidway_input_decode(bad, sizeof bad, &out));
}

static void test_protocol_status(void)
{
    CHECK(HIDWAY_STATUS_PKT_LEN == 25);
    hidway_status_pkt_t in = {0}, out;
    in.session_id = 0x11223344;
    in.seq = 999;
    in.client_time_us = 42;
    in.frames_ok = 123456;
    in.seq_gaps = 7;
    in.flags = 0x0003;
    in.leds = 0x02;

    uint8_t buf[HIDWAY_STATUS_PKT_LEN];
    CHECK(hidway_status_encode(buf, &in) == HIDWAY_STATUS_PKT_LEN);
    CHECK(hidway_status_decode(buf, sizeof buf, &out));
    CHECK(out.session_id == in.session_id && out.seq == in.seq);
    CHECK(out.client_time_us == in.client_time_us && out.frames_ok == in.frames_ok);
    CHECK(out.seq_gaps == in.seq_gaps && out.flags == in.flags && out.leds == in.leds);
    CHECK(!hidway_status_decode(buf, 4, &out));
}

static void test_crc16(void)
{
    /* CRC-16/CCITT-FALSE test vector: "123456789" -> 0x29B1. */
    const uint8_t v[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK(hidway_crc16(v, sizeof v) == 0x29B1);
    CHECK(hidway_crc16((const uint8_t *)"", 0) == 0xFFFF);
}

static void cobs_roundtrip(const uint8_t *in, size_t len)
{
    uint8_t enc[1024], dec[1024];
    size_t e = hidway_cobs_encode(in, len, enc);
    for (size_t i = 0; i < e; i++)
        CHECK(enc[i] != 0x00); /* encoded data must be zero-free */
    size_t d = hidway_cobs_decode(enc, e, dec);
    CHECK(d == len);
    CHECK(memcmp(dec, in, len) == 0);
}

static void test_cobs(void)
{
    uint8_t a[] = {1, 2, 3};
    uint8_t b[] = {0, 0, 0};
    uint8_t c[] = {1, 0, 2, 0, 0, 3};
    cobs_roundtrip(a, sizeof a);
    cobs_roundtrip(b, sizeof b);
    cobs_roundtrip(c, sizeof c);

    /* Long run with no zeros (forces the 0xFF code split at 254). */
    uint8_t big[600];
    for (size_t i = 0; i < sizeof big; i++)
        big[i] = (uint8_t)(i % 7 == 0 ? 0 : (i & 0xFF) | 1);
    cobs_roundtrip(big, sizeof big);

    uint8_t allnz[300];
    for (size_t i = 0; i < sizeof allnz; i++)
        allnz[i] = (uint8_t)((i & 0xFE) | 1);
    cobs_roundtrip(allnz, sizeof allnz);

    /* A 0x00 inside encoded data is malformed. */
    uint8_t bad[] = {2, 5, 0, 3};
    uint8_t out[16];
    CHECK(hidway_cobs_decode(bad, sizeof bad, out) == 0);
}

static void test_serial(void)
{
    CHECK(HIDWAY_SER_PAYLOAD_LEN == 40);

    hidway_serial_state_t s = {0}, got = {0};
    s.type = HIDWAY_SER_STATE;
    s.seq = 0xBEEF;
    s.mods = 0x11;
    hidway_bitmap_set(s.keys, 0x1A);
    hidway_bitmap_set(s.keys, 0x04);
    s.buttons = 0x0A;
    s.x = -123456;
    s.y = 99999;
    s.wheel = -5;
    s.pan = 3;

    uint8_t frame[HIDWAY_SER_FRAME_MAX];
    size_t n = hidway_serial_build(&s, frame, sizeof frame);
    CHECK(n > 0);
    CHECK(frame[n - 1] == 0x00); /* delimiter */

    hidway_deframer_t d;
    hidway_deframer_reset(&d);
    bool done = false;
    for (size_t i = 0; i < n; i++)
        done = hidway_deframer_push(&d, frame[i], &got);
    CHECK(done);
    CHECK(got.type == s.type && got.seq == s.seq && got.mods == s.mods);
    CHECK(memcmp(got.keys, s.keys, HIDWAY_KEY_BITMAP_BYTES) == 0);
    CHECK(got.buttons == s.buttons);
    CHECK(got.x == s.x && got.y == s.y && got.wheel == s.wheel && got.pan == s.pan);

    /* Corruption: flip a byte in the middle -> CRC rejects the frame. */
    hidway_deframer_reset(&d);
    frame[3] ^= 0xFF;
    done = false;
    for (size_t i = 0; i < n; i++)
        done = hidway_deframer_push(&d, frame[i], &got);
    CHECK(!done);
    frame[3] ^= 0xFF;

    /* Leading noise before the first delimiter is skipped cleanly. */
    hidway_deframer_reset(&d);
    hidway_deframer_push(&d, 0x11, &got);
    hidway_deframer_push(&d, 0x22, &got);
    hidway_deframer_push(&d, 0x00, &got); /* flush garbage */
    done = false;
    for (size_t i = 0; i < n; i++)
        done = hidway_deframer_push(&d, frame[i], &got);
    CHECK(done);

    /* A frame cut short (partial write), then the relay's resync delimiter:
     * the fragment is dropped and the next full frame decodes. */
    hidway_deframer_reset(&d);
    for (size_t i = 0; i < n / 2; i++)
        CHECK(!hidway_deframer_push(&d, frame[i], &got));
    CHECK(!hidway_deframer_push(&d, 0x00, &got));
    done = false;
    for (size_t i = 0; i < n; i++)
        done = hidway_deframer_push(&d, frame[i], &got);
    CHECK(done && got.seq == s.seq);

    /* A bare delimiter (empty frame) is ignored. */
    hidway_deframer_reset(&d);
    CHECK(!hidway_deframer_push(&d, 0x00, &got));
    done = false;
    for (size_t i = 0; i < n; i++)
        done = hidway_deframer_push(&d, frame[i], &got);
    CHECK(done);

    /* RELEASE normalizes to nothing held. */
    s.type = HIDWAY_SER_RELEASE;
    n = hidway_serial_build(&s, frame, sizeof frame);
    hidway_deframer_reset(&d);
    done = false;
    for (size_t i = 0; i < n; i++)
        done = hidway_deframer_push(&d, frame[i], &got);
    CHECK(done && got.mods == 0 && got.buttons == 0);
    for (size_t i = 0; i < HIDWAY_KEY_BITMAP_BYTES; i++)
        CHECK(got.keys[i] == 0);
}

int main(void)
{
    test_bitmap();
    test_keymap();
    test_protocol_input();
    test_protocol_status();
    test_crc16();
    test_cobs();
    test_serial();
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
