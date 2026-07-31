# feldd

![feldd, browser-configurable controller firmware for the Teenage Engineering SP-1](docs/feldd-og.png)

**feldd turns the Teenage Engineering SP-1 into a configurable MIDI and keyboard controller. Its 4 faders and 9 buttons become fully remappable over USB and 3.5 mm TRS MIDI, set up entirely from your browser.**

> The SP-1 is an unreleased TE device. This is unofficial community firmware, not affiliated with or endorsed by Teenage Engineering.

> [!CAUTION]
> **Experimental MIDI looper branch:** the looper logic has passed laptop-based
> host tests, but this integration has **not been built as SP-1 firmware, flashed,
> or tested on hardware**. The USB receive path, timing thread, controls, MIDI
> output, recovery behavior, and memory use are therefore unverified on-device.
> Do not present this branch as safe to flash. A firmware build, image inspection,
> and deliberate hardware review are still pending; flashing experimental
> firmware can leave the device malfunctioning or require bootloader recovery.

**Latest firmware: v0.27.3 (stable)** (get it at [feldd.com](https://feldd.com)). Beta in soak: **v0.28.0-beta** &mdash; MIDI thru (forward USB-in MIDI out the 3.5mm TRS jack, a USB-to-TRS bridge), on the `?beta=1` link.

**New in 0.27:** Bluetooth. feldd reprograms the SP-1's built-in radio, in place from the browser, into a wireless BLE-MIDI controller and a Bluetooth keyboard, turn it on with •• + PLAY and pair with a phone, laptop, or iPad, no cable. (0.24 added MIDI clock; PLAY is a normal mappable button.)

## What it does

- 🎛️ **4 faders + 9 buttons → MIDI**, map any control to any CC or note, each on its own channel; a button can also send a value you pick (set-on-press, momentary, or toggle)
- ⌨️ **Keyboard mode**, buttons can type keys and shortcuts (Cmd+C, Cmd+Z) so it drives any app, not just music software
- 🎵 **Chords + note-sets on any button**, build a chord per button as a hand-picked set, a root and quality, or a range for an M8 cluster, one press fires the whole set
- 🪜 **8 layers per profile**, hold PLAY to shift to a layer you pick (the side light follows), or flip PLAY to a normal mappable button
- 💾 **8 profiles per mode on the device**, step them live with •• + Vol, no computer needed
- 🎚️ **•• is the function button**, hold it and tap a control to step layers or profiles, flip MIDI/Keyboard mode, or reach the utility row (battery, LED brightness, MIDI panic); hold •• to peek your profile
- 🏷️ **Name your controls**, per-control labels save with your .feldd file so a shared profile explains itself
- 🎹 **USB-MIDI 1.0 + TRS (3.5 mm) MIDI** out, drives your DAW or your hardware
- 📶 **Wireless Bluetooth (BLE-MIDI + keyboard)**, feldd reprograms the SP-1's onboard radio from the browser so it pairs with a phone, laptop, or iPad with no cable (0.27+)
- 🔌 **Configure live in the browser**, read, edit, write, and watch your controls move in real time over USB

## Use it, all in the browser, no app to install

### → **[feldd.com](https://feldd.com)**

- **[Configure your SP-1](https://feldd.com/sp-1/configure)**, map the faders + buttons, manage your 8 profiles, and watch them fire live. *(Chromium browser, uses WebSerial.)*
- **[Flash the firmware](https://feldd.com/sp-1/flash)**, a guided walkthrough using the community **Solderless** tool.

Flash once, then configure and reconfigure from the browser whenever you like.

## What's not in this repo

feldd's **Bluetooth radio firmware is deliberately not open-sourced here.** Getting the SP-1's
onboard radio (a Cypress/Infineon CYW20706) to speak Bluetooth means loading a compiled radio image
onto it, and that image plus the vendor's download minidriver are **not ours to redistribute as
source.** So the following are intentionally omitted from this tree:

- `firmware/app/src/cybt_blobs.h` — the CYW20706 payloads (the vendor download minidriver + our
  compiled BLE-MIDI app image for the radio). Vendor-derived binaries, not our source to publish.
- `firmware/app/src/bt_provision.*`, `bt_download.*`, `bt_probe.*` and the rest of the radio-flashing
  subsystem, which embeds and depends on that image.
- The Bluetooth build overlays and their host tests.

The controller firmware in this repo builds and runs completely without any of that. The Bluetooth
feature itself is **fully written up** in [`bluetooth/`](bluetooth) (architecture, how the reflash
works, findings), and the **flashable Bluetooth build** — the one that includes the radio image — is
the public download at [feldd.com](https://feldd.com). A few build files (`CMakeLists.txt`, `Kconfig`,
`config_cdc.c`) still carry the conditional hooks that reference the omitted sources; they compile out
by default.

## 🤖 Claude Code console (feldd-cc)

Turn the SP-1 into a physical console for **Claude Code**: the track lights show when an agent is
working / needs you / done, and the buttons drive the session (Play = approve, rocker = scroll). It's
a small local daemon plus Claude Code hooks, talking to feldd's `led` verb (shipped in 0.16.0-beta)
and its button monitor stream over USB. Setup is itself a Claude Code task — point your agent at
[`feldd-cc/`](feldd-cc) and have it follow [`feldd-cc/SETUP.md`](feldd-cc/SETUP.md). The led verb +
monitor stream are hardware-validated.

## ⚠️ Flashing safety

You're modifying an irreplaceable device. feldd links above the TE bootloader (`0x20000`) and keeps the Track 1 + 4 DFU escape hatch plus a charge-standby gate, so a bad flash is recoverable by re-flashing the known-good `sp1_looper.bin`. Read the warnings on the [flash page](https://feldd.com/sp-1/flash) before you start.

---

## Building from source

> Most people never need this, just use **[feldd.com](https://feldd.com)**. This section is for developers who want to build or fork the firmware.

feldd is a Zephyr / nRF Connect SDK app for the nRF52840 inside the SP-1.

- Set up the toolchain + west workspace: `scripts/setup-zephyr-ws.sh`
- Get the **`sp1` board definition** from the **marisko** project and point `BOARD_ROOT` (in `scripts/fw.sh`) at it
- Build: `scripts/fw.sh build` · flashable image: `scripts/fw.sh bin` · vector-table/size check: `scripts/fw.sh info`
- `scripts/sp1ctl.py`, host CLI for the JSON-lines config protocol; `firmware/test/`, pure-logic tests (`cd firmware/test && make`)
- Protocol + packed profile format: `scripts/sp1-profile-format.md`

## Credits

- **[Solderless](https://solderless.engineering)**, the flash + stem-loader tools that make SP-1 hacking possible
- **[Tim Knapen / SP-1-dev](https://github.com/timknapen/SP-1-dev)**, SP-1 developer docs + wiki
- **marisko**, the `sp1` Zephyr board definition
- **Eric Lewis / sp1-midi**, gold-standard nRF52 BSP + boot-safety reference

## License

MIT, see [`LICENSE`](LICENSE). Dependencies (Zephyr, the board definition, etc.) under their own licenses.
