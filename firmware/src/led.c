#include "led.h"

#include <stdbool.h>

#include "pico/stdlib.h"

#if defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/cyw43_arch.h"
#endif

#define LED_PULSE_MS 40

static bool led_ready;
static bool led_state;
static absolute_time_t click_until, key_until;

static void ext_init(int pin)
{
    if (pin < 0)
        return;
    gpio_init((uint)pin);
    gpio_set_dir((uint)pin, GPIO_OUT);
    gpio_put((uint)pin, false);
}

static void ext_put(int pin, bool on)
{
    if (pin >= 0)
        gpio_put((uint)pin, on);
}

static void led_put(bool on)
{
    if (!led_ready || on == led_state)
        return;
    led_state = on;
#if defined(CYW43_WL_GPIO_LED_PIN)
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, on);
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_put(PICO_DEFAULT_LED_PIN, on);
#endif
}

void led_init(void)
{
#if defined(CYW43_WL_GPIO_LED_PIN)
    led_ready = cyw43_arch_init() == 0;
#elif defined(PICO_DEFAULT_LED_PIN)
    gpio_init(PICO_DEFAULT_LED_PIN);
    gpio_set_dir(PICO_DEFAULT_LED_PIN, GPIO_OUT);
    led_ready = true;
#endif
    led_state = true; /* force the first led_put(false) through */
    led_put(false);
    ext_init(HIDWAY_LED_RED_PIN);
    ext_init(HIDWAY_LED_GREEN_PIN);
    ext_init(HIDWAY_LED_YELLOW_PIN);
    ext_init(HIDWAY_LED_BLUE_PIN);
    click_until = key_until = get_absolute_time();
}

void led_pulse_click(void)
{
    click_until = make_timeout_time_ms(LED_PULSE_MS);
}

void led_pulse_key(void)
{
    key_until = make_timeout_time_ms(LED_PULSE_MS);
}

void led_task(led_mode_t mode)
{
    absolute_time_t now = get_absolute_time();
    uint32_t ms = to_ms_since_boot(now);
    bool blink = (ms / 125) & 1; /* 4 Hz */

    switch (mode) {
    case LED_OFF:
        led_put(false);
        break;
    case LED_IDLE:
        led_put(true);
        break;
    case LED_ACTIVE:
        led_put((ms / 62) & 1);
        break;
    case LED_ERROR:
        led_put(blink);
        break;
    }
    ext_put(HIDWAY_LED_RED_PIN, mode == LED_IDLE || (mode == LED_ERROR && blink));
    ext_put(HIDWAY_LED_GREEN_PIN, mode == LED_ACTIVE);
    ext_put(HIDWAY_LED_YELLOW_PIN, mode != LED_OFF && absolute_time_diff_us(now, click_until) > 0);
    ext_put(HIDWAY_LED_BLUE_PIN, mode != LED_OFF && absolute_time_diff_us(now, key_until) > 0);
}
