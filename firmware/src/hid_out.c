#include "hid_out.h"

#include <string.h>

#include "hidway_motion.h"
#include "pico/time.h"
#include "tusb.h"
#include "usb_descriptors.h"

#define REMOTE_WAKEUP_INTERVAL_US 100000

static struct {
    uint8_t modifiers;
    hidway_kro6_t kro6;
    bool kb_dirty;

    uint8_t buttons;
    bool buttons_dirty;
    hidway_motion_t motion;

    uint8_t leds;
    absolute_time_t next_wakeup;
} st;

void hid_out_init(void)
{
    memset(&st, 0, sizeof st);
    st.next_wakeup = get_absolute_time();
}

void hid_out_set_keyboard(uint8_t modifiers, const uint8_t bitmap[HIDWAY_KEY_BITMAP_BYTES])
{
    if (modifiers != st.modifiers) {
        st.modifiers = modifiers;
        st.kb_dirty = true;
    }
    if (hidway_kro6_update(&st.kro6, bitmap))
        st.kb_dirty = true;
}

void hid_out_set_buttons(uint8_t buttons)
{
    buttons &= HIDWAY_MOUSE_BUTTONS_MASK;
    if (buttons != st.buttons) {
        st.buttons = buttons;
        st.buttons_dirty = true;
    }
}

void hid_out_add_motion(int32_t dx, int32_t dy, int32_t wheel, int32_t pan)
{
    hidway_motion_add(&st.motion, dx, dy, wheel, pan);
}

void hid_out_release_all(void)
{
    static const uint8_t none[HIDWAY_KEY_BITMAP_BYTES];
    hid_out_set_keyboard(0, none);
    hid_out_set_buttons(0);
    hidway_motion_clear(&st.motion);
}

uint8_t hid_out_leds(void)
{
    return st.leds;
}

static void put_le16(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)((uint16_t)v & 0xFF);
    p[1] = (uint8_t)((uint16_t)v >> 8);
}

static void send_keyboard(void)
{
    if (!st.kb_dirty || !tud_hid_n_ready(ITF_KEYBOARD))
        return;
    if (tud_hid_n_keyboard_report(ITF_KEYBOARD, 0, st.modifiers, st.kro6.slots))
        st.kb_dirty = false;
}

static void send_mouse(void)
{
    if (!st.buttons_dirty && !hidway_motion_pending(&st.motion))
        return;
    if (!tud_hid_n_ready(ITF_MOUSE))
        return;

    hidway_motion_t chunk;
    bool sent;

    if (tud_hid_n_get_protocol(ITF_MOUSE) == HID_PROTOCOL_BOOT) {
        /* Boot format has no wheel; drop it rather than keep it pending forever. */
        st.motion.wheel = st.motion.pan = 0;
        hidway_motion_take(&st.motion, 127, 0, &chunk);
        uint8_t r[HIDWAY_MOUSE_BOOT_REPORT_LEN] = {
            (uint8_t)(st.buttons & 0x07u),
            (uint8_t)(int8_t)chunk.x,
            (uint8_t)(int8_t)chunk.y,
        };
        sent = tud_hid_n_report(ITF_MOUSE, 0, r, sizeof r);
    } else {
        hidway_motion_take(&st.motion, 32767, 127, &chunk);
        uint8_t r[HIDWAY_MOUSE_REPORT_LEN];
        r[0] = st.buttons;
        put_le16(&r[1], (int16_t)chunk.x);
        put_le16(&r[3], (int16_t)chunk.y);
        r[5] = (uint8_t)(int8_t)chunk.wheel;
        r[6] = (uint8_t)(int8_t)chunk.pan;
        sent = tud_hid_n_report(ITF_MOUSE, 0, r, sizeof r);
    }

    if (sent)
        st.buttons_dirty = false;
    else
        hidway_motion_give_back(&st.motion, &chunk);
}

void hid_out_task(void)
{
    if (!tud_mounted()) {
        hidway_motion_clear(&st.motion);
        return;
    }

    if (tud_suspended()) {
        /* Motion never wakes the host; a deliberate key/button press may. */
        hidway_motion_clear(&st.motion);
        if ((st.kb_dirty || st.buttons_dirty) && time_reached(st.next_wakeup)) {
            st.next_wakeup = make_timeout_time_us(REMOTE_WAKEUP_INTERVAL_US);
            tud_remote_wakeup();
        }
        return;
    }

    send_keyboard();
    send_mouse();
}

/* ------------------------------------------------------ TinyUSB callbacks */

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                               uint8_t *buffer, uint16_t reqlen)
{
    (void)instance;
    (void)report_id;
    (void)report_type;
    (void)buffer;
    (void)reqlen;
    return 0; /* not supported: STALL */
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type,
                           const uint8_t *buffer, uint16_t bufsize)
{
    (void)report_id;
    if (instance == ITF_KEYBOARD && report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1)
        st.leds = buffer[0];
}

void tud_hid_set_protocol_cb(uint8_t instance, uint8_t protocol)
{
    (void)protocol;
    /* Report formats differ between protocols; resend current state. */
    if (instance == ITF_KEYBOARD)
        st.kb_dirty = true;
    else
        st.buttons_dirty = true;
}
