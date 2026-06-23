# feldd-cc — the SP-1 as a Claude Code console

Turn the Teenage Engineering SP-1 (running feldd) into a physical control surface for Claude Code:
**lights show when an agent needs you, buttons drive it.** No custom terminal — Claude Code hooks +
a small local daemon + feldd's existing USB-CDC JSON channel.

```
Claude Code  --hooks (HTTP)-->  feldd_cc daemon  --CDC JSON-->  SP-1 (feldd)
  (events)                          |   ^                         LEDs + buttons
                                    |   +-- monitor stream (buttons/faders)
                                    +------ tmux send-keys ------> the Claude pane
```

- **State → lights:** Claude Code hooks POST lifecycle events to the daemon; it maps them to the SP-1's
  track LEDs (a session is *working* / *needs you* / *done*).
- **Buttons → Claude:** the daemon reads feldd's CDC monitor stream and types into your session.
  Default `tmux send-keys` (precise, multi-session, no permissions); non-tmux users can switch to a
  macOS focused-window backend or lights-only via `input.backend` (see [`USAGE.md`](USAGE.md)). The
  **lights need no tmux at all** , every Claude Code user gets them.

**Setup is itself a Claude Code task.** Point your Claude at this folder and say *"set up my feldd-cc
console using WIZARD.md."* It runs a short **guided wizard** , conversationally asks about buttons,
lights, scope, and single-vs-multi session, writes the config, wires the hooks, and self-tests so you
*see* it work. ([`SETUP.md`](SETUP.md) is the manual runbook if you'd rather do it by hand.)

## Status
v3 host daemon: state lights + button input + permission approve/deny + a **cockpit mode** (8 LEDs =
8 sessions, Track = jump, Vol+ = next-needs-you, four fader jobs: scroll / scrubber / calm dial /
assignable, and an opt-in autopilot drip). See [`USAGE.md`](USAGE.md#cockpit-mode-sessionsmode-cockpit).
Pairs with the feldd
firmware **`led` verb, shipped in feldd 0.16.0-beta** (firmware
source is in this repo's [`firmware/`](../firmware); flash the ready image from
**[feldd.com/sp-1/guide?beta=1](https://feldd.com/sp-1/guide?beta=1)**). The led verb + the monitor
stream are **hardware-validated** on a real SP-1. Flashing is SWD-validate-only per the
never-blind-flash rule.

## The control map (v1)
| SP-1 control | monitor ix | does |
|---|---|---|
| **Play** | 0 | send **Enter** to the active Claude pane (approve / submit) |
| **Track 1-4** | 1-4 | **select** which session the controls target (its LED) |
| **Vol-** | 6 | send **Esc** (interrupt) to the active pane |
| **FWD / RWD** | 7 / 8 | **scroll** the active pane (PageDown / PageUp) |
| **4 track LEDs** | — | one per session: off=idle, solid=working, **blink=needs you**, brief flash=done |

The session that's *needs-you* auto-becomes the active target, so the common loop is: an LED blinks →
hit **Play** to approve / answer.

**Approve permissions from the controller.** When Claude *would* prompt you to allow a tool, the daemon
holds that prompt open: the session LED blinks and **Play = allow, Vol- = deny** answer the real
permission dialog. It only fires when **your own permission setup** would have prompted anyway (no
imposed gate list; `--dangerously-skip-permissions` correctly never triggers it), never under
`claude -p`, and times out to the on-screen prompt after `permission.timeout_s`. Toggle with
`permission.enabled` (see [`USAGE.md`](USAGE.md)).

## Wire contract (feldd CDC, JSON objects, brace-framed, no newline)
- **Controls in (in feldd today):** after `{"t":"monset","on":true}` the device emits
  `{"t":"mon","k":"b","ix":N,"s":0|1}` (button N press/release) and `{"t":"mon","k":"f","ix":N,"v":V}` (fader).
  Indices: Play=0, Track1-4=1-4, Vol+=5, Vol-=6, FWD=7, RWD=8.
- **LEDs out (the `led` verb, feldd 0.16.0-beta+):**
  `{"t":"led","ix":0-7,"on":true|false}` · `{"t":"led","mask":0-255}` (bit i = LED i) · `{"t":"led","release":true}`
  → `{"t":"led_r","ok":true,"active":B,"mask":M}`. ix 0-3 = the 4 track LEDs (primary), 4-7 = side LEDs.
  The override is the highest-priority LED writer while active and a clean no-op after `release`.

## Hooks → state map (daemon)
| Claude Code hook | session state |
|---|---|
| `SessionStart` | register, idle (LED off) |
| `UserPromptSubmit`, `PreToolUse`, `SubagentStart` | working (solid) |
| `Notification`, `PermissionRequest` | **needs you (blink)** |
| `PostToolUse`(/Failure) | working → settles |
| `Stop`, `SubagentStop` | done (brief flash) → idle |
| `SessionEnd` | unregister (LED off) |

Caveat (per research): `Stop` fires on *every* response-finish and not on interrupt; `PermissionRequest`
hooks don't fire in headless `-p`. **Capture real payloads first** with `capture-hooks.sh` before
trusting the map.

## Files
- `feldd_cc.py` — the bridge daemon (HTTP hook server + CDC serial bridge + tmux input).
- `hooks.settings.json` — the Claude Code hooks snippet (HTTP hooks → the daemon).
- `capture-hooks.sh` — log every real hook payload so you can verify the event schema first.
- `bench_led.py` — a tiny standalone tester that drives the 8 LEDs over serial (no daemon needed).
- `WIZARD.md` — the guided, agent-run setup wizard (the easy path).
- `SETUP.md` — the manual setup runbook (do-it-by-hand path).
- `USAGE.md` — running it day to day: sessions, the LED↔session mapping, pinning
  projects to Track LEDs, the buttons, the daemon, and troubleshooting.
- `feldd_cc.config.json` — your customizations (button actions / lights / session model); omit any key
  to keep its default. See `feldd_cc.config.example.json` for the full schema.
- `test_feldd_cc.py` — host tests for the config + state logic (`python3 test_feldd_cc.py`).

## Roadmap
- v1: single/few sessions, track LEDs = state, Play/rocker/Esc input via tmux. **done.**
- v2: hold a `PermissionRequest` hook open so Play/Vol- resolve the *real* allow/deny (the `agentsd`
  trick), respecting your own permission setup. **done** , see "Approve permissions from the controller."
- v3: the **cockpit** , 8 LEDs = 8 sessions, Track = jump (tmux focus follows), Vol+ = next-needs-you,
  faders = scroll / scrubber / calm dial / assignable, and an opt-in autopilot drip. **done** (daemon +
  host tests; tmux jump/scroll + the monitor fader stream to be verified on-device).
- v4 (next): **Macro pad** , the free buttons (Track in single mode, Vol+) send saved prompts.
  Possible cockpit follow-ups: a cross-ladder modifier for direct 5-8 button select, and the sliding
  8-LED window for >8 sessions (path to 16).

Prior art this is modeled on: `paultyng/agentsd`, `bobek-balinek/claude-lamp`, `danielrosehill/Claude-Macropad-V2`.

> Run the SP-1 in **MIDI mode** (not Keyboard mode) for the console — in Keyboard mode the device's own
> keystrokes would double the daemon's `Enter`.
