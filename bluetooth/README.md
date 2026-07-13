# feldd Bluetooth: wireless BLE-MIDI and BLE-HID on the Teenage Engineering SP-1

An unofficial, community write-up of how [feldd](https://feldd.com) (the SP-1's custom nRF52840
firmware) drives the SP-1's onboard Bluetooth module to do **wireless BLE-MIDI** and a **wireless
BLE-HID keyboard**. Honest, reproducible, and documented so the next person can rebuild every
result on real hardware.

To our knowledge nobody outside this project had driven the SP-1's Bluetooth module before. feldd
is the first. Every hardware claim in these docs was captured on a real device and is reproducible
with the tooling described. Where something wasn't verified on hardware, we say so.

## What works

Confirmed on real hardware, both features running through feldd:

- **BLE-MIDI** — the SP-1 advertises as a wireless MIDI device. Verified receiving live MIDI on
  Macs, iPhones, and Teenage Engineering gear: the **OP-XY** (which pairs the SP-1 straight from
  its `COM > devices` menu) and the **TP-7**. Faders and notes go over the air, no cable.
- **BLE-HID keyboard** — an iPhone attaches the SP-1 as a Bluetooth keyboard and receives keystrokes
  over BLE.

## The two-chip architecture (the key idea)

The SP-1 has **two** processors that matter here:

1. The **nRF52840** runs feldd. This is the brain, but it has **no Bluetooth antenna** wired up, so
   it cannot do wireless on its own radio.
2. The **Infineon CYBT-353027-02** module (Cypress/Infineon **CYW20706A2** silicon: its own
   Cortex-M3, Bluetooth radio, and antenna). This is the SP-1's second chip and the only path to the
   air.

So all wireless goes through the module, and feldd's job is to **drive and program that module** over
the wired link between the two chips. In the stock SP-1, the module ships an Infineon app that only
speaks **Bluetooth Classic (A2DP)** — it has no BLE stack at all. That is the whole reason this
project exists: to get BLE onto the SP-1 you have to replace the module's app with a custom
**AIROC** app, and then feldd advertises BLE-MIDI / BLE-HID through it.

## The one safety headline (read this before anything else)

**Never full-erase / chip-erase the Bluetooth module. Use an "Upgrade Download" only.**

The module stores its identity and radio calibration in a **Static Section (SS)**: the module's
Bluetooth address (BD_ADDR), bonding keys, and RF calibration. There is **no public restore image**
for a stock SP-1 module, so if the SS is wiped it is **unrecoverable** — you would have a module that
can no longer be calibrated back to spec.

- A **Full / CHIP_ERASE** wipes everything, including the SS. Do not do it. Ever.
- An **Upgrade Download** writes only the Downloadable Section (the app) and preserves the SS.
  This is the only write path this project uses on real hardware.
- The module is otherwise **recoverable**: a Recovery-Reset boot ROM (mask ROM) always re-enters
  download mode, so a bad *app* can be re-flashed. It's the SS that can't be brought back.

This was validated on a disposable eval board first: the custom app was flashed byte-perfect over an
occupied sector while the SS survived byte-identical, proving the erase granularity stays away from
the SS. See `findings.md`.

## Who this is for

This spans three very different readers. Find yourself first and read only the depth that applies —
a stock owner does not need (and should not run) the reflash internals.

- **Stock SP-1 owner (most people).** You have an SP-1 and no debugger. You get wireless BLE-MIDI /
  BLE-HID by flashing a future feldd build that carries the feature — feldd drives and programs the
  module for you, on-device, through the normal feldd flash. What's useful for you here is the model
  (why there are two chips, why BLE needs a custom module app) and the results. **Read:** this page
  and `hardware-and-architecture.md`. You can stop there.
- **SWD-equipped / burner-unit owner.** You have an SP-1 you can debug and are willing to experiment
  on. You can run feldd's on-device module-download path and reproduce the reflash yourself. **Read:**
  the above plus `reflashing-the-module.md` and `findings.md`, and treat the safety headline as
  non-negotiable. Never blind-flash a unit you care about.
- **Eval-board owner.** You have an Infineon **CYBT-353027-EVAL** board. This is the safe place to
  rehearse the whole write path (download-mode entry, the SS-preserving Upgrade, the build) on a
  disposable target before it touches any SP-1. **Read:** everything, plus `module-app/` for the
  app itself.

## How the custom module app is built (and why we don't ship the source)

The custom app that runs on the module is **derived from Infineon's
`mtb-example-btsdk-ble-hello-sensor`** example, built with Infineon's ModusToolbox BTSDK. That
example's source carries Cypress/Infineon's restrictive license (all rights reserved,
non-transferable, no redistribution), so **we do not reproduce their source here.** Instead:

1. Download `mtb-example-btsdk-ble-hello-sensor` from Infineon and set up ModusToolbox / the BTSDK.
2. Apply **our changes**, which are described in prose in `module-app/` — the BLE-MIDI service and
   UUIDs, an unencrypted CCCD so a host can subscribe without bonding (per Apple's BLE-MIDI spec),
   removing the fixed-address override so the app advertises with the module's real SS BD_ADDR, the
   advertised device name, and (for HID) the HOGP-required characteristics that make iOS attach a
   keyboard.
3. Build for the **v1 BT stack** (the CYW20706 is a v1-stack part), and flash via the SS-preserving
   Upgrade Download.

`module-app/` documents each edit so you can recreate it against your own copy of the example.

## Docs in this set

| File | Covers |
|------|--------|
| `README.md` | This overview — mission, two-chip architecture, what works, who it's for, the safety headline |
| `hardware-and-architecture.md` | The CYBT-353027-02 / CYW20706A2, how it wires to the nRF52840, what the module does in a stock SP-1, and why BLE needs a custom app |
| `reflashing-the-module.md` | The reproducible, SS-preserving reflash method: download-mode entry, the Upgrade-Download write path, and the safety rules |
| `findings.md` | The evidence trail — every hardware finding, captured on real hardware, so results can be reproduced and trusted |
| `module-app/` | The custom AIROC app: our changes described in prose, and how to apply them to Infineon's example |

## A note on accuracy

This is reverse-engineering, so precision matters. The docs cite the phase or capture behind each
claim, never invent bytes / opcodes / pins / timings, and keep the two chips distinct: **feldd runs
on the nRF52840; the stock or custom app runs on the CYW20706 inside the module.** "The app" always
means the module's app, never feldd. If a value wasn't observed on hardware or found in a source,
the docs say "unknown / needs verification" rather than guess.
