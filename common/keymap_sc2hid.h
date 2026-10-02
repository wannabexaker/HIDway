/*
 * HIDway - Windows Set-1 scan code -> USB HID usage mapping.
 *
 * Mapping by scan code (not virtual key) keeps the relay layout-independent:
 * the position pressed on the remote keyboard reproduces the same position on
 * the gaming PC, and that PC's own keyboard layout decides the character -
 * exactly as a directly attached keyboard behaves.
 */
#ifndef HIDWAY_KEYMAP_SC2HID_H
#define HIDWAY_KEYMAP_SC2HID_H

#include <stdbool.h>
#include <stdint.h>

/*
 * HID Keyboard/Keypad page usage for a Set-1 scan code, or 0 when the code is
 * a modifier (use hidway_sc_to_modifier) or is unmapped. `e0` marks an
 * E0-prefixed (extended) scan code.
 */
uint8_t hidway_sc_to_usage(uint8_t scancode, bool e0);

/*
 * Modifier mask bit for a modifier scan code: 1u << (usage - 0xE0), i.e.
 *   LCtrl 0x01  LShift 0x02  LAlt 0x04  LGui 0x08
 *   RCtrl 0x10  RShift 0x20  RAlt 0x40  RGui 0x80
 * Returns 0 when the scan code is not a modifier.
 */
uint8_t hidway_sc_to_modifier(uint8_t scancode, bool e0);

/* Short human-readable name for a HID usage, for on-screen display. Returns a
 * pointer to static storage; unknown usages render as "0xNN" into `tmp`. */
const char *hidway_usage_name(uint8_t usage, char tmp[8]);

#endif /* HIDWAY_KEYMAP_SC2HID_H */
