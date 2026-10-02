/*
 * HIDway - relative motion accumulator.
 *
 * Motion that arrives faster than the USB poll rate (or in clumps after
 * network jitter) is summed here and drained one report at a time, limited
 * to what a single HID report can carry. Nothing is ever dropped silently;
 * callers decide when to clear (e.g. on release-all or resync).
 */
#ifndef HIDWAY_MOTION_H
#define HIDWAY_MOTION_H

#include <stdbool.h>
#include <stdint.h>

/* Upper bound on pending motion per axis, so a stuck producer can't overflow. */
#define HIDWAY_MOTION_CAP (1L << 24)

typedef struct {
    int32_t x, y, wheel, pan;
} hidway_motion_t;

void hidway_motion_clear(hidway_motion_t *m);
void hidway_motion_add(hidway_motion_t *m, int32_t dx, int32_t dy, int32_t wheel, int32_t pan);
bool hidway_motion_pending(const hidway_motion_t *m);

/*
 * Remove at most +/-xy_limit (X, Y) and +/-wheel_limit (wheel, pan) from the
 * accumulator and return it in *out. If the report carrying it can't be
 * sent, hand it back with hidway_motion_give_back().
 */
void hidway_motion_take(hidway_motion_t *m, int32_t xy_limit, int32_t wheel_limit, hidway_motion_t *out);
void hidway_motion_give_back(hidway_motion_t *m, const hidway_motion_t *chunk);

#endif /* HIDWAY_MOTION_H */
