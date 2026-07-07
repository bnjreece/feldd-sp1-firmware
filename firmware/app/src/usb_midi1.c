/*
 * usb_midi1.c — hand-rolled, output-only USB-MIDI 1.0 device_next class.
 *
 * feldd exposes CDC-ACM (the JSON console) + this always-on USB-MIDI 1.0
 * function in the sample_usbd composite. (The old USB-MIDI 2.0 / usbd_midi2
 * function was dropped: feldd never used any 2.0 feature, and a single MIDI-1.0
 * function enumerates cleanly on Mac/Win/iOS/old-Linux without the
 * dual-MIDIStreaming host-compat risk.) A host binds this and sees a real MIDI
 * port with NO mode toggle. VID/PID are unchanged (0x1915/0x5211).
 *
 * The surface is OUTPUT-ONLY (device->host): buttons/faders -> CC/note. In
 * USB-MIDI 1.0 terms (section 6.2.2) that is ONE bulk IN endpoint carrying 4-byte
 * event packets sourced from ONE Embedded MIDI OUT jack (jackID 2), itself fed by
 * ONE External MIDI IN jack (jackID 1). A device->host bulk IN endpoint MUST
 * associate an Embedded MIDI OUT jack (matches Linux f_midi + TinyUSB); an earlier
 * revision wired it to an Embedded MIDI IN jack, which is spec-illegal and hard-
 * faulted the TE OP-XY host (v1.1.18) mid-enumeration. We ALSO expose a bulk OUT
 * endpoint (host->device) even though feldd consumes nothing: the OP-XY sends
 * clock/notes/CC to every connected device and its v1.1.18 firmware faults on a
 * device with no input to receive them, so we present a full two-way port and simply
 * discard whatever the host sends. The class is modeled on usbd_midi2.c's device_next shape
 * (USBD_DEFINE_CLASS, the descriptor blob + fs/hs pointer arrays, the
 * usbd_class_api callbacks, the ring_buf -> usbd_ep_enqueue TX path) but stripped
 * to the static, single-instance, send-only minimum — no devicetree node.
 *
 * BUILD-VERIFIED only: like the rest of the USB path, real-host enumeration of
 * the CDC + MIDI 1.0 composite is DEFERRED to the bench (Renode has no USBD model).
 */
#include "usb_midi1.h"

#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(usb_midi1, LOG_LEVEL_INF);

/* USB Device Class Definition for MIDI Devices 1.0 constants. */
/* B.4.1 MS Class-Specific Interface Descriptor Subtypes */
#define MS_HEADER		0x01
#define MIDI_IN_JACK		0x02
#define MIDI_OUT_JACK		0x03
/* B.4.2 MS Class-Specific Endpoint Descriptor Subtypes */
#define MS_GENERAL		0x01
/* B.4.3 MIDI IN and OUT Jack types */
#define JACK_EMBEDDED		0x01
#define JACK_EXTERNAL		0x02

/* AudioControl / MIDIStreaming interface coordinates (A.1/A.4/A.5). These mirror
 * the magic numbers usbd_midi2.c pulls from usbd_uac2_macros.h; we spell them out
 * locally so this class stays self-contained. */
#define AUDIO			0x01	/* bInterfaceClass: AUDIO */
#define AUDIOCONTROL		0x01	/* bInterfaceSubClass: AUDIOCONTROL */
#define MIDISTREAMING		0x03	/* bInterfaceSubClass: MIDISTREAMING */

/* Jack IDs (bID). BIDIRECTIONAL topology per USB-MIDI 1.0 section 6.2.2. feldd only
 * SOURCES MIDI (controls -> host), but the TE OP-XY host SENDS clock/notes/CC TO every
 * connected device (its Devices screen exposes per-device "sent" toggles) and its
 * v1.1.18 firmware hard-faults post-configuration on a device that has NO input (OUT)
 * endpoint to receive that stream. So we present a full two-way MIDI port and simply
 * discard anything the host sends us.
 *   device->host path: External MIDI IN jack (1) -> Embedded MIDI OUT jack (2) -> bulk IN EP
 *   host->device path: bulk OUT EP -> Embedded MIDI IN jack (3) -> External MIDI OUT jack (4)
 * (A device->host bulk IN EP MUST associate an Embedded MIDI OUT jack; a host->device
 * bulk OUT EP an Embedded MIDI IN jack. Source jacks are listed before the jacks that
 * reference them.) */
#define EXT_IN_JACK_ID		0x01	/* External MIDI IN jack: source of the device->host stream */
#define EMB_OUT_JACK_ID		0x02	/* Embedded MIDI OUT jack: feeds the host, assoc'd w/ the bulk IN EP */
#define EMB_IN_JACK_ID		0x03	/* Embedded MIDI IN jack: receives from host, assoc'd w/ the bulk OUT EP */
#define EXT_OUT_JACK_ID		0x04	/* External MIDI OUT jack: sink of the host->device stream */

/* Initial endpoint-address hints; the stack re-assigns the real address during
 * usbd_init (see usbd_init.c:assign_ep_addr), then we read it back at send time.
 * 0x81 = the first IN endpoint direction bit + a placeholder index. */
#define EP_IN_ADDR		0x81
/* First OUT endpoint direction bit + placeholder index; re-assigned at usbd_init. */
#define EP_OUT_ADDR		0x01

/* One USB packet of 4-byte events. nRF52840 bulk MPS = 64. */
#define MIDI1_MPS		64U
/* TX ring: a handful of 4-byte events of slack so a fast fader sweep that
 * outruns host polling drops cleanly rather than blocking. */
#define MIDI1_TX_QUEUE_SIZE	64

/* net_bufs in flight: IN transfers + one persistent OUT read (host->device). */
UDC_BUF_POOL_DEFINE(usb_midi1_buf_pool, 4, MIDI1_MPS,
		    sizeof(struct udc_buf_info), NULL);

/* B.4.1 Class-Specific MS Interface Header Descriptor (USB-MIDI 1.0 6.1.2.1). */
struct usb_midi1_cs_if_header {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint16_t bcdMSC;
	uint16_t wTotalLength;
} __packed;

/* B.4.3 MIDI IN Jack Descriptor (6.1.2.2). */
struct usb_midi1_in_jack {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bJackType;
	uint8_t bJackID;
	uint8_t iJack;
} __packed;

/* B.4.4 MIDI OUT Jack Descriptor with a single input pin (6.1.2.3). */
struct usb_midi1_out_jack {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bJackType;
	uint8_t bJackID;
	uint8_t bNrInputPins;
	uint8_t baSourceID1;
	uint8_t baSourcePin1;
	uint8_t iJack;
} __packed;

/* USB-MIDI 1.0 Standard MS Bulk Data Endpoint Descriptor (USB-MIDI 1.0 6.2.1):
 * the 9-byte audio-class endpoint descriptor (standard 7 + bRefresh +
 * bSynchAddress), NOT Zephyr's 7-byte usb_ep_descriptor. MIDI-1.0 hosts
 * (Linux/ALSA, f_midi, TinyUSB) require 9; a 7-byte EP shifts the following
 * class-specific descriptor and the host drops the port. */
struct usb_midi1_ms_ep_descriptor {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bEndpointAddress;
	uint8_t bmAttributes;
	uint16_t wMaxPacketSize;
	uint8_t bInterval;
	uint8_t bRefresh;
	uint8_t bSynchAddress;
} __packed;

/* USB-MIDI 1.0 Class-Specific MS Bulk Data Endpoint Descriptor with one
 * associated embedded jack (B.5.2 / 6.2.2). */
struct usb_midi1_cs_ep {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint8_t bNumEmbMIDIJack;
	uint8_t baAssocJackID1;
} __packed;

/* B.3.1 Class-Specific AC Interface Header Descriptor (collects the MS iface). */
struct usb_midi1_cs_ac_header {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bDescriptorSubtype;
	uint16_t bcdADC;
	uint16_t wTotalLength;
	uint8_t bInCollection;
	uint8_t baInterfaceNr1;
} __packed;

/* The full descriptor blob, in wire order. The stack walks this array (via the
 * fs/hs pointer arrays below) at usbd_init: it stamps bInterfaceNumber on each
 * standard interface descriptor and re-assigns bEndpointAddress on each endpoint.
 * Class-specific cross-references (the AC header's baInterfaceNr1) are NOT
 * auto-patched, so usb_midi1_init() fixes that up after assignment. */
struct usb_midi1_descriptors {
	struct usb_association_descriptor iad;

	/* AudioControl interface (no endpoints; just collects the MS interface). */
	struct usb_if_descriptor if0_std;
	struct usb_midi1_cs_ac_header if0_cs;

	/* MIDIStreaming interface (no alt-settings). */
	struct usb_if_descriptor if1_std;
	struct usb_midi1_cs_if_header if1_cs_header;
	/* device->host jacks */
	struct usb_midi1_in_jack ext_in_jack;	/* External MIDI IN jack (source), listed first */
	struct usb_midi1_out_jack emb_out_jack;	/* Embedded MIDI OUT jack -> the bulk IN endpoint */
	/* host->device jacks (so the host has a sink to send to; feldd discards it) */
	struct usb_midi1_in_jack emb_in_jack;	/* Embedded MIDI IN jack <- the bulk OUT endpoint */
	struct usb_midi1_out_jack ext_out_jack;	/* External MIDI OUT jack, sunk from the embedded IN jack */
	/* endpoints + their class-specific descriptors */
	struct usb_midi1_ms_ep_descriptor in_ep_fs;	/* bulk IN, device->host */
	struct usb_midi1_cs_ep cs_in_ep;	/* associates the embedded OUT jack */
	struct usb_midi1_ms_ep_descriptor out_ep_fs;	/* bulk OUT, host->device */
	struct usb_midi1_cs_ep cs_out_ep;	/* associates the embedded IN jack */
};

struct usb_midi1_config {
	struct usb_midi1_descriptors *desc;
	const struct usb_desc_header **fs_descs;
};

struct usb_midi1_data {
	struct usbd_class_data *class_data;
	struct k_work tx_work;
	uint8_t tx_queue_buf[MIDI1_TX_QUEUE_SIZE];
	struct ring_buf tx_queue;
	bool enabled;
};

/* wTotalLength of the class-specific MS interface descriptors = the MS header +
 * the Jack/Element descriptors ONLY (USB-MIDI 1.0 §6.1.2.1). The class-specific
 * bulk-ENDPOINT descriptor is NOT counted here — it belongs to the endpoint, not
 * the interface header's collection. (Matches Linux f_midi, which sums only the
 * header + jacks.) Including it overruns a spec-strict host's CS-interface parse. */
#define MIDI1_MS_TOTAL_LEN						\
	(sizeof(struct usb_midi1_cs_if_header) +			\
	 sizeof(struct usb_midi1_in_jack) +				\
	 sizeof(struct usb_midi1_out_jack) +				\
	 sizeof(struct usb_midi1_in_jack) +				\
	 sizeof(struct usb_midi1_out_jack))

static struct usb_midi1_descriptors usb_midi1_desc = {
	.iad = {
		.bLength = sizeof(struct usb_association_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE_ASSOC,
		.bFirstInterface = 0,		/* stamped at init */
		.bInterfaceCount = 2,		/* AudioControl + MIDIStreaming */
		.bFunctionClass = AUDIO,
		.bFunctionSubClass = MIDISTREAMING,
		.bFunctionProtocol = 0,
		.iFunction = 0,
	},
	.if0_std = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,		/* stamped at init */
		.bAlternateSetting = 0,
		.bNumEndpoints = 0,
		.bInterfaceClass = AUDIO,
		.bInterfaceSubClass = AUDIOCONTROL,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.if0_cs = {
		.bLength = sizeof(struct usb_midi1_cs_ac_header),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = MS_HEADER,
		.bcdADC = sys_cpu_to_le16(0x0100),
		.wTotalLength = sys_cpu_to_le16(sizeof(struct usb_midi1_cs_ac_header)),
		.bInCollection = 1,
		.baInterfaceNr1 = 1,		/* MS interface; corrected at init */
	},
	.if1_std = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 1,		/* stamped at init */
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,		/* bulk IN (device->host) + bulk OUT (host->device) */
		.bInterfaceClass = AUDIO,
		.bInterfaceSubClass = MIDISTREAMING,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	.if1_cs_header = {
		.bLength = sizeof(struct usb_midi1_cs_if_header),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = MS_HEADER,
		.bcdMSC = sys_cpu_to_le16(0x0100),
		.wTotalLength = sys_cpu_to_le16(MIDI1_MS_TOTAL_LEN),
	},
	.ext_in_jack = {
		.bLength = sizeof(struct usb_midi1_in_jack),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = MIDI_IN_JACK,
		.bJackType = JACK_EXTERNAL,
		.bJackID = EXT_IN_JACK_ID,
		.iJack = 0,
	},
	.emb_out_jack = {
		.bLength = sizeof(struct usb_midi1_out_jack),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = MIDI_OUT_JACK,
		.bJackType = JACK_EMBEDDED,
		.bJackID = EMB_OUT_JACK_ID,
		.bNrInputPins = 1,
		.baSourceID1 = EXT_IN_JACK_ID,	/* sourced from the external IN jack */
		.baSourcePin1 = 1,
		.iJack = 0,
	},
	.in_ep_fs = {
		.bLength = sizeof(struct usb_midi1_ms_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = EP_IN_ADDR,	/* re-assigned at init */
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(MIDI1_MPS),
		.bInterval = 0,
		.bRefresh = 0,
		.bSynchAddress = 0,
	},
	.cs_in_ep = {
		.bLength = sizeof(struct usb_midi1_cs_ep),
		.bDescriptorType = USB_DESC_CS_ENDPOINT,
		.bDescriptorSubtype = MS_GENERAL,
		.bNumEmbMIDIJack = 1,
		.baAssocJackID1 = EMB_OUT_JACK_ID,	/* bulk IN EP carries the Embedded OUT jack */
	},
	.emb_in_jack = {
		.bLength = sizeof(struct usb_midi1_in_jack),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = MIDI_IN_JACK,
		.bJackType = JACK_EMBEDDED,
		.bJackID = EMB_IN_JACK_ID,
		.iJack = 0,
	},
	.ext_out_jack = {
		.bLength = sizeof(struct usb_midi1_out_jack),
		.bDescriptorType = USB_DESC_CS_INTERFACE,
		.bDescriptorSubtype = MIDI_OUT_JACK,
		.bJackType = JACK_EXTERNAL,
		.bJackID = EXT_OUT_JACK_ID,
		.bNrInputPins = 1,
		.baSourceID1 = EMB_IN_JACK_ID,	/* sunk from the embedded IN jack */
		.baSourcePin1 = 1,
		.iJack = 0,
	},
	.out_ep_fs = {
		.bLength = sizeof(struct usb_midi1_ms_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = EP_OUT_ADDR,	/* re-assigned at init */
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(MIDI1_MPS),
		.bInterval = 0,
		.bRefresh = 0,
		.bSynchAddress = 0,
	},
	.cs_out_ep = {
		.bLength = sizeof(struct usb_midi1_cs_ep),
		.bDescriptorType = USB_DESC_CS_ENDPOINT,
		.bDescriptorSubtype = MS_GENERAL,
		.bNumEmbMIDIJack = 1,
		.baAssocJackID1 = EMB_IN_JACK_ID,	/* bulk OUT EP carries the Embedded IN jack */
	},
};

static const struct usb_desc_header *usb_midi1_fs_descs[] = {
	/* NO Interface Association Descriptor. A class-compliant USB-MIDI (Audio 1.0)
	 * function groups its AudioControl + MIDIStreaming interfaces via the AC
	 * header's collection (if0_cs.bInCollection / baInterfaceNr1), NOT an IAD.
	 * IADs are for CDC / true multi-function devices. The Teenage Engineering
	 * OP-XY (fw v1.1.18) USB host HARD-FAULTS (Blackfin data-CPLB miss, PC
	 * 0x8c06c29c) the instant it parses an IAD in front of an Audio function:
	 * the crash regs R1=8 / R2=2 are the IAD's bLength / bInterfaceCount. Dropping
	 * the IAD makes feldd a textbook MIDI device that strict hosts accept.
	 * (usb_midi1_desc.iad stays defined but is intentionally NOT emitted.) */
	(struct usb_desc_header *)&usb_midi1_desc.if0_std,
	(struct usb_desc_header *)&usb_midi1_desc.if0_cs,
	(struct usb_desc_header *)&usb_midi1_desc.if1_std,
	(struct usb_desc_header *)&usb_midi1_desc.if1_cs_header,
	(struct usb_desc_header *)&usb_midi1_desc.ext_in_jack,
	(struct usb_desc_header *)&usb_midi1_desc.emb_out_jack,
	(struct usb_desc_header *)&usb_midi1_desc.emb_in_jack,
	(struct usb_desc_header *)&usb_midi1_desc.ext_out_jack,
	(struct usb_desc_header *)&usb_midi1_desc.in_ep_fs,
	(struct usb_desc_header *)&usb_midi1_desc.cs_in_ep,
	(struct usb_desc_header *)&usb_midi1_desc.out_ep_fs,
	(struct usb_desc_header *)&usb_midi1_desc.cs_out_ep,
	NULL,
};

static const struct usb_midi1_config usb_midi1_cfg = {
	.desc = &usb_midi1_desc,
	.fs_descs = usb_midi1_fs_descs,
};

static struct usb_midi1_data usb_midi1_state;

/* ---- endpoint TX path (modeled on usbd_midi2.c's tx ring + ep_enqueue) ---- */

static struct net_buf *usb_midi1_buf_alloc(uint8_t ep)
{
	struct net_buf *buf;
	struct udc_buf_info *info;

	buf = net_buf_alloc(&usb_midi1_buf_pool, K_NO_WAIT);
	if (buf == NULL) {
		return NULL;
	}

	info = udc_get_buf_info(buf);
	info->ep = ep;

	return buf;
}

/* Drain whatever the ring holds into one bulk-IN transfer and enqueue it. */
static void usb_midi1_tx_work(struct k_work *work)
{
	struct usb_midi1_data *data = CONTAINER_OF(work, struct usb_midi1_data, tx_work);
	struct net_buf *buf;
	int ret;

	if (ring_buf_is_empty(&data->tx_queue)) {
		return;
	}

	buf = usb_midi1_buf_alloc(usb_midi1_cfg.desc->in_ep_fs.bEndpointAddress);
	if (buf == NULL) {
		/* No free net_buf: the previous transfer is still in flight. The
		 * request-completion callback re-submits this work when it lands,
		 * so the ring contents are not lost. */
		return;
	}

	net_buf_add(buf, ring_buf_get(&data->tx_queue, buf->data, buf->size));

	ret = usbd_ep_enqueue(data->class_data, buf);
	if (ret) {
		LOG_DBG("Failed to enqueue Tx net_buf -> %d", ret);
		net_buf_unref(buf);
	}
}

/* Arm one bulk-OUT read to receive host->device MIDI. feldd sources only; the OUT
 * endpoint exists purely so a host that insists on a two-way MIDI port (the OP-XY)
 * has a sink to send its clock/notes/CC to. We drop whatever arrives. */
static void usb_midi1_out_prep(struct usb_midi1_data *data)
{
	struct net_buf *buf;

	buf = usb_midi1_buf_alloc(usb_midi1_cfg.desc->out_ep_fs.bEndpointAddress);
	if (buf == NULL) {
		return;
	}

	if (usbd_ep_enqueue(data->class_data, buf)) {
		net_buf_unref(buf);
	}
}

/* ---- usbd_class_api callbacks ---- */

/* Transfer completion. IN (device->host): free + kick another Tx if the ring
 * refilled. OUT (host->device): discard the received MIDI, free, and re-arm the read
 * so the host always has an open sink. The endpoint direction bit selects the path. */
static int usb_midi1_request(struct usbd_class_data *const class_data,
			     struct net_buf *const buf, const int err)
{
	struct usbd_context *uds_ctx = usbd_class_get_ctx(class_data);
	struct usb_midi1_data *data = usbd_class_get_private(class_data);
	struct udc_buf_info *bi = udc_get_buf_info(buf);

	if (err && err != -ECONNABORTED) {
		LOG_DBG("transfer error %d on ep 0x%02x", err, bi->ep);
	}

	if (USB_EP_DIR_IS_OUT(bi->ep)) {
		/* Host->device MIDI: discard it, then re-arm the read (unless the
		 * transfer was aborted on disable/suspend). */
		(void)usbd_ep_buf_free(uds_ctx, buf);
		if (err != -ECONNABORTED && data->enabled) {
			usb_midi1_out_prep(data);
		}
		return 0;
	}

	if (!ring_buf_is_empty(&data->tx_queue)) {
		k_work_submit(&data->tx_work);
	}

	return usbd_ep_buf_free(uds_ctx, buf);
}

static void usb_midi1_enable(struct usbd_class_data *const class_data)
{
	struct usb_midi1_data *data = usbd_class_get_private(class_data);

	data->enabled = true;
	LOG_DBG("USB-MIDI 1.0 enabled");

	/* Open the host->device read sink so the host has somewhere to send. */
	usb_midi1_out_prep(data);

	/* Flush anything that queued before the host enabled the interface. */
	if (!ring_buf_is_empty(&data->tx_queue)) {
		k_work_submit(&data->tx_work);
	}
}

static void usb_midi1_disable(struct usbd_class_data *const class_data)
{
	struct usb_midi1_data *data = usbd_class_get_private(class_data);

	data->enabled = false;
	LOG_DBG("USB-MIDI 1.0 disabled");
}

static void usb_midi1_suspended(struct usbd_class_data *const class_data)
{
	struct usb_midi1_data *data = usbd_class_get_private(class_data);

	data->enabled = false;
	LOG_DBG("USB-MIDI 1.0 suspended");
}

static void usb_midi1_resumed(struct usbd_class_data *const class_data)
{
	struct usb_midi1_data *data = usbd_class_get_private(class_data);

	data->enabled = true;
	LOG_DBG("USB-MIDI 1.0 resumed");
}

/* init() runs AFTER the stack has stamped the real bInterfaceNumber on each
 * standard interface descriptor (usbd_init.c:init_configuration_inst). The AC
 * header's baInterfaceNr1 cross-reference is NOT auto-patched, so point it at the
 * now-assigned MIDIStreaming interface — the same fixup usbd_cdc_acm_init() does
 * for its UNION descriptor. */
static int usb_midi1_init(struct usbd_class_data *const class_data)
{
	struct usb_midi1_data *data = usbd_class_get_private(class_data);

	usb_midi1_cfg.desc->if0_cs.baInterfaceNr1 =
		usb_midi1_cfg.desc->if1_std.bInterfaceNumber;

	LOG_DBG("USB-MIDI 1.0 init: AC -> MS iface %u",
		usb_midi1_cfg.desc->if1_std.bInterfaceNumber);

	ARG_UNUSED(data);
	return 0;
}

static void *usb_midi1_get_desc(struct usbd_class_data *const class_data,
				const enum usbd_speed speed)
{
	ARG_UNUSED(class_data);
	ARG_UNUSED(speed);

	/* Bulk descriptors are speed-agnostic at 64-byte MPS; one set for FS/HS.
	 * The nRF52840 is full-speed only. */
	return usb_midi1_cfg.fs_descs;
}

static struct usbd_class_api usb_midi1_api = {
	.request = usb_midi1_request,
	.enable = usb_midi1_enable,
	.disable = usb_midi1_disable,
	.suspended = usb_midi1_suspended,
	.resumed = usb_midi1_resumed,
	.init = usb_midi1_init,
	.get_desc = usb_midi1_get_desc,
};

USBD_DEFINE_CLASS(usb_midi1, &usb_midi1_api, &usb_midi1_state, NULL);

/* ---- public send path ---- */

/* Queue one 4-byte USB-MIDI 1.0 event for the host. Non-blocking: if the host
 * has not enabled the interface, or the TX ring is full, drop the packet (the
 * caller, midi_out, keeps a last-value-wins copy and retries via its pump). */
void usb_midi1_send(const uint8_t pkt[4])
{
	struct usb_midi1_data *data = &usb_midi1_state;

	if (!data->enabled) {
		return;	/* no host listening — drop, like usbd_midi_send's -EIO */
	}

	if (ring_buf_space_get(&data->tx_queue) < 4) {
		return;	/* ring full — drop, like usbd_midi_send's -ENOBUFS */
	}

	ring_buf_put(&data->tx_queue, pkt, 4);
	k_work_submit(&data->tx_work);
}

/* SYS_INIT-time bring-up of the per-instance ring + work item. Runs at the same
 * POST_KERNEL/DEVICE priority usbd_midi2 uses for its DEVICE_DT_INST_DEFINE. */
static int usb_midi1_preinit(void)
{
	struct usb_midi1_data *data = &usb_midi1_state;

	data->class_data = &usb_midi1;
	ring_buf_init(&data->tx_queue, MIDI1_TX_QUEUE_SIZE, data->tx_queue_buf);
	k_work_init(&data->tx_work, usb_midi1_tx_work);

	return 0;
}

SYS_INIT(usb_midi1_preinit, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE);
