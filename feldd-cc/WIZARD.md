# WIZARD.md — set up the feldd-cc console (run by the user's Claude Code agent)

**You are an agent setting up the feldd-cc console for the user.** Walk the golden
path below, **conversationally** , ask one thing at a time, show the default in
`[brackets]`, and let the user just say "go" to accept all defaults or tweak any
step. Do the file edits and commands yourself; confirm before anything
destructive. Keep it friendly and short. The daemon files live in this folder.

---

## Beat 0 , Prereqs (check, don't nag)
1. **Firmware:** the SP-1 must run **feldd 0.16.0-beta+** (it has the `led` verb)
   and be in **MIDI mode**, plugged in over USB. Verify with:
   ```bash
   python3 bench_led.py
   ```
   It finds the SP-1 and walks the 8 LEDs. If it can't find a port: the SP-1 isn't
   plugged in, isn't in MIDI mode, or isn't flashed. **Do not flash it for them** ,
   point them to https://feldd.com/sp-1/guide?beta=1 (flash onto an SWD-recoverable
   unit) and continue once it's ready. They can still proceed with `--dry-run` to
   configure without hardware.
2. **Dependency:** `pip3 install --break-system-packages pyserial`.

## Beat 1 , Basics (two quick questions)
- **Hooks scope:** "Should the console react to **all** your Claude Code sessions,
  or just **one project**?" `[all]`
  - all → you'll edit `~/.claude/settings.json`
  - one project → you'll edit `<that-project>/.claude/settings.json`
- **Session model:** "Track a **single** agent, or up to **4 at once** (one per
  Track button)?" `[single]` → `sessions.mode` = `single` or `multi`.

## Beat 2 , Buttons
Show the default map and ask "keep these, or change any?":
| Button | Default |
|---|---|
| Play | Enter (approve / submit) |
| Vol- | Esc (interrupt) |
| Rocker FWD / RWD | scroll (PageDown / PageUp) |
| Track 1-4 | select the active session (multi mode only) |

For any change, capture the action as one of:
- a key list, e.g. `["Enter"]`, `["Escape"]`, `["C-c"]` (tmux send-keys syntax)
- a shell command, e.g. `{"shell": "say done"}` (runs on their machine , only what
  they'd run themselves)
- `null` = do nothing

## Beat 3 , Lights
Show the defaults and ask "keep or tweak?":
- **working = solid**, **needs-you = blink** (3 Hz), **done = brief flash** (0.6 s)
- which Claude events map to each state (the defaults are good; only change on request)
- `blink_hz`, `done_hold_s`, and (single mode) `whole_row` = light one LED `[false]`
  or the whole front row `true`.

## Beat 4 , Write the config
Compose **only the keys the user changed** into `feldd_cc.config.json` next to
`feldd_cc.py` (everything omitted falls back to defaults). Schema +
example: `feldd_cc.config.example.json`. Show them the final file.

## Beat 5 , Wire the hooks (back up first)
Merge the `hooks` block from `hooks.settings.json` into the settings file chosen in
Beat 1:
1. **Back it up**: copy the target `settings.json` to `settings.json.bak`.
2. **Deep-merge**: if it has no `hooks` key, add ours wholesale; if it does, append
   our entries to each event's array , **do not clobber** the user's existing hooks.
3. Write valid JSON and **show the diff**. These are HTTP hooks that POST to the
   local daemon; harmless (fast-fail) when the daemon is down.

## Beat 6 , Run it + self-test (the payoff)
1. **Start the daemon** in its own tmux window so it persists:
   ```bash
   tmux new-window -n feldd-cc 'cd <this folder> && python3 feldd_cc.py'
   ```
   Confirm the log shows `opened SP-1 at ...` and `hook server on ...`.
2. **Self-test** , fire a fake session at the LEDs and have them watch the track row:
   ```bash
   H=http://localhost:9200/hook; S='{"session_id":"demo","cwd":"/tmp"'
   curl -s -X POST $H -d "$S,\"hook_event_name\":\"UserPromptSubmit\"}"; sleep 5   # SOLID (working)
   curl -s -X POST $H -d "$S,\"hook_event_name\":\"Notification\"}";     sleep 7   # BLINK (needs you)
   curl -s -X POST $H -d "$S,\"hook_event_name\":\"Stop\"}";             sleep 4   # FLASH then off (done)
   ```
   Ask them to confirm they saw solid → blink → flash. Then have them **press Play**
   on the SP-1 and confirm an Enter lands in a Claude pane.
3. **Go live:** start (or restart) a real `claude` in a tmux pane so the new hooks
   load. The track LED now tracks that session.

## Done. To undo
Remove the feldd-cc `hooks` block from the settings file (restore the `.bak`),
delete `feldd_cc.config.json`, and stop the daemon (`tmux kill-window -t feldd-cc`).

## Tips
- A second Enter per Play press = the SP-1 is in **Keyboard mode**; switch it to MIDI.
- Buttons reach the wrong pane = pick the session with **Track 1-4** (multi mode).
- Hook event names can change across Claude Code versions; `capture-hooks.sh` logs
  real payloads so you can reconcile `lights.events` in the config.
