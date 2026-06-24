# feldd-cc setup runbook (manual path)

> **Most people should use the wizard instead:** open Claude Code here and say *"Read WIZARD.md and
> set up my feldd-cc console."* It does all of the below, in order, with the questions asked for you.
> This runbook is the do-it-by-hand alternative.

This is written to be run **by a Claude Code agent**. Point your Claude at this folder ("set up the
feldd-cc console using SETUP.md") and it can do every step. A human can follow it too. **Order matters:
config and hooks come before the daemon** , the daemon is useless until Claude Code's hooks are POSTing
to it.

## 0. Prerequisites
- An **SP-1 running feldd 0.16.0-beta or newer** (that firmware has the `led` verb). Flash the ready
  image from **[feldd.com/sp-1/guide?beta=1](https://feldd.com/sp-1/guide?beta=1)** onto an
  SWD-recoverable unit (never blind-flash a stock SP-1). No device yet? You can still run the daemon
  with `--dry-run` to see the logic.
- The SP-1 in **MIDI mode** (not Keyboard mode), plugged in over USB-C.
- **Python 3** and **tmux**. Claude Code installed and run inside a tmux pane (e.g. the `cs` flow).

## 1. Install the dependency
```bash
pip3 install --break-system-packages pyserial
```

## 2. Confirm the device + firmware (optional but recommended)
```bash
python3 bench_led.py    # finds the SP-1 config port, walks the 8 LEDs, then releases
```
You should see each track + side LED light in turn. If `bench_led.py` can't find a port, the SP-1
isn't plugged in / not in MIDI mode / pyserial isn't installed. (Skip this step in `--dry-run`.)

## 3. Choose your config (optional)
With **no config file** the daemon uses its defaults (multi mode, Play=Enter, tmux backend). To
customize the session model (`single` / `multi` / `cockpit`), pins, faders, or autopilot, write
`feldd_cc.config.json` next to `feldd_cc.py`; omit any key to keep its default. See
`feldd_cc.config.example.json` for the **schema** (it is a reference, not a starter , don't copy it
in wholesale). If a `feldd_cc.config.json` is already there from a prior run, back it up first
(`cp feldd_cc.config.json feldd_cc.config.json.bak`) rather than building on it blindly.

## 4. Merge the hooks into Claude Code settings (do this BEFORE the daemon)
The daemon only does something once Claude Code's hooks POST events to it, so wire them first. Merge
the `hooks` block from `hooks.settings.json` into `~/.claude/settings.json`.

- If `~/.claude/settings.json` has no `hooks` key: copy this file's `hooks` object in wholesale.
- If it already has `hooks`: deep-merge — append our hook entries to each matching event array; do
  not clobber the user's existing hooks.
- These are HTTP hooks that POST each event to `http://localhost:9200/hook`; they do nothing harmful
  if the daemon is down (the POST just fails silently).

An agent should read both JSON files, merge, and write `~/.claude/settings.json` back (valid JSON).

## 5. Run the daemon
```bash
python3 feldd_cc.py            # auto-detects the SP-1 CDC port (/dev/cu.usbmodem*)
# or pin the port:  FELDD_PORT=/dev/cu.usbmodemXXXX python3 feldd_cc.py
# or no hardware:   python3 feldd_cc.py --dry-run
```
It serves the hook endpoint on `http://localhost:9200/hook` and holds the serial link. Leave it
running (its own tmux pane or a background process). Restart a `claude` session afterward so the
hooks from step 4 load.

## 6. Verify the loop
1. Start (or restart) a `claude` session in a tmux pane so the new hooks load.
2. Send it a prompt → the SP-1 track LED should go **solid** (working).
3. Let it ask for permission / finish → the LED should **blink** (needs you) / **flash** (done).
4. Press **Play** on the SP-1 → an Enter lands in the Claude pane.

If LEDs move but buttons don't reach Claude: the daemon couldn't find the tmux pane running `claude`
(check `tmux list-panes -a`); if buttons reach the wrong pane, select the session with **Track 1-4**.

## Troubleshooting
- **No port found:** SP-1 unplugged, not in MIDI mode, or pyserial missing. `--dry-run` to proceed
  without hardware.
- **A second `Enter` per press:** the SP-1 is in **Keyboard mode** — switch it to MIDI mode (the
  device's own keystrokes are doubling the daemon's).
- **Schema drift:** Claude Code hook payloads can change. Run `capture-hooks.sh` (logs every real
  payload) and reconcile the hooks→state map in `feldd_cc.py` before trusting it.
- **Recover a bad firmware flash:** Track 1 + 4 while plugging in USB → bootloader → reflash a
  known-good image (e.g. `feldd-0.15.0.bin`) from feldd.com.
