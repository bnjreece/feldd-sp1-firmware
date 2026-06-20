/*
 * midi_out.c — dual MIDI output path: TRS UART (31250 baud, TX-only) + USB-MIDI.
 *
 * The mapping engine (mapping.c) emits struct midi_msg's into a midi_sink_fn;
 * midi_out_send is that sink and fans each message out to BOTH sinks:
 *
 *  - TRS: ship the raw status/data bytes out uart1's TXD (P0.20 tip) one byte
 *    at a time via uart_poll_out (no IRQ/DMA needed at MIDI's 31.25 kbps,
 *    ~320 us/byte). The ring (P0.23) gates the PNP current source for the opto
 *    on the OP-XY side; it's a DT-active-LOW GPIO so logical-1 = enabled.
 *  - USB: encode the channel-voice message as a 4-byte USB-MIDI 1.0 event and
 *    queue it on the hand-rolled USB-MIDI 1.0 function (usb_midi1.c, brought up
 *    by usbdev_start in main). usb_midi1_send drops cleanly when no host has
 *    enabled the interface or its TX ring is full — that's the common idle case
 *    and is swallowed.
 *
 * BUILD-VERIFIED only: the on-hardware TRS electrical bench-test (scope P0.20 +
 * the ring line, send into the OP-XY over a Type-A TRS cable) AND the USB-MIDI
 * computer-enumeration check are DEFERRED until Unit A + a scope + the OP-XY are
 * on the bench (Renode has no USBD/SAADC model).
 */
#include "midi_out.h"
#include <errno.h>

#ifndef MIDI_OUT_HOST_TEST

#include "usb_midi1.h"
#include "midi1_codec.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

static const struct device *const trs = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct gpio_dt_spec ring =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), midi_ring_gpios);

int midi_out_init(void)
{
    if (!device_is_ready(trs)) {
        return -1;
    }
    if (gpio_is_ready_dt(&ring)) {
        gpio_pin_configure_dt(&ring, GPIO_OUTPUT_INACTIVE);
        gpio_pin_set_dt(&ring, 1);   /* logical-1 = ON; GPIO_ACTIVE_LOW (DT) drives the pin low */
    }
    return 0;
}

/* TRS sink: raw status/data bytes out uart1 TXD. */
static void trs_send(const struct midi_msg *m)
{
    uart_poll_out(trs, m->status);
    uart_poll_out(trs, m->d1);
    if (m->len == 3) {
        uart_poll_out(trs, m->d2);
    }
}

/* USB-MIDI 1.0 sink: encode the channel-voice message as a 4-byte USB-MIDI 1.0
 * event and queue it on the always-on 1.0 function. Best-effort — usb_midi1_send
 * drops cleanly if no host is listening or its ring is full. */
static void midi1_send(const struct midi_msg *m)
{
    uint8_t pkt[4];
    if (midi1_event(0, m->status, m->d1, m->d2, pkt) == 4) {
        usb_midi1_send(pkt);
    }
}

#else  /* MIDI_OUT_HOST_TEST: Zephyr I/O shell excluded; both sinks are no-ops */

#include <stdint.h>
#include <stdbool.h>

#define ARG_UNUSED(x) ((void)(x))

static void trs_send(const struct midi_msg *m) { (void)m; }
static void midi1_send(const struct midi_msg *m) { (void)m; }

int midi_out_init(void) { return 0; }

#endif /* MIDI_OUT_HOST_TEST */

void midi_out_send(const struct midi_msg *m, void *ctx)
{
    ARG_UNUSED(ctx);
    trs_send(m);
    midi1_send(m);
}
