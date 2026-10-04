/*
 * midi_out.c - dual MIDI output path: TRS UART (31250 baud, TX-only) + USB-MIDI.
 *
 * The mapping engine (mapping.c) emits struct midi_msg's into a midi_sink_fn;
 * midi_out_send is that sink and fans each message out to BOTH sinks:
 *
 *  - TRS: an INTERRUPT-DRIVEN uart1 TX draining a two-tier priority ring
 *    (midi_rt_ring). Normal CC/note bytes go to the normal tier; real-time bytes
 *    (clock 0xF8, transport Start/Stop/Continue) go to the PRIORITY tier via
 *    midi_out_rt(), so a fast fader-CC burst can never delay a clock tick by more
 *    than one in-flight byte (~320 us at 31250 baud). A real-time byte may land
 *    between the status and data bytes of a channel message; that is legal MIDI 1.0
 *    (single-byte system real-time is allowed anywhere in the stream). Ring access
 *    is irq_lock'd: producers run from BOTH thread context (fader/button CC) and
 *    ISR context (the clock timer), while the UART TX ISR is the sole consumer.
 *  - USB: encode the channel-voice message as a 4-byte USB-MIDI 1.0 event and
 *    queue it on the hand-rolled USB-MIDI 1.0 function (usb_midi1.c). usb_midi1_send
 *    drops cleanly when no host has enabled the interface or its ring is full.
 *
 * The TRS electrical path is HARDWARE-VALIDATED: feldd drives the OP-XY over the
 * 3.5 mm TRS out (Type A, data on the tip P0.20). The USB clock-out for gen mode is
 * added separately once usb_midi1_send is confirmed ISR-safe.
 */
#include "midi_out.h"
#include "trigger_out.h"
#include <errno.h>

#ifdef CONFIG_FELDD_BT_LINK
#include "bt_link.h"
#endif

#ifndef MIDI_OUT_HOST_TEST

#include "usb_midi1.h"
#include "midi1_codec.h"
#include "midi_rt_ring.h"
#include "held_notes.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/irq.h>

static const struct device *const trs = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct gpio_dt_spec ring =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), midi_ring_gpios);

static struct midi_rt_ring trs_ring;

/* Which notes the JACK is currently sounding — see held_notes.h. Fed from the one
 * choke point both producers pass through (trs_enqueue_msg), so the controller
 * path and the MIDI-thru path are covered by the same three lines. */
static struct held_notes trs_held;

/* When the TX ISR last handed a byte to the UART, in cycles. midi_out_trs_idle()
 * needs "has the last byte finished shifting out", and uart_irq_tx_complete() does
 * not answer that on an idle, TX-disabled UARTE — it returns 0, so a gate built on
 * it never opens. Measured on hardware 2026-08-05: the deferred handoff to a pulse
 * mode never completed, while the immediate handoff back to MIDI always did.
 *
 * Timing it instead is driver-independent and cannot get stuck. */
static volatile uint32_t trs_last_tx_cycles;

/* UART TX ISR: drain the priority ring into the TX FIFO one byte at a time; stop
 * the TX IRQ when the ring is empty. */
static void trs_uart_isr(const struct device *dev, void *user_data)
{
    ARG_UNUSED(user_data);
    if (!uart_irq_update(dev)) {
        return;
    }
    while (uart_irq_tx_ready(dev)) {
        uint8_t b;
        unsigned int key = irq_lock();
        bool got = midi_rt_next(&trs_ring, &b);
        irq_unlock(key);
        if (!got) {
            uart_irq_tx_disable(dev);
            break;
        }
        trs_last_tx_cycles = k_cycle_get_32();
        uart_fifo_fill(dev, &b, 1);
    }
}

/* Enqueue one byte (rt = priority tier) and kick the TX IRQ. Best-effort: a full
 * ring drops the byte, which never happens at MIDI rates with a 128-byte tier. */
static void trs_enqueue(uint8_t b, bool rt)
{
    if (trigger_out_owns_trs()) {
        return;   /* jack is emitting analog pulses; USB + BLE sinks unaffected */
    }
    unsigned int key = irq_lock();
    if (rt) {
        (void)midi_rt_put_rt(&trs_ring, b);
    } else {
        (void)midi_rt_put(&trs_ring, b);
    }
    irq_unlock(key);
    uart_irq_tx_enable(trs);
}

/* Enqueue a whole NORMAL-tier message under ONE irq_lock so the two producers --
 * the main-loop controller path (trs_send) and the usbd-thread MIDI-thru path
 * (midi_out_thru) -- can never interleave byte-wise on the TRS wire. Uses the
 * ring's all-or-nothing admission, so a near-full ring drops the whole message
 * rather than emitting a truncated (running-status-corrupting) one. */
static void trs_enqueue_msg(const uint8_t *b, uint8_t len)
{
    if (trigger_out_owns_trs()) {
        return;   /* as trs_enqueue: the UART does not own the pin right now */
    }
    unsigned int key = irq_lock();
    bool admitted = midi_rt_put_msg(&trs_ring, b, len);
    /* Track ONLY what was admitted. put_msg is all-or-nothing and returns false
     * on a full ring, which with MIDI thru enabled is ordinary rather than a
     * corner case: a dropped Note-On never reached the wire and would become a
     * phantom, while a dropped Note-Off means the note IS still sounding and its
     * bit must stay set. Both are handled by simply not feeding a refused
     * message. Inside the lock so the two producers cannot interleave. */
    if (admitted && len >= 3) {
        held_notes_feed(&trs_held, b[0], b[1], b[2]);
    }
    irq_unlock(key);
    uart_irq_tx_enable(trs);
}

/* Discard everything queued and release every note the jack is sounding.
 *
 * Called when the jack is about to stop being a MIDI output. Order matters and is
 * the same rule chord_flush_all() already follows ("Fix 6"): PURGE FIRST, then
 * emit. Emitting first would let a stale queued Note-On drain afterwards and
 * re-strand the very note just released.
 *
 * Discarding is right for the queue and wrong for the notes, which is why they
 * are handled separately. Queued real-time bytes are worthless late — a stale
 * burst of clock ticks jitters the downstream tempo worse than silence — and
 * queued voice messages are for a jack that is about to stop carrying MIDI. But
 * a note already sounding has no other way home: the host does not learn the jack
 * is gone and will send its own Note-Off into what is now a pulse output.
 *
 * Only notes the JACK sent are released. Not All Notes Off, which would kill
 * notes the SP-1 never originated — indefensible when playing into an instrument
 * running its own sequencer — and not All Sound Off, which guillotines release
 * tails. */
void midi_out_trs_release_all(void)
{
    unsigned int key = irq_lock();
    midi_rt_ring_init(&trs_ring);          /* purge BEFORE emitting */
    struct held_notes snapshot = trs_held;
    held_notes_init(&trs_held);
    irq_unlock(key);

    int cursor = 0;
    uint8_t ch, note;
    while (held_notes_next(&snapshot, &cursor, &ch, &note)) {
        uint8_t off[3] = { (uint8_t)(0x80u | (ch & 0x0Fu)), note, 0u };
        key = irq_lock();
        (void)midi_rt_put_msg(&trs_ring, off, 3);
        irq_unlock(key);
    }
    uart_irq_tx_enable(trs);
}

/* Has everything handed to the UART actually reached the wire?
 *
 * A ring reporting empty is NOT the same as an idle line: up to one byte (320 us
 * at 31250 baud) can still be in the UARTE's shift register. Disconnecting
 * PSEL.TXD at that moment truncates it electrically — the exact corruption
 * midi_rt_put_msg's all-or-nothing admission exists to prevent, one layer below
 * where any of its host tests can see it. */
/* One MIDI byte is 10 bits at 31250 baud = 320 us. Wait THREE byte-times after the
 * last byte was queued: the UARTE's own FIFO can still hold a byte or two behind
 * the one shifting, and ~1 ms is imperceptible on a profile switch that costs a
 * human hundreds of milliseconds. Erring long is free here; erring short truncates
 * a byte electrically, which is the one failure the ring's atomicity cannot catch. */
#define TRS_DRAIN_GUARD_US 960u

int midi_out_trs_idle(void)
{
    unsigned int key = irq_lock();
    int empty = !midi_rt_pending(&trs_ring);
    uint32_t last = trs_last_tx_cycles;
    irq_unlock(key);
    if (!empty) {
        return 0;
    }
    uint32_t elapsed_us = k_cyc_to_us_floor32(k_cycle_get_32() - last);
    return elapsed_us >= TRS_DRAIN_GUARD_US;
}

int midi_out_init(void)
{
    if (!device_is_ready(trs)) {
        return -1;
    }
    midi_rt_ring_init(&trs_ring);
    uart_irq_rx_disable(trs);
    uart_irq_tx_disable(trs);
    uart_irq_callback_user_data_set(trs, trs_uart_isr, NULL);
    if (gpio_is_ready_dt(&ring)) {
        gpio_pin_configure_dt(&ring, GPIO_OUTPUT_INACTIVE);
        gpio_pin_set_dt(&ring, 1);   /* logical-1 = ON; GPIO_ACTIVE_LOW (DT) drives the pin low */
    }
    return 0;
}

/* TRS sink: enqueue the raw status/data bytes (NORMAL tier). Respect m->len so a
 * 1-byte system real-time message emits ONLY its status byte. */
static void trs_send(const struct midi_msg *m)
{
    uint8_t b[3] = { m->status, m->d1, m->d2 };
    trs_enqueue_msg(b, m->len);   /* atomic: never interleaves with a thru message */
}

/* Real-time byte (clock 0xF8 / transport 0xFA/FB/FC) to the TRS PRIORITY tier.
 * Safe to call from ISR context (irq_lock'd ring op + a register write to enable
 * the TX IRQ). USB clock-out is added later.
 *
 * Mirror the FULL MIDI-out: also fan the real-time byte onto the BLE sink so a
 * BLE-slaved host receives clock/transport ticks, not just the button-driven
 * Start/Stop edges that flow through midi_out_send. Encode as a 1-byte system
 * real-time midi_msg (len=1); bt_link_send_midi is a no-op unless a BLE host is
 * connected+MIDI-subscribed, and bt_link_core's >=0xF8 never-drop headroom keeps
 * these from being starved by droppable CC. The BLE tx-ring producer is irq_lock'd
 * inside bt_link (like the TRS ring here), so this is ISR/thread-safe. */
void midi_out_rt(uint8_t status)
{
    trs_enqueue(status, true);
#ifdef CONFIG_FELDD_BT_LINK
    struct midi_msg m = { .status = status, .d1 = 0, .d2 = 0, .len = 1 };
    bt_link_send_midi(&m);   /* no-op unless a BLE host is MIDI-subscribed */
#endif
}

/* MIDI-thru: forward `len` raw channel-voice bytes to the TRS jack ONLY (normal
 * tier), never USB or BLE, so a host->device stream cannot echo back to the host.
 * Called from the USB class OUT completion (usbd thread) when the global thru
 * switch is on. */
void midi_out_thru(const uint8_t *bytes, uint8_t len)
{
    trs_enqueue_msg(bytes, len);   /* normal tier, atomic + all-or-nothing */
}

/* USB-MIDI 1.0 sink: encode the channel-voice message as a 4-byte event and queue
 * it on the always-on 1.0 function. Best-effort. */
static void midi1_send(const struct midi_msg *m)
{
    uint8_t pkt[4];
    if (midi1_event(0, m->status, m->d1, m->d2, pkt) == 4) {
        usb_midi1_send(pkt);
    }
}

#else  /* MIDI_OUT_HOST_TEST: Zephyr I/O shell excluded; sinks are no-ops */

#include <stdint.h>
#include <stdbool.h>

#define ARG_UNUSED(x) ((void)(x))

static void trs_send(const struct midi_msg *m) { (void)m; }
static void midi1_send(const struct midi_msg *m) { (void)m; }

int  midi_out_init(void) { return 0; }
void midi_out_rt(uint8_t status) { (void)status; }
void midi_out_thru(const uint8_t *bytes, uint8_t len) { (void)bytes; (void)len; }
void midi_out_trs_release_all(void) { }
int  midi_out_trs_idle(void) { return 1; }

#endif /* MIDI_OUT_HOST_TEST */

void midi_out_send(const struct midi_msg *m, void *ctx)
{
    ARG_UNUSED(ctx);
    /* LOCAL note source for the analog trigger. midi_out_send is the single
     * fan-out for everything this device generates — the mapping engine and the
     * chord engine both land here — so one call makes an SP-1 BUTTON fire the
     * trigger, not just a note arriving from a host.
     *
     * That is what makes the feature work standalone: map a button to the trigger
     * note and the SP-1 advances a sequencer with nothing else attached, which is
     * the whole point when there is no computer in the bag.
     *
     * No double-fire risk: host->device notes reach trigger_out_on_voice from the
     * USB OUT callback instead, and a note is never both. The button still emits
     * its note over USB/BLE, so a DAW records the trigger lane alongside
     * everything else. A chord containing the trigger note fires ONCE — the
     * coalescing CAS in trigger_out_fire collapses simultaneous notes. */
    if (trigger_out_owns_trs() && m->len >= 3u) {
        const uint8_t vb[3] = { m->status, m->d1, m->d2 };
        trigger_out_on_voice(vb, 3u);
    }
    trs_send(m);
    midi1_send(m);
#ifdef CONFIG_FELDD_BT_LINK
    bt_link_send_midi(m);   /* BLE sink: no-op unless a BLE host is connected+subscribed */
#endif
}
