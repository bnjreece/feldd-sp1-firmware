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

try:
    import serial  # pyserial
except ImportError:
    serial = None

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
        # the remaining LEDs. e.g. {"~/bnjmn/iamkeen": 0, "~/bnjmn/feldd-sp-1": 1}
        "assign": {},
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
            if 0 <= led < NUM_TRACK_LEDS:
                self.assign.append((key, _norm(key), led))
        self.pinned = {led for _, _, led in self.assign}
        self.lock = threading.Lock()
        self.sessions = {}        # sid -> {"led": int, "state": str, "cwd": str, "t": float}
        self.free = [0] if self.single else [i for i in range(NUM_TRACK_LEDS) if i not in self.pinned]
        self.active = None        # sid the buttons currently drive

    def _ensure(self, sid, cwd, tmux=None):
        s = self.sessions.get(sid)
        if s:
            if cwd:
                s["cwd"] = cwd
            return s
        if self.single:
            led = 0                       # all sessions share LED 0 in single mode
        else:
            led = self._assigned_led(cwd, tmux)  # a pinned session always gets its LED
            if led is None:
                led = self.free.pop(0) if self.free else min(
                    self.sessions.values(), key=lambda x: x["t"])["led"]
        s = {"led": led, "state": "idle", "cwd": cwd or "", "pane": None, "tmux": tmux, "t": time.time()}
        self.sessions[sid] = s
        return s

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

    def set_state(self, sid, cwd, st, pane=None, tmux=None):
        with self.lock:
            s = self._ensure(sid, cwd, tmux)
            if pane:
                s["pane"] = pane
            if tmux:
                s["tmux"] = tmux
            s["state"], s["t"] = st, time.time()
            if st == "needs":            # auto-focus the session that wants you
                self.active = sid
            elif self.active is None:
                self.active = sid

    def end(self, sid):
        with self.lock:
            s = self.sessions.pop(sid, None)
            if s and not self.single and s["led"] not in self.free and s["led"] not in self.pinned:
                self.free.append(s["led"])
                self.free.sort()
            if self.active == sid:
                self.active = next(iter(self.sessions), None)

    def select_by_led(self, led):
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

    def led_mask(self, now):
        """8-bit mask of which LEDs should be lit right now (handles blink/done)."""
        lights = self.cfg["lights"]
        blink_hz, done_hold = lights["blink_hz"], lights["done_hold_s"]
        whole_row = lights.get("whole_row", False)
        with self.lock:
            mask = 0
            blink_on = int(now * blink_hz * 2) % 2 == 0
            for s in self.sessions.values():
                st, led = s["state"], s["led"]
                on = (st == "working"
                      or (st == "needs" and blink_on)
                      or (st == "done" and now - s["t"] < done_hold))
                if st == "done" and now - s["t"] >= done_hold:
                    s["state"] = "idle"
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
        send_keys_to(pane, cwd, *action)


def handle_button(state, ix):
    # Track 1-4 select which session the controls target (multi mode only).
    if TRK1 <= ix <= TRK4 and not state.single:
        sid = state.select_by_led(ix - TRK1)
        log("select led %d -> %s" % (ix - TRK1, (sid or "none")[:8]))
        return
    name = IX_BTN.get(ix)
    if name:
        run_button_action(state, state.cfg["buttons"].get(name))


# --------------------------------------------------------------------------- SP-1 CDC link
class Device:
    def __init__(self, port_glob, dry):
        self.dry = dry
        self.ser = None
        self.last_mask = -1
        self.released = False
        if dry:
            log("dry-run: not opening a serial port")
            return
        if serial is None:
            log("pyserial not installed (pip install pyserial); falling back to dry-run")
            self.dry = True
            return
        ports = sorted(glob.glob(port_glob))
        if not ports:
            log("no SP-1 serial port matching %s; falling back to dry-run" % port_glob)
            self.dry = True
            return
        self.ser = serial.Serial(ports[0], 115200, timeout=0.1)
        log("opened SP-1 at", ports[0])
        self.write({"t": "monset", "on": True})   # ask feldd for control events

    def write(self, obj):
        line = json.dumps(obj, separators=(",", ":"))
        if self.dry:
            log("DEV<-", line)
            return
        try:
            self.ser.write(line.encode())
        except Exception as e:
            log("serial write error:", e)

    def push_mask(self, mask):
        if mask == self.last_mask:
            return
        self.last_mask = mask
        self.write({"t": "led", "mask": mask})

    def release(self):
        if not self.released:
            self.write({"t": "led", "release": True})
            self.released, self.last_mask = True, -1

    def read_loop(self, state):
        """Brace-frame JSON objects off the CDC stream and dispatch button events."""
        if self.dry or self.ser is None:
            return
        buf, depth, instr, esc = bytearray(), 0, False, False
        while True:
            try:
                chunk = self.ser.read(64)
            except Exception as e:
                log("serial read error:", e); time.sleep(0.5); continue
            for b in chunk:
                c = chr(b)
                buf.append(b)
                if instr:
                    if esc: esc = False
                    elif c == "\\": esc = True
                    elif c == '"': instr = False
                    continue
                if c == '"': instr = True
                elif c == "{": depth += 1
                elif c == "}":
                    depth -= 1
                    if depth == 0:
                        try:
                            obj = json.loads(bytes(buf).decode("utf-8", "ignore"))
                            if obj.get("t") == "mon" and obj.get("k") == "b" and obj.get("s") == 1:
                                handle_button(state, int(obj.get("ix", -1)))
                        except Exception:
                            pass
                        buf.clear()
                if depth == 0 and c not in "{} \r\n\t":
                    buf.clear()           # resync on junk between objects


# --------------------------------------------------------------------------- HTTP hook server
def make_handler(state):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass
        def do_POST(self):
            pane = (parse_qs(urlparse(self.path).query).get("pane") or [None])[0]
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n) if n else b"{}"
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
            try:
                apply_hook(state, json.loads(body or b"{}"), pane or None)
            except Exception as e:
                log("hook parse error:", e)
        def do_GET(self):
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
    return H


# --------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("FELDD_CC_PORT", "9200")))
    ap.add_argument("--serial-glob", default=os.environ.get("FELDD_PORT", "/dev/cu.usbmodem*"))
    ap.add_argument("--config", default=None, help="path to feldd_cc.config.json")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    global CFG
    CFG = load_config(args.config)
    log("session mode:", CFG["sessions"]["mode"])
    state = State(CFG)
    dev = Device(args.serial_glob, args.dry_run)

    httpd = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(state))
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    log("hook server on http://127.0.0.1:%d/hook" % args.port)
    threading.Thread(target=dev.read_loop, args=(state,), daemon=True).start()

    period = 1.0 / RENDER_HZ
    try:
        while True:
            if state.any_sessions():
                dev.released = False
                dev.push_mask(state.led_mask(time.time()))
            else:
                dev.release()
            time.sleep(period)
    except KeyboardInterrupt:
        dev.release()
        log("bye")


if __name__ == "__main__":
    main()
