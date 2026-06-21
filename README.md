# feldd

![feldd, browser-configurable controller firmware for the Teenage Engineering SP-1](docs/feldd-og.png)

**feldd turns the Teenage Engineering SP-1 into a configurable MIDI and keyboard controller. Its 4 faders and 9 buttons become fully remappable over USB and 3.5 mm TRS MIDI, set up entirely from your browser.**

> The SP-1 is an unreleased TE device. This is unofficial community firmware, not affiliated with or endorsed by Teenage Engineering.

**Latest firmware: v0.13.0 (stable), v0.14.0 (beta)** (get it at [feldd.com](https://feldd.com)).

## What it does

- 🎛️ **4 faders + 9 buttons → MIDI**, map any control to any CC or note, each on its own channel
- ⌨️ **Keyboard mode**, buttons can type keys and shortcuts (Cmd+C, Cmd+Z) so it drives any app, not just music software
- 🎵 **Chords + note-sets on any button**, build a chord per button as a hand-picked set, a root and quality, or a range for an M8 cluster, one press fires the whole set
- 🪜 **4 layers per profile**, a hold-PLAY shift stack, so every profile is really four, switched on-device
- 💾 **8 profiles per mode on the device**, switch them live with the •• count-dial, no computer needed
- 💡 **On-device controls + lights**, the side lights show your layer and mode, hold PLAY to peek your profile
- 🎹 **USB-MIDI 1.0 + TRS (3.5 mm) MIDI** out, drives your DAW or your hardware
- 🔌 **Configure live in the browser**, read, edit, write, and watch your controls move in real time over USB

## Use it, all in the browser, no app to install

### → **[feldd.com](https://feldd.com)**

- **[Configure your SP-1](https://feldd.com/sp-1/configure)**, map the faders + buttons, manage your 8 profiles, and watch them fire live. *(Chromium browser, uses WebSerial.)*
- **[Flash the firmware](https://feldd.com/sp-1/flash)**, a guided walkthrough using the community **Solderless** tool.

Flash once, then configure and reconfigure from the browser whenever you like.

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
