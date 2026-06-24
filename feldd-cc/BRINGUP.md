# BRINGUP.md — first plug-in of the cockpit on a real SP-1

The cockpit is **daemon-side only, no firmware change.** It uses what already ships in
feldd **0.16.0-beta**: the `led` verb (LEDs out) and the monitor stream (faders + button
press/release in). Host tests stub the tmux + serial I/O, so a handful of things can only
be confirmed on the device. Run these in order; each lists what to watch and the likely
cause if it fails.

## Prereqs
- SP-1 on **feldd 0.16.0-beta+**, in **MIDI mode** (not Keyboard), plugged in over USB.
  No new firmware , if a unit is older, flash 0.16.0-beta from the guide (`?beta=1`).
  Sanity: `python3 bench_led.py` should walk all 8 LEDs.
- `feldd_cc.config.json` has `"sessions": {"mode": "cockpit"}`, `input.backend: "tmux"`.
- Daemon running: `python3 feldd_cc.py` , log shows `opened SP-1 at ...`, `hook server
  on ...`, and a `monset` line. Watch it live with `tmux attach -t feldd-cc`.

## 1. Lights out (already hardware-validated, sanity only)
Fire a fake session and watch the front row:
```bash
H=http://localhost:9200/hook; S='{"session_id":"demo","cwd":"/tmp"'
curl -s -X POST $H -d "$S,\"hook_event_name\":\"UserPromptSubmit\"}"; sleep 4   # SOLID
curl -s -X POST $H -d "$S,\"hook_event_name\":\"Notification\"}";     sleep 5   # BLINK
curl -s -X POST $H -d "$S,\"hook_event_name\":\"Stop\"}";             sleep 3   # FLASH -> off
```
Expect solid → blink → flash. (This path is the `led` verb, already validated.)

## 2. Buttons + faders flow IN (the main unverified path)
With the daemon log attached, press **each Track button, Vol+, Vol-, the rocker (FWD/RWD)**,
and **move each fader**. Expect one log line per event (`select led N`, `next-needs`, a
fader handler firing).
- **Nothing logs at all** → the monitor input stream isn't reaching the daemon:
  - device not in **MIDI mode**;
  - the stream is **DTR-gated** , the firmware only emits when DTR is asserted (pyserial
    asserts it by default, but if your OS/pyserial opened with DTR low the stream is
    silent); try `ser.dtr=True` after open;
  - **`g_mon` not armed** , the daemon sends `{"t":"monset","on":true}` once on open; if
    you reconnected the SP-1 without restarting the daemon, just restart it.
- Falsified (it's NOT the daemon logic) if buttons/faders never log despite a good link.

## 3. Jump (Track = switch your tmux client)
Have ~3 named `claude` sessions attached in one terminal. Press **Track 2** → your
terminal should switch to that session and the correct pane be active.
- **Focus doesn't move / wrong client** → `tmux switch-client` targets the client tied to
  the daemon's tmux invocation. If you're attached from a *different* client (e.g. iPhone
  via Tailscale **and** a desktop client), it can target the wrong one. Fix: run the daemon
  in the same tmux server you're attached to, or change `tmux_focus` to a single atomic
  `switch-client -t name:win.pane`. Falsified if the foreground never changes or the wrong
  pane is selected.

## 4. Scroll (fader 1 = scrollback position)
In a `claude` pane with lots of scrollback, sweep **fader 1** from 0 → 127. Expect 0 = live
bottom (copy-mode cancelled), 127 = oldest line at the top, mid-throw ≈ proportional.
- **Off by ~one screen / inverted** → `#{history_size}` excludes the visible screen, so the
  math may need `+ #{pane_height}` or a switch to absolute `send-keys -X goto-line <N>`.
  Falsified if copy-mode won't engage in the claude TUI or it lands at the wrong place.

## 5. The MRU front row
Open more than 4 sessions. Confirm the **4 you've touched most recently** hold the front
(Track) LEDs, and that **scrubbing (fader 2) or Vol+ to a bench/off-board session seats it
on a front button** (the least-recently-used front session steps back to the bench). A
session that goes **needs-you** should auto-claim a front button.

## 6. Autopilot (only if you run dangerous mode)
Set `permission.enabled:false`, `autopilot.enabled:true`, `faders.assign.preset:"autopilot"`,
`input.backend:"tmux"`. Arm a *stopped* session by dialing **fader 4** up. Confirm: its LED
**slow-pulses**; after the cadence it sends `continue` to that exact pane; a real question
(needs-you) **pauses** it; it **parks** (LED hard-alert) after `deadman` hands-off continues;
pulling fader 4 to 0 disarms. If it ever types into the wrong pane, stop and check the
session actually has a captured `$TMUX_PANE` (pane-less sessions disarm by design).

## Gotcha
A **doubled Enter** on every Play press = the SP-1 is in **Keyboard mode**; switch to MIDI.
