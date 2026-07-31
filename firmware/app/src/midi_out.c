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
#include <errno.h>

#ifdef CONFIG_FELDD_BT_LINK
#include "bt_link.h"
#endif

#ifndef MIDI_OUT_HOST_TEST

#include "usb_midi1.h"
#include "midi1_codec.h"
#include "midi_rt_ring.h"
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>
#include <zephyr/irq.h>

static const struct device *const trs = DEVICE_DT_GET(DT_NODELABEL(uart1));
static const struct gpio_dt_spec ring =
    GPIO_DT_SPEC_GET(DT_PATH(zephyr_user), midi_ring_gpios);

static struct midi_rt_ring trs_ring;

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
        uart_fifo_fill(dev, &b, 1);
    }
}

/* Enqueue one byte (rt = priority tier) and kick the TX IRQ. Best-effort: a full
 * ring drops the byte, which is guarded by a 512-byte normal tier. */
static void trs_enqueue(uint8_t b, bool rt)
{
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
    unsigned int key = irq_lock();
    (void)midi_rt_put_msg(&trs_ring, b, len);
    irq_unlock(key);
    uart_irq_tx_enable(trs);
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

#endif /* MIDI_OUT_HOST_TEST */

void midi_out_send(const struct midi_msg *m, void *ctx)
{
    ARG_UNUSED(ctx);
    trs_send(m);
    midi1_send(m);
#ifdef CONFIG_FELDD_BT_LINK
    bt_link_send_midi(m);   /* BLE sink: no-op unless a BLE host is connected+subscribed */
#endif
}
