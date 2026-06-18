# feldd

**Custom controller firmware for the Teenage Engineering SP-1 stem player.**

feldd turns the SP-1's 4 faders and 9 buttons into a fully configurable USB-MIDI + TRS-MIDI controller with 8 on-device profiles — all mapped and managed from your browser, no app to install.

**Configure it → [feldd.com/sp-1/configure](https://feldd.com/sp-1/configure)** (Chromium / WebSerial)

## What it does

- 🎛️ **4 faders + 9 buttons → MIDI** (CC / notes), fully remappable
- 💾 **8 profiles** stored on-device in NVS; switch banks live with the •• button
- 🎹 **USB-MIDI 2.0** *and* **TRS (3.5 mm) MIDI** out
- 🔌 **Live, bidirectional config** over USB-CDC — read, edit, write, and monitor the hardware in real time from the web configurator
- ⚡ Boot-safe: links above the TE bootloader, keeps the DFU escape hatch + a charge-standby gate

> The SP-1 is an unreleased TE device. **This is unofficial community firmware — not affiliated with or endorsed by Teenage Engineering.**

## Flashing

Flash with the community **[Solderless](https://solderless.engineering)** tool. Step-by-step walkthrough: **[feldd.com/sp-1/flash](https://feldd.com/sp-1/flash)**.

> ⚠️ **Flash at your own risk.** You're modifying an irreplaceable device. feldd links at `0x20000` (above the TE bootloader) and keeps the **Track 1 + 4 DFU escape hatch** plus a charge-standby gate, so a bad flash is recoverable by re-flashing the known-good `sp1_looper.bin` — but read the warnings first.

## Building

feldd is a **Zephyr / nRF Connect SDK** application for the nRF52840 inside the SP-1.

1. Set up the NCS toolchain + west workspace — see `scripts/setup-zephyr-ws.sh`.
2. You need the **`sp1` board definition** from the **marisko** SP-1 firmware project; feldd builds with `-DBOARD_ROOT=<path-to-marisko>`. Point `BOARD_ROOT` in `scripts/fw.sh` at it.
3. Build:
   - `scripts/fw.sh build` — compile
   - `scripts/fw.sh bin` — produce the flashable image (stripped to the `0x20000` app region)
   - `scripts/fw.sh info` — vector-table VMA + size sanity check

### Host tools & tests

- `scripts/sp1ctl.py` — a host-side CLI that speaks feldd's JSON-lines config protocol over the USB-CDC port (`list` / `read` / `write` / `setactive` / `monset`). Handy for scripting and testing without the web UI.
- `firmware/test/` — pure-logic host tests (control mapping, profile codec, protocol, gestures): `cd firmware/test && make`.

## Config protocol & profile format

The web configurator and `sp1ctl.py` talk to the device over a small newline-framed JSON protocol on USB-CDC (`list`, `read`, `write`, `setactive`, `monset`, …). The packed on-device profile format (69 bytes, base64 on the wire) is documented in **`scripts/sp1-profile-format.md`**.

## Credits

Standing on the shoulders of the SP-1 hacking community:

- **[Solderless](https://solderless.engineering)** — the firmware-flash + stem-loader web tools that make any of this possible
- **[Tim Knapen / SP-1-dev](https://github.com/timknapen/SP-1-dev)** — the SP-1 developer documentation + wiki
- **marisko** — the `sp1` Zephyr board definition feldd builds against
- **Eric Lewis / sp1-midi** — gold-standard nRF52 BSP + boot-safety reference

## License

MIT — see [`LICENSE`](LICENSE). The firmware in this repo is MIT; its dependencies (Zephyr, the board definition, etc.) are under their own licenses.
