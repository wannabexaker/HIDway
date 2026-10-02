#include "t0.h"

#include "bootsel.h"
#include "hid_out.h"
#include "hidway_kbd.h"
#include "pico/time.h"
#include "tusb.h"

#define T0_BUTTON_POLL_US   10000
#define T0_MOTION_PERIOD_US 1000
#define T0_MOTION_X         5
#define T0_KEY              HID_KEY_W

static absolute_time_t next_poll;
static absolute_time_t next_motion;
static bool held;

void t0_init(void)
{
    next_poll = next_motion = get_absolute_time();
    held = false;
}

bool t0_active(void)
{
    return held;
}

void t0_task(void)
{
    absolute_time_t now = get_absolute_time();

    if (absolute_time_diff_us(next_poll, now) >= 0) {
        next_poll = delayed_by_us(now, T0_BUTTON_POLL_US);
        bool pressed = bootsel_pressed();
        if (pressed != held) {
            held = pressed;
            uint8_t bitmap[HIDWAY_KEY_BITMAP_BYTES] = {0};
            if (held) {
                hidway_bitmap_set(bitmap, T0_KEY);
                next_motion = now;
            }
            hid_out_set_keyboard(0, bitmap);
        }
    }

    if (held && absolute_time_diff_us(next_motion, now) >= 0) {
        next_motion = delayed_by_us(next_motion, T0_MOTION_PERIOD_US);
        if (absolute_time_diff_us(next_motion, now) > 10 * T0_MOTION_PERIOD_US)
            next_motion = delayed_by_us(now, T0_MOTION_PERIOD_US); /* don't burst after a stall */
        hid_out_add_motion(T0_MOTION_X, 0, 0, 0);
    }
}
