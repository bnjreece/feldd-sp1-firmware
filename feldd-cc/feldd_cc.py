#!/usr/bin/env python3
"""feldd-cc: bridge the SP-1 (feldd) to Claude Code as a physical console.

Claude Code hooks  --HTTP-->  this daemon  --CDC JSON-->  SP-1 LEDs   (state out)
SP-1 buttons       --CDC JSON-->  this daemon  --tmux send-keys-->  Claude pane  (input in)

Run:  python3 feldd_cc.py            (auto-detect the SP-1 CDC port)
      FELDD_PORT=/dev/cu.usbmodemXXXX python3 feldd_cc.py
      python3 feldd_cc.py --dry-run  (no device; logs the LED/input logic)

Hooks: merge hooks.settings.json into ~/.claude/settings.json (POSTs events to :9200/hook).
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

try:
    import serial  # pyserial
except ImportError:
    serial = None

NUM_TRACK_LEDS = 4            # the 4 reliable front LEDs (ix 0..3); side LEDs (4..7) left off in v1
BLINK_HZ = 3.0               # "needs you" blink rate
DONE_HOLD_S = 0.6            # how long the "done" flash stays lit
RENDER_HZ = 20.0
# button indices from feldd's monitor stream
PLAY, TRK1, TRK4, VOLU, VOLD, FWD, RWD = 0, 1, 4, 5, 6, 7, 8

log = lambda *a: print(time.strftime("%H:%M:%S"), *a, file=sys.stderr, flush=True)


# ----------------------------------------------------------------------------- state
class State:
    """Per-session LED state + which session the physical controls target."""
    def __init__(self):
        self.lock = threading.Lock()
        self.sessions = {}        # sid -> {"led": int, "state": str, "cwd": str, "t": float}
        self.free = list(range(NUM_TRACK_LEDS))
        self.active = None        # sid the buttons currently drive

    def _ensure(self, sid, cwd):
        s = self.sessions.get(sid)
        if s:
            if cwd:
                s["cwd"] = cwd
            return s
        led = self.free.pop(0) if self.free else min(
            self.sessions.values(), key=lambda x: x["t"])["led"]
        s = {"led": led, "state": "idle", "cwd": cwd or "", "t": time.time()}
        self.sessions[sid] = s
        return s

    def set_state(self, sid, cwd, st):
        with self.lock:
            s = self._ensure(sid, cwd)
            s["state"], s["t"] = st, time.time()
            if st == "needs":            # auto-focus the session that wants you
                self.active = sid
            elif self.active is None:
                self.active = sid

    def end(self, sid):
        with self.lock:
            s = self.sessions.pop(sid, None)
            if s and s["led"] not in self.free:
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

    def active_cwd(self):
        with self.lock:
            s = self.sessions.get(self.active)
            return s["cwd"] if s else None

    def led_mask(self, now):
        """8-bit mask of which LEDs should be lit right now (handles blink/done)."""
        with self.lock:
            mask = 0
            blink_on = int(now * BLINK_HZ * 2) % 2 == 0
            for s in self.sessions.values():
                st, led = s["state"], s["led"]
                on = (st == "working"
                      or (st == "needs" and blink_on)
                      or (st == "done" and now - s["t"] < DONE_HOLD_S))
                if st == "done" and now - s["t"] >= DONE_HOLD_S:
                    s["state"] = "idle"
                if on:
                    mask |= (1 << led)
            return mask

    def any_sessions(self):
        with self.lock:
            return bool(self.sessions)


# ----------------------------------------------------------------------------- hooks -> state
# Claude Code hook event name -> session state. (Verify real payloads via capture-hooks.sh first.)
EVENT_STATE = {
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
}

def apply_hook(state, event):
    name = event.get("hook_event_name") or event.get("event") or ""
    sid = event.get("session_id") or "default"
    cwd = event.get("cwd") or ""
    if name == "SessionEnd":
        state.end(sid)
        log("hook", name, sid[:8])
        return
    st = EVENT_STATE.get(name)
    if st:
        state.set_state(sid, cwd, st)
        log("hook", name, sid[:8], "->", st)


# ----------------------------------------------------------------------------- tmux input
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

def send_keys(cwd, *keys):
    pane = find_pane(cwd)
    if not pane:
        log("input: no claude tmux pane found (cwd=%s)" % (cwd or "?"))
        return
    try:
        subprocess.run(["tmux", "send-keys", "-t", pane, *keys], timeout=2)
        log("input -> pane %s: %s" % (pane, " ".join(keys)))
    except Exception as e:
        log("input error:", e)

BUTTON_KEYS = {
    PLAY: ["Enter"],         # approve / submit
    VOLD: ["Escape"],        # interrupt
    FWD:  ["PageDown"],      # scroll
    RWD:  ["PageUp"],
}

def handle_button(state, ix):
    if TRK1 <= ix <= TRK4:                  # track buttons select which session is active
        sid = state.select_by_led(ix - TRK1)
        log("select led %d -> %s" % (ix - TRK1, (sid or "none")[:8]))
        return
    keys = BUTTON_KEYS.get(ix)
    if keys:
        send_keys(state.active_cwd(), *keys)


# ----------------------------------------------------------------------------- SP-1 CDC link
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


# ----------------------------------------------------------------------------- HTTP hook server
def make_handler(state):
    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):  # quiet
            pass
        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0))
            body = self.rfile.read(n) if n else b"{}"
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
            try:
                apply_hook(state, json.loads(body or b"{}"))
            except Exception as e:
                log("hook parse error:", e)
        def do_GET(self):
            self.send_response(200); self.send_header("Content-Length", "0"); self.end_headers()
    return H


# ----------------------------------------------------------------------------- main
def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=int(os.environ.get("FELDD_CC_PORT", "9200")))
    ap.add_argument("--serial-glob", default=os.environ.get("FELDD_PORT", "/dev/cu.usbmodem*"))
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    state = State()
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
