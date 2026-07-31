#ifndef USB_MIDI1_H
#define USB_MIDI1_H
#include <stdbool.h>
#include <stdint.h>

/* Hand-rolled, bidirectional USB-MIDI 1.0 device_next class (the MIDI function
 * alongside CDC-ACM; the old USB-MIDI 2.0 function was dropped). A host binds
 * this and sees a real MIDI port with no mode toggle. The class is defined with
 * USBD_DEFINE_CLASS, so sample_usbd's usbd_register_all_classes picks it up
 * automatically at boot; there is no DT node or extra CONFIG to wire.
 *
 * Enqueue one 4-byte USB-MIDI 1.0 event packet (built by midi1_event) on our
 * bulk IN endpoint, to be flushed to the host. The send is non-blocking: on a
 * full TX ring (or before the host has enabled the interface) the packet is
 * dropped, never blocked — same graceful-drop posture as midi_out's USB sink.
 * Safe to call from the control loop. */
void usb_midi1_send(const uint8_t pkt[4]);

/* Register the non-blocking channel-voice receive seam used by the MIDI delay.
 * Called from USB completion context after packet validation. The callback must
 * copy/enqueue and return; bytes points to a temporary 2- or 3-byte message. */
typedef void (*usb_midi1_voice_rx_fn)(const uint8_t *bytes, uint8_t len,
                                     void *ctx);
void usb_midi1_set_voice_rx(usb_midi1_voice_rx_fn fn, void *ctx);

/* Link-state seam for safety cleanup. Disable/suspend reports false; reconnect
 * never restarts the delay automatically. */
typedef void (*usb_midi1_link_fn)(bool connected, void *ctx);
void usb_midi1_set_link_callback(usb_midi1_link_fn fn, void *ctx);

#endif
