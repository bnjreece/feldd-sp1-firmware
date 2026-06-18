/*
 * usbdev.c — USB composite bring-up for the SP-1: CDC ACM console + USB-MIDI 2.0.
 *
 * Both classes ride a single device_next USBD context built by the vendored
 * sample_usbd helper. sample_usbd_init_device(NULL) -> sample_usbd_setup_device
 * calls usbd_register_all_classes(), which registers EVERY compiled-in class
 * instance: the cdc_acm_uart0 node (CONFIG_USBD_CDC_ACM_CLASS) AND the usb_midi
 * node (CONFIG_USBD_MIDI2_CLASS). The helper's sample_fix_code_triple() already
 * emits the IAD (Miscellaneous/0x02/0x01) composite code triple when MIDI2 is
 * enabled, so no custom composite init is needed — the same path the CDC-only
 * console used now enumerates both.
 *
 * The midi2 ops MUST be installed before usbd_enable so the host-side
 * enable/discovery callbacks land. The SP-1 is an output-only control surface
 * (faders/buttons -> CC), so inbound MIDI *data* is ignored, and we don't gate
 * sends on the ready flag (usbd_midi_send already returns -EIO until the host
 * enables the interface — midi_out swallows that). The ONE inbound thing we do
 * answer is UMP Stream discovery: rx_packet_cb hands UMP_MT_UMP_STREAM packets
 * to the ump_stream_responder, which replies (over the same usbd_midi_send
 * path) with our in-band endpoint name, function-block name, and a
 * hwinfo-derived Product Instance ID — built straight from the usb_midi DT
 * node. Requires CONFIG_MIDI2_UMP_STREAM_RESPONDER=y (see prj.conf).
 *
 * BUILD-VERIFIED only: USB-MIDI enumeration on a real computer is DEFERRED
 * until Unit A is on the bench (Renode has no USBD model).
 */
#include "usbdev.h"
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/audio/midi.h>
#include <sample_usbd.h>
#include <ump_stream_responder.h>

#define USB_MIDI_DT_NODE DT_NODELABEL(usb_midi)

static const struct device *const midi = DEVICE_DT_GET(USB_MIDI_DT_NODE);

/* Endpoint/function-block topology + names harvested from the usb_midi DT node
 * (label = "SP-1 Control Surface", child block label = "SP-1 MIDI"). */
static const struct ump_endpoint_dt_spec ump_ep_dt =
    UMP_ENDPOINT_DT_SPEC_GET(USB_MIDI_DT_NODE);

/* Reply to UMP Stream discovery over the same send path used for CC output. */
static const struct ump_stream_responder_cfg responder_cfg =
    UMP_STREAM_RESPONDER(midi, usbd_midi_send, &ump_ep_dt);

/* Output-only surface: we ignore inbound MIDI *data*, but we DO answer UMP
 * Stream Endpoint/Function-Block discovery so the host can read our in-band
 * name + topology. Everything else is dropped. */
static void rx_packet_cb(const struct device *dev, const struct midi_ump ump)
{
    ARG_UNUSED(dev);

    if (UMP_MT(ump) == UMP_MT_UMP_STREAM) {
        ump_stream_respond(&responder_cfg, ump);
    }
}

/* Host enabled/disabled the MIDI interface — nothing to do; sends are guarded
 * by usbd_midi_send's own -EIO when the host isn't ready. */
static void ready_cb(const struct device *dev, const bool ready)
{
    ARG_UNUSED(dev);
    ARG_UNUSED(ready);
}

static const struct usbd_midi_ops ops = {
    .rx_packet_cb = rx_packet_cb,
    .ready_cb = ready_cb,
};

const struct device *usb_midi_dev(void)
{
    return midi;
}

int usbdev_start(void)
{
    if (!device_is_ready(midi)) {
        return -1;
    }

    /* Install ops BEFORE enable. */
    usbd_midi_set_ops(midi, &ops);

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
