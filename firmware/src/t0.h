/*
 * Phase-0 go/no-go test input source.
 *
 * While BOOTSEL is held: hold the W key and move the mouse +5 counts/ms on X.
 * It exists only to answer one question before anything else is built:
 * does the game accept input from this USB HID device?
 */
#ifndef HIDWAY_T0_H
#define HIDWAY_T0_H

#include <stdbool.h>

void t0_init(void);
void t0_task(void);
bool t0_active(void);

#endif /* HIDWAY_T0_H */
