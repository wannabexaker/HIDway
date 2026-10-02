/*
 * USB descriptors: composite device with two standard HID interfaces.
 *
 *   ITF0  boot keyboard, 6KRO (works in BIOS/UEFI)
 *   ITF1  boot-capable mouse; report protocol carries 16-bit X/Y,
 *         5 buttons (4/5 = back/forward), vertical wheel and AC Pan
 *
 * The device identifies itself honestly as "HIDway"; it does not copy any
 * other vendor's IDs or strings.
 */
#include <string.h>

#include "pico/unique_id.h"
#include "tusb.h"
#include "usb_descriptors.h"

#ifndef HIDWAY_USB_VID
#define HIDWAY_USB_VID 0xCAFE /* TinyUSB development VID; see docs */
#endif
#ifndef HIDWAY_USB_PID
#define HIDWAY_USB_PID 0x4877
#endif
#ifndef HIDWAY_USB_BCD
#define HIDWAY_USB_BCD 0x0001
#endif

static const tusb_desc_device_t desc_device = {
    .bLength = sizeof(tusb_desc_device_t),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = 0x00,
    .bDeviceSubClass = 0x00,
    .bDeviceProtocol = 0x00,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = HIDWAY_USB_VID,
    .idProduct = HIDWAY_USB_PID,
    .bcdDevice = HIDWAY_USB_BCD,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&desc_device;
}

/* ---------------------------------------------------------------- reports */

static const uint8_t desc_hid_keyboard[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

static const uint8_t desc_hid_mouse[] = {
    0x05, 0x01,       /* Usage Page (Generic Desktop) */
    0x09, 0x02,       /* Usage (Mouse) */
    0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x01,       /*   Usage (Pointer) */
    0xA1, 0x00,       /*   Collection (Physical) */
    0x05, 0x09,       /*     Usage Page (Button) */
    0x19, 0x01,       /*     Usage Minimum (1) */
    0x29, 0x05,       /*     Usage Maximum (5) */
    0x15, 0x00,       /*     Logical Minimum (0) */
    0x25, 0x01,       /*     Logical Maximum (1) */
    0x95, 0x05,       /*     Report Count (5) */
    0x75, 0x01,       /*     Report Size (1) */
    0x81, 0x02,       /*     Input (Data, Var, Abs) */
    0x95, 0x01,       /*     Report Count (1) */
    0x75, 0x03,       /*     Report Size (3) */
    0x81, 0x01,       /*     Input (Const) - padding */
    0x05, 0x01,       /*     Usage Page (Generic Desktop) */
    0x09, 0x30,       /*     Usage (X) */
    0x09, 0x31,       /*     Usage (Y) */
    0x16, 0x01, 0x80, /*     Logical Minimum (-32767) */
    0x26, 0xFF, 0x7F, /*     Logical Maximum (32767) */
    0x75, 0x10,       /*     Report Size (16) */
    0x95, 0x02,       /*     Report Count (2) */
    0x81, 0x06,       /*     Input (Data, Var, Rel) */
    0x09, 0x38,       /*     Usage (Wheel) */
    0x15, 0x81,       /*     Logical Minimum (-127) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x06,       /*     Input (Data, Var, Rel) */
    0x05, 0x0C,       /*     Usage Page (Consumer) */
    0x0A, 0x38, 0x02, /*     Usage (AC Pan) */
    0x15, 0x81,       /*     Logical Minimum (-127) */
    0x25, 0x7F,       /*     Logical Maximum (127) */
    0x75, 0x08,       /*     Report Size (8) */
    0x95, 0x01,       /*     Report Count (1) */
    0x81, 0x06,       /*     Input (Data, Var, Rel) */
    0xC0,             /*   End Collection */
    0xC0,             /* End Collection */
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return instance == ITF_KEYBOARD ? desc_hid_keyboard : desc_hid_mouse;
}

/* ---------------------------------------------------------- configuration */

enum {
    STRID_LANGID = 0,
    STRID_MANUFACTURER,
    STRID_PRODUCT,
    STRID_SERIAL,
    STRID_ITF_KEYBOARD,
    STRID_ITF_MOUSE,
    STRID_COUNT
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + ITF_COUNT * TUD_HID_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_COUNT, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),
    TUD_HID_DESCRIPTOR(ITF_KEYBOARD, STRID_ITF_KEYBOARD, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_keyboard), EPNUM_KEYBOARD, HID_EP_SIZE, HID_POLL_MS),
    TUD_HID_DESCRIPTOR(ITF_MOUSE, STRID_ITF_MOUSE, HID_ITF_PROTOCOL_MOUSE,
                       sizeof(desc_hid_mouse), EPNUM_MOUSE, HID_EP_SIZE, HID_POLL_MS),
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
    (void)index;
    return desc_configuration;
}

/* ---------------------------------------------------------------- strings */

static const char *const string_desc[STRID_COUNT] = {
    [STRID_MANUFACTURER] = "HIDway",
    [STRID_PRODUCT] = "HIDway Remote HID",
    [STRID_ITF_KEYBOARD] = "HIDway Keyboard",
    [STRID_ITF_MOUSE] = "HIDway Mouse",
};

#define MAX_STRING_CHARS 32
static uint16_t desc_str[MAX_STRING_CHARS + 1];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
    (void)langid;
    char serial[2 * PICO_UNIQUE_BOARD_ID_SIZE_BYTES + 1];
    const char *str;
    size_t len;

    if (index == STRID_LANGID) {
        desc_str[1] = 0x0409; /* English (United States) */
        len = 1;
    } else {
        if (index >= STRID_COUNT)
            return NULL;
        if (index == STRID_SERIAL) {
            pico_get_unique_board_id_string(serial, sizeof serial);
            str = serial;
        } else {
            str = string_desc[index];
        }
        len = strlen(str);
        if (len > MAX_STRING_CHARS)
            len = MAX_STRING_CHARS;
        for (size_t i = 0; i < len; i++)
            desc_str[1 + i] = (uint8_t)str[i];
    }

    desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * len + 2));
    return desc_str;
}
