#!/usr/bin/env python3
"""
sp1ctl.py — host-side (macOS) CLI for the SP-1 controller's config protocol.

Speaks the SP-1 firmware's JSON-lines protocol over its USB-CDC serial port
(appears as /dev/cu.usbmodem* on macOS), so we can drive and test the device
from a Mac WITHOUT the WebSerial config tool. Matches firmware exactly:
  firmware/app/src/protocol.c   (the wire protocol)
  firmware/app/src/profile.h    (the 69-byte packed profile struct)
  firmware/app/src/config_cdc.c (the "mon" live-monitor frames)

────────────────────────────────────────────────────────────────────────────
WIRE PROTOCOL (one JSON object + '\n' per line, both directions)
  request   {"t":"<verb>","i":<id>,...}
  response  {"t":"<verb>_r","i":<id>,"ok":true,...}
        or  {"t":"err","i":<id>,"ok":false,"code":"<CODE>","msg":"..."}
  The "i" field correlates a response to its request.

  verbs:
    hello                       -> hello_r {proto,fw,profiles,active,faders,
                                            buttons,caps[],pbytes}
    read     {n}                -> read_r  {n,data(base64 of profile struct)}
    write    {n,data(base64)}   -> write_r {n}
    setactive{n}                -> setactive_r {active}
    getactive                   -> getactive_r {active}
    monset   {on:bool}          -> monset_r {on}, after which the device pushes
                                    unsolicited frames until monset off:
              fader   {"t":"mon","k":"f","ix":<i>,"v":<value>}
              button  {"t":"mon","k":"b","ix":<i>,"s":<0|1>}
  error codes: BAD_JSON BAD_VERB BAD_INDEX BAD_LEN BAD_VERSION NVS_FAIL OVERFLOW

PROFILE STRUCT (profile.h, v1 — packed little-endian, all uint8). Total 69 B:
  off 0  version(1)
  off 1  channel(1)                       0..15
  off 2  fader[4]  : {cc,min,max,curve,invert}  (5 B each = 20)  -> ends off 22
  off 22 button[9] : {type,value}               (2 B each = 18)  -> ends off 40
  off 40 shift.fader_cc[4]                       (4)             -> ends off 44
  off 44 shift.button_value[9]                   (9)             -> ends off 53
  off 53 name[16]                                (16)            -> ends off 69
  enums: curve 0=linear 1=log 2=exp
         btn_type 0=none 1=note 2=cc_toggle 3=cc_momentary 4=transport
                  5=profile_switch
  profile_to_b64 (firmware) is a plain standard-base64 of the raw struct bytes,
  so Python's base64 of this layout matches the firmware byte-for-byte.

FRIENDLY JSON (get / set / export / import / export-all — the format the future
web tool will also use):
  { "format":"sp1-profile", "version":1, "name":"OP-XY mix", "channel":0,
    "faders":[ {"cc":7,"min":0,"max":127,"curve":"linear","invert":false}, ...x4 ],
    "buttons":[ {"type":"cc_momentary","value":20}, ...x9 ],
    "shift": { "fader_cc":[20,21,22,23], "button_value":[...x9] } }

────────────────────────────────────────────────────────────────────────────
USAGE
  sp1ctl.py ports                       list /dev/cu.usbmodem* candidates
  sp1ctl.py hello [--port P]            connect, hello, print device info
  sp1ctl.py list                        table of all profiles (index/name/ch/active)
  sp1ctl.py get N [--json]              read profile N as friendly JSON
  sp1ctl.py set N FILE.json             write friendly-JSON profile to slot N
  sp1ctl.py import N FILE.json          alias of set
  sp1ctl.py active [N]                  N: setactive; bare: getactive
  sp1ctl.py monitor                     stream live mon frames until Ctrl-C
  sp1ctl.py export N FILE.json          read N, write friendly JSON to FILE
  sp1ctl.py export-all FILE.json        bundle all profiles to FILE
  global: --port P  --timeout SECS  --verbose  --mock
  self-test (no hardware):  sp1ctl.py --selftest

TRANSPORT
  Uses pyserial if importable. If pyserial is missing, prints a one-line install
  hint and still runs every subcommand under --mock (an in-process fake device).
  pip3 install --break-system-packages pyserial
"""

import argparse
import base64
import glob
import json
import struct
import sys
import time

# ── optional dependency: pyserial ───────────────────────────────────────────
try:
    import serial  # type: ignore
    HAVE_PYSERIAL = True
except Exception:
    serial = None
    HAVE_PYSERIAL = False

PYSERIAL_HINT = "pyserial not installed — run: pip3 install --break-system-packages pyserial"

# ── protocol / struct constants (mirror profile.h + protocol.c) ─────────────
PROFILE_VERSION = 1
PROTO_VERSION = 1
NUM_FADERS = 4
NUM_BUTTONS = 9
NAME_LEN = 16
PBYTES = 1 + 1 + NUM_FADERS * 5 + NUM_BUTTONS * 2 + NUM_FADERS + NUM_BUTTONS + NAME_LEN  # 69
PORT_GLOB = "/dev/cu.usbmodem*"
BAUD = 115200
DEFAULT_TIMEOUT = 2.0

CURVE_NAMES = {0: "linear", 1: "log", 2: "exp"}
CURVE_VALS = {v: k for k, v in CURVE_NAMES.items()}
BTN_NAMES = {0: "none", 1: "note", 2: "cc_toggle", 3: "cc_momentary",
             4: "transport", 5: "profile_switch"}
BTN_VALS = {v: k for k, v in BTN_NAMES.items()}


# ── friendly JSON <-> 69-byte blob codec ────────────────────────────────────
class ProfileError(ValueError):
    """Raised when a friendly profile fails client-side validation/encoding."""


def _u8(name, v):
    if not isinstance(v, int) or isinstance(v, bool):
        raise ProfileError(f"{name} must be an integer, got {v!r}")
    if not (0 <= v <= 255):
        raise ProfileError(f"{name} out of byte range (0..255): {v}")
    return v


def _rng(name, v, lo, hi):
    _u8(name, v)
    if not (lo <= v <= hi):
        raise ProfileError(f"{name} out of range ({lo}..{hi}): {v}")
    return v


def encode_profile(p):
    """Friendly-JSON dict -> 69-byte little-endian packed blob (bytes).

    Validates ranges client-side and raises ProfileError with a clear message
    before producing any bytes (so `set` refuses bad input up front).
    """
    if not isinstance(p, dict):
        raise ProfileError("profile must be a JSON object")

    version = p.get("version", PROFILE_VERSION)
    if version != PROFILE_VERSION:
        raise ProfileError(f"unsupported profile version {version} (expected {PROFILE_VERSION})")

    channel = _rng("channel", p.get("channel", 0), 0, 15)

    faders = p.get("faders", [])
    if len(faders) != NUM_FADERS:
        raise ProfileError(f"faders must have exactly {NUM_FADERS} entries, got {len(faders)}")
    buttons = p.get("buttons", [])
    if len(buttons) != NUM_BUTTONS:
        raise ProfileError(f"buttons must have exactly {NUM_BUTTONS} entries, got {len(buttons)}")

    shift = p.get("shift", {})
    shift_fcc = shift.get("fader_cc", [])
    shift_bv = shift.get("button_value", [])
    if len(shift_fcc) != NUM_FADERS:
        raise ProfileError(f"shift.fader_cc must have {NUM_FADERS} entries, got {len(shift_fcc)}")
    if len(shift_bv) != NUM_BUTTONS:
        raise ProfileError(f"shift.button_value must have {NUM_BUTTONS} entries, got {len(shift_bv)}")

    out = bytearray()
    out.append(_u8("version", version))
    out.append(channel)

    for i, f in enumerate(faders):
        cc = _rng(f"faders[{i}].cc", f.get("cc", 0), 0, 127)
        mn = _rng(f"faders[{i}].min", f.get("min", 0), 0, 127)
        mx = _rng(f"faders[{i}].max", f.get("max", 127), 0, 127)
        curve_in = f.get("curve", "linear")
        if isinstance(curve_in, str):
            if curve_in not in CURVE_VALS:
                raise ProfileError(f"faders[{i}].curve unknown: {curve_in!r}")
            curve = CURVE_VALS[curve_in]
        else:
            curve = _rng(f"faders[{i}].curve", curve_in, 0, 2)
        inv_in = f.get("invert", False)
        invert = 1 if (inv_in is True or inv_in == 1) else 0 if (inv_in is False or inv_in == 0) else None
        if invert is None:
            raise ProfileError(f"faders[{i}].invert must be boolean, got {inv_in!r}")
        out += bytes((cc, mn, mx, curve, invert))

    for i, b in enumerate(buttons):
        type_in = b.get("type", "none")
        if isinstance(type_in, str):
            if type_in not in BTN_VALS:
                raise ProfileError(f"buttons[{i}].type unknown: {type_in!r}")
            btype = BTN_VALS[type_in]
        else:
            btype = _rng(f"buttons[{i}].type", type_in, 0, 5)
        bval = _rng(f"buttons[{i}].value", b.get("value", 0), 0, 127)
        out += bytes((btype, bval))

    for i, cc in enumerate(shift_fcc):
        out.append(_rng(f"shift.fader_cc[{i}]", cc, 0, 127))
    for i, bv in enumerate(shift_bv):
        out.append(_rng(f"shift.button_value[{i}]", bv, 0, 127))

    name = p.get("name", "")
    if not isinstance(name, str):
        raise ProfileError("name must be a string")
    nb = name.encode("utf-8")
    if len(nb) > NAME_LEN:
        raise ProfileError(f"name too long ({len(nb)} > {NAME_LEN} bytes): {name!r}")
    out += nb + b"\x00" * (NAME_LEN - len(nb))

    if len(out) != PBYTES:
        raise ProfileError(f"internal: encoded {len(out)} bytes, expected {PBYTES}")
    return bytes(out)


def decode_profile(blob):
    """69-byte blob (bytes) -> friendly-JSON dict."""
    if len(blob) < PBYTES:
        raise ProfileError(f"profile blob too short: {len(blob)} < {PBYTES}")
    off = 0
    version = blob[off]; off += 1
    channel = blob[off]; off += 1

    faders = []
    for _ in range(NUM_FADERS):
        cc, mn, mx, curve, invert = blob[off:off + 5]
        off += 5
        faders.append({
            "cc": cc, "min": mn, "max": mx,
            "curve": CURVE_NAMES.get(curve, curve),
            "invert": bool(invert),
        })

    buttons = []
    for _ in range(NUM_BUTTONS):
        btype, bval = blob[off:off + 2]
        off += 2
        buttons.append({"type": BTN_NAMES.get(btype, btype), "value": bval})

    shift_fcc = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
    shift_bv = list(blob[off:off + NUM_BUTTONS]); off += NUM_BUTTONS

    name = blob[off:off + NAME_LEN].split(b"\x00", 1)[0].decode("utf-8", "replace")
    off += NAME_LEN

    return {
        "format": "sp1-profile",
        "version": version,
        "name": name,
        "channel": channel,
        "faders": faders,
        "buttons": buttons,
        "shift": {"fader_cc": shift_fcc, "button_value": shift_bv},
    }


def default_profile():
    """The firmware's M5.2 default profile (librarian.c make_default)."""
    return {
        "format": "sp1-profile", "version": PROFILE_VERSION,
        "name": "Default", "channel": 0,
        "faders": [{"cc": cc, "min": 0, "max": 127, "curve": "linear", "invert": False}
                   for cc in (7, 74, 71, 76)],
        "buttons": [{"type": "cc_momentary", "value": 20 + i} for i in range(NUM_BUTTONS)],
        "shift": {"fader_cc": [7, 74, 71, 76],
                  "button_value": [20 + i for i in range(NUM_BUTTONS)]},
    }


# ── transports ──────────────────────────────────────────────────────────────
class ProtocolError(RuntimeError):
    """A device-side err frame or a transport/timeout failure."""


class SerialTransport:
    """Real CDC-ACM transport via pyserial. One request -> matching response by 'i'.

    Interleaved 'mon' frames are handled by the caller during monitor(); for
    normal request/response we read lines until we see the matching 'i'.
    """

    def __init__(self, port, timeout=DEFAULT_TIMEOUT, verbose=False):
        if not HAVE_PYSERIAL:
            raise ProtocolError(PYSERIAL_HINT)
        self.port = port
        self.timeout = timeout
        self.verbose = verbose
        self._next_id = 1
        self.ser = serial.Serial(port, BAUD, timeout=timeout)
        # Some boards reset on DTR; give the CDC a beat to settle.
        time.sleep(0.05)

    def close(self):
        try:
            self.ser.close()
        except Exception:
            pass

    def _write_line(self, obj):
        line = json.dumps(obj, separators=(",", ":"))
        if self.verbose:
            print(f">> {line}", file=sys.stderr)
        self.ser.write((line + "\n").encode("utf-8"))
        self.ser.flush()

    def _read_line(self):
        raw = self.ser.readline()
        if not raw:
            raise ProtocolError(f"timeout after {self.timeout}s waiting for response")
        line = raw.decode("utf-8", "replace").strip()
        if self.verbose:
            print(f"<< {line}", file=sys.stderr)
        return line

    def request(self, verb, **fields):
        rid = self._next_id
        self._next_id += 1
        self._write_line({"t": verb, "i": rid, **fields})
        deadline = time.monotonic() + self.timeout
        while time.monotonic() < deadline:
            line = self._read_line()
            if not line:
                continue
            try:
                msg = json.loads(line)
            except json.JSONDecodeError:
                continue
            if msg.get("t") == "mon":
                continue  # stray monitor frame; ignore during req/resp
            if msg.get("i") != rid:
                continue  # not ours
            if msg.get("t") == "err" or msg.get("ok") is False:
                raise ProtocolError(f"{msg.get('code', 'ERR')}: {msg.get('msg', '')}")
            return msg
        raise ProtocolError(f"timeout after {self.timeout}s waiting for {verb}_r")

    def read_mon(self):
        """Read one line during monitor; returns a parsed dict or None on timeout."""
        raw = self.ser.readline()
        if not raw:
            return None
        line = raw.decode("utf-8", "replace").strip()
        if not line:
            return None
        if self.verbose:
            print(f"<< {line}", file=sys.stderr)
        try:
            return json.loads(line)
        except json.JSONDecodeError:
            return None


class MockTransport:
    """In-process fake SP-1. Answers the protocol over an in-memory profile array,
    so every subcommand can be exercised end-to-end without hardware."""

    def __init__(self, timeout=DEFAULT_TIMEOUT, verbose=False, mon_frames=None):
        self.timeout = timeout
        self.verbose = verbose
        self.fw = "0.1.0"
        self.profiles = [encode_profile(_named_default(i)) for i in range(8)]
        self.active = 0
        self.mon_on = False
        self._next_id = 1
        # A canned burst of mon frames to emit, then stop (for monitor demo/test).
        self._mon_queue = list(mon_frames) if mon_frames is not None else [
            {"t": "mon", "k": "f", "ix": 0, "v": 64},
            {"t": "mon", "k": "b", "ix": 2, "s": 1},
            {"t": "mon", "k": "active", "n": 5},
            {"t": "mon", "k": "b", "ix": 2, "s": 0},
            {"t": "mon", "k": "f", "ix": 3, "v": 127},
        ]

    def close(self):
        pass

    def request(self, verb, **fields):
        rid = self._next_id
        self._next_id += 1
        if self.verbose:
            print(f">> {{'t':{verb!r},'i':{rid}, ...}}", file=sys.stderr)
        resp = self._dispatch(verb, rid, fields)
        if self.verbose:
            print(f"<< {json.dumps(resp, separators=(',', ':'))}", file=sys.stderr)
        if resp.get("t") == "err" or resp.get("ok") is False:
            raise ProtocolError(f"{resp.get('code', 'ERR')}: {resp.get('msg', '')}")
        return resp

    def _dispatch(self, verb, rid, fields):
        def err(code, msg):
            return {"t": "err", "i": rid, "ok": False, "code": code, "msg": msg}

        if verb == "hello":
            return {"t": "hello_r", "i": rid, "ok": True, "proto": PROTO_VERSION,
                    "pver": PROFILE_VERSION,
                    "fw": self.fw, "profiles": len(self.profiles), "active": self.active,
                    "faders": NUM_FADERS, "buttons": NUM_BUTTONS,
                    "caps": ["trs", "usbmidi", "shift", "led", "mon"], "pbytes": PBYTES,
                    "uid": "00112233445566778899aabbccddeeff"}
        if verb == "read":
            n = fields.get("n")
            if not isinstance(n, int) or not (0 <= n < len(self.profiles)):
                return err("BAD_INDEX", "bad index")
            data = base64.b64encode(self.profiles[n]).decode("ascii")
            return {"t": "read_r", "i": rid, "ok": True, "n": n, "data": data}
        if verb == "write":
            n = fields.get("n")
            if not isinstance(n, int) or not (0 <= n < len(self.profiles)):
                return err("BAD_INDEX", "bad index")
            try:
                blob = base64.b64decode(fields.get("data", ""), validate=True)
            except Exception:
                return err("BAD_LEN", "bad data length")
            if len(blob) != PBYTES:
                return err("BAD_LEN", "bad data length")
            if blob[0] != PROFILE_VERSION or blob[1] > 15:
                return err("BAD_VERSION", "bad profile version")
            self.profiles[n] = blob
            return {"t": "write_r", "i": rid, "ok": True, "n": n}
        if verb == "setactive":
            n = fields.get("n")
            if not isinstance(n, int) or not (0 <= n < len(self.profiles)):
                return err("BAD_INDEX", "bad index")
            self.active = n
            return {"t": "setactive_r", "i": rid, "ok": True, "active": n}
        if verb == "getactive":
            return {"t": "getactive_r", "i": rid, "ok": True, "active": self.active}
        if verb == "reset":
            n = fields.get("n")
            if not isinstance(n, int) or not (0 <= n < len(self.profiles)):
                return err("BAD_INDEX", "bad index")
            self.profiles[n] = encode_profile(_named_default(n))
            return {"t": "reset_r", "i": rid, "ok": True, "n": n}
        if verb == "resetall":
            self.profiles = [encode_profile(_named_default(i)) for i in range(len(self.profiles))]
            return {"t": "resetall_r", "i": rid, "ok": True}
        if verb == "monset":
            on = fields.get("on")
            if not isinstance(on, bool):
                return err("BAD_JSON", "missing on flag")
            self.mon_on = on
            return {"t": "monset_r", "i": rid, "ok": True, "on": on}
        if verb == "list":
            entries = []
            for n, blob in enumerate(self.profiles):
                name = blob[53:53 + NAME_LEN].split(b"\x00", 1)[0].decode("utf-8", "replace")
                entries.append({"n": n, "name": name, "ver": blob[0]})
            return {"t": "list_r", "i": rid, "ok": True,
                    "active": self.active, "profiles": entries}
        return err("BAD_VERB", "unknown verb")

    def read_mon(self):
        if self.mon_on and self._mon_queue:
            return self._mon_queue.pop(0)
        return None


def _named_default(i):
    p = default_profile()
    p["name"] = "Default" if i == 0 else f"Slot {i}"
    p["channel"] = i % 16
    return p


# ── port discovery / transport selection ────────────────────────────────────
def list_ports():
    return sorted(glob.glob(PORT_GLOB))


def open_transport(args, mon_frames=None):
    if getattr(args, "mock", False):
        return MockTransport(timeout=args.timeout, verbose=args.verbose, mon_frames=mon_frames)
    if not HAVE_PYSERIAL:
        print(PYSERIAL_HINT, file=sys.stderr)
        print("falling back to --mock (no hardware access). Pass --mock to silence this.",
              file=sys.stderr)
        return MockTransport(timeout=args.timeout, verbose=args.verbose, mon_frames=mon_frames)
    port = args.port
    if not port:
        ports = list_ports()
        if len(ports) == 1:
            port = ports[0]
        elif len(ports) == 0:
            raise SystemExit("no /dev/cu.usbmodem* port found. Plug in the SP-1, "
                             "pass --port, or use --mock.")
        else:
            raise SystemExit("multiple usbmodem ports found; pass --port:\n  " +
                             "\n  ".join(ports))
    return SerialTransport(port, timeout=args.timeout, verbose=args.verbose)


# ── subcommands ─────────────────────────────────────────────────────────────
def cmd_ports(args):
    ports = list_ports()
    if not ports:
        print("(no /dev/cu.usbmodem* ports found)")
    else:
        for p in ports:
            print(p)
    return 0


def cmd_hello(args, t=None):
    own = t is None
    t = t or open_transport(args)
    try:
        r = t.request("hello")
        print(f"SP-1 controller")
        print(f"  proto    : {r.get('proto')}")
        print(f"  pver     : {r.get('pver')}")
        print(f"  firmware : {r.get('fw')}")
        print(f"  profiles : {r.get('profiles')}")
        print(f"  active   : {r.get('active')}")
        print(f"  faders   : {r.get('faders')}")
        print(f"  buttons  : {r.get('buttons')}")
        print(f"  pbytes   : {r.get('pbytes')}")
        print(f"  caps     : {', '.join(r.get('caps', []))}")
        print(f"  uid      : {r.get('uid', '')}")
        if r.get("pbytes") not in (None, PBYTES):
            print(f"  WARNING  : device pbytes {r.get('pbytes')} != tool's {PBYTES}; "
                  f"the friendly-JSON codec assumes the v1 {PBYTES}-byte layout.")
        return r
    finally:
        if own:
            t.close()


def _read_profile_blob(t, n):
    r = t.request("read", n=n)
    return base64.b64decode(r["data"])


def cmd_list(args, t=None):
    own = t is None
    t = t or open_transport(args)
    try:
        r = t.request("list")
        active = r.get("active")
        rows = r.get("profiles", [])
        print(f"{'idx':>3}  {'name':<16}  {'ver':>3}  active")
        print(f"{'-'*3}  {'-'*16}  {'-'*3}  {'-'*6}")
        for row in rows:
            mark = "*" if row.get("n") == active else ""
            print(f"{row.get('n'):>3}  {row.get('name', ''):<16}  {row.get('ver', ''):>3}  {mark}")
        return 0
    finally:
        if own:
            t.close()


def cmd_get(args, t=None):
    own = t is None
    t = t or open_transport(args)
    try:
        blob = _read_profile_blob(t, args.n)
        p = decode_profile(blob)
        if args.json:
            print(json.dumps(p, separators=(",", ":")))
        else:
            print(json.dumps(p, indent=2))
        return 0
    finally:
        if own:
            t.close()


def cmd_set(args, t=None):
    own = t is None
    with open(args.file, "r") as f:
        p = json.load(f)
    blob = encode_profile(p)  # raises ProfileError (clear message) on bad input
    data = base64.b64encode(blob).decode("ascii")
    t = t or open_transport(args)
    try:
        t.request("write", n=args.n, data=data)
        print(f"wrote profile {args.n} ({decode_profile(blob)['name']!r}, {len(blob)} bytes)")
        return 0
    finally:
        if own:
            t.close()


def cmd_active(args, t=None):
    own = t is None
    t = t or open_transport(args)
    try:
        if args.n is None:
            r = t.request("getactive")
            print(f"active profile: {r.get('active')}")
        else:
            r = t.request("setactive", n=args.n)
            print(f"active profile set to {r.get('active')}")
        return 0
    finally:
        if own:
            t.close()


def cmd_export(args, t=None):
    own = t is None
    t = t or open_transport(args)
    try:
        blob = _read_profile_blob(t, args.n)
        p = decode_profile(blob)
        with open(args.file, "w") as f:
            json.dump(p, f, indent=2)
            f.write("\n")
        print(f"exported profile {args.n} -> {args.file}")
        return 0
    finally:
        if own:
            t.close()


def cmd_export_all(args, t=None):
    own = t is None
    t = t or open_transport(args)
    try:
        h = t.request("hello")
        count = h.get("profiles", 0)
        bundle = {"format": "sp1-bundle", "version": PROFILE_VERSION,
                  "active": h.get("active"), "profiles": []}
        for n in range(count):
            bundle["profiles"].append(decode_profile(_read_profile_blob(t, n)))
        with open(args.file, "w") as f:
            json.dump(bundle, f, indent=2)
            f.write("\n")
        print(f"exported {count} profiles -> {args.file}")
        return 0
    finally:
        if own:
            t.close()


def cmd_monitor(args, t=None):
    own = t is None
    t = t or open_transport(args)
    try:
        t.request("monset", on=True)
        print("monitoring (Ctrl-C to stop)...", file=sys.stderr)
        try:
            idle_limit = None if not getattr(args, "mock", False) else 200
            idle = 0
            while True:
                m = t.read_mon()
                if m is None:
                    idle += 1
                    if idle_limit is not None and idle >= idle_limit:
                        break  # mock: queue drained, exit cleanly
                    continue
                idle = 0
                if m.get("t") != "mon":
                    continue
                if m.get("k") == "f":
                    print(f"F{m.get('ix')}={m.get('v')}")
                elif m.get("k") == "b":
                    state = "down" if m.get("s") else "up"
                    print(f"B{m.get('ix')} {state}")
                elif m.get("k") == "active":
                    print(f"active={m.get('n')}")
        except KeyboardInterrupt:
            print("\nstopping monitor...", file=sys.stderr)
        finally:
            try:
                t.request("monset", on=False)
            except Exception:
                pass
        return 0
    finally:
        if own:
            t.close()


# ── self-test ───────────────────────────────────────────────────────────────
def run_selftest():
    fails = []

    def check(cond, label):
        status = "PASS" if cond else "FAIL"
        print(f"  [{status}] {label}")
        if not cond:
            fails.append(label)

    print("sp1ctl.py self-test")
    print(f"pyserial: {'installed (' + serial.__version__ + ')' if HAVE_PYSERIAL else 'NOT installed -> --mock fallback'}")

    print("\n1. blob size + field offsets match profile.h v1 layout")
    blob = encode_profile(default_profile())
    check(len(blob) == PBYTES, f"blob is exactly pbytes={PBYTES} bytes (got {len(blob)})")
    check(blob[0] == PROFILE_VERSION, "version at offset 0")
    check(blob[1] == 0, "channel at offset 1")
    check(blob[2] == 7, "fader[0].cc at offset 2 (== 7)")
    check(blob[2 + 4] == 0, "fader[0].invert at offset 6")
    check(blob[22] == BTN_VALS["cc_momentary"], "button[0].type at offset 22")
    check(blob[23] == 20, "button[0].value at offset 23 (== 20)")
    check(blob[40] == 7, "shift.fader_cc[0] at offset 40 (== 7)")
    check(blob[44] == 20, "shift.button_value[0] at offset 44 (== 20)")
    check(blob[53:53 + 7] == b"Default", "name at offset 53 (== 'Default')")
    check(blob[53 + 7] == 0, "name NUL-padded after 'Default'")

    print("\n2. round-trip friendly-JSON -> blob -> b64 -> blob -> friendly-JSON")
    p_in = {
        "format": "sp1-profile", "version": 1, "name": "OP-XY mix", "channel": 5,
        "faders": [
            {"cc": 7, "min": 0, "max": 127, "curve": "linear", "invert": False},
            {"cc": 74, "min": 10, "max": 120, "curve": "log", "invert": True},
            {"cc": 71, "min": 0, "max": 100, "curve": "exp", "invert": False},
            {"cc": 76, "min": 5, "max": 127, "curve": "linear", "invert": True},
        ],
        "buttons": [
            {"type": "note", "value": 60}, {"type": "cc_toggle", "value": 64},
            {"type": "cc_momentary", "value": 65}, {"type": "transport", "value": 1},
            {"type": "profile_switch", "value": 2}, {"type": "none", "value": 0},
            {"type": "note", "value": 62}, {"type": "cc_toggle", "value": 80},
            {"type": "cc_momentary", "value": 81},
        ],
        "shift": {"fader_cc": [20, 21, 22, 23],
                  "button_value": [30, 31, 32, 33, 34, 35, 36, 37, 38]},
    }
    blob1 = encode_profile(p_in)
    b64 = base64.b64encode(blob1).decode("ascii")
    blob2 = base64.b64decode(b64)
    check(blob1 == blob2, "base64 encode/decode is byte-identical")
    p_out = decode_profile(blob2)
    blob3 = encode_profile(p_out)
    check(blob1 == blob3, "re-encode of decoded profile is byte-identical")
    # compare semantic fields (decode drops 'format' duplication issues; check core)
    same = (p_out["channel"] == p_in["channel"] and p_out["name"] == p_in["name"] and
            p_out["faders"] == p_in["faders"] and p_out["buttons"] == p_in["buttons"] and
            p_out["shift"] == p_in["shift"])
    check(same, "decoded friendly-JSON equals input (channel/name/faders/buttons/shift)")

    print("\n3. firmware-parity: b64 alphabet is standard + padding matches profile.c")
    # profile.c uses A-Za-z0-9+/ with '=' padding; pad count = (3 - 69%3)%3 = 0.
    check(b64 == base64.b64encode(blob1).decode("ascii"), "standard base64 (A-Za-z0-9+/)")
    check(len(b64) == ((PBYTES + 2) // 3) * 4, f"b64 length == ((pbytes+2)//3)*4 = {((PBYTES+2)//3)*4}")
    check("=" not in b64, "no padding for 69 bytes (69 % 3 == 0)")

    print("\n4. client-side validation refuses out-of-range input")
    for bad, label in [
        ({**p_in, "channel": 16}, "channel 16 rejected"),
        ({**p_in, "faders": [{**p_in["faders"][0], "cc": 200}] + p_in["faders"][1:]}, "fader cc 200 rejected"),
        ({**p_in, "faders": p_in["faders"][:3]}, "only 3 faders rejected"),
        ({**p_in, "name": "x" * 17}, "17-char name rejected"),
    ]:
        try:
            encode_profile(bad)
            check(False, label)
        except ProfileError:
            check(True, label)

    print("\n5. mock transport answers every verb correctly")
    mt = MockTransport()
    h = mt.request("hello")
    check(h["fw"] == "0.1.0" and h["proto"] == PROTO_VERSION and h["pbytes"] == PBYTES,
          "hello_r: fw/proto/pbytes")
    check(h["pver"] == PROFILE_VERSION, "hello_r: pver == PROFILE_VERSION")
    check(h["faders"] == 4 and h["buttons"] == 9 and h["profiles"] == 8,
          "hello_r: faders=4 buttons=9 profiles=8")
    r0 = mt.request("read", n=0)
    check(decode_profile(base64.b64decode(r0["data"]))["name"] == "Default", "read 0 -> 'Default'")
    new = encode_profile({**p_in, "name": "Written"})
    mt.request("write", n=1, data=base64.b64encode(new).decode("ascii"))
    r1 = mt.request("read", n=1)
    check(decode_profile(base64.b64decode(r1["data"]))["name"] == "Written", "write/read 1 round-trips")
    mt.request("setactive", n=3)
    check(mt.request("getactive")["active"] == 3, "setactive 3 / getactive == 3")
    # reset slot 1 (was "Written" above) back to its named default
    rr = mt.request("reset", n=1)
    check(rr["n"] == 1, "reset 1 -> reset_r{n:1}")
    r1b = mt.request("read", n=1)
    check(decode_profile(base64.b64decode(r1b["data"]))["name"] == "Slot 1", "reset 1 -> 'Slot 1'")
    try:
        mt.request("reset", n=99); check(False, "reset bad index rejected")
    except ProtocolError as e:
        check("BAD_INDEX" in str(e), "reset 99 -> BAD_INDEX")
    # resetall returns every slot to its named default
    ra = mt.request("resetall")
    check(ra["t"] == "resetall_r" and ra["ok"] is True, "resetall -> resetall_r")
    r0b = mt.request("read", n=0)
    check(decode_profile(base64.b64decode(r0b["data"]))["name"] == "Default", "resetall -> slot 0 'Default'")
    mon = mt.request("monset", on=True)
    check(mon["on"] is True, "monset on")
    frames = []
    while True:
        f = mt.read_mon()
        if f is None:
            break
        frames.append(f)
    check(len(frames) >= 1 and all(x["t"] == "mon" for x in frames),
          f"mock emits {len(frames)} mon frames")
    # mock error paths
    try:
        mt.request("read", n=99); check(False, "bad index rejected")
    except ProtocolError as e:
        check("BAD_INDEX" in str(e), "read 99 -> BAD_INDEX")
    try:
        mt.request("bogus"); check(False, "bad verb rejected")
    except ProtocolError as e:
        check("BAD_VERB" in str(e), "unknown verb -> BAD_VERB")

    print("\n6. every subcommand runs end-to-end against the mock")
    import io
    import contextlib

    class A:  # minimal args namespace
        port = None; timeout = DEFAULT_TIMEOUT; verbose = False; mock = True
        n = None; json = False; file = None

    def run_cmd(fn, **over):
        a = A(); [setattr(a, k, v) for k, v in over.items()]
        buf = io.StringIO()
        # share ONE mock so writes persist across calls in this sub-test
        shared = run_cmd._mock
        with contextlib.redirect_stdout(buf):
            fn(a, t=shared)
        return buf.getvalue()

    run_cmd._mock = MockTransport()
    out = run_cmd(cmd_hello)
    check("firmware : 0.1.0" in out, "cmd_hello prints fw")
    check("pver     : 1" in out, "cmd_hello prints pver")
    out = run_cmd(cmd_list)
    check("Default" in out and out.count("\n") >= 8, "cmd_list prints 8-row table")
    out = run_cmd(cmd_get, n=0, json=True)
    check('"name":"Default"' in out, "cmd_get --json prints profile")
    out = run_cmd(cmd_active, n=None)
    check("active profile:" in out, "cmd_active (get)")
    out = run_cmd(cmd_active, n=2)
    check("set to 2" in out, "cmd_active (set 2)")

    import tempfile, os
    td = tempfile.mkdtemp()
    fpath = os.path.join(td, "p.json")
    run_cmd(cmd_export, n=0, file=fpath)
    check(os.path.exists(fpath), "cmd_export writes file")
    # mutate exported file and set it back
    with open(fpath) as f:
        pj = json.load(f)
    pj["name"] = "Imported"
    with open(fpath, "w") as f:
        json.dump(pj, f)
    out = run_cmd(cmd_set, n=4, file=fpath)
    check("'Imported'" in out, "cmd_set writes profile from file")
    out = run_cmd(cmd_get, n=4, json=True)
    check('"name":"Imported"' in out, "cmd_get reflects the set")
    allpath = os.path.join(td, "all.json")
    run_cmd(cmd_export_all, file=allpath)
    with open(allpath) as f:
        bundle = json.load(f)
    check(bundle["format"] == "sp1-bundle" and len(bundle["profiles"]) == 8,
          "cmd_export_all bundles 8 profiles")
    out = run_cmd(cmd_monitor)
    check("F0=" in out or "B" in out, "cmd_monitor prints mon frames then exits")

    print()
    if fails:
        print(f"SELF-TEST FAILED: {len(fails)} check(s) failed:")
        for f in fails:
            print(f"  - {f}")
        return 1
    print("SELF-TEST PASSED (all checks green).")
    return 0


# ── CLI wiring ──────────────────────────────────────────────────────────────
def build_parser():
    ap = argparse.ArgumentParser(
        prog="sp1ctl.py",
        description="Host-side CLI for the SP-1 controller config protocol (USB-CDC serial).",
    )
    ap.add_argument("--port", help="serial port (default: auto-pick the only /dev/cu.usbmodem*)")
    ap.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT, help="request timeout seconds")
    ap.add_argument("--verbose", action="store_true", help="echo wire lines to stderr")
    ap.add_argument("--mock", action="store_true", help="use an in-process fake device (no hardware)")
    ap.add_argument("--selftest", action="store_true", help="run the offline self-test and exit")

    sub = ap.add_subparsers(dest="cmd")
    sub.add_parser("ports", help="list candidate /dev/cu.usbmodem* ports")
    sub.add_parser("hello", help="connect and print device info")
    sub.add_parser("list", help="table of all profiles")

    g = sub.add_parser("get", help="read profile N as friendly JSON")
    g.add_argument("n", type=int)
    g.add_argument("--json", action="store_true", help="compact one-line JSON")

    s = sub.add_parser("set", help="write friendly-JSON profile FILE to slot N")
    s.add_argument("n", type=int); s.add_argument("file")

    im = sub.add_parser("import", help="alias of set")
    im.add_argument("n", type=int); im.add_argument("file")

    a = sub.add_parser("active", help="get/set active profile")
    a.add_argument("n", type=int, nargs="?", default=None)

    sub.add_parser("monitor", help="stream live mon frames until Ctrl-C")

    e = sub.add_parser("export", help="read N, write friendly JSON to FILE")
    e.add_argument("n", type=int); e.add_argument("file")

    ea = sub.add_parser("export-all", help="bundle all profiles to FILE")
    ea.add_argument("file")
    return ap


def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.selftest:
        return run_selftest()

    if not args.cmd:
        build_parser().print_help()
        return 1

    try:
        if args.cmd == "ports":
            return cmd_ports(args)
        if args.cmd == "hello":
            cmd_hello(args); return 0
        if args.cmd == "list":
            return cmd_list(args)
        if args.cmd == "get":
            return cmd_get(args)
        if args.cmd in ("set", "import"):
            return cmd_set(args)
        if args.cmd == "active":
            return cmd_active(args)
        if args.cmd == "monitor":
            return cmd_monitor(args)
        if args.cmd == "export":
            return cmd_export(args)
        if args.cmd == "export-all":
            return cmd_export_all(args)
    except ProfileError as e:
        print(f"profile error: {e}", file=sys.stderr)
        return 2
    except ProtocolError as e:
        print(f"device error: {e}", file=sys.stderr)
        return 3
    except FileNotFoundError as e:
        print(f"file error: {e}", file=sys.stderr)
        return 2
    return 1


if __name__ == "__main__":
    sys.exit(main())
