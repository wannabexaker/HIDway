#ifndef HIDWAY_USB_DESCRIPTORS_H
#define HIDWAY_USB_DESCRIPTORS_H

/*
 * Interface numbers double as TinyUSB HID instance numbers, because the
 * HID interfaces are declared in this order in the configuration descriptor.
 */
enum {
    ITF_KEYBOARD = 0,
    ITF_MOUSE,
    ITF_COUNT
};

#define EPNUM_KEYBOARD 0x81
#define EPNUM_MOUSE    0x82
#define HID_EP_SIZE    8
#define HID_POLL_MS    1 /* bInterval: 1 ms -> 1000 Hz on Full-Speed */

/* Report-protocol mouse report: buttons, X16, Y16, wheel, pan. */
#define HIDWAY_MOUSE_REPORT_LEN 7
/* Boot-protocol mouse report: buttons, X8, Y8. */
#define HIDWAY_MOUSE_BOOT_REPORT_LEN 3

#endif /* HIDWAY_USB_DESCRIPTORS_H */
