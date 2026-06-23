# USAGE.md — running the feldd-cc console day to day

Setup is in [`WIZARD.md`](WIZARD.md) (the guided agent path) or [`SETUP.md`](SETUP.md)
(by hand). This is how you *use* it once it's running, and how to make a multi-agent
setup legible.

## tmux is required (and that's fine)
The console sends keystrokes to your Claude session with `tmux send-keys`, so your
`claude` sessions **must run inside tmux**. If you already run everything in tmux,
you're set , nothing to change.

How a button finds the right pane: the daemon sends to the tmux pane whose **working
directory matches the active session's cwd** and that is running `claude`. So:
- Sessions in **different directories** route cleanly.
- Two sessions in the **same directory** are ambiguous , it'll pick one. Give
  concurrent agents distinct dirs (or use pins, below) if you hit this.

## The daemon
One background process bridges everything. Start it in its own tmux window so it
persists:
```bash
tmux new-window -n feldd-cc 'cd <this folder> && python3 feldd_cc.py'
```
- **Watch it:** `tmux attach -t feldd-cc` (it logs every hook + button; `Ctrl-b d` to detach)
- **Stop it:** `tmux kill-window -t feldd-cc` , the hooks then just fast-fail (harmless)
- It **releases the LEDs** back to feldd's normal display whenever no sessions are active.

## How the lights map to sessions

### Single mode (`sessions.mode: "single"`)
One LED (or the whole front row, if `lights.whole_row: true`) shows your current
session: **solid** = working, **blink** = needs you, **flash** = done.

### Multi mode (`sessions.mode: "multi"`)
Up to four sessions, **one Track LED each**:
- **Auto-assign by arrival:** the 1st session to fire an event takes **Track 1's
  LED**, the 2nd takes **Track 2's**, and so on.
- **Pin a project (recommended for multi):** make the mapping stable by pinning a
  project path to a fixed LED in `feldd_cc.config.json`:
  ```json
  "sessions": {
    "mode": "multi",
    "assign": { "~/bnjmn/iamkeen": 0, "~/bnjmn/feldd-sp-1": 1 }
  }
  ```
  A session whose cwd **is, or is under,** an assigned path always lands on that LED
  (Track 1 = LED 0). Everyone else auto-fills the remaining LEDs. Pinned LEDs are
  reserved, so an unpinned session never steals your main repo's light.
  **After editing the config, restart the daemon** (`tmux kill-window -t feldd-cc`
  then start it again) so it reloads.
- **Which LED is which?** Trigger activity in a session and watch which LED goes
  solid, or `tmux attach -t feldd-cc` and read the `select led N -> <session>` lines.

## Selecting + the buttons
- **Track 1-4** select which session the buttons drive (Track 2 = the session on LED
  2). A session that hits **needs you** auto-grabs focus, so the common loop is:
  a light blinks → press **Play** to answer it.
- Default button map (all remappable in `feldd_cc.config.json`):

  | Button | Does |
  |---|---|
  | **Play** | Enter (approve / submit) to the active pane |
  | **Vol-** | Esc (interrupt) |
  | **Rocker FWD / RWD** | scroll (PageDown / PageUp) |
  | **Track 1-4** | select the active session (multi mode) |

  Each button can be a key list (`["Enter"]`, tmux key syntax), a shell command
  (`{"shell": "say done"}`), or `null` (nothing).

## Customizing (`feldd_cc.config.json`)
Edit the file next to `feldd_cc.py`; omit any key to keep its default. Full schema:
[`feldd_cc.config.example.json`](feldd_cc.config.example.json). Axes: `buttons`,
`lights` (`events` map, `blink_hz`, `done_hold_s`, `whole_row`), `sessions` (`mode`,
`assign`). **Restart the daemon after editing.**

## Troubleshooting
- **A second Enter per Play press** , the SP-1 is in **Keyboard mode**; switch it to
  **MIDI mode** (its own keystrokes are doubling the daemon's).
- **Buttons reach the wrong pane** , select the session with **Track 1-4**, or give
  concurrent sessions distinct directories / pins.
- **A light won't turn off** , a steady side LED while charging is the charger's own
  LED, not one feldd controls.
- **Hook events look wrong** , Claude Code's event names can change across versions;
  `capture-hooks.sh` logs real payloads so you can reconcile `lights.events`.
- **Undo everything** , `tmux kill-window -t feldd-cc`, restore your settings backup
  (`cp ~/.claude/settings.json.feldd-cc.bak ~/.claude/settings.json`), delete
  `feldd_cc.config.json`.
