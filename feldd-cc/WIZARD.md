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

## Beat 0.5 , Clean slate (don't clobber, don't inherit)
**Before asking anything, check for an existing `feldd_cc.config.json` next to `feldd_cc.py`.**
If one is there and **you** didn't just write it this session, it's a leftover (a previous run, or
someone else's checkout). Do NOT silently build on it or assume setup is done , say: *"I see an
existing config; want to **start fresh** (I'll back it up to `feldd_cc.config.json.bak`) or
**tweak the existing one**?"* and wait. Default to **start fresh** for a first-time setup. Also note:
`feldd_cc.config.example.json` is a **schema reference, not a starter** , never copy it into place;
you compose the real config in Beat 4 from only what the user changed.

## Beat 1 , Basics (a few quick questions)
- **Hooks scope:** "Should the console react to **all** your Claude Code sessions,
  or just **one project**?" `[all]`
  - all → you'll edit `~/.claude/settings.json`
  - one project → you'll edit `<that-project>/.claude/settings.json`
- **Session model:** "Track a **single** agent, up to **4 at once** (one per Track
  button), or the **cockpit** (up to **8**, plus faders to read/select/tune and a
  self-driving autopilot)?" `[single]` → `sessions.mode` = `single` / `multi` / `cockpit`.
  - If **multi** or **cockpit**, optionally **pin sessions to LEDs** so the mapping is
    stable. Ask how they run claude: from **named tmux sessions** (often all in one dir)
    → pin by **tmux session name**; from **project directories** → pin by **path**.
    Record into `sessions.assign` as `{ "<tmux-session-name or path>": <LED> }` (multi:
    0-3; cockpit: 0-7). To see their tmux session names:
    `tmux list-sessions -F "#{session_name}"`.
  - If **cockpit**, walk the extras (Beat 2.5).
- **Input backend:** "Do you run `claude` inside **tmux**?" Yes → `input.backend` =
  `tmux` (precise, multi-session, no permissions). No → on macOS offer `osascript`
  (types into the *focused* window; needs the Accessibility permission), otherwise
  `none` (lights only). The **lights work either way**; this only decides whether the
  buttons type.
- **Approve permissions from the SP-1?** "When Claude **would ask you** to allow a
  tool, want to answer it from the controller , the LED blinks, **Play = allow, Vol- =
  deny**?" `[yes]` → `permission.enabled` = `true`. Make clear it **respects their own
  permission setup** (it only fires when Claude would have prompted anyway , someone on
  `--dangerously-skip-permissions` will simply never see it, and that's correct), times
  out to the on-screen prompt after `permission.timeout_s` `[90]`, and never fires
  under `claude -p`. No → `enabled: false` (the LED still blinks as a heads-up; they
  answer on screen). **This is the one hook that blocks** , note that Beat 5 wires a
  `PermissionRequest` hook with a long timeout for it.
- **Run in a bypass / dangerous mode?** If they run `--dangerously-skip-permissions`,
  no prompts ever fire, so the approve/deny above is moot for them , `PermissionRequest`
  hooks don't fire in bypass mode. Offer the alternative: **make Play a one-press "keep
  going" nudge** that types a word then Enter to push a stopped agent onward (useful
  after a `Stop`, when a bare Enter would submit nothing). Ask the word `[continue]`
  (e.g. `continue`, `keep going`, `yes`, `proceed`) and write `buttons.play` =
  `["<word>", "Enter"]`. A multi-word phrase is fine as long as it stays **one** config
  string (`["keep going", "Enter"]`); avoid a bare word that is itself a tmux key name
  (`Space`, `Tab`, `Up`, `Enter`, `C-c`, ...) , those get interpreted, not typed. Leave
  Play = `["Enter"]` for anyone who runs with permissions on. Prefer to keep Play =
  approve/submit? Put the nudge on the free **Vol+** instead: `buttons.vol_plus` =
  `["continue", "Enter"]`.

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

## Beat 2.5 , Cockpit extras (only if mode = cockpit)
The cockpit lights all 8 LEDs and **follows your attention (MRU)**: the front 4 (the
Track buttons) hold the sessions you most recently focused or that need you, the side 4
are the bench. Controls: **Track 1-4 = jump** to the four front sessions (tmux focus
follows); reach a **bench/off-board** session with the **scrubber or Vol+**, which
**promotes it onto a front button** (there is no Play+Track shift, the hardware can't
chord same-ladder buttons). **Vol+ = jump to the next needs-you.** Confirm those, then
the faders:
- **Fader 1 = scroll** the focused session, **2 = scrub** focus across all sessions,
  **3 = calm dial** (down = only needs-you lit, up = show all states). These are good
  defaults; only change `faders.scroll/scrubber/calm` if they want a different layout.
- **Fader 4 = assignable.** Ask which preset `[coarse]`:
  - `coarse` , snap-scroll the focused session by deciles (safe, useful).
  - `autopilot` , self-drive a stopped session (only offer/recommend if they run
    `--dangerously-skip-permissions`). If chosen, set `autopilot.enabled: true` AND
    `permission.enabled: false` AND keep `input.backend: tmux` , autopilot refuses to arm
    unless all three hold (the daemon enforces it, not just docs). Confirm the cadence
    buckets `autopilot.steps_s` `[0,90,30,10]`, the **deadman cap** `autopilot.deadman`
    `[8]` (or pitch a time budget), and the `autopilot.nudge` `[["continue","Enter"]]`.
    Spell out the guardrails: a real question pauses it (sticky even through a Stop), a
    pane-less session disarms, the deadman parks it (a held fader can't reset it, only
    your real prompt does), the LED slow-pulses while armed.
  - `rotate` , a second scrubber. `custom` , map `faders.assign.action` to a key list /
    `{"shell":..}` fired at the top of the throw.

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
4. **One hook blocks on purpose:** `PermissionRequest` POSTs to `/await` with a long
   `--max-time` so the SP-1 can answer the allow/deny (Beat 1's permission choice). If
   they set `permission.enabled: false`, the daemon returns immediately and it behaves
   like the others. All the rest are instant (`--max-time 2`).

## Beat 6 , Run it + self-test (the payoff)
1. **Start the daemon** so it keeps running:
   - in tmux: `tmux new-window -n feldd-cc 'cd <this folder> && python3 feldd_cc.py'`
   - without tmux: `nohup python3 feldd_cc.py >/tmp/feldd-cc.log 2>&1 &` (or a
     login/launchd service)

   Confirm the log shows `opened SP-1 at ...` and `hook server on ...`.
2. **Self-test** , fire a fake session at the LEDs and have them watch the track row:
   ```bash
   H=http://localhost:9200/hook; S='{"session_id":"demo","cwd":"/tmp"'
   curl -s -X POST $H -d "$S,\"hook_event_name\":\"UserPromptSubmit\"}"; sleep 5   # SOLID (working)
   curl -s -X POST $H -d "$S,\"hook_event_name\":\"Notification\"}";     sleep 7   # BLINK (needs you)
   curl -s -X POST $H -d "$S,\"hook_event_name\":\"Stop\"}";             sleep 4   # FLASH then off (done)
   ```
   Ask them to confirm they saw solid → blink → flash. Then test input for their
   backend: **press Play** , with `tmux` an Enter lands in the Claude pane; with
   `osascript` it lands in the focused terminal (focus it first); with `none` there's
   no button input (lights only).
3. **Go live:** start (or restart) a real `claude` session (in a tmux pane if using
   the `tmux` backend) so the new hooks load. The track LED now tracks that session.
4. **Permission approve/deny (if enabled):** the next time Claude **would prompt** them
   for a tool, the session LED blinks , **Play allows, Vol- denies**. (Nothing to
   pre-test: it only fires on a real prompt, and someone running
   `--dangerously-skip-permissions` correctly never sees it.)

## Done. To undo
Remove the feldd-cc `hooks` block from the settings file (restore the `.bak`),
delete `feldd_cc.config.json`, and stop the daemon (`tmux kill-window -t feldd-cc`).

## Tips
- A second Enter per Play press = the SP-1 is in **Keyboard mode**; switch it to MIDI.
- Buttons reach the wrong pane = pick the session with **Track 1-4** (multi mode).
- Hook event names can change across Claude Code versions; `capture-hooks.sh` logs
  real payloads so you can reconcile `lights.events` in the config.
