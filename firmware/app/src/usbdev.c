/*
 * usbdev.c — USB composite bring-up for the SP-1: CDC ACM console + USB-MIDI 1.0.
 *
 * Both classes ride a single device_next USBD context built by the vendored
 * sample_usbd helper. sample_usbd_init_device(NULL) -> sample_usbd_setup_device
 * calls usbd_register_all_classes(), which registers EVERY compiled-in class
 * instance: the cdc_acm_uart0 node (CONFIG_USBD_CDC_ACM_CLASS) AND the
 * hand-rolled USB-MIDI 1.0 class (usb_midi1.c, registered via USBD_DEFINE_CLASS).
 * The helper's sample_fix_code_triple() emits the IAD composite code triple, so
 * no custom composite init is needed — the same path the CDC-only console used
 * now enumerates both.
 *
 * (The old USB-MIDI 2.0 / usbd_midi2 function + its UMP-stream responder were
 * dropped: feldd used no 2.0 feature, and a single MIDI-1.0 function enumerates
 * cleanly on Mac/Win/iOS/old-Linux without the dual-MIDIStreaming compat risk.)
 *
 * BUILD-VERIFIED only: USB-MIDI enumeration on a real computer is DEFERRED
 * until Unit A is on the bench (Renode has no USBD model).
 */
#include "usbdev.h"
#include "usb_hid.h"
#include <zephyr/usb/usbd.h>
#include <sample_usbd.h>

int usbdev_start(void)
{
    /* Studio 2a: bind the boot-keyboard report descriptor + ops onto the stock
     * usbd_hid class instance BEFORE sample_usbd_init_device(). CRITICAL ordering:
     * the HID class's .init callback runs DURING usbd_init() (which
     * sample_usbd_init_device calls) and REQUIRES the report descriptor already
     * registered — if it isn't, usbd_hid_init returns -EINVAL and the WHOLE
     * composite (CDC + MIDI + HID) fails to enumerate. So register first, exactly
     * like the hid-keyboard sample (register -> init -> enable). The only failure
     * path here is the hid_dev_0 DT node not being ready, which is a build/DT
     * error, not a runtime one. */
    (void)feldd_usb_hid_init();

    struct usbd_context *ctx = sample_usbd_init_device(NULL);
    if (ctx == NULL) {
        return -1;   /* no USB; the control loop + WDT still run */
    }

    int err = usbd_enable(ctx);
    if (err) {
        return err;
    }

    return 0;
}
