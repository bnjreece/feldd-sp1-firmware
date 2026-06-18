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
 *  - USB: convert the channel-voice message to a MIDI-1 Universal MIDI Packet
 *    (group 0) and usbd_midi_send into the usb_midi2 device (brought up by
 *    usbdev_start in main). usbd_midi_send returns -EIO when no host has
 *    enabled the interface — that's the common idle case and is swallowed.
 *
 * BUILD-VERIFIED only: the on-hardware TRS electrical bench-test (scope P0.20 +
 * the ring line, send into the OP-XY over a Type-A TRS cable) AND the USB-MIDI
 * computer-enumeration check are DEFERRED until Unit A + a scope + the OP-XY are
 * on the bench (Renode has no USBD/SAADC model).
 */
#include "midi_out.h"
#include <errno.h>

/* The pending-table / eviction logic below is PURE C (depends only on
 * struct midi_msg and usb_try_send's return code), so it is host-testable the
 * same way buttons.c is: compile midi_out.c with -DMIDI_OUT_HOST_TEST and the
 * Zephyr I/O shell (TRS uart, ring GPIO, the real usbd_midi_send) is excluded,
 * letting the test drive usb_send()/midi_out_pump() and inspect the table.
 * The note-non-lossy eviction is the part under test. */
#ifndef MIDI_OUT_HOST_TEST

#include "usbdev.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/usb/class/usbd_midi2.h>
#include <zephyr/audio/midi.h>

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

/* Build the group-0 MIDI-1 UMP for a channel-voice message and hand it to the
 * USB MIDI device. Returns the usbd_midi_send rc (0 ok, -ENOBUFS ring full,
 * -EIO no host). */
static int usb_try_send(const struct midi_msg *m)
{
    uint8_t command = (m->status >> 4) & 0x0f;
    uint8_t channel = m->status & 0x0f;
    struct midi_ump ump =
        UMP_MIDI1_CHANNEL_VOICE(0, command, channel, m->d1, m->d2);
    return usbd_midi_send(usb_midi_dev(), ump);
}

#else  /* MIDI_OUT_HOST_TEST: Zephyr I/O shell excluded, usb_try_send is a hook */

#include <stdint.h>
#include <stdbool.h>

#define ARG_UNUSED(x) ((void)(x))

/* Host-test seam: the test provides usb_try_send so it can simulate a full USB
 * TX ring (-ENOBUFS), then drain it. trs_send is a no-op (no UART on host). */
int  usb_try_send(const struct midi_msg *m);
static void trs_send(const struct midi_msg *m) { (void)m; }

int midi_out_init(void) { return 0; }

#endif /* MIDI_OUT_HOST_TEST */

/* True for channel-voice NOTE OFF (0x80) / NOTE ON (0x90): these are EVENTS, not
 * states, so dropping one strands a note on (or, for a missed NOTE ON, a note
 * the player expected). They must never be silently evicted. CC (0xB0) is a
 * state value where last-value-wins makes a drop harmless. */
static inline bool midi_is_note(const struct midi_msg *m)
{
    uint8_t hi = m->status & 0xf0;
    return hi == 0x80 || hi == 0x90;
}

/* F2 USB last-value-wins pending table.
 *
 * usbd_midi_send can return -ENOBUFS when the USB TX ring is full (e.g. a fast
 * fader sweep outruns the host's polling). Swallowing that would silently drop
 * the *final* CC on USB while TRS got it, stranding the DAW parameter at the
 * wrong value. Instead we keep a tiny fixed table keyed by (status, d1) — i.e.
 * per channel+CC / per note — holding the latest message for each key
 * (last-value-wins). midi_out_pump() (called once per control-loop tick) retries
 * the ring and clears entries that land. The resting value therefore always
 * arrives within a tick or two of the ring draining.
 *
 * -EIO means "host not enabled" (no host listening) — we drop those silently and
 * do NOT queue, otherwise the table would fill with values no host will ever
 * accept. Only -ENOBUFS is queued. */
#define PENDING_CAP 16

struct pending_entry {
    bool             used;
    uint32_t         seq;          /* insertion order, for oldest-eviction */
    struct midi_msg  msg;          /* status+d1 are the key; d2/len updated */
};

static struct pending_entry pending[PENDING_CAP];
static uint32_t             pending_seq;

void midi_out_pump(void);   /* fwd: pending_upsert flushes via the pump when full of notes */

/* Find a free slot, else the oldest CC (0xB0) entry to evict. Note events
 * (0x80/0x90) are NEVER eviction candidates — dropping one strands a note.
 * Returns the slot index, or -1 if every used slot is a note (table is full of
 * un-droppable note events). */
static int pending_pick_slot(void)
{
    int free_slot   = -1;
    int oldest_cc   = -1;

    for (int i = 0; i < PENDING_CAP; i++) {
        if (!pending[i].used) {
            if (free_slot < 0) {
                free_slot = i;
            }
            continue;
        }
        if (midi_is_note(&pending[i].msg)) {
            continue;   /* never an eviction candidate */
        }
        /* CC (or any non-note) entry: eligible for last-value-wins eviction. */
        if (oldest_cc < 0 || pending[i].seq < pending[oldest_cc].seq) {
            oldest_cc = i;
        }
    }

    if (free_slot >= 0) {
        return free_slot;
    }
    return oldest_cc;   /* -1 if the table is wall-to-wall note events */
}

/* Upsert m into the pending table (last-value-wins for the same status+d1).
 *
 * If the key exists, overwrite its d2/len. Else place it in a free slot, or,
 * when full, evict the OLDEST CC entry (0xB0) — never a NOTE OFF/ON, since a
 * dropped note is a stuck note, while a dropped CC is harmless (last-value-wins
 * still lands the resting value). If the table is full of note events, force a
 * synchronous best-effort flush (drain the ring via midi_out_pump, then a direct
 * send) instead of dropping a note. */
static void pending_upsert(const struct midi_msg *m)
{
    for (int i = 0; i < PENDING_CAP; i++) {
        if (pending[i].used &&
            pending[i].msg.status == m->status &&
            pending[i].msg.d1 == m->d1) {
            /* same key: last-value-wins, refresh value + recency */
            pending[i].msg = *m;
            pending[i].seq = ++pending_seq;
            return;
        }
    }

    int slot = pending_pick_slot();

    if (slot < 0) {
        /* Table is full of un-droppable note events. We must NOT drop the
         * incoming note. Best-effort: pump the ring to land already-queued
         * notes (which frees slots), then retry slot selection. */
        midi_out_pump();
        slot = pending_pick_slot();
    }

    if (slot < 0) {
        /* Ring still full and every slot is still a note: send m directly so
         * the note is not lost. If even the direct send fails (-ENOBUFS), we
         * have no slot to park it in — but a synchronous best-effort direct
         * send is strictly better than the old silent drop, and the TRS path
         * already carried this note. Do NOT evict a queued note. */
        (void)usb_try_send(m);
        return;
    }

    /* Eviction (slot held a CC) is silent: the console is bound to
     * cdc_acm_uart0, the SAME CDC port as the JSON-lines protocol, so any printk
     * here under USB backpressure would inject a non-JSON line into the host
     * parse stream. Last-value-wins keeps the CC resting value correct, so the
     * CC drop needs no log. */
    pending[slot].used = true;
    pending[slot].seq  = ++pending_seq;
    pending[slot].msg  = *m;
}

/* Clear any pending entry whose key matches m (status+d1) — used when an
 * immediate send for that key just succeeded. */
static void pending_clear_key(const struct midi_msg *m)
{
    for (int i = 0; i < PENDING_CAP; i++) {
        if (pending[i].used &&
            pending[i].msg.status == m->status &&
            pending[i].msg.d1 == m->d1) {
            pending[i].used = false;
            return;
        }
    }
}

/* USB sink: convert the channel-voice message to a MIDI-1 UMP (group 0) and
 * send it to the host. On -ENOBUFS (ring full) queue last-value-wins for retry
 * by midi_out_pump(); on -EIO (no host) or success, ensure no stale entry stays
 * pending for this key. */
static void usb_send(const struct midi_msg *m)
{
    int rc = usb_try_send(m);
    if (rc == -ENOBUFS) {
        pending_upsert(m);
    } else {
        /* success (0) or no-host (-EIO): this key is now current, so drop any
         * older queued value for it. */
        pending_clear_key(m);
    }
}

void midi_out_pump(void)
{
    for (int i = 0; i < PENDING_CAP; i++) {
        if (!pending[i].used) {
            continue;
        }
        int rc = usb_try_send(&pending[i].msg);
        if (rc != -ENOBUFS) {
            /* landed (0) or host gone (-EIO): either way stop retrying. */
            pending[i].used = false;
        }
        /* still -ENOBUFS: leave it for the next pump. */
    }
}

void midi_out_send(const struct midi_msg *m, void *ctx)
{
    ARG_UNUSED(ctx);
    trs_send(m);
    usb_send(m);
}
