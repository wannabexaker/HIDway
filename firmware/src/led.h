#ifndef HIDWAY_LED_H
#define HIDWAY_LED_H

/*
 * Status LEDs: the on-board LED plus four optional external ones, each wired
 * GPIO -> series resistor -> LED -> GND (330 ohm for red/yellow, 100 ohm for
 * blue or bright green).
 *
 *   mode        on-board   red (GP19)   green (GP18)
 *   LED_OFF     off        off          off     no USB host, or host asleep
 *   LED_IDLE    on         on           off     connected, not armed
 *   LED_ACTIVE  8 Hz       off          on      input is being received
 *   LED_ERROR   4 Hz       4 Hz         off     link problem (see main.c)
 *
 * Yellow (GP17) flashes on every mouse button press, blue (GP16) on every
 * key press. With the GND pin in between, the five pins in a row are:
 *   pin 21 GP16 blue | 22 GP17 yellow | 23 GND | 24 GP18 green | 25 GP19 red
 * Override a pin with -DHIDWAY_LED_<COLOUR>_PIN=<n>, or -1 to leave it
 * untouched.
 */
#ifndef HIDWAY_LED_RED_PIN
#define HIDWAY_LED_RED_PIN 19
#endif
#ifndef HIDWAY_LED_GREEN_PIN
#define HIDWAY_LED_GREEN_PIN 18
#endif
#ifndef HIDWAY_LED_YELLOW_PIN
#define HIDWAY_LED_YELLOW_PIN 17
#endif
#ifndef HIDWAY_LED_BLUE_PIN
#define HIDWAY_LED_BLUE_PIN 16
#endif

typedef enum {
    LED_OFF,
    LED_IDLE,
    LED_ACTIVE,
    LED_ERROR,
} led_mode_t;

/* On Pico W the LED sits on the Wi-Fi chip, whose init takes a while:
 * call this before starting USB. */
void led_init(void);
void led_task(led_mode_t mode);

/* A short flash: yellow for a mouse button press, blue for a key press. */
void led_pulse_click(void);
void led_pulse_key(void);

#endif /* HIDWAY_LED_H */
