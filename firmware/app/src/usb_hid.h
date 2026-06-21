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
 * This is a momentary TAP (press+release queued together). The Keyboard-mode
 * path no longer uses it for held buttons (a tap can't auto-repeat); it remains
 * for any one-shot keystroke that genuinely wants a single tap.
 *
 * Non-blocking, best-effort: if the host has not made the HID interface ready
 * (enabled==false) the call is dropped silently, never blocked - same
 * graceful-drop posture as usb_midi1_send(). Safe to call from the control loop. */
void usb_hid_send_key(uint8_t modifiers, uint8_t keycode);

/* Submit ONE raw 8-byte boot-keyboard report verbatim (no auto-release). This is
 * how a held key stays asserted: main.c builds the report from the currently-held
 * buttons (kbd_build_report) and sends it on every keyboard-button edge — key-down
 * on a press, the same keys still present while held, an all-zero report on the
 * last release. The host OS then does the native typematic auto-repeat.
 *
 * report must point to KB_REPORT_COUNT (8) bytes: [mod][reserved][k0..k5].
 * Same non-blocking graceful-drop posture as usb_hid_send_key: dropped silently
 * if the host has not enabled the interface, or if the ring is full. */
void usb_hid_send_report(const uint8_t *report);

#endif
