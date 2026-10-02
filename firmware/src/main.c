/*
 * HIDway firmware - Pico / Pico W as a standard USB HID keyboard + mouse.
 */
#include "hardware/watchdog.h"
#include "hid_out.h"
#include "led.h"
#include "pico/stdlib.h"
#include "tusb.h"

#if HIDWAY_T0
#include "t0.h"
#endif

#define HIDWAY_WATCHDOG_MS 100

static led_mode_t led_mode(void)
{
    if (!tud_mounted())
        return LED_BLINK_SLOW;
    if (tud_suspended())
        return LED_OFF;
#if HIDWAY_T0
    if (t0_active())
        return LED_BLINK_FAST;
#endif
    return LED_ON;
}

int main(void)
{
    led_init();
    hid_out_init();
    tud_init(BOARD_TUD_RHPORT);

#if HIDWAY_T0
    t0_init();
#endif

    /* A hang resets the chip; the host then sees the device disappear,
     * which releases every key and button. */
    watchdog_enable(HIDWAY_WATCHDOG_MS, true);

    for (;;) {
        tud_task();
#if HIDWAY_T0
        t0_task();
#endif
        hid_out_task();
        led_task(led_mode());
        watchdog_update();
    }
}
