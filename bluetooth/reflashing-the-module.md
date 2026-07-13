# Reflashing the SP-1's Bluetooth module — toolchain + safe reflash

This is the reproducible method for putting a custom Bluetooth LE application onto the Teenage
Engineering SP-1's onboard Infineon **CYBT-353027-02** module (Cypress/Infineon **CYW20706A2**
silicon), so the SP-1's `feldd` firmware can drive **wireless BLE-MIDI** and a **wireless BLE-HID
keyboard**.

The module's factory application is **Bluetooth Classic only** (an A2DP sink) — it has no LE stack.
Reaching BLE therefore requires flashing a **custom AIROC application** onto the module. This
document is the how-to for doing that safely and reproducibly.

> **Community note.** As far as we can tell, nobody public had driven this module from the SP-1
> before `feldd` did it. This write-up is unofficial and not affiliated with Teenage Engineering,
> Infineon, or Cypress. Every hardware behavior described here was **captured on real hardware** and
> is reproducible with the tooling below. Where something was inferred from a datasheet rather than
> observed on the wire, it says so.

---

## Who this is for (read this first — it is a safety gate)

The reflash procedure is **not** for a stock SP-1 owner. There are three very different audiences and
you must know which one you are before touching hardware:

- **Stock SP-1 owner (most people).** You get wireless BLE by flashing a `feldd` build that carries
  the Bluetooth feature; `feldd` then drives and flashes the onboard module for you, on-device,
  through the normal `feldd` flash flow. **You never run the procedures on this page directly.** Stop
  here.
- **Debugger-equipped SP-1 owner ("burner").** An SP-1 wired for SWD, used for firmware development.
  You can run the on-device module-reflash pipeline described below, under the burner-safety rules.
- **Eval-board owner (CYBT-353027-EVAL).** You have Infineon's disposable evaluation board for this
  exact module. This is where you **rehearse** the full reflash — the toolchain, the minidriver
  launch, and the DS-only Upgrade — before any of it touches a real SP-1.

The single irreversible mistake this whole method is designed to avoid is a **chip erase that wipes
the module's Static Section** (its factory BD_ADDR, crypto keys, and RF calibration). Teenage
Engineering's stock module image is not public, so there is **no restore image** if you destroy it.
Everything below is structured so that the destructive command is never issued and the write is
confined to the application region.

---

## The memory model you must respect

The CYW20706A2 has on-chip ROM (the Bluetooth stack + a boot/download ROM) and RAM, but **no on-chip
application flash**. All persistent state lives in a discrete **512 KB SPI serial-flash chip** on the
module, memory-mapped at base **`0xFF000000`**. That serial flash holds three logical sections
(per Infineon's *AIROC HCI Firmware Download* doc, 002-32056):

| Section | Contents | Rule |
|---------|----------|------|
| **SS — Static Section** | Factory BD_ADDR, crypto keys, RF calibration. Programmed once, meant to last the life of the part. | **Never erase. Not restorable.** |
| **VS — Volatile Section** | Run-time records kept across power cycles. | Preserve. |
| **DS — Data Section** | The application image + config, processed at normal boot. | This is the only thing you write. |

### Upgrade vs Full Download — the load-bearing safety fact

- **Upgrade Download** writes the **DS only**. It issues **no chip-erase**; the minidriver auto-erases
  only the sectors it is about to write. **SS and VS are preserved.** This is the only path you ever
  use.
- **Full Download** issues a **chip-erase first**, then reprograms SS + DS. Because the SS is not
  restorable, a Full Download leaves an alive-but-identity-dead module. **Never do this.**

There is **no "dump flash to file"** command in the AIROC toolchain — ChipLoad only writes and
CRC-verifies. A backup can only be composed by hand from repeated read-memory reads over `0xFF000000`.
This is exactly why you favor Upgrade: so a restore is never needed.

**Captured on real hardware:** the boot ROM can read the off-chip serial flash (including the SS)
with **no minidriver** — a read-only memory read at `0xFF000000` returns the real Static Section
bytes (the module's own BD_ADDR appears in the returned data, matching what the running app reports),
and the read is deterministic across runs while an on-chip RAM control read varies run-to-run,
confirming it is genuine flash. So a **read-only SS backup is feasible** on a debugger-equipped unit
with no write/launch/erase machinery in the build at all. Treat that backup as limited insurance —
restoring it would itself require a flash write — not as a license to run Full Download.

---

## Step 1 (the one manual step): get the toolchain on disk

You need two things from Infineon's **ModusToolbox**, and they come from two different places:

### 1a. The programmer pieces — minidriver + ChipLoad (no login)

The flash tooling itself lives in the small **"ModusToolbox Programming Tools"** package (roughly
50 MB), which downloads from Infineon's software-tools portal **without an account**. You do not have
to run the installer — the `.pkg` payload can be extracted read-only (on macOS,
`pkgutil --expand-full`, no admin/root). Inside the extracted programmer you will find, under the
`CYBT_353027_EVAL` board folder:

- `minidriver.hex` — the **CYW20706-specific** RAM minidriver (a per-part file; do **not** substitute
  a 20719 or 20819 minidriver from another example — those are different silicon).
- `20706A2_OCF.btp` — the flash layout descriptor (SS at offset `0x0`, DS at offset `0x4000` →
  `0xFF004000`).
- `CYW20706A2_IDFILE.txt`, `platform.conf`.
- `chipload/bin/ChipLoad` — the native flashing binary.

Copy those into **your ModusToolbox build tree** (a working folder of your choosing — for example a
`board/` subfolder holding the four board files plus a `chipload/` folder for the binary). These are
Infineon-licensed binaries; keep them local and **do not commit them to a public repo**.

On macOS, Gatekeeper may quarantine the unsigned `ChipLoad` binary; clear it once with
`xattr -dr com.apple.quarantine <path-to-ChipLoad>`.

### 1b. The build toolchain — only needed to compile a custom app (Infineon login)

To *build* a custom AIROC application (rather than just flash a prebuilt one) you need the BTSDK build
toolchain — the make/linker/CGS flow that compiles the app into a DS image. That ships inside the full
ModusToolbox BTSDK pack, which **does** require a free Infineon account to download.

1. Download **ModusToolbox for your OS** from Infineon (the 3.x line still supports the 20706) and
   install it.
2. Launch **Project Creator** and create **one** BTSDK project targeting the **`CYBT-353027-EVAL`**
   BSP (if it is not listed, `CYW920706WCDEVAL` is the same silicon). Seed it with any Bluetooth
   (BTSDK) example. The specific example does not matter — creating any project clones the full
   `wiced_btsdk` tree (baselib, BSP, tools) onto disk, which is all you need.
3. Let it finish resolving dependencies.

That download is the only login-gated part. Once the files are local, no further login is needed.

> If the Project Creator UI differs across ModusToolbox versions, don't worry about the exact clicks —
> the only goal is to land a BTSDK 20706 project on disk so the build toolchain and baselib are
> present.

---

## Step 2: the custom AIROC application

The custom module application is derived from Infineon's public BTSDK example
**`mtb-example-btsdk-ble-hello-sensor`** (target CYW20706A2, `COMPONENT_btstack_v1`).

> **Source is not redistributed here.** The example carries Cypress's restrictive license (all rights
> reserved, non-transferable, no redistribution). **Download the example yourself from Infineon** and
> apply the changes below to your own copy. What follows describes *our modifications in prose* so you
> can reproduce them; it does not reproduce any Cypress/Infineon source.

The app advertises as a combined BLE-MIDI + BLE-HID (HOGP) keyboard peripheral. Getting the HID
keyboard to actually attach on iOS/macOS took a chain of independent GATT-database fixes. Each was
verified on hardware; a host is only "fully ready" when it has connected, subscribed to the HID
report, and encrypted the link.

Changes to apply to the example's GATT database and connection handling:

1. **Use the *writable* characteristic macro for every writable characteristic.** In the btstack_v1
   database byte format, a characteristic whose permission byte includes any write bit **must** be
   declared with the `_WRITABLE` variant of the characteristic macro, because that variant emits an
   extra max-write-length byte in the value-attribute header. Two HID characteristics — **Protocol
   Mode (0x2A4E)** and **HID Control Point (0x2A4C)** — are writable but the example declares them with
   the plain (non-writable) macro. The missing length byte **misaligns the ROM's database parser** at
   that handle, so every attribute after it (the Input Report `0x2A4D`, its client-config descriptor,
   and its Report Reference) decodes as garbage and effectively disappears. The host then finds no
   keyboard report characteristic, never subscribes, and never launches the Keyboard Setup Assistant.
   MIDI, whose handles come *before* the corruption, is unaffected — which is exactly why MIDI worked
   while HID did not. **Fix: declare both with the writable macro.**

2. **Drop the authenticated (MITM) permissions on the HID attributes.** This SDK's database
   permissions are either "no security" or "authenticated" — there is **no encryption-only**
   permission. The SP-1 pairs **Just Works** (no passkey display), which yields an *un*authenticated
   key, so an authenticated permission on the HID Report Map, Input Report, or its client-config
   descriptor can never be satisfied and the host's subscribe write is rejected. **Fix: use plain
   readable / readable+write-request permissions on those attributes**, and enforce security for the
   keystroke stream at runtime instead (see #3). Leave the BLE-MIDI descriptor unencrypted, per
   Apple's BLE-MIDI spec.

3. **Force encryption on connect, and gate keystrokes on it at runtime.** With plain HID permissions,
   iOS will not initiate pairing on its own, so on connection-up the app calls the SDK's
   set-encryption routine for the LE transport with the no-MITM security level. This forces the
   Just-Works bond. The keystroke path then refuses to send notifications unless the link is actually
   encrypted (a runtime `link_encrypted` gate), which restores the security the dropped permissions
   used to provide.

4. **Report Reference must reference Report ID 0.** The boot-keyboard report map contains no Report ID
   item, so the Report Reference descriptor must be `{id 0, type input}`, not id 1.

5. **Set the GAP Appearance to HID Keyboard.** The advertisement already declares the HID-keyboard
   appearance; set the GAP Appearance characteristic to match (HID keyboard, not the generic tag) so
   hosts treat the device as a keyboard.

6. **Give Protocol Mode a write handler.** iOS/macOS write Protocol Mode during HOGP setup; the
   example's default write case returns an error, which can stall the host's keyboard init. Add an
   accept-and-store case for the Protocol Mode value handle.

7. **Add the HOGP-mandatory PnP ID characteristic (0x2A50) to the Device Information Service.** This
   was the final missing piece: iOS/macOS reads the PnP ID on connect to instantiate the HID device.
   Without it, the host bonds and subscribes but **never attaches** the keyboard (the on-screen
   keyboard won't hide and no keys arrive). Add a read-only PnP ID characteristic with a valid
   vendor/product/version value and its attribute-table entry.

**Host-cache gotcha.** iOS caches the GATT database per bond, and this app has no Service Changed
characteristic (0x2A05), so after **any** database change you must **Forget the device on the host and
re-pair**, or it serves the stale table. (Adding a Service Changed indicate characteristic so
re-pairing isn't required is a reasonable follow-up.)

Build the app with the BTSDK make/CGS flow. The output is a downloadable DS image
(`..._download.hex`) — that hex is what you flash. A quick offline sanity check on the built image is
to byte-grep the ELF for the fixed Protocol Mode signature (the writable variant adds the length byte)
and for the plain HID client-config-descriptor permission.

---

## Step 3: flash it — DS-only Upgrade

There are two flash paths. **Rehearse on the eval board first**, then use the on-device path on a
debugger-equipped SP-1.

### The download flow (WICED-HCI, over the module's HCI UART)

Both paths speak the same WICED-HCI download protocol to the module's boot ROM. Opcodes below are from
Infineon's primary docs (HCI opcode bytes are little-endian on the wire); the whole entry sequence is
hardware-validated:

1. **Recovery Reset into the download ROM.** The module runs an auto-launching app, so a plain reset
   boots the app, not the ROM — you must force recovery. Assert the module's CTS low (host RTS
   asserted), pulse RST_N low ~10 ms, deassert, wait ~10 ms, then release CTS. The ROM enters an
   autobaud download state.
2. **`HCI_RESET` (`0x0C03`)** — `01 03 0C 00` → `04 0E 04 01 03 0C 00`. **The download ROM
   re-autobauds on every download-mode entry**, so a single HCI_RESET is not enough — loop it until
   the ack lands (it commonly acks only on the 2nd–4th send). Every download step must tolerate this.
3. **`DOWNLOAD_MINIDRIVER` announce (`0xFC2E`)** — `01 2E FC 00` → `04 0E 04 01 2E FC 00`. This is a
   zero-payload "begin download" announce; it does **not** carry the minidriver bytes.
4. **Load the minidriver** via `WRITE_RAM` (`0xFC4C`) in 240-byte chunks to RAM `0x00220000`
   (increment the address by `0xF0` per chunk), then **`LAUNCH_RAM` (`0xFC4E`)** at `0x00220000`. The
   minidriver adds the serial-flash sector tracking, baud change, and CRC verify that the ROM lacks.
   (Use the CYW20706-specific minidriver from Step 1a — never a 20719/20819 one.)
5. **Write the DS** through the minidriver's Upgrade path — DS sectors only.
6. **VerifyCRC (`0xFCCC`)** against the written image.

Opcodes you must **never** issue in a real flash: **`CHIP_ERASE` (`0xFFCE`** — note the second byte is
`FF`, not `FC`) and any Full Download. The special erase value that wipes the lowest valid NV range
takes out SS+VS+DS and is irreversible.

### Path A — Mac-side flash on the eval board (ChipLoad)

Get the eval board into download mode, then drive ChipLoad. The ChipLoad invocation:

```
ChipLoad -BLUETOOLMODE -PORT <port> -BAUDRATE 115200 \
  -MINIDRIVER board/minidriver.hex \
  -BTP board/20706A2_OCF.btp \
  -CONFIG <app_download.hex> \
  -NOERASE
```

**`-NOERASE` is mandatory on any non-disposable target.** The `.btp` default sector-erase mode is
"Chip erase"; `-NOERASE` overrides it so ChipLoad never issues a chip-erase and only the sectors being
written are erased — SS preserved. (ChipLoad also supports `-NOWRITE`, `-NOVERIFY`,
`-ENTERDOWNLOADMODE`, `-NODLMINIDRIVER` for dry-runs and staged rehearsals.) Rehearse the chip-erase
path, if ever, **only** on a genuinely disposable eval board.

### Path B — on-device flash from `feldd` (the SP-1 drives its own module)

On a debugger-equipped SP-1, `feldd` embeds the built module image as a blob and flashes it over the
module's WICED-HCI UART using the same DS-only Upgrade path. The high-level sequence:

1. Build the module app (Step 2) and regenerate the embedded blob from the **fresh** module hex.
   (Verify the generated blob header is newer than the module hex, or you will embed a stale
   database.) The blob generator is validation-gated: it asserts the DS base sits at the configured DS
   floor, every record is at or above the floor, and records are contiguous — and refuses to emit
   otherwise.
2. Build the "armed" downloader `feldd` (the config that enables the DS write). The armed config sets
   the download-arm flag and **must never** set any chip-erase flag; a device-tree overlay provides
   the module UART.
3. **Run the non-destructive dry-run first.** It talks to the module, runs the identity gate, and does
   a DS dry-run without writing. It must report the module's SS BD_ADDR, "identity OK", and a clean DS
   range before you proceed. If not — stop.
4. **Run the armed write** — the only destructive step. The firmware's identity gate **hard-aborts**
   unless the module's SS BD_ADDR matches the value pinned for that specific unit, so an armed build
   cannot flash the wrong module.
5. Restore the normal controller `feldd` afterward so the SP-1 runs as a controller again.

Kill any host-side CC daemon or serial session before flashing — DTR toggling on the SP-1's USB can
disturb `feldd` mid-flash.

**Safety invariants (never violate):**

- Set a **DS floor strictly above the VS region end** so a DS write can never reach VS or the SS at
  `0xFF000000`. (The `.btp`'s standard DS base is `0xFF004000`; `feldd` pins a floor above VS and
  refuses any record below it.)
- The module's SS / BD_ADDR must survive **byte-identical** — the identity gate reads it
  pre-minidriver and aborts on any mismatch.
- **Never** enable a chip-erase config flag. Always Upgrade, never Full.
- The module is recoverable: the boot ROM's Recovery Reset (mask ROM) always re-enters download mode,
  so a wedged app or a bad DS write is not a brick — as long as the SS was never erased.

### Confirming success on hardware

- **BLE-MIDI:** the SP-1 advertises and connects as a BLE-MIDI peripheral. Captured on real hardware:
  BLE-MIDI to Macs and iPhones, and to Teenage Engineering gear — the **OP-XY** (pair from its
  COM › devices menu) and the **TP-7**.
- **BLE-HID keyboard:** captured on real hardware — a bonded iPhone reads the PnP ID, attaches `feldd`
  as a keyboard, and types over BLE. The host progresses through connected → subscribed → bonded/
  encrypted → attached; a status flags byte reaching the "connected + MIDI + HID + encrypted" state
  followed by the host reading the PnP ID indicates a fully-ready keyboard.

---

## The eval board: CYBT-353027-EVAL controls, switches, and recovery

The disposable eval board is where you rehearse everything. Its controls (from Infineon's user manual,
doc 002-23324, and KBA223509 / KBA223428, "Programming an EZ-BT WICED Module"):

**Connector / power**
- **J5** — the single micro-USB. It powers the board **and** is the Cypress USB-Serial bridge to the
  host. The red POWER LED (D7) indicates power. It enumerates as two serial ports: an HCI port and a
  PUART port.
- **U1** — the CYBT-353027-02 module itself (CYW20706A2).

**Buttons (momentary)**
- **SW1 = XRES** — reset, routed to the module's XRES (active-low).
- **SW2 = RECOVER** — routed to the module's recovery strap (SPI2 chip-select).
- **SW3 = USER** — a user button (module GPIO).

**DIP switches — critical for programming**
- **SW4 = HCI UART** (switches 1–4 = CTS / RTS / RXD / TXD). This connects the module's HCI UART to the
  host through the USB bridge. **All four must be ON to program or reach download mode over USB.** If
  any is OFF, the host cannot talk to the module even when it *is* in download mode — the symptom is a
  silent HCI_RESET (no `04 0E 04 01 03 0C 00` ack). Watch for tape or film over the switch bank
  holding one OFF; confirm all four are physically toward the "ON" label.
- **SW5 = PUART** — not needed for programming.

**Getting into download mode.** The module runs an auto-launching app, so a plain reset boots the app,
not the ROM — you must force recovery (SW4 must already be all-ON). Two equivalent methods:

1. **Button method:** hold **SW2 (RECOVER)**, press and hold **SW1 (XRES)** ~1–2 s, then **release
   SW1 first, and only then release SW2** (RECOVER must stay held until after reset is released so it
   is sampled at the reset edge).
2. **Power-cycle method:** hold **SW2 (RECOVER)**, power-cycle the board (unplug/replug USB, or a
   controllable USB hub) **while still holding SW2**, then release SW2 after power returns.

Verify you're in download mode with an HCI_RESET autobaud on the HCI port (`01 03 0C 00` →
`04 0E 04 01 03 0C 00`).

---

## Appendix: passive logic-analyzer sniff of a *stock* SP-1

This is a fully non-destructive way to capture what the **factory** SP-1 firmware does to the module at
boot — the one thing a `feldd`-driven capture can never show, because once `feldd` replaces stock, it
can only show what `feldd` itself sends. Run it on a **stock unit** (factory firmware intact) with an
8-channel USB logic analyzer. It is passive tapping only; nothing is written.

**Channel map** — all on the nRF ↔ module UART plus reset, with a common ground to SP-1 ground:

| Ch | Signal | nRF pin |
|----|--------|---------|
| CH0 | nRF TX → module RX | P1.02 |
| CH1 | module TX → nRF RX | P1.04 |
| CH2 | RTS | P1.01 |
| CH3 | CTS | P1.03 |
| CH4 | Reset (active-low; use as trigger) | P0.10 |
| CH5 | optional SPI chip-select (a second control path, if it toggles) | P1.05 |

**Logic level — confirm with a meter first.** The SP-1 nRF I/O is likely **1.8 V**. Many cheap logic
analyzers have a ~1.4–1.5 V input threshold that is marginal at 1.8 V; a capture that decodes as
garbage is usually a threshold problem, not a baud problem. Use an analyzer with an adjustable/low
threshold or add a fast level shifter.

**Analyzer setup:**
- Sample rate **≥ 4 MS/s** (≥ 8× the 115200 bit rate) for clean edges and accurate
  reset→first-byte timing.
- Two async-serial/UART decoders — one on CH0, one on CH1 — both **115200, 8N1, LSB-first**.
- **Trigger on CH4 rising edge** (P0.10 is active-low, so reset *release* is the low→high edge) with a
  large pre-trigger buffer to catch the very first post-reset bytes.
- Keep leads short and loosely twist each signal with ground to avoid cross-talk at 1.8 V.

**What each capture proves:**
1. **First bytes after reset-release → AIROC/WICED vs raw H4.** WICED HCI-Control framing (a
   group/command header) confirms the module runs an embedded app driven by the host. Raw H4 (standard
   `0x01` command / `0x04` event with standard opcodes) would mean a hosted controller.
2. **Reset→first-byte timing** and the boot handshake — the timing `feldd` must reproduce.
3. **Any vendor/config commands at boot** (baud change, BD_ADDR set, patch/minidriver download,
   service records) — tells you whether the factory firmware does a per-boot download or just talks to
   the resident app.
4. **RTS/CTS behavior** — confirms hardware flow control and baud, and whether the factory firmware
   ever drives CTS low (the recovery strap) during a normal boot (it should not).
5. **CH5 activity** — presence or absence of a second SPI control path.

Save the capture plus a short note of the first ~1 KB decoded on CH0/CH1, timestamped relative to the
CH4 edge. That settles the module's boot mode empirically, from the wire rather than from docs.
