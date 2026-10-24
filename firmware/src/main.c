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
#else
#include "link.h"
#endif

#define HIDWAY_WATCHDOG_MS 100
#define HIDWAY_CRASH_SHOW_MS 3000

static bool hang_reboot; /* this boot follows a hang */

static led_mode_t led_mode(void)
{
    if (!tud_mounted() || tud_suspended())
        return LED_OFF; /* host off, starting up or asleep: stay dark */
    if (hang_reboot && to_ms_since_boot(get_absolute_time()) < HIDWAY_CRASH_SHOW_MS)
        return LED_ERROR;
#if HIDWAY_T0
    if (t0_active())
        return LED_ACTIVE;
#else
    if (link_error())
        return LED_ERROR;
    if (link_active())
        return LED_ACTIVE;
#endif
    return LED_IDLE;
}

int main(void)
{
    hang_reboot = watchdog_enable_caused_reboot();
    led_init();
    hid_out_init();
    tud_init(BOARD_TUD_RHPORT);

#if HIDWAY_T0
    t0_init();
#else
    link_init();
#endif

    /* A hang resets the chip; the host then sees the device disappear,
     * which releases every key and button. */
    watchdog_enable(HIDWAY_WATCHDOG_MS, true);

    for (;;) {
        tud_task();
#if HIDWAY_T0
        t0_task();
#else
        link_task();
#endif
        hid_out_task();
        led_task(led_mode());
        watchdog_update();
    }
}
