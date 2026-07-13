# The feldd BLE module app (CYW20706 / CYBT-353027-02)

This is the custom AIROC application that runs **on the SP-1's Bluetooth module** — the Infineon
**CYBT-353027-02** (CYW20706A2 silicon: its own Arm Cortex-M3, BT radio, and antenna). It advertises
**BLE-MIDI** and a **BLE-HID keyboard**, so a host receives MIDI (and keystrokes) over the air.

feldd (the SP-1's custom nRF52840 firmware) cannot do Bluetooth LE on its own radio, so all wireless
goes through this second chip. feldd builds this app, embeds the resulting image as a blob, and flashes
it onto the module's serial flash over the module's WICED-HCI UART using a **Download-Section-only
Upgrade** (never a full chip-erase — see the safety note below).

The SP-1's *stock* module app is **Bluetooth-Classic-only** (an A2DP sink with no LE stack), so BLE was
not reachable by driving the stock app. Getting BLE required flashing this custom app onto the module.
As far as we know, nobody public had driven this module from the SP-1 before. This document is the
honest, reproducible record of what the app is and what we changed, so you can rebuild it yourself.

**Everything below was captured on real hardware.** Confirmed: BLE-MIDI to macOS and iOS, to the
Teenage Engineering OP-XY (it pairs from its own `COM > devices` menu) and TP-7; and a BLE-HID keyboard
that iOS attaches and types from over the air.

---

## Where the code comes from (and the license you must check)

This app is **derived from Infineon's `mtb-example-btsdk-ble-hello-sensor`** (the `CYW20706A2` /
`COMPONENT_btstack_v1` target). We do **not** redistribute that example's source here.

To reproduce this work:

1. **Download `mtb-example-btsdk-ble-hello-sensor` from Infineon** (via ModusToolbox / the BTSDK, or
   Infineon's GitHub), targeting `CYBT-353027-EVAL` / `CYW20706A2`.
2. Apply the changes described in prose below to that example's files (`hello_sensor.c`,
   `hello_sensor.h`, `hello_uuid.h`, the `COMPONENT_btstack_v1/` GATT-DB + config files, and the
   makefile).
3. Build it with ModusToolbox as described, and let feldd flash the result to the module.

> **License caveat — verify before you redistribute.** The example repository advertises an MIT
> license, **but the per-file source headers carry Cypress/Infineon's restrictive terms**: "all rights
> reserved," a "personal, non-exclusive, **non-transferable**" license to copy/modify/compile solely
> for use with Cypress integrated-circuit products, and "any reproduction, modification, translation,
> compilation, or representation of this Software except as specified above is prohibited without the
> express written permission of Cypress." That is why this document describes our changes in **prose
> only** and does not paste Cypress-derived source. If you intend to redistribute a modified copy,
> reconcile the file-header terms against the repo's MIT claim yourself first.

Our own files (the MIDI packetizer and the feldd-specific glue) are described the same way for
consistency; the reproducible detail is all here.

---

## What runs on the module, in one paragraph

The app boots on the module, brings up the LE stack, builds a GATT database that contains a **BLE-MIDI
service**, a **HOGP HID keyboard service**, and the standard **GAP / Device Information / Battery**
services, then advertises. It advertises using the module's own factory BD_ADDR from the module's
Static Section (LE privacy is off), so the address is stable and valid. A private WICED-HCI command
group lets feldd (on the nRF) push MIDI events and HID reports into the module and control advertising
and bonding over the module UART. When a host subscribes, MIDI events feldd forwards are packetized
per Apple's BLE-MIDI spec and notified out; HID keystrokes are notified over an encrypted link.

---

## Advertisement data

The app advertises as a single LE peripheral that a host can discover as **either** a MIDI device **or**
a keyboard:

- **Primary advertisement:** Flags + the 16-bit **HID service UUID `0x1812`** + **Appearance `0x03C1`
  (HID Keyboard)**. This is what makes an OS surface the device as a keyboard.
- **Scan response:** the 128-bit **BLE-MIDI service UUID** + the complete device name.

Both fit inside the 31-byte AD limit (roughly an 11-byte primary AD and a 25-byte scan response), so one
advertiser exposes both profiles. The advertised name and the GAP device-name characteristic come from
a single build define (`DEV_NAME`, fed by `APP_NAME` in the makefile — default `feldd`).

**Profile scoping (MIDI-only vs keyboard) is a deliberate choice, not an accident.** Which service you
put in the *primary* advertisement changes host behavior: some macOS versions will auto-connect a bonded
"keyboard" and claim the module's single LE link, which can starve CoreMIDI. If you hit that, you can
build/advertise a **MIDI-only** profile — primary AD = Flags + the 128-bit BLE-MIDI UUID, Appearance set
to Unknown, no keyboard identity on the air — while keeping the full HID GATT service in the database.
The keyboard identity is only needed in the air when you actually want the host to attach a keyboard. The
combined single-advertiser shape above is what shipped; the MIDI-only split is the fallback if a specific
host contends for the link.

---

## GATT services

### BLE-MIDI service (the primary feature)

- A 128-bit **BLE-MIDI service** with a single 128-bit **MIDI I/O characteristic** (notify + write),
  using the Apple-defined BLE-MIDI service and characteristic UUIDs. In the source these are the two
  128-bit UUIDs in `hello_uuid.h`, stored little-endian; the example's original "Hello Sensor" vendor
  service/characteristic are repurposed to these.
- **The MIDI CCCD is intentionally unencrypted** (readable + write-without-authentication; **no** AUTH
  permission bits). Per Apple's BLE-MIDI spec a host must be able to subscribe **without bonding**. If
  you leave the example's AUTH permission flags on the MIDI CCCD, a macOS subscribe fails with
  "Encryption is insufficient," because CoreBluetooth will not pair programmatically for MIDI. Do **not**
  gate the MIDI CCCD on security.
- Outbound MIDI is assembled by a small, dependency-free **packetizer** (our own
  `feldd_midi_pack.{c,h}`, no BTSDK/hardware deps, unit-testable on a host). It builds Apple BLE-MIDI
  notify packets from raw MIDI bytes forwarded by feldd: a timestamp-high header byte
  (`0x80 | ((ms>>7) & 0x3F)`), then per event a timestamp-low byte (`0x80 | (ms & 0x7F)`) followed by the
  1–3 MIDI bytes, flushing before it would exceed `ATT_MTU − 3` (20 usable bytes at the default 23-byte
  MTU).

### HOGP HID keyboard service

A standard **HID-over-GATT (HOGP)** keyboard service coexisting with BLE-MIDI. It is added above the
Battery service so that all attribute handles stay strictly ascending, as the GATT-DB byte format
requires. It contains:

- **HID Information**, **HID Control Point**, **Protocol Mode** (report protocol).
- A **Report Map** describing a standard 8-byte boot keyboard report (`[modifiers][reserved][6 key
  codes]`, no Report ID item in the map).
- An **Input Report** characteristic with its **CCCD** and a **Report Reference** descriptor.

The GAP **Appearance** characteristic is set to **HID Keyboard (961 / `0x03C1`)** to match the
advertisement, so hosts treat the device as a keyboard.

### Device Information service — including the PnP ID

Manufacturer Name / Model Number / System ID as in the example, **plus a read-only PnP ID
characteristic (`0x2A50`)**. The PnP ID is **HOGP-mandatory**: the Report Host (iOS/macOS) reads it on
connect to instantiate the HID device. We add it with a 7-byte value: vendor-ID source `0x01`
(Bluetooth SIG), VID `0x0131`, PID `0x0001`, version `1.0.0`. Adding this was the final piece that made
iOS actually *attach* the keyboard rather than just bond and subscribe (see the HID fixes below).

---

## feldd's private WICED-HCI command group

feldd talks to the module over the module's WICED-HCI UART using a **private command group `0xF0`**.
The app registers a transport RX handler that decodes frames for this group and dispatches:

- **`0x01` MIDI** — raw MIDI bytes to forward; fed into the packetizer and notified on the MIDI
  characteristic when a host is subscribed.
- **`0x02` HID** — a HID report from feldd to notify on the Input Report characteristic (gated on an
  encrypted link; see below).
- **`0x03` PING** — liveness check; the app replies with its READY event.
- **`0x04` ADV** — user Bluetooth control: start/stop advertising. (If you implement the MIDI-only vs
  keyboard split, this is the natural place to also carry a profile selector as an optional second
  payload byte, kept backward-compatible so an older build that reads only the first byte still works.)
- **`0x05` CLEAR_BONDS** — forget bonds and re-advertise (wired to a device button combo on the SP-1).

The app emits two events back to feldd: a **LINK_STATE** event (sent on connect/disconnect and on
subscription/encryption changes) and a **READY** event (sent at boot and in reply to PING). feldd
mirrors the link state in its own status flags so the controller side knows when a host is connected,
MIDI-subscribed, HID-subscribed, and encrypted.

---

## The HID fixes (why the keyboard didn't work, and what we changed)

BLE-MIDI worked before the keyboard did. The HID keyboard needed a chain of independent module-side
fixes, each found and byte-verified on hardware. If you port this, apply all of them:

1. **Use the writable-characteristic macro for writable HID chars.** In the v1 btstack GATT-DB byte
   format, any characteristic whose permissions include a write bit **must** be declared with the
   `_WRITABLE` characteristic macro, which emits an extra max-write-length byte in the value-attribute
   header. **Protocol Mode (`0x2A4E`) and HID Control Point (`0x2A4C`) are writable** but the example's
   plain macro omits that byte. With the byte missing, the ROM DB parser **misaligns at that handle**, so
   every attribute after it — the Input Report, its CCCD, its Report Reference — decodes as garbage and
   effectively does not exist. The host then finds no keyboard report characteristic (no Keyboard Setup
   Assistant, no subscribe). MIDI, whose handles are *before* the corruption, is unaffected — which is
   exactly why MIDI worked and HID did not. **Fix:** declare both writable HID chars with the writable
   macro.

2. **Drop AUTH permissions on the HID characteristics; enforce security at runtime instead.** This
   SDK's GATT-DB permissions are effectively "no security" or "authenticated (MITM)" — there is **no
   encryption-only** permission. The SP-1 pairs **Just Works** (no passkey display), which yields an
   *unauthenticated* key, so AUTH permissions on the HID chars/CCCD can never be satisfied and the host's
   subscribe is rejected. **Fix:** use plain readable / readable+write permissions on the Report Map,
   Input Report, and Report CCCD, and gate the actual keystroke notifications at **runtime** on a
   `link_encrypted` flag (no encrypted link → no HID notifications). Keep the MIDI CCCD unencrypted, as
   above.

3. **Force encryption on connect.** With plain HID permissions, some hosts (iOS) never initiate pairing
   on their own. On connection-up the app calls the stack's "set encryption" for the LE transport with
   an *unauthenticated* (no-MITM) security requirement, so the module drives the Just-Works bond itself.

4. **Add the PnP ID (`0x2A50`).** HOGP-mandatory; without it the host will bond and subscribe but never
   *attach* the keyboard (on-screen keyboard won't hide, no keys). See the Device Information section.

5. **Report Reference must use Report ID 0.** The boot-keyboard report map has no Report ID item, so the
   Report Reference descriptor must be `{id 0, type input}`.

6. **GAP Appearance must match the advertisement.** Set it to HID Keyboard (961), not the example's
   generic tag, so hosts treat the device as a keyboard.

7. **Give Protocol Mode a write handler.** iOS/macOS write Protocol Mode = 1 during HOGP setup; the
   example's write handler default-rejects unknown handles, which can stall the host's keyboard init. Add
   an accept-and-store case for the Protocol Mode value handle.

**Host-side gotcha:** iOS caches the GATT database per bond and this app has no Service Changed
characteristic, so after any GATT-DB change you must **Forget the device and re-pair** or the host
serves a stale table. (A natural future improvement is to add a Service Changed indicate characteristic
and persist the HID CCCD per bond so a subscription survives a reconnect.)

---

## Building the app

- **Stack version:** the CYW20706 uses the **v1 BT stack (`COMPONENT_btstack_v1`)**. Build that target.
  The stack-version-specific sources — the GATT database + write handler
  (`COMPONENT_btstack_v1/hello_sensor_gatt.c`) and the stack config
  (`COMPONENT_btstack_v1/wiced_bt_cfg.c`) — are where the GATT-DB byte layout, the advertisement config,
  and the write handler live. The example also ships a v3 tree for other chips; you do not build it for
  the 20706.
- **Toolchain:** build with **ModusToolbox** (BTSDK) targeting `CYBT-353027-EVAL`. The output you want is
  the module download image (`..._download.hex`), which feldd converts to the embedded blob and flashes.
- **Always do a clean build after changing the name.** `APP_NAME` feeds a compile-time `-D` define for
  the advertised/GAP name, and make does **not** track compile-define changes, so an incremental build
  can bake a stale name. `make clean build`.

---

## Safety: how this app reaches the module (never chip-erase)

This app is written to the module's serial flash using a **Download-Section-only "Upgrade Download"** —
it writes only the module's Download Section and **never** issues a full chip-erase. That distinction is
critical and non-negotiable:

- The module's **Static Section** holds the factory **BD_ADDR + RF calibration**. There is **no public
  Teenage Engineering restore image** for it. A full chip-erase would wipe the Static Section and the
  module would be **unrecoverable**.
- Therefore: **only ever use the Upgrade (DS-only) path. Never a full/chip-erase.** The DS write floor
  is placed strictly above the module's other reserved sections so a DS write can never reach the Static
  Section. feldd's flasher reads the module's BD_ADDR before writing and hard-aborts if it does not match
  the expected unit.
- The module app itself is recoverable independently of this: the module's boot ROM always re-enters
  download mode via a Recovery-Reset, so a bad app image can be re-flashed.

**Audience note.** Reflashing the module is a developer operation for an SWD-equipped or eval-board
unit. A stock SP-1 owner never runs the reflash by hand — they get BLE by flashing a feldd build that
carries the feature, and feldd drives the SS-preserving Upgrade on-device. Never blind-flash firmware
onto a stock keeper unit.

---

## Status

Both features are working on real hardware: **BLE-MIDI** (macOS, iOS, OP-XY, TP-7) and a **BLE-HID
keyboard** (iOS attaches feldd as a keyboard and types over the air), from the custom AIROC app built as
above and flashed to the module with the Static Section preserved byte-for-byte.
