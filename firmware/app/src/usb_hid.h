#ifndef USB_HID_H
#define USB_HID_H
#include <stdint.h>

/* First-party Zephyr usbd_hid boot-keyboard function (Studio phase 2a).
 *
 * Unlike usb_midi1.c (a hand-rolled USBD_DEFINE_CLASS), the HID surface uses
 * Zephyr's stock usbd_hid.c class, instantiated from the hid_dev_0 DT node
 * (compatible "zephyr,hid-device") in app.overlay. That class instance
 * auto-registers into the sample_usbd composite via usbd_register_all_classes
 * (alongside CDC + MIDI1; it is not on the helper's blocklist). All this module
 * does is bind a standard 8-byte boot-keyboard report descriptor + a minimal
 * hid_device_ops to that instance, then expose a non-blocking key-send helper. */

/* Register the report descriptor + callbacks on the HID class instance. MUST be
 * called BEFORE usbd_enable (the API requires registration before the device is
 * enabled), so usbdev_start() calls it just ahead of usbd_enable(). Returns 0 on
 * success, <0 on error (and the rest of the USB composite still comes up).
 *
 * Named feldd_* (not usb_hid_init) because Zephyr's stock usbd_hid class already
 * exports an internal symbol usb_hid_init — a plain usb_hid_init here collides at
 * link time. */
int feldd_usb_hid_init(void);

/* Type one key: submit an 8-byte boot-keyboard input report
 * {modifiers, 0, keycode, 0,0,0,0,0} (press) immediately followed by an all-zero
 * report (release). keycode is a USB HID usage (e.g. HID_KEY_A == 0x04).
 *
 * Non-blocking, best-effort: if the host has not made the HID interface ready
 * (enabled==false) the call is dropped silently, never blocked - same
 * graceful-drop posture as usb_midi1_send(). Safe to call from the control loop. */
void usb_hid_send_key(uint8_t modifiers, uint8_t keycode);

#endif
