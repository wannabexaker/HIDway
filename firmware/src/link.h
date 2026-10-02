/*
 * Serial link input source: receives framed state from the Raspberry Pi over
 * UART and drives the HID output. Replaces the T0 test source in normal builds.
 */
#ifndef HIDWAY_LINK_H
#define HIDWAY_LINK_H

#include <stdbool.h>

void link_init(void);
void link_task(void);

/* True if a valid frame arrived recently (for the status LED). */
bool link_active(void);

#endif /* HIDWAY_LINK_H */
