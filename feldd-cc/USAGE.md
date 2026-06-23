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

## Customizing (`feldd_cc.config.json`)
Edit the file next to `feldd_cc.py`; omit any key to keep its default. Full schema:
[`feldd_cc.config.example.json`](feldd_cc.config.example.json). Axes: `buttons`,
`lights` (`events` map, `blink_hz`, `done_hold_s`, `whole_row`), `sessions` (`mode`,
`assign`), `input` (`backend`), `permission` (`enabled`, `timeout_s`). **Restart the
daemon after editing.**

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
