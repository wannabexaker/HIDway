#ifndef HIDWAY_BOOTSEL_H
#define HIDWAY_BOOTSEL_H

#include <stdbool.h>

/*
 * Read the BOOTSEL button. The button shares the flash chip-select line, so
 * this briefly disables interrupts and runs from RAM; call it at most every
 * few milliseconds.
 */
bool bootsel_pressed(void);

#endif /* HIDWAY_BOOTSEL_H */
