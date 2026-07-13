# feldd Bluetooth: findings and results

This is the durable evidence trail for bringing **wireless Bluetooth Low Energy** to the Teenage
Engineering SP-1 through [feldd](https://feldd.com), the SP-1's custom nRF52840 firmware. It documents
what was proven on real hardware, how to reproduce it, and the safety rules that make it non-destructive.

**The result, in one line:** feldd now drives the SP-1's onboard Infineon **CYBT-353027-02**
(CYW20706 silicon) Bluetooth module to do **wireless BLE-MIDI** and a **wireless BLE-HID keyboard**.
Confirmed on hardware: BLE-MIDI to Macs and iPhones, to the TE **OP-XY** (it pairs from its own
`COM > devices` menu) and the **TP-7**; and a BLE-HID keyboard that an iPhone attaches and types from
over the air.

This is unofficial community work. As far as we know, nobody outside TE had driven this module from
the SP-1 before feldd did. Everything below came from a capture on a real device and is reproducible
with the method described. Where something is inferred rather than measured, it says so. The SP-1
community has publicly flagged AI hallucinations about this hardware, so precision here is the point:
no invented bytes, opcodes, pins, or timings.

---

## The two-chip model

The nRF52840 that runs feldd has no Bluetooth antenna wired on the SP-1, so it cannot do BLE on its own
radio. All wireless goes through the SP-1's **second chip**: the onboard Infineon CYBT-353027-02 module,
which is CYW20706 silicon with its own Cortex-M3, BT radio, and antenna. feldd talks to that module over
a UART using Infineon's WICED / AIROC HCI-Control protocol.

Throughout this document, "the module app" always means the app running on the **CYW20706**, never
feldd. feldd runs on the **nRF52840**. Do not conflate the two chips.

---

## Hardware findings (captured on real hardware)

Each finding below was captured on a real SP-1 (an SWD-equipped development unit) or on a
CYBT-353027-EVAL development board, not inferred from documentation.

### The module silicon is CYW20706A2

Confirmed by reading the module directly, not assumed from the part number. This matters because a
common BTSDK HOGP example targets a different chip (CYW20819) on a different board; the SP-1's module is
a **CYW20706A2** running the **v1 BT stack** (`COMPONENT_btstack_v1`), and app builds must target that.

### The stock module app is Bluetooth Classic only, with no LE stack

The SP-1's factory module app is a fully responsive WICED HCI-Control endpoint for the DEVICE command
group. A passive LE-group scan over that interface returned **zero** response while a DEVICE-group read
answered in the same session. Conclusion: the stock app is a **Bluetooth Classic / A2DP sink** with no
LE stack at all.

The consequence drives the whole project: **BLE is not reachable by driving the stock app.** It requires
flashing a **custom AIROC app** onto the module. There is no configuration path or hidden LE mode; the
capability simply is not present in the factory firmware.

### The module's download boot ROM always answers, so the module is recoverable

At power-on the module comes up in embedded (AIROC) mode. Holding the HCI-UART **CTS line low as RST_N
de-asserts** puts it into the mask-ROM firmware-download mode, and it then stays there. This is the same
condition the eval board's physical Recover switch produces. The boot ROM is in mask ROM and always
re-enters download mode on this condition, which means **a module can be re-flashed even after a failed
write** — recovery is a hardware condition at reset, not something a bad app can lock you out of.

There is no app-side "reboot into download mode" API on the CYW20706; a plain watchdog reset just
relaunches the current app unless CTS is being held low at that reset.

### READ_RAM reads off-chip flash, including the Static Section, before any minidriver

Using only the read-only `READ_RAM` (0xFC4D) command at the download-ROM level, reads succeed against
RAM and against the module's **off-chip SPI serial flash at base 0xFF000000**. Reading 0xFF000000
returns the module's **Static Section (SS)**, which contains the factory **BD_ADDR**, RF calibration,
and keys. The read is genuine, not an artifact: the flash read is **byte-identical across runs**, whereas
RAM reads vary — determinism itself authenticates that it is really flash. This makes a **read-only SS
backup feasible with no risk**, and it is the basis of the safety gate described later.

### The burner-safe DS-only Upgrade is validated: the Static Section survives byte-identical

The one truly unrecoverable mistake on this module would be a **full chip-erase**, which wipes the
Static Section. TE does not publish a restore image, so a wiped SS means a permanently bricked radio.
The entire method avoids that by using **only an Upgrade Download** — writing the **Data Section (DS)**
and never touching the SS, never issuing CHIP_ERASE.

Validated on hardware, on a development board first: a DS write of a real ~86 KB app over an already
**occupied** factory DS sector, with **no chip-erase**, verified **byte-for-byte MATCH** on read-back,
and — the load-bearing result — the **SS survived byte-identical**. Two facts fall out of this:

- The module's minidriver **auto-erases occupied sectors** as part of a DS write, so you do not need a
  chip-erase to overwrite an existing app.
- Flash erase granularity is a **4 KB sector**, not a 64 KB block. A DS write above the SS therefore
  **cannot reach the SS at 0xFF000000**. The "a 64 KB block erase would wipe the SS" brick risk is
  **ruled out** for the Upgrade path.

Two further protections were observed while probing: a normal `WRITE_RAM` to the SS region is
**rejected** — the minidriver protects the Static Section from a DS-style write — and the SS read-back
after a DS write is unchanged. Extra insurance that a bug cannot casually clobber the factory BD_ADDR.

### Cypress varies the SS layout across products (so never hardcode DS location blindly)

The SS is a TLV stream (`[type u8][len u16 LE][payload]`), not a fixed-offset struct. The DS-base pointer
lives at `payload+0` of the **type-0x02 record, wherever that record happens to sit**. On the SP-1 module
that record places the DS base at **0xFF003000**; on the CYBT-353027-EVAL board the same record sits at a
different offset and points the DS base at 0xFF004000, and the Volatile Section extent differs too. The
practical lesson: **discover the DS base by walking the SS TLV, and treat any assumption about a fixed
byte offset as wrong.** This directly shaped the safety gate below.

---

## The reproducible reflash method (DS-only, SS-preserving)

This is the method that put the custom app on the module without ever risking the SS. It is a
development / SWD-unit procedure; a stock-unit owner does not run it by hand.

1. **Build the custom module app** with Infineon's BTSDK (`make`, targeting the CYBT-353027-EVAL /
   CYW20706A2, `COMPONENT_btstack_v1`). This produces a download image whose records include an SS
   record at 0xFF000000 and the app DS at the DS base.
2. **Extract a DS-only artifact** — keep only the records at or above the DS base; **discard the SS
   record**. The SS record in a freshly built image carries a *foreign* BD_ADDR; writing it to a real
   unit would overwrite that unit's factory BD_ADDR even without a chip-erase. Dropping it is what keeps
   the write safe.
3. **Load the minidriver into RAM and launch it** (`DOWNLOAD_MINIDRIVER` announce → `WRITE_RAM` the
   minidriver → `LAUNCH_RAM`). Read-back-verify the minidriver bytes before launching. The minidriver is
   what enables flash writes; the download ROM alone refuses several commands ("command disallowed") until
   it is running.
4. **Dry-run first (non-destructive):** talk to the module, read and verify the SS, walk the DS plan, and
   print the intended write range — **without writing**. Nothing should surprise you at the armed step.
5. **Armed DS write:** `WRITE_RAM` the DS chunks to the DS base, then **verify the DS reads back MATCH**
   and **the SS is byte-identical** to the pre-write snapshot.
6. **Cold-boot the module** so it launches the newly flashed app, which advertises on boot.

**Safety invariants (never violate):**

- **Never issue CHIP_ERASE.** The build that performs the write is configured so chip-erase is not even
  compilable into the image.
- **feldd writes only a compile-time-constant DS address**, never a value computed or discovered at
  runtime. Per-unit SS discovery is **compare-only**: it can *refuse*, it can never *redirect* a write.
  A misparsed or unexpected SS degrades to a safe abort, never a mis-targeted write. Every DS chunk is
  gated against an unconditional floor check (a plain `if`, not an assertion) so a write can never land
  below the DS floor and reach the Volatile Section or the SS.
- **The SS must survive byte-identical.** An identity gate reads the SS before the minidriver stage and
  hard-aborts on any mismatch.

### Shipping this safely to stock units (the on-device provisioning safety model)

To bring BLE to a normal SP-1 (no debugger), feldd itself reprograms the SP-1's own module on-device.
Because a wrong write permanently bricks a user's radio, the design is built so the shipped binary is
**structurally incapable of the one unrecoverable act**:

- The only flash-mutating operation compiled into the image is a bounded DS `WRITE_RAM`; there is no
  CHIP_ERASE builder and no SS-write path in the release binary, enforced by an ELF-grep audit gate.
- Provisioning is gated by **template-equality**, not by a single byte. Before writing, feldd reads the
  live SS twice (requiring the two reads to be byte-identical, since flash reads are deterministic and
  RAM reads are not) and requires the SS header, the type-0x02 DS descriptor (which pins the DS base
  **and** the Volatile Section extent in one comparison), and the trailing bytes to match the known
  factory template exactly. The **only** per-unit bytes it will admit are the 6 BD_ADDR bytes. Any
  deviation → refuse, reboot the factory app, never guess, never relocate.
- Provisioning is **explicit opt-in** (a consent step, USB kept connected, battery/VBUS pre-checked),
  never a silent auto-flash, because a blocking multi-minute write on a marginal battery is its own
  brick class.
- **Fleet uniformity is sampled, not assumed.** Because Cypress is known to vary SS layout across
  products, at least two genuinely-stock units must be **read-only SS-probed** and shown byte-identical
  to the template (outside the BD_ADDR bytes) before any community radio is written. The read-only probe
  answers this with zero write risk.

And under everything: if a write is interrupted, the module's Recovery-Reset boot ROM re-enters download
mode, so the write is idempotently retryable — the module is recoverable.

---

## BLE-MIDI result

The custom module app advertises a standard **BLE-MIDI** service and streams MIDI over the air.

- Service UUID `03B80E5A-EDE8-4B33-A751-6CE34EC4C700`, I/O characteristic
  `7772E5DB-3868-4112-A1A9-F2669D106BF3` (notify + CCCD). These are the standard Apple BLE-MIDI UUIDs.
- **The MIDI CCCD is left unencrypted** (`READABLE | WRITE_REQ`, no authentication flags), per Apple's
  BLE-MIDI specification, so a host can subscribe **without bonding**. With authenticated permissions a
  macOS subscribe fails "Encryption is insufficient," because CoreBluetooth will not pair programmatically.
- Captured on hardware: a Mac scans, sees the advertised name and BLE-MIDI service, connects, subscribes
  **unbonded with no encryption error**, and receives a live stream of MIDI note events — with no gear
  attached. That closes the loop: feldd flashes the module, the module boots and advertises, and a host
  receives live MIDI wirelessly.

---

## BLE-HID keyboard: the fix chain that made iOS attach the keyboard

BLE-MIDI worked before BLE-HID did. The keyboard would type over USB but not over BLE: iOS never popped
the Keyboard Setup Assistant, and the module never saw the host subscribe to the HID input report. It
took a chain of independent module-app fixes, each a separate blocker, all byte-verified on hardware.

1. **GATT-DB `_WRITABLE`-macro corruption (the primary bug).** In the v1 BT-stack GATT-DB byte format, a
   characteristic with any write permission bit **must** be declared with the `_WRITABLE` variant of the
   characteristic macro, which emits an extra max-write-length byte in the value-attribute header. The
   **Protocol Mode (0x2A4E)** and **HID Control Point (0x2A4C)** characteristics were writable but used
   the plain (non-`_WRITABLE`) macro, so that length byte was missing. The ROM's DB parser then
   **misaligned** at that handle, and every attribute after it — the **Input Report (0x2A4D)**, its CCCD,
   and its Report Reference — decoded as garbage and effectively did not exist. The host found no keyboard
   report characteristic, so there was nothing to subscribe to. MIDI (whose handles sit *before* the
   corruption) was unaffected, which is exactly why MIDI worked and HID did not. **Fix:** declare both
   writable characteristics with the `_WRITABLE` macro.
2. **Authenticated permissions are unsatisfiable under Just-Works pairing.** This SDK's GATT-DB
   permissions offer only "no security" or **authenticated (MITM)** — there is no encryption-only
   permission. The SP-1 pairs **Just Works** (no display, no passkey), which yields an *un*authenticated
   key, so authenticated permissions on the HID characteristics and CCCD can never be satisfied and the
   host's subscribe write is rejected. **Fix:** drop the authenticated flags on the Report Map, Input
   Report, and Report CCCD (use plain readable / readable+write-request). Security for the keystroke
   stream is instead enforced at **runtime**: the module only sends HID notifications on an encrypted
   link. (The BLE-MIDI CCCD stays unencrypted per the Apple spec — do not gate it.)
3. **Force encryption on connect.** With plain HID permissions, iOS will not initiate pairing on its own.
   The module now calls `wiced_bt_dev_set_encryption(... BT_TRANSPORT_LE, BTM_BLE_SEC_ENCRYPT_NO_MITM)`
   when the GATT connection comes up, so it **forces** the Just-Works bond. This got iOS to actually bond
   and encrypt.
4. **The HOGP-mandatory PnP ID (0x2A50) — the final piece.** With bonding and subscription both working,
   iOS still never *attached* the keyboard: the on-screen keyboard would not hide and no keys arrived.
   The HID-over-GATT profile requires a **PnP ID** characteristic in the Device Information Service; the
   iOS/macOS HID host reads it on connect to instantiate the HID device, and without it the host bonds
   and subscribes but never attaches. **Fix:** add a read-only PnP ID characteristic to the DIS (value =
   SIG vendor source, a vendor ID, product ID, and version). With it in place, a bonded iPhone reconnects,
   iOS reads the PnP ID, attaches feldd as a keyboard, and **types over BLE**.

Two supporting fixes were needed alongside the four above:

- **Report Reference must use Report ID 0.** The boot-keyboard report map has no Report ID item, so the
  Report Reference descriptor must be `{id 0, type input}`, not `{id 1, ...}`.
- **GAP Appearance must match the advertisement.** The advertisement declares HID Keyboard (0x03C1), so
  the GAP Appearance characteristic was set to HID Keyboard (961) rather than a generic tag, so hosts
  treat the device as a keyboard.
- **Protocol Mode needs an accept-and-store write handler.** iOS/macOS write Protocol Mode = 1 during
  HOGP setup; without a handler that accepts it, the host's keyboard init can stall.

**Cache gotcha that masks any DB change:** iOS caches the GATT table per bond, and the module has no
**Service Changed** characteristic (0x2A05) to tell the host to re-discover. After any change to the
module's GATT database you must **Forget the device on iOS and re-pair**, or iOS serves the stale table.

### Known limitation: BLE-HID subscription is not yet persistent across reconnects

BLE-HID works reliably right after a fresh pair, but the keyboard subscription can drop on a later
reconnect or after a module power-cycle while BLE-MIDI keeps flowing. This is a textbook HID-over-GATT
interop issue, not SP-1-specific, and it has a well-understood cause and fix:

- The **HID input-report CCCD is held in RAM only** and is not persisted per bond, so a module reboot
  resets it to zero. A bonded HOGP host does **not** re-write the CCCD on reconnect (per the Core Spec it
  assumes the server persisted it), and with no Service Changed characteristic it won't re-discover a
  cached bonded database either. So the HID subscription silently vanishes while MIDI (whose host
  re-subscribes every session) keeps working.
- A single-slot bond store means pairing a second host (for example a Mac after an iPhone) can evict the
  first host's link key, which manifests as a Mac↔iPhone ping-pong until a Forget-and-re-pair.

**The fix (module-app side, shipped as one blob revision):** persist and restore the HID CCCD per bond
(mirroring the working MIDI path), add a multi-slot bond store, and add a GATT Service Changed
characteristic (0x2A05) so future DB changes no longer require a Forget/re-pair. Until then, the reliable
workaround is: pair from OS Bluetooth settings on **exactly one host per unit**, keep Bluetooth on for
the session, and re-pair after any module power-cycle.

---

## OP-XY and TE gear: it pairs with stock feldd, no custom central needed

**Hardware-confirmed:** the TE **OP-XY** pairs to stock feldd over BLE-MIDI with **no firmware change,
no custom BLE-central mode, and no MIDI-only advertising**. Pair *from the OP-XY*: `COM > devices`, then
select "feldd." The OP-XY is a **BLE-MIDI host** — it connects to feldd's advertised BLE-MIDI service the
same way a Mac or the TP-7 does.

Two theories were disproved on the way here, worth recording so they aren't repeated:

- "The OP-XY is peripheral-only, so feldd needs a BLE-central / host role." **Wrong** — the OP-XY hosts.
- "feldd must advertise MIDI-only (drop the HID keyboard identity) or the OP-XY won't see it." **Not
  required** — stock feldd, advertising as an HID keyboard with BLE-MIDI in the scan response, is
  discoverable and connectable from the OP-XY's `COM > devices` menu as-is.

The general lesson: don't declare a hardware capability impossible from spec-reading before trying the
device's own connect menu. TE host devices (OP-XY, TP-7, and by expectation the OP-1 Field) all host, so
they all pair with standard feldd.

A feldd **BLE-central** role remains the only path for true peripheral-to-peripheral links (SP-1 ↔ SP-1),
but it is not needed for any TE host device.

---

## Safety recap

- **Never chip-erase the module.** The Static Section (BD_ADDR + RF cal + keys) has no public restore
  image; wiping it is the one unrecoverable brick. Use **Upgrade Download only** — write the DS, preserve
  the SS.
- **The SS is preserved and verified**: read it before writing, write only the DS above it, confirm it is
  byte-identical after. The 4 KB sector erase granularity means a DS write cannot reach it.
- **The module is recoverable**: the mask-ROM download mode always re-enters on the CTS-low-at-reset
  condition, so an interrupted or failed write is retryable.
- **Never blind-flash a stock keeper.** Validate on an SWD-equipped unit or a development board first.
  For stock units, on-device provisioning is explicit opt-in, template-gated, and structurally incapable
  of chip-erase.

## Reproducing this

You need an SP-1 (an SWD-equipped unit is strongly recommended for development) or a CYBT-353027-EVAL
development board, feldd, and Infineon's ModusToolbox BTSDK. The custom module app is derived from
Infineon's `mtb-example-btsdk-ble-hello-sensor` example. That example's source is distributed under
Infineon's own license and is **not redistributed here**; download it from Infineon and apply the changes
described above in prose:

- Swap the sensor GATT for the BLE-MIDI service and I/O characteristic (standard Apple BLE-MIDI UUIDs),
  with an unencrypted CCCD.
- Add the HOGP keyboard: correct `_WRITABLE` macros on the writable characteristics, plain (non-auth) HID
  permissions with a runtime encrypted-link gate, forced encryption on connect, the PnP ID characteristic,
  the Report Reference with Report ID 0, HID Keyboard appearance, and a Protocol Mode write handler.
- Build for the CYW20706A2 / `COMPONENT_btstack_v1`, extract the **DS-only** artifact (drop the SS
  record), and flash via the DS-only Upgrade path with the SS-preserving safety gate.

Build the toolchain from Infineon's public downloads, target the module's discovered DS base, dry-run
before the armed write, and verify the DS read-back and the byte-identical SS every time.
