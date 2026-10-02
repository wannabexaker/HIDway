#ifndef HIDWAY_LED_H
#define HIDWAY_LED_H

typedef enum {
    LED_OFF,
    LED_ON,
    LED_BLINK_SLOW, /* 1 Hz: waiting for the USB host */
    LED_BLINK_FAST, /* 8 Hz: input is being sent */
} led_mode_t;

/* On Pico W the LED sits on the Wi-Fi chip, whose init takes a while:
 * call this before starting USB. */
void led_init(void);
void led_task(led_mode_t mode);

#endif /* HIDWAY_LED_H */
