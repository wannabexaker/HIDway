#include "link.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hid_out.h"
#include "led.h"
#include "pico/time.h"
#include "protocol.h" /* HIDWAY_TARGET_* */
#include "serial.h"
#include "tusb.h"

#include <string.h>

/* UART0 on GP0 (TX) / GP1 (RX). RX carries the input state from the Pi; TX
 * reports the target PC's state (on / asleep / off) back to it. */
#define LINK_UART      uart0
#define LINK_TX_PIN    0
#define LINK_RX_PIN    1
#define LINK_BAUD      921600

#define LINK_TIMEOUT_MS 250 /* no valid frame -> release everything */
#define LINK_CATCHUP_MS 100 /* larger gap -> resync instead of replaying motion */
#define LINK_LED_MS     200
#define LINK_ERROR_MS   3000 /* how long a link problem is shown */
#define LINK_BAD_GAP_MS 1000 /* two dropped frames this close = a wiring problem */
#define LINK_TARGET_MS  3000 /* repeat the target report at least this often */

/* Single-producer (IRQ) / single-consumer (main) byte ring. */
#define RING_SIZE 2048
#define RING_MASK (RING_SIZE - 1)
static volatile uint8_t ring[RING_SIZE];
static volatile uint16_t ring_head, ring_tail;

static hidway_deframer_t deframer;
static bool have_base;
static int32_t base_x, base_y;
static int16_t base_wheel, base_pan;
static absolute_time_t last_frame_time;
static bool linked; /* a session is running: STATE frames since the last RELEASE */

/* Status LEDs: press edges, and recent link problems. */
static uint8_t prev_mods, prev_buttons, prev_keys[HIDWAY_KEY_BITMAP_BYTES];
static uint32_t seen_bad;
static absolute_time_t last_bad, error_until;

/* Target report to the Pi. */
static uint8_t sent_target = 0xFF;
static absolute_time_t next_target;

static void report_target(absolute_time_t now)
{
    uint8_t t = !tud_mounted() ? HIDWAY_TARGET_OFF : tud_suspended() ? HIDWAY_TARGET_ASLEEP : HIDWAY_TARGET_ON;
    if (t == sent_target && absolute_time_diff_us(next_target, now) < 0)
        return;
    uint8_t frame[HIDWAY_COBS_MAX(HIDWAY_SER_TARGET_PAYLOAD_LEN) + 1];
    size_t n = hidway_serial_build_target(t, frame, sizeof frame);
    if (n)
        uart_write_blocking(LINK_UART, frame, n); /* 6 bytes: fits the TX FIFO */
    sent_target = t;
    next_target = delayed_by_ms(now, LINK_TARGET_MS);
}

static void flag_error(absolute_time_t now)
{
    error_until = delayed_by_ms(now, LINK_ERROR_MS);
}

static void forget_presses(void)
{
    prev_mods = prev_buttons = 0;
    memset(prev_keys, 0, sizeof prev_keys);
}

static void note_presses(const hidway_serial_state_t *s)
{
    bool key = (s->mods & (uint8_t)~prev_mods) != 0;
    for (int i = 0; i < HIDWAY_KEY_BITMAP_BYTES; i++)
        key |= (s->keys[i] & (uint8_t)~prev_keys[i]) != 0;
    if (key)
        led_pulse_key();
    if (s->buttons & (uint8_t)~prev_buttons)
        led_pulse_click();
    prev_mods = s->mods;
    prev_buttons = s->buttons;
    memcpy(prev_keys, s->keys, sizeof prev_keys);
}

static void __isr link_uart_isr(void)
{
    while (uart_is_readable(LINK_UART)) {
        uint8_t b = (uint8_t)uart_getc(LINK_UART);
        uint16_t next = (uint16_t)((ring_head + 1) & RING_MASK);
        if (next != ring_tail) { /* drop on overflow rather than block */
            ring[ring_head] = b;
            ring_head = next;
        }
    }
}

void link_init(void)
{
    uart_init(LINK_UART, LINK_BAUD);
    gpio_set_function(LINK_TX_PIN, GPIO_FUNC_UART);
    gpio_set_function(LINK_RX_PIN, GPIO_FUNC_UART);
    uart_set_hw_flow(LINK_UART, false, false);
    uart_set_format(LINK_UART, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(LINK_UART, true);

    int irq = LINK_UART == uart0 ? UART0_IRQ : UART1_IRQ;
    irq_set_exclusive_handler(irq, link_uart_isr);
    irq_set_enabled(irq, true);
    uart_set_irq_enables(LINK_UART, true, false); /* RX only */

    hidway_deframer_reset(&deframer);
    have_base = false;
    linked = false;
    last_frame_time = get_absolute_time();
    seen_bad = 0;
    last_bad = error_until = nil_time;
    forget_presses();
    sent_target = 0xFF;
    next_target = get_absolute_time();
}

static void apply_frame(const hidway_serial_state_t *s)
{
    absolute_time_t now = get_absolute_time();

    if (s->type != HIDWAY_SER_STATE && s->type != HIDWAY_SER_RELEASE)
        return; /* not input (the Pi never sends anything else) */
    if (s->type == HIDWAY_SER_RELEASE) {
        hid_out_release_all();
        have_base = false; /* next STATE re-baselines, no motion jump */
        forget_presses();
        last_frame_time = now;
        linked = false; /* a clean end: the timeout below is not a problem */
        return;
    } else {
        hid_out_set_keyboard(s->mods, s->keys);
        hid_out_set_buttons(s->buttons);
        note_presses(s);

        int64_t gap_us = absolute_time_diff_us(last_frame_time, now);
        if (have_base && gap_us <= (int64_t)LINK_CATCHUP_MS * 1000) {
            int32_t dx = (int32_t)((uint32_t)s->x - (uint32_t)base_x);
            int32_t dy = (int32_t)((uint32_t)s->y - (uint32_t)base_y);
            int16_t dw = (int16_t)(s->wheel - base_wheel);
            int16_t dp = (int16_t)(s->pan - base_pan);
            hid_out_add_motion(dx, dy, dw, dp);
        }
        /* else: resync silently (big gap or first frame) */
        base_x = s->x;
        base_y = s->y;
        base_wheel = s->wheel;
        base_pan = s->pan;
        have_base = true;
    }

    last_frame_time = now;
    linked = true;
}

void link_task(void)
{
    hidway_serial_state_t s;
    while (ring_tail != ring_head) {
        uint8_t b = ring[ring_tail];
        ring_tail = (uint16_t)((ring_tail + 1) & RING_MASK);
        if (hidway_deframer_push(&deframer, b, &s))
            apply_frame(&s);
    }

    absolute_time_t now = get_absolute_time();
    report_target(now);
    if (deframer.bad != seen_bad) { /* corrupt frames: one is noise, two close together a problem */
        seen_bad = deframer.bad;
        if (!is_nil_time(last_bad) && absolute_time_diff_us(last_bad, now) < (int64_t)LINK_BAD_GAP_MS * 1000)
            flag_error(now);
        last_bad = now;
    }

    if (linked && absolute_time_diff_us(last_frame_time, now) > (int64_t)LINK_TIMEOUT_MS * 1000) {
        /* The input stopped without a RELEASE: relay, Pi or wiring trouble. */
        hid_out_release_all();
        have_base = false;
        linked = false;
        forget_presses();
        flag_error(now);
    }
}

bool link_error(void)
{
    return !is_nil_time(error_until) && absolute_time_diff_us(get_absolute_time(), error_until) > 0;
}

bool link_active(void)
{
    return linked &&
           absolute_time_diff_us(last_frame_time, get_absolute_time()) < (int64_t)LINK_LED_MS * 1000;
}
