/*
 * usb_hid.c — first-party Zephyr usbd_hid boot-keyboard function (Studio 2a).
 *
 * The THIRD function in feldd's sample_usbd composite: CDC-ACM console +
 * USB-MIDI 1.0 (usb_midi1.c) + this HID keyboard. It rides Zephyr's STOCK class:
 * usbd_hid.c is instanced from the hid_dev_0 DT node (compatible
 * "zephyr,hid-device", app.overlay) and self-registers into sample_usbd via
 * usbd_register_all_classes. This module binds the report descriptor + ops and
 * exposes a non-blocking usb_hid_send_key(). One interrupt-IN endpoint (1 of 7).
 *
 * TWO contracts the stock class imposes (both bit the first draft; see the
 * pre-flash audit):
 *  1. REGISTER BEFORE INIT. The class .init runs DURING usbd_init() and requires
 *     the report descriptor already registered, or the WHOLE composite fails to
 *     enumerate. feldd_usb_hid_init() therefore runs BEFORE sample_usbd_init_device
 *     (see usbdev.c), exactly like the hid-keyboard sample.
 *  2. ASYNC SEND. hid_device_submit_report() BLOCKS on K_FOREVER unless an
 *     input_report_done callback is supplied — that would stall the 8 ms control
 *     loop (and hang if the host stops polling). And the class is ZERO-COPY (it
 *     DMAs straight from the buffer we pass), so a report must stay valid until
 *     its done callback. So we push 8-byte reports into a ring from the control
 *     loop (never blocks), and a k_work submits ONE at a time into a dedicated
 *     `inflight` buffer; input_report_done frees it and kicks the next.
 *
 * BUILD-VERIFIED only: real-host HID enumeration + actual keystrokes are DEFERRED
 * to the bench (Renode has no USBD model). The send drops silently until a host
 * enables the interface, so it can never wedge the control loop.
 */
#include "usb_hid.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/class/usbd_hid.h>
#include <zephyr/usb/class/hid.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_hid, LOG_LEVEL_INF);

/* Standard 8-byte boot-keyboard input report: byte0 modifiers, byte1 reserved,
 * bytes2..7 = up to 6 keycodes. */
enum kb_report_idx {
	KB_MOD_KEY = 0,
	KB_RESERVED,
	KB_KEY_CODE1,
	KB_KEY_CODE2,
	KB_KEY_CODE3,
	KB_KEY_CODE4,
	KB_KEY_CODE5,
	KB_KEY_CODE6,
	KB_REPORT_COUNT,
};

/* Report descriptor copied verbatim from the hid-keyboard sample. */
static const uint8_t hid_report_desc[] = HID_KEYBOARD_REPORT_DESC();

/* The single hid_dev_0 DT node (compat zephyr,hid-device). */
static const struct device *hid_dev = DEVICE_DT_GET_ONE(zephyr_hid_device);

/* ---- async, non-blocking TX (audit-mandated; see contract #2 above) ---- */
#define HID_RING_BYTES 64   /* 8 boot-keyboard reports of slack */
static uint8_t         hid_ring_buf[HID_RING_BYTES];
static struct ring_buf hid_ring;
static struct k_work   hid_tx_work;
static uint8_t         hid_inflight[KB_REPORT_COUNT] __aligned(4); /* zero-copy: valid until done */
static volatile bool   hid_enabled;   /* host put the interface in the active config */
static volatile bool   hid_busy;      /* a submit is in flight (cleared in done) */

/* Submit the next queued report if the endpoint is free. Runs ONLY on the system
 * workqueue (single consumer), so the ring get + submit + busy are serialized;
 * usb_hid_send_key is the single producer (SPSC ring, no lock needed). */
static void hid_tx_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	if (hid_busy || !hid_enabled) {
		return;
	}
	if (ring_buf_size_get(&hid_ring) < KB_REPORT_COUNT) {
		return;   /* no complete report queued */
	}

	ring_buf_get(&hid_ring, hid_inflight, KB_REPORT_COUNT);
	/* Set busy BEFORE submit: an immediate input_report_done landing between a
	 * successful submit and the flag-set would otherwise leave busy stuck true
	 * with nothing in flight (a permanent queue stall). Clear it only if the
	 * submit itself fails (e.g. -EACCES if the host just disabled the iface —
	 * nothing was enqueued; the report is dropped and the next keypress re-kicks). */
	hid_busy = true;
	if (hid_device_submit_report(hid_dev, KB_REPORT_COUNT, hid_inflight) != 0) {
		hid_busy = false;
	}
}

/* IN transfer completed: the inflight buffer is free — send the next queued
 * report. Runs on the USB workqueue. */
static void hid_input_report_done(const struct device *dev, const uint8_t *const report)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(report);
	hid_busy = false;
	k_work_submit(&hid_tx_work);
}

/* ---- hid_device_ops ---- */

static void hid_iface_ready(const struct device *dev, const bool ready)
{
	ARG_UNUSED(dev);
	hid_enabled = ready;
	LOG_INF("HID keyboard interface %s", ready ? "ready" : "not ready");
	if (ready) {
		k_work_submit(&hid_tx_work);   /* flush anything queued before enable */
	}
}

/* HID Get_Report (control pipe): hand back the last input report we submitted. */
static int hid_get_report(const struct device *dev, const uint8_t type,
			  const uint8_t id, const uint16_t len, uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);

	if (type != HID_REPORT_TYPE_INPUT) {
		return 0;
	}
	uint16_t n = MIN(len, (uint16_t)KB_REPORT_COUNT);
	memcpy(buf, hid_inflight, n);
	return n;
}

/* Host LED state via Set_Report — feldd drives no keyboard LEDs, accept-and-ignore
 * OUTPUT, reject other types. */
static int hid_set_report(const struct device *dev, const uint8_t type,
			  const uint8_t id, const uint16_t len,
			  const uint8_t *const buf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(id);
	ARG_UNUSED(len);
	ARG_UNUSED(buf);

	if (type != HID_REPORT_TYPE_OUTPUT) {
		return -ENOTSUP;
	}
	return 0;
}

static void hid_set_protocol(const struct device *dev, const uint8_t proto)
{
	ARG_UNUSED(dev);
	LOG_DBG("HID protocol -> %s", proto == 0U ? "Boot" : "Report");
}

static const struct hid_device_ops hid_ops = {
	.iface_ready = hid_iface_ready,
	.get_report = hid_get_report,
	.set_report = hid_set_report,
	.set_protocol = hid_set_protocol,
	.input_report_done = hid_input_report_done,   /* makes submit ASYNC (non-blocking) */
};

/* ---- public API ---- */

int feldd_usb_hid_init(void)
{
	if (!device_is_ready(hid_dev)) {
		LOG_ERR("HID device not ready");
		return -ENODEV;
	}

	ring_buf_init(&hid_ring, sizeof(hid_ring_buf), hid_ring_buf);
	k_work_init(&hid_tx_work, hid_tx_work_fn);

	int ret = hid_device_register(hid_dev, hid_report_desc,
				      sizeof(hid_report_desc), &hid_ops);
	if (ret != 0) {
		LOG_ERR("Failed to register HID device: %d", ret);
		return ret;
	}
	return 0;
}

void usb_hid_send_key(uint8_t modifiers, uint8_t keycode)
{
	if (!hid_enabled) {
		return;   /* no host bound the interface — drop, never block */
	}

	uint8_t press[KB_REPORT_COUNT] = {0};
	press[KB_MOD_KEY]   = modifiers;
	press[KB_KEY_CODE1] = keycode;
	uint8_t release[KB_REPORT_COUNT] = {0};

	/* Queue press + release atomically: need room for BOTH, else drop the whole
	 * keypress rather than send a press with no release (= a stuck key). The
	 * control loop never blocks; the work drains the ring on the system workqueue. */
	if (ring_buf_space_get(&hid_ring) < 2 * KB_REPORT_COUNT) {
		return;
	}
	ring_buf_put(&hid_ring, press, KB_REPORT_COUNT);
	ring_buf_put(&hid_ring, release, KB_REPORT_COUNT);
	k_work_submit(&hid_tx_work);
}
