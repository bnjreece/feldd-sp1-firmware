# USAGE.md — running the feldd-cc console day to day

Setup is in [`WIZARD.md`](WIZARD.md) (the guided agent path) or [`SETUP.md`](SETUP.md)
(by hand). This is how you *use* it once it's running, and how to make a multi-agent
setup legible.

## Lights work everywhere; buttons have a backend
The **lights** (state out) need no tmux at all , every Claude Code user gets them.
The **buttons** (input) need a way to type into your session, chosen by
`input.backend` in `feldd_cc.config.json`:

- **`tmux`** (default) , `tmux send-keys` to the session's **exact pane**. Precise,
  multi-session, no permissions. Each hook carries its pane (`$TMUX_PANE`), so it
  targets the right session even if all your sessions share a working directory (e.g.
  launched from home in named tmux sessions). **Requires your `claude` to run in tmux.**
- **`osascript`** , macOS System Events types into the **focused** window. No tmux
  needed, but it needs the **Accessibility permission** (System Settings → Privacy &
  Security → Accessibility) and is focus-based , keys go to whatever terminal is
  frontmost, so it can't target a specific session.
- **`none`** , lights only, no buttons. Works anywhere.

Linux note: `osascript` is macOS-only; on Linux use `tmux` (or `none`).

## The daemon
One background process bridges everything. Start it in its own tmux window so it
persists:
```bash
tmux new-window -n feldd-cc 'cd <this folder> && python3 feldd_cc.py'
```
- **Watch it:** `tmux attach -t feldd-cc` (it logs every hook + button; `Ctrl-b d` to detach)
- **Stop it:** `tmux kill-window -t feldd-cc` , the hooks then just fast-fail (harmless)
- It **releases the LEDs** back to feldd's normal display whenever no sessions are active.

## Wireless (Bluetooth, macOS)
By default the daemon talks to the SP-1 over **USB**. To run it **wireless** on macOS,
add `--ble`:
```bash
python3 feldd_cc.py --ble
```
One-time setup:
1. Flash **feldd 0.30.0+** and run **Provision Bluetooth** (feldd.com configurator).
2. **Pair the SP-1 in System Settings → Bluetooth** (it becomes a "My Device").
3. Put the SP-1 in **MIDI mode** (`••` + Track 4) so button presses don't type into the Mac.

Then `--ble` **attaches to the device macOS already has** (via CoreBluetooth
`retrieveConnectedPeripherals`) and drives the console over that same link — coexisting
with the OS's keyboard/MIDI use, no separate mode, auto-reconnecting on a drop. (A device
macOS is holding stops advertising, so the app attaches to the existing connection rather
than scanning — which is why plain BLE scanners can't find a paired SP-1.)

## How the lights map to sessions

### Single mode (`sessions.mode: "single"`)
One LED (or the whole front row, if `lights.whole_row: true`) shows your current
session: **solid** = working, **blink** = needs you, **flash** = done.

### Multi mode (`sessions.mode: "multi"`)
Up to four sessions, **one Track LED each**:
- **Auto-assign by arrival:** the 1st session to fire an event takes **Track 1's
  LED**, the 2nd takes **Track 2's**, and so on.
- **Pin a session (recommended for multi):** make the mapping stable in
  `feldd_cc.config.json`. A pin key matches **either a tmux session NAME** (exact) or
  a **project PATH** (cwd is/under it) , pick whichever fits how you run claude:
  ```json
  "sessions": {
    "mode": "multi",
    "assign": { "web": 0, "api": 1, "~/code/notes": 2 }
  }
  ```
  - **By tmux session name** (`"web": 0`) , best if you launch all your sessions from
    one directory (e.g. home) in named tmux sessions. Track 1 = your `web` session,
    always.
  - **By project path** (`"~/code/notes": 2`) , best if each session runs in its own
    project dir. A cwd that is/under the path lands on that LED.

  Pinned LEDs are reserved, so an unpinned session never steals your main repo's
  light. **After editing the config, restart the daemon** (`tmux kill-window -t
  feldd-cc`, then start it again) so it reloads.
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

## Approving permissions from the SP-1 (`permission`)
When Claude Code **would prompt you** to allow a tool call, the daemon can hold that
prompt open and let you answer it from the controller: the session's LED **blinks**,
**Play = allow**, **Vol- = deny**. The same two buttons you already use, now answering
the real permission dialog.

- It only fires when **your own permission setup** would have prompted anyway , it
  reads whatever rules you run with. If you live in `--dangerously-skip-permissions`
  it never fires; if you allow all `Bash` but prompt on writes, only the writes blink.
  There is **no imposed gate list**.
- It never fires under `claude -p` (non-interactive), and after `permission.timeout_s`
  (default 90s) with no press it **falls through to the normal on-screen prompt** , so
  walking away never blocks a turn.
- This needs the **blocking `PermissionRequest` hook** from `hooks.settings.json` (it
  POSTs to `/await` with a long `--max-time`). The other hooks stay instant.
- Turn it off with `"permission": {"enabled": false}` , you keep the blink as a
  notification but answer on screen.

### Recipe: a one-press "keep going" nudge (for `--dangerously-skip-permissions`)
If you run with permissions off, Claude never prompts you, so the approve/deny flow
above never fires. Instead, make **Play** push a stopped agent onward , type a word,
then Enter:
```json
"buttons": { "play": ["continue", "Enter"] }
```
Now after a turn finishes, one press of Play sends `continue` + Enter. Pick any word
(`"yes"`, `"proceed"`, or a phrase as a single string like `"keep going"`). Two
caveats from how `tmux send-keys` parses args: keep a phrase as **one** string
(`["keep going", "Enter"]`, not `["keep", "going", "Enter"]`, which would drop the
space), and don't use a bare word that is itself a tmux key name (`Space`, `Tab`,
`Up`, `Enter`, `C-c`, ...) , those get interpreted instead of typed. A plain word like
`continue` is safe as-is. Want to keep Play = approve/submit? Put the nudge on the
otherwise-free **Vol+** instead: `"vol_plus": ["continue", "Enter"]`.

## Customizing (`feldd_cc.config.json`)
Edit the file next to `feldd_cc.py`; omit any key to keep its default. Full schema:
[`feldd_cc.config.example.json`](feldd_cc.config.example.json). Axes: `buttons`,
`lights` (`events` map, `blink_hz`, `done_hold_s`, `whole_row`), `sessions` (`mode`,
`assign`), `input` (`backend`), `permission` (`enabled`, `timeout_s`). **Restart the
daemon after editing.**

## Cockpit mode (`sessions.mode: "cockpit"`)
The fleet view: **8 LEDs = 8 sessions**, the buttons fly you between them, the faders
read and tune. Set `"sessions": {"mode": "cockpit"}` (pin sessions to LEDs 0-7 the same
way as multi mode).

- **The board follows you (MRU).** The **front 4 LEDs (the Track buttons) hold the 4
  sessions you've most recently focused or that most recently need you** , a sticky
  cache. A session keeps its exact front LED until it's evicted, so nothing shuffles
  under your fingers. The **side 4 LEDs are the bench** (the next most-recent sessions,
  there for glance); anything past that is off-board. Each LED's pattern: off = idle,
  solid = working, fast blink = needs you, quick flash = just finished, slow pulse = on
  autopilot.
- **Track 1-4 = jump** to the four front sessions (switches your tmux client to it, focus
  follows). To reach a **bench or off-board** session, sweep the **fader-2 scrubber** (or
  **Vol+** if it needs you) , doing so **promotes it onto a front button** (evicting the
  least-recently-used front session to the bench). A **needs-you** session also auto-claims
  a front button. A new background session never steals a button just by appearing; it
  waits on the bench until it needs you. (There is no Play-held shift: Play and the Track
  buttons share one resistor ladder, so the hardware can't report them held together.)
- **Vol+ = next:** jumps to (and promotes) the next session that needs you.
- **Faders:** 1 = scroll the focused session, 2 = scrub focus across all sessions (reaches
  sessions past the visible 8), 3 = the **calm dial** (slide down: only needs-you stays
  lit and busy sessions go dark; slide up: show everything), 4 = assignable (`faders.assign.preset`):
  - `coarse` (default) = snap-scroll the focused session by deciles.
  - `autopilot` = self-drive a stopped session (see below).
  - `rotate` = a second scrubber. `custom` = fire an action at the top of the throw.

  Faders use **soft-takeover**: a fader only grabs once you move it past the current
  value, so bumping it never yanks anything.

### Autopilot drip (cockpit, dangerous mode only)
Set `faders.assign.preset: "autopilot"`, `autopilot.enabled: true`, **and**
`permission.enabled: false` (autopilot only arms when the SP-1 permission approve/deny is
off, the dangerous-mode shape, and only with the `tmux` input backend so it has an exact
pane). It is **off by default**. Dialing fader 4 up arms the **focused** session at a
cadence (`autopilot.steps_s`, e.g. 90/30/10s, 0 = disarm). When that session **stops**,
the daemon auto-sends `autopilot.nudge` (`["continue","Enter"]`) to its exact pane and the
LED slow-pulses. Guardrails: a real question (needs-you) pauses it (and stays paused even
if a Stop follows the question); a session with no known pane disarms rather than guess;
after `autopilot.deadman` hands-off continues it **parks** and hard-alerts, and a held
fader can't reset that, only a real prompt from you (or pulling the fader to 0) does.

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
