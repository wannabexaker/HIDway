#include "led.h"

#include <stdbool.h>

#include "pico/stdlib.h"

#if defined(CYW43_WL_GPIO_LED_PIN)
#include "pico/cyw43_arch.h"
#endif

static bool led_ready;
static bool led_state;

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
}

void led_task(led_mode_t mode)
{
    uint32_t ms = to_ms_since_boot(get_absolute_time());

    switch (mode) {
    case LED_OFF:
        led_put(false);
        break;
    case LED_ON:
        led_put(true);
        break;
    case LED_BLINK_SLOW:
        led_put((ms / 500) & 1);
        break;
    case LED_BLINK_FAST:
        led_put((ms / 62) & 1);
        break;
    }
}
