#ifndef USBDEV_H
#define USBDEV_H
#include <zephyr/device.h>
/* USB composite bring-up (device_next): CDC ACM console + USB-MIDI 2.0.
 * usbdev_start sets the midi2 ops, builds the composite via the sample_usbd
 * helper (which auto-registers BOTH the CDC + midi2 class instances), and
 * enables the device. Returns 0 on success, <0 on error. */
int usbdev_start(void);
/* The usbd_midi2 device the midi_out USB sink sends UMP into. May return a
 * device that is not host-enabled yet; usbd_midi_send then returns -EIO. */
const struct device *usb_midi_dev(void);
#endif
