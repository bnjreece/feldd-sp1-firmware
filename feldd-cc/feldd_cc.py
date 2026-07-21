#!/usr/bin/env python3
"""feldd-cc: bridge the SP-1 (feldd) to Claude Code as a physical console.

Claude Code hooks  --HTTP-->  this daemon  --CDC JSON-->  SP-1 LEDs   (state out)
SP-1 buttons       --CDC JSON-->  this daemon  --tmux send-keys-->  Claude pane  (input in)

Run:  python3 feldd_cc.py            (auto-detect the SP-1 CDC port)
      FELDD_PORT=/dev/cu.usbmodemXXXX python3 feldd_cc.py
      python3 feldd_cc.py --dry-run  (no device; logs the LED/input logic)

Behaviour is customizable via feldd_cc.config.json (next to this file, or
$FELDD_CC_CONFIG): button actions, light behaviour, and the session model. The
setup wizard (WIZARD.md) writes that file for you. NO config file = the defaults
below, which are today's exact behaviour.

Hooks: merge hooks.settings.json into ~/.claude/settings.json (POSTs events to :9200/hook).
"""
import argparse
import copy
import glob
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, parse_qs

# pyserial is imported lazily inside sp1_console.transport (SerialTransport), so the
# daemon module no longer needs it at import time.

RENDER_HZ = 20.0
NUM_TRACK_LEDS = 4           # 4 reliable front LEDs (ix 0..3); side LEDs (4..7) off in v1
# feldd monitor button indices
PLAY, TRK1, TRK4, VOLU, VOLD, FWD, RWD = 0, 1, 4, 5, 6, 7, 8
BTN_IX = {"play": PLAY, "vol_plus": VOLU, "vol_minus": VOLD, "fwd": FWD, "rwd": RWD}
IX_BTN = {v: k for k, v in BTN_IX.items()}

log = lambda *a: print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- config
# The four customizable axes. feldd_cc.config.json (next to this file, or
# $FELDD_CC_CONFIG) DEEP-MERGES over these; a missing file = exactly these defaults.
DEFAULTS = {
    # button -> action. action = list of tmux send-keys ARGS, or {"shell": "cmd"},
    # or null/[] = do nothing. Track 1-4 are not here: in "multi" mode they select
    # the session the buttons target; in "single" mode they are unused.
    "buttons": {
        "play":      ["Enter"],      # approve / submit
        "vol_minus": ["Escape"],     # interrupt
        "vol_plus":  None,
        "fwd":       ["PageDown"],   # scroll
        "rwd":       ["PageUp"],
    },
    "lights": {
        # Claude Code hook event -> session state (idle / working / needs / done).
        "events": {
            "SessionStart": "idle",
            "UserPromptSubmit": "working",
            "PreToolUse": "working",
            "SubagentStart": "working",
            "PostToolUse": "working",
            "PostToolUseFailure": "working",
            "Notification": "needs",
            "PermissionRequest": "needs",
            "Stop": "done",
            "SubagentStop": "done",
            "StopFailure": "done",
        },
        "blink_hz": 3.0,        # "needs you" blink rate
        "done_hold_s": 0.6,     # how long the "done" flash holds
        "whole_row": False,     # single mode: light 1 LED (False) or all 4 (True)
    },
    "sessions": {
        "mode": "multi",        # "multi" = a LED per session + Track1-4 select;
                                # "single" = one session, shown on LED 0 (or the row)
        # multi mode: PIN a project to a fixed Track LED (0..3). A session whose cwd
        # is (or is under) the path always lands on that LED; everyone else auto-fills
        # the remaining LEDs. e.g. {"web": 0, "~/code/api": 1}  (name or path)
        "assign": {},
    },
    "input": {
        # how a button press reaches your Claude session:
        #   "tmux"      = tmux send-keys to the session's pane (precise, multi-session,
        #                 no permissions; needs tmux) -- the default.
        #   "osascript" = macOS System Events keystroke to the FOCUSED window (no tmux
        #                 needed, but needs Accessibility permission and can only type
        #                 into whatever terminal is frontmost).
        #   "none"      = lights only, no button input (works everywhere).
        "backend": "tmux",
    },
    "permission": {
        # When Claude would prompt you for tool permission, blink the session's LED
        # and let Play=allow / Vol-=deny resolve it from the SP-1 (driven by the
        # blocking PermissionRequest hook). Only fires when YOUR OWN permission setup
        # would have prompted anyway -- no imposed gate list, never under `claude -p`.
        # After timeout_s with no press, it falls through to the normal on-screen
        # prompt. Set enabled=false to skip hardware approval (lights only).
        "enabled": True,
        "timeout_s": 90,
    },
    # cockpit mode only: which fader ix (0-3) does which job. assign.preset is one of
    # autopilot | coarse | rotate | custom.
    "faders": {
        "scroll": 0,        # scroll the focused session
        "scrubber": 1,      # select / slide the 8-session window
        "calm": 2,          # board sensitivity ("calm dial")
        # assignable; preset = coarse (safe default) | autopilot | rotate | custom
        "assign": {"ix": 3, "preset": "coarse"},
    },
    # autopilot drip: a fader auto-sends the nudge to a STOPPED session on a cadence.
    # OFF by default; only sensible (and only enable it) in --dangerously-skip-permissions.
    "autopilot": {
        "enabled": False,              # hard gate: must opt in
        "steps_s": [0, 90, 30, 10],    # cadence buckets across the fader throw (0 = off)
        "deadman": 8,                  # auto-continues with no human touch -> park
        "nudge": ["continue", "Enter"],
    },
    "hooks_scope": "global",    # informational for the wizard; the daemon ignores it
}


def _deep_merge(base, over):
    for k, v in (over or {}).items():
        if isinstance(v, dict) and isinstance(base.get(k), dict):
            _deep_merge(base[k], v)
        else:
            base[k] = v
    return base


def load_config(path=None):
    cfg = copy.deepcopy(DEFAULTS)
    p = (path or os.environ.get("FELDD_CC_CONFIG")
         or os.path.join(os.path.dirname(os.path.abspath(__file__)), "feldd_cc.config.json"))
    if os.path.exists(p):
        try:
            with open(p) as f:
                _deep_merge(cfg, json.load(f))
            log("config:", p)
        except Exception as e:
            log("config load error (%s); using defaults" % e)
    return cfg


CFG = copy.deepcopy(DEFAULTS)   # replaced in main(); tests pass their own to State


def _norm(p):
    return os.path.normpath(os.path.expanduser(p or ""))


# --------------------------------------------------------------------------- state
class State:
    """Per-session LED state + which session the physical controls target."""
    def __init__(self, cfg=None):
        self.cfg = cfg or CFG
        self.single = self.cfg["sessions"]["mode"] == "single"
        self.cockpit = self.cfg["sessions"]["mode"] == "cockpit"
        # cockpit drives all 8 LEDs (front 0-3 + side 4-7); multi = 4; single = 1.
        self.n_leds = 1 if self.single else (8 if self.cockpit else NUM_TRACK_LEDS)
        # multi mode: project -> fixed Track LED pins. Normalize paths once; drop
        # bad / out-of-range entries. Pinned LEDs stay out of the auto-fill pool.
        # A pin KEY matches EITHER a tmux session NAME (exact) OR a project PATH (cwd
        # is/under it) — so pinning works whether you run claude from project dirs or
        # from home in named tmux sessions.
        self.assign = []   # (raw_key, normalized_path, led)
        for key, led in (self.cfg["sessions"].get("assign") or {}).items():
            try:
                led = int(led)
            except (TypeError, ValueError):
                continue
            if 0 <= led < self.n_leds:
                self.assign.append((key, _norm(key), led))
        self.pinned = {led for _, _, led in self.assign}
        self.lock = threading.Lock()
        self.sessions = {}        # sid -> {"led": int, "state": str, "cwd": str, "t": float}
        self._seq = 0             # monotonic arrival index (stable scrubber order)
        self.free = [0] if self.single else [i for i in range(self.n_leds) if i not in self.pinned]
        self.active = None        # sid the buttons currently drive
        self.pending = {}         # sid -> {"ev": Event, "result": "allow"|"deny"|None} (awaiting Play/Vol-)
        self.fader_target = {}    # fader ix -> logical value (soft-takeover pickup)
        self.fader_last = {}      # fader ix -> last physical value seen
        self.fader_grabbed = set()
        self.calm = 127           # cockpit "calm dial": 0 = only needs-you, 127 = all states
        self.autopilot = {}       # sid -> {"rate": s, "drips": n} (self-driving sessions)
        self.autopilot_parked = set()  # sids the deadman parked; must disarm (fader 0) to re-arm

    def _ensure(self, sid, cwd, tmux=None):
        s = self.sessions.get(sid)
        if s:
            if cwd:
                s["cwd"] = cwd
            return s
        pinned = False
        if self.single:
            led = 0                       # all sessions share LED 0 in single mode
        else:
            led = self._assigned_led(cwd, tmux)  # a pinned session always gets its LED
            pinned = led is not None
            if led is None:
                if self.cockpit:
                    led = self._tier_place()   # front 0-3 free -> front; side 4-7 -> bench; else off
                elif self.free:
                    led = self.free.pop(0)
                else:
                    led = min(self.sessions.values(), key=lambda x: x["t"])["led"]
        now = time.time()
        s = {"led": led, "state": "idle", "cwd": cwd or "", "pane": None, "tmux": tmux,
             "t": now, "seq": self._seq, "awaiting": False, "stopped_at": None,
             "focus_t": now, "pinned": pinned}
        self._seq += 1
        self.sessions[sid] = s
        return s

    # ---- cockpit MRU: front row (0-3) is a sticky LRU cache of the sessions you most
    # recently focused or that most recently needed you; side row (4-7) is the bench.
    # Pinned sessions anchor their LED and are exempt. (All called under self.lock.)
    def _tier_place(self):
        """A NEW session's LED: first free front (0-3), then first free bench (4-7), else
        off-board. Never evicts , a new background session can't steal a front button."""
        occ = {self.sessions[x]["led"] for x in self.sessions
               if isinstance(self.sessions[x]["led"], int)}
        for i in list(range(4)) + list(range(4, 8)):
            if i not in self.pinned and i not in occ:
                return i
        return None

    def _bench_slot_for(self, sid, vacated=None):
        """An LED for a session being demoted to the bench: the slot the promoted session
        just vacated if usable, else a free bench LED, else evict the least-recently-active
        bench session off-board and take its LED, else off-board (None)."""
        side = [i for i in range(4, 8) if i not in self.pinned]
        if isinstance(vacated, int) and vacated in side:
            return vacated
        occ = {self.sessions[x]["led"] for x in self.sessions
               if x != sid and isinstance(self.sessions[x]["led"], int) and self.sessions[x]["led"] in side}
        free = [i for i in side if i not in occ]
        if free:
            return free[0]
        bench = [((self.sessions[x]["state"] == "needs", self.sessions[x]["focus_t"]), x)
                 for x in self.sessions
                 if x != sid and isinstance(self.sessions[x]["led"], int) and self.sessions[x]["led"] in side]
        if bench:
            victim = min(bench)[1]       # spare needs (False<True), then least-recently focused
            freed = self.sessions[victim]["led"]
            self.sessions[victim]["led"] = None   # bench full -> push the victim off-board
            return freed
        return None

    def _rehome_offboard(self):
        """Cockpit: after a slot frees up, pull off-board sessions back onto the board
        (needs first, then most-recently focused) so a working session never stays dark
        while LEDs sit empty."""
        offboard = sorted((x for x in self.sessions if self.sessions[x]["led"] is None),
                          key=lambda x: (self.sessions[x]["state"] != "needs",
                                         -self.sessions[x]["focus_t"]))
        for x in offboard:
            slot = self._tier_place()
            if slot is None:
                break
            self.sessions[x]["led"] = slot

    def _promote(self, sid, now):
        """Ensure sid occupies a FRONT LED (0-3), evicting the least-recently-focused
        front session (preferring one that isn't needs-you) to the bench. No-op for
        pinned sessions, already-front sessions, and non-cockpit modes."""
        if not self.cockpit:
            return
        s = self.sessions.get(sid)
        if not s or s.get("pinned"):
            return
        s["focus_t"] = now
        cur = s["led"]
        if isinstance(cur, int) and cur < 4:
            return                                   # already a front session
        front = [i for i in range(4) if i not in self.pinned]
        occ = {self.sessions[x]["led"]: x for x in self.sessions
               if isinstance(self.sessions[x]["led"], int) and self.sessions[x]["led"] in front}
        freeslots = [i for i in front if i not in occ]
        if freeslots:
            s["led"] = freeslots[0]
            return
        if not front:
            return                                   # all front LEDs are pinned
        cands = [(self.sessions[x]["focus_t"], x) for x in occ.values()]
        nonneeds = [c for c in cands if self.sessions[c[1]]["state"] != "needs"]
        evict = min(nonneeds or cands)[1]
        target = self.sessions[evict]["led"]
        self.sessions[evict]["led"] = self._bench_slot_for(evict, vacated=cur)
        s["led"] = target

    def focus(self, sid):
        """Make sid the active target AND promote it onto a front button (cockpit)."""
        with self.lock:
            if sid in self.sessions:
                self.active = sid
                self._promote(sid, time.time())

    def _assigned_led(self, cwd, tmux):
        """The pinned Track LED for a session, matched by tmux session NAME (exact) or
        by project PATH (cwd is/under it), else None."""
        c = _norm(cwd) if cwd else None
        for raw, p, led in self.assign:
            if tmux and raw == tmux:
                return led
            if c and (c == p or c.startswith(p + os.sep)):
                return led
        return None

    def _pane_owner(self, sid, pane):
        """A subagent shares its parent's tmux PANE but has its own session id. It must
        not claim a separate LED/Track slot (jumping to it just lands on the parent), so
        fold its activity into whichever registered session already owns that pane. Only
        a NEW sid folds, and only on an exact pane match -- two real sessions never merge
        (distinct panes never collide; a missing pane never folds)."""
        if not pane or sid in self.sessions:
            return sid
        for other, s in self.sessions.items():
            if other != sid and s.get("pane") == pane:
                return other
        return sid

    def set_state(self, sid, cwd, st, pane=None, tmux=None):
        with self.lock:
            sid = self._pane_owner(sid, pane)   # fold a same-pane subagent into its parent
            s = self._ensure(sid, cwd, tmux)
            if pane:
                s["pane"] = pane
            if tmux:
                s["tmux"] = tmux
            now = time.time()
            s["state"], s["t"] = st, now
            # Track an autopilot-relevant edge that survives the done->idle decay:
            #   needs  -> awaiting a human answer (sticky through a following Stop)
            #   working-> resumed/answered (clears both)
            #   done   -> stopped, eligible for a drip after the cadence
            if st == "needs":
                s["awaiting"] = True
            elif st == "working":
                s["awaiting"], s["stopped_at"] = False, None
            elif st == "done":
                s["stopped_at"] = now
            if st == "needs":            # auto-focus the session that wants you...
                self.active = sid
                self._promote(sid, now)  # ...and let it claim a front button (cockpit)
            elif self.active is None:
                self.active = sid

    def await_decision(self, sid, cwd, pane, tmux):
        """Register a pending permission decision: blink the session, focus it, and
        return a slot whose .ev fires when Play/Vol- resolves it (or it's cancelled)."""
        with self.lock:
            s = self._ensure(sid, cwd, tmux)
            if pane:
                s["pane"] = pane
            s["state"], s["t"] = "needs", time.time()
            self.active = sid
            slot = {"ev": threading.Event(), "result": None}
            self.pending[sid] = slot
        return slot

    def resolve_active(self, behavior):
        """Play=allow / Vol-=deny resolves the ACTIVE session's pending decision, if
        any. Returns True if it resolved one (so the button skips its normal action)."""
        with self.lock:
            slot = self.pending.pop(self.active, None)
            if not slot:
                return False
            slot["result"] = behavior
            s = self.sessions.get(self.active)
            if s:
                s["state"] = "working" if behavior == "allow" else "idle"
                s["t"] = time.time()
        slot["ev"].set()
        return True

    def cancel_pending(self, sid):
        """Drop a pending decision (it timed out -> the normal on-screen prompt shows)."""
        with self.lock:
            if self.pending.pop(sid, None):
                s = self.sessions.get(sid)
                if s and s["state"] == "needs":
                    s["state"], s["t"] = "working", time.time()

    def fader_grab(self, ix, v):
        """Soft-takeover (pickup): ignore a fader until its physical value reaches the
        logical target, then 'grab' and track it. Returns the value to apply, or None
        while not yet grabbed. Same idea feldd uses on layer/profile switch, so a fader
        that's out of position doesn't yank the parameter when you bump it.

        INVARIANT: fader_grab / reset_fader_grab and the fader_* pickup fields are
        touched ONLY from the device read_loop thread (via handle_fader / handle_button),
        so they are intentionally lock-free. Do NOT call them from the HTTP or render
        thread without adding locking."""
        if ix in self.fader_grabbed:
            self.fader_target[ix] = v
            return v
        tgt = self.fader_target.get(ix)
        last = self.fader_last.get(ix)
        self.fader_last[ix] = v
        if tgt is None or v == tgt or (last is not None and (last - tgt) * (v - tgt) <= 0):
            self.fader_grabbed.add(ix)
            self.fader_target[ix] = v
            return v
        return None

    def reset_fader_grab(self, ix=None):
        """Force a fader (or all) to re-pick-up, e.g. after the focused session changes
        so the per-session scroll fader doesn't jump."""
        if ix is None:
            self.fader_grabbed.clear()
        else:
            self.fader_grabbed.discard(ix)

    def end(self, sid):
        with self.lock:
            s = self.sessions.pop(sid, None)
            if (s and not self.single and isinstance(s["led"], int)
                    and s["led"] not in self.free and s["led"] not in self.pinned):
                self.free.append(s["led"])
                self.free.sort()
            if self.cockpit:
                self._rehome_offboard()      # a freed slot pulls an off-board session back on
            if self.active == sid:
                self.active = next(iter(self.sessions), None)

    def select_by_led(self, led):
        if led is None:
            return None              # never let an off-board (led None) session match
        with self.lock:
            for sid, s in self.sessions.items():
                if s["led"] == led:
                    self.active = sid
                    return sid
        return None

    def active_target(self):
        """(tmux pane, cwd) of the session the buttons currently drive."""
        with self.lock:
            s = self.sessions.get(self.active)
            return (s.get("pane"), s.get("cwd")) if s else (None, None)

    def jump_info(self, sid):
        """(tmux session name, pane) of a session, for focus-follows-select jumps."""
        with self.lock:
            s = self.sessions.get(sid)
            return (s.get("tmux"), s.get("pane")) if s else (None, None)

    def set_active(self, sid):
        with self.lock:
            if sid in self.sessions:
                self.active = sid

    def session_at_fraction(self, frac):
        """The session at frac (0..1) along the stable arrival order (scrubber target).
        Sweeps ALL sessions, so it reaches sessions past the visible 8."""
        with self.lock:
            order = [sid for _, sid in sorted((s["seq"], sid)
                     for sid, s in self.sessions.items())]
            if not order:
                return None
            return order[min(len(order) - 1, int(frac * len(order)))]

    def next_needs(self):
        """The next session in 'needs' state after the active one, by LED order (wraps).
        Drives Vol+ = 'answer the next doorbell'."""
        with self.lock:
            needers = sorted(((s["led"] if isinstance(s["led"], int) else 99), sid)
                             for sid, s in self.sessions.items() if s["state"] == "needs")
            if not needers:
                return None
            cl = self.sessions.get(self.active, {}).get("led")
            cur_led = cl if isinstance(cl, int) else -1
            for led, sid in needers:
                if led > cur_led:
                    return sid
            return needers[0][1]        # past the last -> wrap to the first

    def _autopilot_allowed(self):
        """Autopilot only runs when explicitly enabled AND in a dangerous-mode-shaped
        setup: the SP-1 permission approve/deny must be OFF (it does nothing for a
        --dangerously-skip-permissions user, and we must not auto-type past real
        prompts), and the input backend must be tmux (we need an EXACT pane, never a
        focused window)."""
        if not (self.cfg.get("autopilot") or {}).get("enabled", False):
            return False
        if (self.cfg.get("permission") or {}).get("enabled", True):
            return False
        if (self.cfg.get("input") or {}).get("backend", "tmux") != "tmux":
            return False
        return True

    def arm_autopilot(self, sid, rate):
        """Arm a session for autopilot at `rate` seconds (rate<=0 disarms). No-op unless
        _autopilot_allowed(). Re-arming an already-armed session PRESERVES its drip count
        (so a held/jittering fader can't reset the deadman); a session the deadman parked
        cannot re-arm until it is first disarmed (fader to 0)."""
        if not self._autopilot_allowed():
            return False
        with self.lock:
            if sid not in self.sessions:
                return False
            if rate <= 0:
                self.autopilot.pop(sid, None)
                self.autopilot_parked.discard(sid)   # fader back to 0 clears the park
                return True
            if sid in self.autopilot_parked:
                return False                         # parked by deadman; needs a 0 first
            existing = self.autopilot.get(sid)
            self.autopilot[sid] = {"rate": rate, "drips": existing["drips"] if existing else 0}
            return True

    def human_touch(self, sid):
        """A genuine human prompt (UserPromptSubmit) resets the deadman counter and
        clears any park: you are back in the loop."""
        with self.lock:
            ap = self.autopilot.get(sid)
            if ap:
                ap["drips"] = 0
            self.autopilot_parked.discard(sid)

    def autopilot_tick(self, now, nudge):
        """Drip the nudge to armed, STOPPED sessions past their cadence. Guardrails: an
        awaiting-a-human session is skipped (never type over a real question, even after
        a following Stop); a session with no exact pane is disarmed (never guess a
        target); the deadman cap parks + hard-alerts after too many hands-off continues."""
        if (self.cfg.get("input") or {}).get("backend", "tmux") != "tmux":
            return []                               # drip only via an exact tmux pane
        deadman = (self.cfg.get("autopilot") or {}).get("deadman", 8)
        fires = []
        with self.lock:
            for sid, ap in list(self.autopilot.items()):
                s = self.sessions.get(sid)
                if not s:
                    self.autopilot.pop(sid, None)
                    continue
                if s.get("awaiting"):
                    continue                        # a real question pauses the drip
                stopped_at = s.get("stopped_at")
                if stopped_at is None or now - stopped_at < ap["rate"]:
                    continue                        # not stopped, or not past cadence yet
                pane = s.get("pane")
                if not pane:
                    self.autopilot.pop(sid, None)   # no exact target -> disarm, never guess
                    log("autopilot DISARMED %s (no pane)" % sid[:8])
                    continue
                if ap["drips"] >= deadman:
                    self.autopilot.pop(sid, None)
                    self.autopilot_parked.add(sid)
                    s["state"], s["t"], s["awaiting"] = "needs", now, True   # hard alert
                    log("autopilot PARKED %s (deadman)" % sid[:8])
                    continue
                ap["drips"] += 1
                s["stopped_at"] = now               # reset cadence; cleared when it resumes
                fires.append((pane, s.get("cwd")))
        for pane, cwd in fires:                     # send outside the lock (tmux only)
            send_keys_to(pane, cwd, *nudge)
        return fires

    def led_mask(self, now):
        """8-bit mask of which LEDs should be lit right now (handles blink/done)."""
        lights = self.cfg["lights"]
        blink_hz, done_hold = lights["blink_hz"], lights["done_hold_s"]
        whole_row = lights.get("whole_row", False)
        with self.lock:
            mask = 0
            blink_on = int(now * blink_hz * 2) % 2 == 0
            # calm dial (cockpit): needs-you always shows; done shows above the floor;
            # steady working shows only in the upper half. Non-cockpit = show all.
            show_done = (not self.cockpit) or self.calm >= 1
            show_working = (not self.cockpit) or self.calm >= 64
            breathe_on = int(now) % 2 == 0          # ~0.5 Hz slow pulse for autopilot
            for sid, s in self.sessions.items():
                st, led = s["state"], s["led"]
                if led is None:
                    continue                     # off-board cockpit session: no LED
                on = ((st == "working" and show_working)
                      or (st == "needs" and blink_on)
                      or (st == "done" and show_done and now - s["t"] < done_hold))
                if st == "done" and now - s["t"] >= done_hold:
                    s["state"] = "idle"
                if self.cockpit and sid in self.autopilot:   # self-driving -> slow pulse
                    on = on or breathe_on
                if on:
                    mask |= 0x0F if (self.single and whole_row) else (1 << led)
            return mask

    def any_sessions(self):
        with self.lock:
            return bool(self.sessions)


# --------------------------------------------------------------------------- hooks -> state
_pane_session = {}   # tmux pane id -> session name (cached; a pane's session is stable)

def tmux_session_of(pane):
    if not pane:
        return None
    if pane not in _pane_session:
        try:
            out = subprocess.run(
                ["tmux", "display-message", "-p", "-t", pane, "#{session_name}"],
                capture_output=True, text=True, timeout=2).stdout.strip()
        except Exception:
            out = ""
        _pane_session[pane] = out or None
    return _pane_session[pane]


def apply_hook(state, event, pane=None):
    name = event.get("hook_event_name") or event.get("event") or ""
    sid = event.get("session_id") or "default"
    cwd = event.get("cwd") or ""
    tmux = tmux_session_of(pane)   # the hook carried $TMUX_PANE; resolve its tmux session
    if name == "SessionEnd":
        state.end(sid)
        log("hook", name, sid[:8])
        return
    st = state.cfg["lights"]["events"].get(name)
    if st:
        state.set_state(sid, cwd, st, pane=pane, tmux=tmux)
        if name == "UserPromptSubmit":
            state.human_touch(sid)       # a real human prompt resets the autopilot deadman
        log("hook", name, sid[:8], (tmux or cwd or "?"), "->", st)


# --------------------------------------------------------------------------- tmux input
def find_pane(cwd):
    """Resolve the tmux pane running `claude` for a given working dir (best match)."""
    try:
        out = subprocess.run(
            ["tmux", "list-panes", "-a", "-F",
             "#{pane_id}\t#{pane_current_path}\t#{pane_current_command}"],
            capture_output=True, text=True, timeout=2).stdout
    except Exception:
        return None
    claude_panes = []
    for line in out.splitlines():
        parts = line.split("\t")
        if len(parts) != 3:
            continue
        pid, path, cmd = parts
        is_claude = ("claude" in cmd.lower()) or ("node" in cmd.lower())
        if cwd and path == cwd and is_claude:
            return pid                      # exact cwd + running claude = the one
        if is_claude:
            claude_panes.append(pid)
    return claude_panes[0] if claude_panes else None


def tmux_focus(tmuxname, pane):
    """Bring a session to the foreground (cockpit 'jump'). Resolve the target session
    FRESH from the pane -- the cached name goes stale the moment you rename a tmux
    session, but pane ids survive renames, so a rename must never break the jump. Fall
    back to the cached name only when the pane is gone, and surface a real failure
    instead of logging a success that didn't happen."""
    target = None
    if pane:
        try:
            target = subprocess.run(
                ["tmux", "display-message", "-p", "-t", pane, "#{session_name}"],
                capture_output=True, text=True, timeout=2).stdout.strip() or None
        except Exception:
            target = None
    target = target or tmuxname
    if not target:
        return
    try:
        r = subprocess.run(["tmux", "switch-client", "-t", target],
                           capture_output=True, text=True, timeout=2)
        if r.returncode != 0:
            log("jump failed -> %s: %s" % (target, (r.stderr or "").strip() or "?"))
            return
        if pane:
            subprocess.run(["tmux", "select-pane", "-t", pane], timeout=2)
        log("jump -> %s" % target)
    except Exception as e:
        log("jump error:", e)


def send_keys_to(pane, cwd, *keys):
    target = pane or find_pane(cwd)   # the exact pane the hook gave us, else best-match by cwd
    if not target:
        log("input: no target pane (cwd=%s)" % (cwd or "?"))
        return
    try:
        subprocess.run(["tmux", "send-keys", "-t", target, *keys], timeout=2)
        log("input -> pane %s: %s" % (target, " ".join(keys)))
    except Exception as e:
        log("input error:", e)


# Tmux key name -> macOS key code, for the osascript / focused-window backend.
OSA_KEYCODE = {
    "Enter": 36, "Return": 36, "Escape": 53, "Esc": 53, "Tab": 48, "BTab": 48,
    "Space": 49, "BSpace": 51, "Up": 126, "Down": 125, "Left": 123, "Right": 124,
    "PageUp": 116, "PageDown": 121, "Home": 115, "End": 119, "Delete": 117,
}
_OSA_MOD = {"C": "control down", "M": "option down", "S": "shift down"}


def _osa_fragment(key):
    if key in OSA_KEYCODE:
        return "key code %d" % OSA_KEYCODE[key]
    if len(key) >= 3 and key[1] == "-" and key[0] in _OSA_MOD:   # C-c, M-x, S-Tab ...
        base = key[2:]
        return 'keystroke "%s" using %s' % (base.replace('"', '\\"'), _OSA_MOD[key[0]])
    return 'keystroke "%s"' % key.replace("\\", "\\\\").replace('"', '\\"')


def send_osascript(keys):
    """Type into the FOCUSED window via macOS System Events (no tmux; needs the
    Accessibility permission). Focus-based, so it can't target a specific session."""
    script = ("tell application \"System Events\"\n"
              + "\n".join(_osa_fragment(k) for k in keys) + "\nend tell")
    try:
        subprocess.run(["osascript", "-e", script], timeout=3)
        log("input -> focused (osascript): %s" % " ".join(keys))
    except Exception as e:
        log("osascript error:", e)


def run_button_action(state, action):
    """Perform a configured button action: a list of tmux keys, {"shell": cmd}, or
    null/[] = nothing."""
    if not action:
        return
    pane, cwd = state.active_target()
    if isinstance(action, dict) and action.get("shell"):
        # shell=True is DELIBERATE: `action["shell"]` is a command the user bound to
        # this button in their own feldd_cc.config.json (like a tmux keybinding or a
        # launcher hotkey). The config file is trusted input at the same level as
        # this script, not untrusted network/user input, so this is a feature, not
        # an injection sink. Bind only commands you would run yourself.
        try:
            subprocess.run(action["shell"], shell=True, cwd=cwd or None, timeout=10)  # noqa: S602
            log("input -> shell:", action["shell"])
        except Exception as e:
            log("shell action error:", e)
        return
    if isinstance(action, list):
        backend = (state.cfg.get("input") or {}).get("backend", "tmux")
        if backend == "none":
            return
        if backend == "osascript":
            send_osascript(action)
        else:
            send_keys_to(pane, cwd, *action)


def handle_button(state, ix, pressed=True):
    # All controls act on press. There are NO held-button modifiers: PLAY and Track 1-4
    # are decoded from a single resistor ladder, so the hardware physically can't report
    # two of them held at once (a Play+Track "shift" would emit Play-release THEN
    # Track-press, never both). Sessions 5-8 are reached via the fader-2 scrubber and Vol+.
    if not pressed:
        return

    # Track 1-4 select / jump. In cockpit, selecting also brings the session forward.
    if TRK1 <= ix <= TRK4 and not state.single:
        led = ix - TRK1
        sid = state.select_by_led(led)
        if state.cockpit and sid:           # jump: bring that session to the foreground
            state.focus(sid)                # refresh its recency (keeps its front slot)
            tmux_focus(*state.jump_info(sid))
            state.reset_fader_grab()        # re-pick-up faders for the new focus
        log("select led %d -> %s" % (led, (sid or "none")[:8]))
        return
    # Cockpit Vol+ = jump to the next session that needs you (walk the queue).
    if state.cockpit and ix == VOLU:
        sid = state.next_needs()
        if sid:
            state.focus(sid)                # promote it onto a front button
            tmux_focus(*state.jump_info(sid))
            state.reset_fader_grab()
        log("next-needs -> %s" % ((sid or "none")[:8]))
        return
    # Permission resolve for non-cockpit Play (cockpit Play is handled above on release).
    if ix == PLAY and state.resolve_active("allow"):
        log("permission: ALLOW")
        return
    if ix == VOLD and state.resolve_active("deny"):
        log("permission: DENY")
        return
    name = IX_BTN.get(ix)
    if name:
        run_button_action(state, state.cfg["buttons"].get(name))


# --------------------------------------------------------------------------- faders (cockpit)
def handle_fader(state, ix, v):
    """Route a fader move (ix 0-3, value 0-127) to its configured job. Cockpit only."""
    if not state.cockpit:
        return
    f = state.cfg.get("faders") or {}
    if ix == f.get("scroll"):
        fader_scroll(state, v)
    elif ix == f.get("scrubber"):
        fader_scrubber(state, v)
    elif ix == f.get("calm"):
        fader_calm(state, v)
    elif ix == (f.get("assign") or {}).get("ix"):
        fader_assign(state, v)


def tmux_scroll(pane, pct):
    """Scroll a pane to pct (0-100) of its scrollback: 0 = live bottom, 100 = oldest
    line. (copy-mode mechanics best-effort, to be verified on-device; stubbed in tests.)"""
    try:
        if pct <= 0:
            subprocess.run(["tmux", "send-keys", "-t", pane, "-X", "cancel"], timeout=2)
            return
        size = subprocess.run(["tmux", "display", "-p", "-t", pane, "#{history_size}"],
                              capture_output=True, text=True, timeout=2).stdout.strip()
        line = int(int(size or 0) * (100 - pct) / 100)   # copy-mode line 0 = top of history
        subprocess.run(["tmux", "copy-mode", "-t", pane], timeout=2)
        subprocess.run(["tmux", "send-keys", "-t", pane, "-X", "history-top"], timeout=2)
        if line:
            subprocess.run(["tmux", "send-keys", "-t", pane, "-X", "-N", str(line),
                            "cursor-down"], timeout=2)
        log("scroll %s -> %d%%" % (pane, pct))
    except Exception as e:
        log("scroll error:", e)


def fader_scroll(state, v):
    """Fader 1: absolute scroll position of the focused session (0-127 -> 0-100%)."""
    ix = (state.cfg.get("faders") or {}).get("scroll", 0)
    val = state.fader_grab(ix, v)
    if val is None:
        return                       # not picked up yet
    pane, _ = state.active_target()
    if not pane:
        return
    tmux_scroll(pane, round(val * 100 / 127))


def fader_scrubber(state, v, ix=None):
    """Fader 2 (or fader-4 'rotate' preset): scrub focus across all sessions. ix is the
    physical fader whose pickup we track (so 'rotate' on fader 4 tracks fader 4)."""
    f = state.cfg.get("faders") or {}
    if ix is None:
        ix = f.get("scrubber", 1)
    val = state.fader_grab(ix, v)
    if val is None:
        return
    sid = state.session_at_fraction(val / 127.0)
    if sid:
        state.focus(sid)                             # seat it on a front button as you reach it
        tmux_focus(*state.jump_info(sid))
        state.reset_fader_grab(f.get("scroll", 0))   # new focus -> re-pick-up scroll only


def fader_coarse(state, v, ix):
    """Fader-4 'coarse read' preset: snap the focused session's scroll to deciles, the
    coarse companion to fader 1's fine scroll. (Tool-call-mark jumps are a refinement.)"""
    val = state.fader_grab(ix, v)
    if val is None:
        return
    pane, _ = state.active_target()
    if pane:
        tmux_scroll(pane, round(val * 10 / 127) * 10)


def fader_custom(state, v, action):
    """Fader-4 'custom' preset: fire a configured action (key list / {"shell":..}) when
    the fader reaches the top of its throw. The assignable escape hatch."""
    if action and v >= 120:
        run_button_action(state, action)


def fader_calm(state, v):
    """Fader 3: board sensitivity. 0 = only needs-you sessions stay lit (busy ones go
    dark); full = show every state. One knob for 'leave me alone' vs 'show me all'."""
    ix = (state.cfg.get("faders") or {}).get("calm", 2)
    val = state.fader_grab(ix, v)
    if val is None:
        return
    with state.lock:                # led_mask reads calm under the lock; write under it too
        state.calm = val


def fader_autopilot(state, v, ix=3):
    """Fader-4 'autopilot' preset: arm the FOCUSED session at the cadence step for v
    (0 = disarm). Self-driving: the daemon then drips the nudge while it's stopped."""
    val = state.fader_grab(ix, v)
    if val is None:
        return
    steps = (state.cfg.get("autopilot") or {}).get("steps_s", [0, 90, 30, 10])
    rate = steps[min(len(steps) - 1, int(val / 128.0 * len(steps)))]
    sid = state.active                  # snapshot once (arm re-validates under the lock)
    if sid:
        state.arm_autopilot(sid, rate)


def fader_assign(state, v):
    """Fader 4: assignable. Route the move to the configured preset."""
    a = (state.cfg.get("faders") or {}).get("assign") or {}
    ix = a.get("ix", 3)
    preset = a.get("preset", "coarse")   # safe default (matches DEFAULTS); autopilot is opt-in
    if preset == "autopilot":
        fader_autopilot(state, v, ix)
    elif preset == "coarse":
        fader_coarse(state, v, ix)
    elif preset == "rotate":
        fader_scrubber(state, v, ix)
    elif preset == "custom":
        fader_custom(state, v, a.get("action"))


# --------------------------------------------------------------------------- SP-1 link
# The daemon talks to the SP-1 only through the sp1_console transport seam, so the
# same code runs over USB (SerialTransport, today) or BLE (BleTransport, wireless).
from sp1_console import protocol as _proto
from sp1_console.transport import SerialTransport


class Device:
    """Thin adapter over a Transport, preserving the daemon's push_mask / release /
    read_loop / released surface. Everything above the seam is unchanged."""

    def __init__(self, port_glob, dry, transport=None):
        self.dry = dry
        self.last_mask = -1
        self.released = False
        self._t = None
        if dry:
            log("dry-run: not opening a transport")
            return
        self._t = transport or SerialTransport(port_glob)

    def write(self, obj):
        if self.dry or self._t is None:
            log("DEV<-", _proto.serialize(obj).decode())
            return
        self._t.send(obj)

    def push_mask(self, mask):
        if mask == self.last_mask:
            return
        self.last_mask = mask
        self.write(_proto.led(mask))

    def release(self):
        if not self.released:
            self.write(_proto.led_release())
            self.released, self.last_mask = True, -1

    def _dispatch(self, obj, state):
        ev = _proto.parse_event(obj)
        if isinstance(ev, _proto.ButtonEvent):
            handle_button(state, ev.ix, ev.pressed)
        elif isinstance(ev, _proto.FaderEvent):
            handle_fader(state, ev.ix, ev.value)

    def read_loop(self, state):
        """Register the inbound dispatch and bring the link up. The transport owns
        framing + its own reader/reconnect thread, so this returns once connected;
        the initial open is retried so a later plug-in comes online."""
        if self.dry or self._t is None:
            return
        self._t.on_event(lambda obj: self._dispatch(obj, state))
        self._t.on_link(self._on_link)
        delay = 0.5
        while not self._t.connect():
            time.sleep(delay)
            delay = min(delay * 2, 10.0)

    def _on_link(self, state_str):
        log("SP-1 link", state_str)
        if state_str == "up":
            # the device forgot our LED override across the drop (a fresh USB link,
            # or the firmware's bit4-fall auto-release over BLE), so force a full
            # re-render: reset the dedupe baseline and un-latch release. int assign
            # is atomic enough for the reader-thread -> render-thread handoff.
            self.last_mask = -1
            self.released = False


# --------------------------------------------------------------------------- HTTP hook server
def make_handler(state):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass

        def _send_json(self, body=""):
            data = body.encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_POST(self):
            parsed = urlparse(self.path)
            pane = (parse_qs(parsed.query).get("pane") or [None])[0] or None
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n) if n else b"{}"
            try:
                event = json.loads(body or b"{}")
            except Exception as e:
                log("parse error:", e); event = {}
            if parsed.path == "/await":
                self._await(event, pane)          # PermissionRequest: hold open for Play/Vol-
            else:
                self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
                apply_hook(state, event, pane)    # /hook: reply instantly, drive the lights

        def _await(self, event, pane):
            perm = state.cfg.get("permission") or {}
            sid = event.get("session_id") or "default"
            if not perm.get("enabled", True):
                self._send_json("")               # hardware approval off -> normal prompt
                return
            slot = state.await_decision(sid, event.get("cwd") or "", pane, tmux_session_of(pane))
            log("permission await:", sid[:8], event.get("tool_name", "?"))
            if slot["ev"].wait(timeout=float(perm.get("timeout_s", 90))) and slot["result"]:
                self._send_json(json.dumps({"hookSpecificOutput": {
                    "hookEventName": "PermissionRequest",
                    "decision": {"behavior": slot["result"]}}}))
            else:
                state.cancel_pending(sid)          # timed out -> fall through to the prompt
                self._send_json("")

        def do_GET(self):
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
    return H


# --------------------------------------------------------------------------- main
# The custom feldd wireless-console GATT service (unadvertised; opened post-connect).
CONSOLE_SVC_UUID = "fe1ddcc0-0001-4b1e-9c81-1a2b3c4d5e01"
CONSOLE_RX_UUID = "fe1ddcc0-0002-4b1e-9c81-1a2b3c4d5e01"
CONSOLE_TX_UUID = "fe1ddcc0-0003-4b1e-9c81-1a2b3c4d5e01"


def build_transport(args):
    """Pick the transport from args. Default: USB (return None -> Device uses
    SerialTransport). --ble: wireless. On macOS the SP-1 is normally PAIRED in System
    Settings (kept as a BT keyboard/MIDI device, so it does NOT advertise), so BLE
    uses CoreBluetooth's retrieveConnectedPeripherals to attach to the held device and
    open the console service on the same link. On other platforms it falls back to the
    bleak scan path (BleTransport)."""
    if not args.ble:
        return None
    if sys.platform == "darwin":
        from sp1_console.cb_transport import CoreBluetoothTransport
        log("transport: BLE via macOS CoreBluetooth (pair the SP-1 in System Settings first)")
        return CoreBluetoothTransport(service_uuid=CONSOLE_SVC_UUID, rx_uuid=CONSOLE_RX_UUID,
                                      tx_uuid=CONSOLE_TX_UUID, name_hint=args.ble_name)
    from sp1_console.transport import BleTransport
    log("transport: BLE via bleak scan")
    return BleTransport(name=args.ble_name, service_uuid=CONSOLE_SVC_UUID,
                        rx_uuid=CONSOLE_RX_UUID, tx_uuid=CONSOLE_TX_UUID)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("FELDD_CC_PORT", "9200")))
    ap.add_argument("--serial-glob", default=os.environ.get("FELDD_PORT", "/dev/cu.usbmodem*"))
    ap.add_argument("--config", default=None, help="path to feldd_cc.config.json")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--ble", action="store_true",
                    help="use Bluetooth (macOS: attach to the SP-1 paired in System Settings)")
    ap.add_argument("--ble-name", default="feldd", help="BLE device name hint (default: feldd)")
    args = ap.parse_args()

    global CFG
    CFG = load_config(args.config)
    log("session mode:", CFG["sessions"]["mode"])
    state = State(CFG)
    dev = Device(args.serial_glob, args.dry_run, transport=build_transport(args))

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(state))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log("hook server on http://127.0.0.1:%d/hook" % args.port)
    threading.Thread(target=dev.read_loop, args=(state,), daemon=True).start()

    period = 1.0 / RENDER_HZ
    try:
        while True:
            if state.any_sessions():
                dev.released = False
                now = time.time()
                state.autopilot_tick(now, CFG["autopilot"].get("nudge", ["continue", "Enter"]))
                dev.push_mask(state.led_mask(now))
            else:
                dev.release()
            time.sleep(period)
    except KeyboardInterrupt:
        dev.release()
        log("bye")


if __name__ == "__main__":
    main()
