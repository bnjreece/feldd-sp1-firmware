# SP-1 Bluetooth: hardware and download architecture

This is the technical model a reproducer needs to understand how feldd (the SP-1's custom
nRF52840 firmware) brings **wireless BLE-MIDI and a wireless BLE-HID keyboard** to the Teenage
Engineering SP-1. It covers the SP-1's two-chip design, the Infineon CYW20706 module inside it,
the module's memory map, the safe-vs-destructive download operations, the exact download opcodes,
and why the module's stock firmware can't do BLE at all.

This is unofficial community work. As far as we can tell, nobody public had ever driven this
module from the SP-1 before feldd did. Everything marked "captured on real hardware" below came
from a real capture on a real device and is reproducible with the tooling described here. Where a
fact is a chip capability we have not yet demonstrated, it says so. Do not fill gaps by guessing:
this hardware has a public history of confidently-wrong AI claims, and the whole point of this
document is to be honest and reproducible instead.

Primary sources are Infineon's own documents:

- **CYBT-353027-02 datasheet**, doc 002-23132 (module + silicon)
- **AIROC HCI Firmware Download**, doc 002-32056 (download flow, SS/VS/DS, minidriver)
- **AIROC HCI Control Protocol**, doc 002-16618 (WICED-HCI wire format, opcodes)

---

## 1. Two chips, not one

The SP-1 has **two** microcontrollers relevant to wireless:

1. **The nRF52840** — the SP-1's main application processor. feldd runs here. Critically, on the
   SP-1 the nRF's own radio has **no antenna**, so feldd cannot do Bluetooth on the nRF itself.
   All wireless has to go through the second chip.

2. **The Infineon CYBT-353027-02 module** — a self-contained Bluetooth module with its own
   Cortex-M3, Bluetooth radio, and antenna. In the stock SP-1 this module runs Teenage
   Engineering's factory Bluetooth firmware (a Bluetooth-Classic A2DP audio sink). It is a
   completely separate computer from the nRF, with its own firmware and its own persistent
   storage.

The two chips are wired together over a **UART**. feldd's job, for wireless, is to act as the
**host** that drives the module over that UART: talk to it, and — for BLE — flash a custom
application onto it.

> Terminology used throughout: **"the module"** or **"the module app"** always means firmware
> running on the CYW20706 inside the CYBT-353027-02. **"feldd"** always means the firmware on the
> nRF52840. Do not conflate them.

### The nRF ↔ module link (captured on real hardware)

The link is a standard 4-wire UART with hardware flow control, plus a reset line, all on nRF
GPIOs:

| Signal | Direction | nRF pin |
|--------|-----------|---------|
| UART TX | nRF → module (module RX) | **P1.02** |
| UART RX | module → nRF (nRF RX) | **P1.04** |
| RTS | flow control | **P1.01** |
| CTS | flow control | **P1.03** |
| Reset (`RST_N`, active-low) | nRF → module | **P0.10** |
| SPI CS (`PIN_CY_SPI_CSN`) | optional second control path | **P1.05** |

Default UART framing is **115200 8N1, LSB-first**. The SP-1's nRF I/O is 1.8 V — if you tap these
lines with a logic analyzer, verify the actual Vio with a meter first, because many cheap
analyzers have an input threshold that is marginal at 1.8 V (garbage decodes are usually a
threshold problem, not a baud problem).

The **CTS line is load-bearing for entering download mode** — see §5.

---

## 2. The silicon: CYW20706A2

The module is built on the **Cypress/Infineon AIROC CYW20706, metal revision A2 (CYW20706A2)**.
Establishing this exactly matters, because the wrong part number sends you to the wrong toolchain
files.

- **Cortex-M3** core.
- **848 KB on-chip ROM** — this holds the Bluetooth stack plus the boot/download ROM.
- **352 KB RAM**, base **0x00200000**.
- **No on-chip application flash.** This is the key architectural fact for the download story.

> **It is the CYW20706, not the CYW20819.** The "20819" confusion comes from Infineon's BTSDK
> HOGP-keyboard example, whose *eval-board target* is the CYW920819 — a different chip in other
> modules. Use the **CYW20706** baselib / BSP, minidriver, and `.btp` flash-map files, never the
> 20719 or 20819 example files. Their NV addresses do not apply.

---

## 3. Where persistent state lives: off-chip SPI flash at 0xFF000000

Because the CYW20706 has no on-chip application flash, everything persistent — the app image, its
config, and the factory identity — lives in a **discrete 512 KB SPI serial-flash chip** on the
module (an FM25Q04-class part, on the chip's SPI2). It is memory-mapped at base **0xFF000000**.

This flash holds three logical sections (per doc 002-32056 §2):

| Section | Contents | Lifetime |
|---------|----------|----------|
| **SS — Static Section** | **BD_ADDR (the module's Bluetooth MAC), cryptographic keys, RF calibration** | "Programmed at the factory... intended to remain for the lifetime of the device." |
| **VS — Volatile Section** | Run-time records kept across power cycles | Mutable at runtime |
| **DS — Data Section** | The application code/data/config loaded at normal boot | Replaced when you flash a new app |

On this board's flash map (from the `.btp`), the sections sit at:

- **SS @ offset 0** → mapped **0xFF000000**
- **DS @ offset 0x4000** → mapped **0xFF004000** (the app; a second DS slot follows higher up)

**The Static Section is the thing you must never lose.** Teenage Engineering's SS is not public,
and there is no restore image for it. If a device's SS is erased, its factory BD_ADDR and RF
calibration are gone permanently — the module stays alive but with no valid factory identity.
This single fact drives every safety rule below.

### Captured on real hardware: the boot ROM can read the SS with no minidriver

The Infineon docs imply that reading the off-chip flash requires loading a "minidriver" helper
first (see §6). On this silicon that turned out **not** to be true for reads. Using only the
download ROM's built-in `READ_RAM` command, a read at `0xFF000000` returns real Static-Section
bytes — the module's factory **BD_ADDR** appears at data offset 21 of the first chunk, matching
the address read back over WICED-HCI from the stock app. The read is **deterministic across runs**
(byte-identical every time), which confirms it is genuine flash and not an artifact — an on-chip
RAM control read at the same time varied run-to-run, as RAM should.

Consequence: a **read-only Static-Section backup is feasible with the boot ROM alone**, no
minidriver, no write/launch/erase machinery involved. The whole SS is only about 40 real bytes
followed by `0xFF` padding. (The BD_ADDR bytes are intentionally not reproduced here — dump your
own device's.)

One important handshake detail: the download ROM **re-runs autobaud on every download-mode
entry**, so a single `HCI_RESET` is not always enough — the first one or two can be dropped while
the ROM locks baud. Any download-mode step must **loop `HCI_RESET` (with CTS held low) until the
ack lands.**

---

## 4. WICED-HCI: how you talk to the module

The module speaks **WICED HCI-Control** (Infineon's AIROC HCI control protocol, doc 002-16618) —
a vendor framing layered on the standard H4 HCI transport, not raw Bluetooth HCI. Commands are
grouped (DEVICE, LE, GATT, etc.). feldd carries a small WICED-HCI codec to build and parse these
frames over the nRF↔module UART.

Standard HCI vendor-specific commands (VSCs) share the wire with WICED-HCI control frames. The
download commands in §6 are VSCs. HCI opcodes are little-endian on the wire (opcode `0xFC4C`
transmits as `4C FC`).

**Captured on real hardware:** driving the *stock* module app over WICED-HCI, the DEVICE group is
fully responsive — you can read the module's BD_ADDR from it with no reflash. That is how the
"stock app is Classic-only" result below was established.

---

## 5. Why the stock app can't do BLE — and how you recover

**The headline finding (captured on real hardware):** the module's stock firmware is a
Bluetooth-**Classic** A2DP audio sink. It has a fully working WICED HCI-Control endpoint for the
DEVICE group, but **no LE stack at all**. A passive LE-group WICED-HCI command (`19 01 01 01 00 01`)
gets **zero response**, while a DEVICE-group read answers in the very same session.

So BLE is **not reachable by driving the stock app**. There is no command, no config toggle, no
hidden mode. Getting BLE onto the SP-1 requires **flashing a custom AIROC application onto the
module** that carries an LE stack and the BLE-MIDI / BLE-HID GATT services. That is the entire
reason the download architecture below matters.

**The module is recoverable.** The CYW20706's mask boot ROM always re-enters download mode on a
Recovery Reset (proven on hardware), and it lives in on-chip ROM that no flash operation can
touch. So a bad *app* is always recoverable by re-downloading. The **only** unrecoverable outcome
is erasing the **Static Section**, because TE's SS image is not public. The module has no SWD;
Recovery-Reset-into-the-ROM is the only recovery path, and it recovers a bad app, **not** a wiped
SS.

### Entering download mode: Recovery Reset

Boot into the download ROM via **Recovery Reset** (doc 002-32056 §3.1), which is a hardware strap,
not a command:

1. Hold the module's **CTS LOW** (i.e. assert the host's RTS).
2. Assert **`RST_N` (P0.10) LOW** for ~10 ms, then deassert.
3. Wait ~10 ms.
4. Release **CTS HIGH**.

The ROM then autobauds. Confirm with `HCI_RESET` → `01 03 0C 00` should ack with
`04 0E 04 01 03 0C 00`. **The CTS-low-across-reset-release strap is what selects download mode**
(captured on real hardware — `HCI_RESET` only acks with CTS held low; a plain reset just
relaunches the app). Loop the reset+`HCI_RESET` until the ack lands, per the autobaud finding in
§3.

---

## 6. Upgrade Download vs Full Download: the one safety rule

There are two ways to write the module's flash. **The difference is the whole safety story.**

| | Upgrade Download | Full Download |
|---|---|---|
| What it writes | **DS only** | SS **and** DS |
| Chip erase first? | **No** — erases only the DS sectors it's about to write | **Yes — `CHIP_ERASE` first** |
| SS / VS | **Preserved** (BD_ADDR + keys + RF cal survive) | **Destroyed** |
| Safe on an irreplaceable module? | **Yes** | **No — permanently loses factory identity** |

**The rule for the SP-1 module: always Upgrade, never Full.** (doc 002-32056 §2, §4.1, §4.2.)
Because the minidriver auto-erases *only the sectors it is about to write*, a DS-only write never
touches the SS or VS. A Full Download issues a `CHIP_ERASE` that wipes the whole flash — including
the SS you can never restore.

A stock SP-1 owner never runs any of this directly. The path a maintainer / SWD-equipped developer
uses is the Upgrade Download, and it has been **validated on real hardware**: a fresh app was
flashed byte-perfect over an already-occupied DS sector, and the Static Section survived
byte-identical afterward — proving the erase granularity is the 4 KB sector, not a 64 KB block that
would reach the SS.

### Safety invariant, enforced in the write path

The DS write floor is set **strictly above the end of the VS** (VS ends at `0xFF002000`; the DS
floor is placed above it, and the real app DS is at `0xFF004000`). A DS write therefore can never
reach the VS or the SS at `0xFF000000`. Before any real write, the feldd downloader reads the
module's SS BD_ADDR and **hard-aborts unless it matches the expected device** — an identity gate
so you cannot accidentally write the wrong module. And `CHIP_ERASE` / Full Download is simply never
compiled into the path that touches a keeper device.

> Practice the destructive operations (CHIP_ERASE, SS wipe/restore, first-time minidriver launch)
> on a **disposable CYBT-353027-EVAL board** — a USB Arduino-form eval board whose SS is blank from
> the factory, so there is no factory identity to lose and USB-based recovery is always available.
> Bring only a validated, read-only-or-Upgrade recipe to any real device.

---

## 7. The download machinery: minidriver + opcodes

### The minidriver

The boot ROM can read flash and write RAM, but it lacks the SPI-flash sector management needed to
*program* the off-chip flash. That capability is supplied by a **minidriver**: a small
RAM-resident helper you load and launch, which adds serial-flash sector tracking, chip-erase, baud
change, and CRC verify.

- The minidriver is **CYW20706-specific**. For this module it is
  **`uart_legacy_ramcfg_only.hex`**, which ships inside ModusToolbox's BTSDK (not a standalone
  download). Do **not** substitute the 20719/20819 example minidrivers.
- Load it via `WRITE_RAM` into RAM **0x00220000** (chunks up to 249 bytes, advancing the address),
  then `LAUNCH_RAM 0x00220000`. The load address comes from the hex file's type-05
  Start-Linear-Address record.
- Downloading a minidriver is **announced** first with `DOWNLOAD_MINIDRIVER` (`0xFC2E`), a
  zero-payload "begin download" command that does **not** itself carry any minidriver bytes. The
  bytes come from the separate `WRITE_RAM`/`LAUNCH_RAM` load. (The Firmware-Download doc documents
  the load; the Control-Protocol doc documents the announce — same flow, two altitudes. `0xFC2E` is
  neither an alias for `WRITE_RAM` nor a wrong-chip opcode.)

**Captured on real hardware:** the minidriver protects the Static Section. A DS-style `WRITE_RAM`
aimed at the SS region is **rejected** once the minidriver is running — a second layer of defense
under the "never issue a Full Download" discipline.

### The exact opcodes (wire bytes; HCI opcode is little-endian)

| Command | Opcode | Wire (command → expected ack) | Notes |
|---------|--------|-------------------------------|-------|
| `HCI_RESET` | 0x0C03 | `01 03 0C 00` → `04 0E 04 01 03 0C 00` | Download-mode handshake; loop until acked |
| `UPDATE_BAUDRATE` | 0xFC18 | `01 18 FC 06 00 00 <baud LE32>` | Optional speed-up after entry |
| `DOWNLOAD_MINIDRIVER` | 0xFC2E | `01 2E FC 00` → `04 0E 04 01 2E FC 00` | Announce only — carries no bytes |
| `WRITE_RAM` | 0xFC4C | `01 4C FC <nn> <addr LE32> <data…>` (nn = 4+N) → `04 0E 04 01 4C FC 00` | **Destructive only if addr is flash-mapped 0xFF000000+** |
| `READ_RAM` | 0xFC4D | `01 4D FC 05 <addr LE32> <len8>` (len ≤ 255) → `04 0E <N+4> 01 4D FC 00 <N data>` | **Read-only. Writes nothing, erases nothing.** |
| `LAUNCH_RAM` | 0xFC4E | `01 4E FC 04 <addr LE32>` → `04 0E 04 01 4E FC 00` | Launching RAM code at 0x00220000 is non-destructive |
| `CHIP_ERASE` | 0xFFCE | `01 CE FF 04 <addr LE32>` → `04 0E 04 01 CE FF 00` | **Note second byte `FF`, not `FC`.** `EF EE BE FC` (0xFCBEEEEF) = "erase lowest valid NV range" = wipes SS+VS+DS. **IRREVERSIBLE. Never send to a keeper.** |
| `VerifyCRC` | 0xFCCC | `01 CC FC 08 <addr LE32> <len LE32>` → `04 0E 08 01 CC FC 00 <crc32>` | Minidriver-gated; verifies a known image |
| `SECTOR_ERASE` | — | (named only; no published opcode) | |

Notes grounded in the docs and in hardware:

- `READ_RAM` and `CHIP_ERASE` appear only in the Control-Protocol doc, not the Firmware-Download
  doc. `READ_RAM` is safe at the ROM level with no minidriver (that is how the SS backup in §3 is
  taken); restrict it to verified RAM/ROM/flash addresses and avoid peripheral registers with
  read-to-clear side effects.
- **The `LAUNCH_RAM` completion-reboot address is `0xFFFFFFFF`** (warm reboot into the freshly
  written app), **not** `0x00000000` — captured on hardware, `0x0` jumps to PC=0 and core-dumps,
  while `0xFFFFFFFF` warm-boots cleanly. (The two Infineon docs disagreed on this; hardware
  resolved it.)
- There is **no turnkey "dump flash to file"** command in the AIROC toolchain — ChipLoad only
  writes + CRC-verifies, DetectAndId only reads chip ID, and the RF test tool is unrelated. A flash
  backup is composable **only** from a `READ_RAM` loop over `0xFF000000` (≤ 255 B/chunk). That, plus
  the fact that TE's SS image is not public, is exactly why you favor Upgrade Download so a restore
  is never needed.

---

## 8. Putting it together: the reflash pipeline

The full write path, once the minidriver is in hand, is:

1. **Recovery Reset** (CTS-low strap) → loop `HCI_RESET` until the ROM acks.
2. `DOWNLOAD_MINIDRIVER` (`0xFC2E`) announce.
3. `WRITE_RAM` (`0xFC4C`) the `uart_legacy_ramcfg_only.hex` minidriver into RAM `0x00220000`
   (≤ 249 B/chunk).
4. `LAUNCH_RAM` (`0xFC4E`) `0x00220000` — the minidriver is now running.
5. **Identity gate:** read the SS BD_ADDR; abort unless it matches the intended device.
6. `WRITE_RAM` the custom AIROC **DS** at **0xFF004000** — an **Upgrade** (DS-only, no chip erase,
   SS preserved).
7. `VerifyCRC` (`0xFCCC`) the written image.
8. `LAUNCH_RAM 0xFFFFFFFF` to warm-boot into the new app.

On the SP-1, feldd embeds the built module image as a blob and runs this pipeline over the
nRF↔module UART. A **non-destructive dry-run** (talks to the module, passes the identity gate,
computes the DS chunking, but skips the write) runs first; only after it passes does the armed,
write-enabled build touch the flash.

---

## 9. What this enables (captured on real hardware)

With a custom AIROC BLE app flashed DS-only onto the module (SS preserved), the SP-1 does both:

- **BLE-MIDI** — confirmed to Macs, iPhones, and Teenage Engineering gear: the **OP-XY** (pairs
  from its `COM > devices` menu) and the **TP-7**. BLE is the only way to reach the TP-7, which is
  USB-device-only.
- **BLE-HID keyboard** — confirmed attaching to iOS as a keyboard and typing over BLE. Getting the
  HID (HOGP) profile fully accepted by iOS required getting the GATT database, pairing/encryption,
  and the HOGP-mandatory PnP ID characteristic all correct on the module side; those details live
  in the module-app notes.

In all confirmed cases the SP-1 module acts as a **BLE peripheral** (the OP-XY, phone, or Mac is
the central). The CYW20706 also advertises a **BLE central** capability (`wiced_bt_ble_scan` + a
full GATT client), which would let the SP-1 scan for and connect to advertising peripherals
directly — but that role is a **chip capability we have not yet demonstrated on hardware**, and it
is called out as such rather than claimed.

---

## 10. Building the custom module app

The custom module firmware is derived from Infineon's **`mtb-example-btsdk-ble-hello-sensor`**
BTSDK example, targeting **CYBT-353027-EVAL / CYW20706A2** with `COMPONENT_btstack_v1`, built with
**ModusToolbox** (`make`, not the nRF/Zephyr toolchain).

The example's source files carry Cypress/Infineon's restrictive "all rights reserved /
non-transferable / no redistribution" license header, so **this documentation cannot reproduce
that source code.** To build the app yourself:

1. Install **ModusToolbox 3.x** (GUI installer, requires an Infineon login) and add the
   **CYW20706A2 / CYBT-353027-EVAL BSP** via the Library Manager. This lands ChipLoad, the
   `uart_legacy_ramcfg_only.hex` minidriver, the board `.btp` flash map, and the BTSDK examples on
   disk.
2. Download the `mtb-example-btsdk-ble-hello-sensor` example from Infineon.
3. Apply feldd's modifications, described in prose in the module-app notes rather than as code —
   the substance is: add the **BLE-MIDI** GATT service (per the Apple BLE-MIDI spec — its CCCD
   stays unencrypted, as that spec requires) and a **HID keyboard (HOGP)** service, and get the
   HID service accepted by iOS by (a) declaring every writable characteristic with the correct
   `_WRITABLE` GATT-DB macro so the attribute table stays byte-aligned, (b) using pairing/encryption
   permissions the SP-1's Just-Works pairing can actually satisfy (and enforcing link encryption
   for the keystroke stream at runtime instead), (c) a boot-keyboard **Report Reference of Report
   ID 0**, (d) a GAP **Appearance** of HID Keyboard matching the advertisement, and (e) the
   HOGP-mandatory **PnP ID (0x2A50)** characteristic in the Device Information Service, without which
   iOS bonds and subscribes but never actually attaches the keyboard.
4. Build with the ModusToolbox `make` flow for the CYBT-353027-EVAL target to produce the
   downloadable DS image, then flash it with the Upgrade Download in §8.

> A GATT caching note worth knowing: iOS caches the GATT table per bond, and the boot-keyboard app
> has no Service Changed characteristic (0x2A05), so after any change to the module's GATT database
> you must **Forget the device on iOS and re-pair**, or it will serve the stale table. Adding a
> Service Changed indicate characteristic removes that requirement.
