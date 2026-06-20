#ifndef USBDEV_H
#define USBDEV_H
/* USB composite bring-up (device_next): CDC ACM console + USB-MIDI 1.0.
 * usbdev_start builds the composite via the sample_usbd helper (which
 * auto-registers BOTH the CDC + USB-MIDI 1.0 class instances) and enables the
 * device. Returns 0 on success, <0 on error. */
int usbdev_start(void);
#endif
