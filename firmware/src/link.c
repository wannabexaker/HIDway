#include "link.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/uart.h"
#include "hid_out.h"
#include "pico/time.h"
#include "serial.h"

/* UART0 on GP0 (TX) / GP1 (RX). TX is unused for now (Pi -> Pico only). */
#define LINK_UART      uart0
#define LINK_TX_PIN    0
#define LINK_RX_PIN    1
#define LINK_BAUD      921600

#define LINK_TIMEOUT_MS 250 /* no valid frame -> release everything */
#define LINK_CATCHUP_MS 100 /* larger gap -> resync instead of replaying motion */
#define LINK_LED_MS     200

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
static bool linked;

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
}

static void apply_frame(const hidway_serial_state_t *s)
{
    absolute_time_t now = get_absolute_time();

    if (s->type == HIDWAY_SER_RELEASE) {
        hid_out_release_all();
        have_base = false; /* next STATE re-baselines, no motion jump */
    } else {
        hid_out_set_keyboard(s->mods, s->keys);
        hid_out_set_buttons(s->buttons);

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

    if (linked && absolute_time_diff_us(last_frame_time, get_absolute_time()) >
                      (int64_t)LINK_TIMEOUT_MS * 1000) {
        hid_out_release_all();
        have_base = false;
        linked = false;
    }
}

bool link_active(void)
{
    return linked &&
           absolute_time_diff_us(last_frame_time, get_absolute_time()) < (int64_t)LINK_LED_MS * 1000;
}
