/*
 * HID output stage: holds the desired keyboard/mouse state and turns it into
 * USB reports whenever an endpoint is free. Input sources (the T0 test mode
 * now, the UART link later) only ever write state here; they never build
 * reports themselves.
 */
#ifndef HIDWAY_HID_OUT_H
#define HIDWAY_HID_OUT_H

#include <stdbool.h>
#include <stdint.h>

#include "hidway_kbd.h"

#define HIDWAY_MOUSE_BUTTONS_MASK 0x1Fu /* L, R, M, back (4), forward (5) */

void hid_out_init(void);

/* Full keyboard state: modifier byte (bit n = usage 0xE0+n) + usage bitmap. */
void hid_out_set_keyboard(uint8_t modifiers, const uint8_t bitmap[HIDWAY_KEY_BITMAP_BYTES]);
void hid_out_set_buttons(uint8_t buttons);
void hid_out_add_motion(int32_t dx, int32_t dy, int32_t wheel, int32_t pan);

/* Release every key and button and drop pending motion. */
void hid_out_release_all(void);

/* Host keyboard LEDs (Num/Caps/Scroll...) from the last output report. */
uint8_t hid_out_leds(void);

/* Call from the main loop. */
void hid_out_task(void);

#endif /* HIDWAY_HID_OUT_H */
