#!/usr/bin/env python3
"""
sp1ctl.py — host-side (macOS) CLI for the SP-1 controller's config protocol.

Speaks the SP-1 firmware's JSON-lines protocol over its USB-CDC serial port
(appears as /dev/cu.usbmodem* on macOS), so we can drive and test the device
from a Mac WITHOUT the WebSerial config tool. Mirrors firmware:
  firmware/app/src/protocol.c   (the wire protocol)
  firmware/app/src/profile.h    (the v3 packed profile struct)
  firmware/app/src/config_cdc.c (the "mon" live-monitor frames)

PARITY NOTE — host/firmware parity for `mode` + `list` is RESTORED (hierarchy plan).
  The phase-2b device-mode toggle (MIDI vs Keyboard) is set ON-DEVICE via the
  PLAY+rocker gesture and persisted by the librarian (see firmware mode.h /
  lib_header.h). The serial `mode` get/set verb is now ALSO dispatched by the
  firmware (protocol.c get_mode/set_mode hooks), so it is byte/verb-exact here.
  The §0 mode>profile>layer rework makes profiles MODE-SCOPED BANKS: NUM_PROFILES
  is 16 (bank 0 = MIDI global 0..7, bank 1 = Keyboard global 8..15), each mode
  remembers its own WITHIN-bank active index (0..7), and `list` is BANKED — it
  returns all 16 slots with a per-entry `bank` plus a top-level `mode` and the
  within-mode `active`. setactive/getactive operate on the WITHIN index of the
  current mode (bounded 0..7); read/write address a GLOBAL slot (0..15). All of
  read/write/setactive/getactive/reset/resetall/monset/mode/list and the v3
  profile blob are byte/verb-exact with the current firmware.

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
    setactive{n}                -> setactive_r {active}  n = WITHIN-bank index
                                    (0..7) of the CURRENT mode (not a global slot)
    getactive                   -> getactive_r {active}  within-bank index of the
                                    current mode
    list                        -> list_r {mode,active,profiles:[{n,bank,name,ver}]}
                                    ALL 16 global slots, each tagged bank=n//8
                                    (0=MIDI, 1=Keyboard); top-level mode + the
                                    within-mode active
    monset   {on:bool}          -> monset_r {on}, after which the device pushes
                                    unsolicited frames until monset off:
              fader   {"t":"mon","k":"f","ix":<i>,"v":<value>}
              button  {"t":"mon","k":"b","ix":<i>,"s":<0|1>}
    mode     {[v:0|1]}           -> mode_r {v}   get/set the global device mode
                                    (0 MIDI / 1 KEYBOARD). Dispatched by firmware
                                    protocol.c (parity restored — see PARITY NOTE).
  error codes: BAD_JSON BAD_VERB BAD_VALUE BAD_INDEX BAD_LEN BAD_VERSION
               NVS_FAIL OVERFLOW

PROFILE STRUCT (profile.h, v5 — packed little-endian, all uint8). Total 180 B:
  off 0   version(1)                      == 5
  off 1   channel(1)                      0..15  (profile default / "set all")
  off 2   fader[4]  : {cc,min,max,curve,invert}  (5 B each = 20)  -> ends off 22
  off 22  button[9] : {type,value}              (2 B each = 18)  -> ends off 40
  off 40  shift.fader_cc[4]                      (4)             -> ends off 44
  off 44  shift.button_value[9]                  (9)             -> ends off 53
  off 53  name[16]                               (16)            -> ends off 69
  off 69  fader_channel[4]      (v2)             (4)             -> ends off 73
  off 73  button_channel[9]     (v2)             (9)             -> ends off 82
  off 82  button_key[9]         (v3)             (9)             -> ends off 91
  off 91  button_mod[9]         (v3)             (9)             -> ends off 100
  off 100 button_key_shift[9]   (v4)             (9)             -> ends off 109
  off 109 button_mod_shift[9]   (v4)             (9)             -> ends off 118
  off 118 layer[0] (L3): fader_cc[4]+button_value[9]+button_key[9]+button_mod[9]
                          (v5)                    (31)            -> ends off 149
  off 149 layer[1] (L4): fader_cc[4]+button_value[9]+button_key[9]+button_mod[9]
                          (v5)                    (31)            -> ends off 180
  enums: curve 0=linear 1=log 2=exp
         btn_type 0=none 1=note 2=cc_toggle 3=cc_momentary 4=transport
                  5=profile_switch
  v2 appends a MIDI channel per control so one profile can drive multiple tracks
  (4 faders -> 4 channels = a mixer). v3 appends a per-button USB-HID keyboard
  binding (button_key[i] = HID usage id, 0 = unbound; button_mod[i] = HID
  modifier bitmask: bit0 LCtrl, bit1 LShift, bit2 LAlt, bit3 LGUI) for Keyboard
  mode. v4 appends the SHIFT-LAYER keymap (button_key_shift[9] + button_mod_shift[9])
  — Keyboard mode's second keymap, selected by the double-tap-PLAY shift latch,
  mirroring the MIDI shift bank. v5 appends TWO more layers (L3, L4) as a clean
  tail: each is fader_cc[4] + button_value[9] (MIDI) + button_key[9] + button_mod[9]
  (Keyboard) = 31 B. The double-tap-PLAY gesture now cycles 4 layers (0->1->2->3->0);
  layer 0 = inline, 1 = shift.*, 2 = layer[0], 3 = layer[1]. Legacy v1 (69 B),
  v2 (82 B), v3 (100 B) and v4 (118 B) blobs decode with the missing trailing
  fields defaulted to 0; encode always emits v5. profile_to_b64 (firmware) is a
  plain standard-base64 of the raw struct bytes, so Python's base64 of this layout
  matches the firmware byte-for-byte (180 % 3 == 0 -> 240 chars, no padding).

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
PROFILE_VERSION = 8
PROTO_VERSION = 1
NUM_FADERS = 4
NUM_BUTTONS = 9
NUM_LAYERS = 4
NAME_LEN = 16
# Mode-scoped banks (§0 mode>profile>layer, mirrors profile.h / lib_bank.h):
# profiles are NUM_MODES banks of NUM_BANK_PROFILES each. bank 0 = MIDI (global
# 0..7), bank 1 = Keyboard (global 8..15). The •• cycle wraps WITHIN a bank
# (0..7); read/write address a GLOBAL slot (0..15); each mode remembers its own
# within-bank active. global = bank(mode)*NUM_BANK_PROFILES + within;
# bank = global // NUM_BANK_PROFILES.
NUM_BANK_PROFILES = 8
NUM_MODES = 2
PROFILES = NUM_MODES * NUM_BANK_PROFILES   # 16 total NVS slots
# v1 packed (69) + v2 per-control channels (fader[4]+button[9]=13 -> 82)
# + v3 keymap (button_key[9]+button_mod[9]=18 -> 100)
# + v4 shift keymap (button_key_shift[9]+button_mod_shift[9]=18 -> 118)
# + v5 two appended layers (L3, L4): fader_cc[4]+button_value[9]+button_key[9]
#   +button_mod[9]=31 bytes/layer * 2 = 62 -> 180. base64 = 240 chars, no padding.
PBYTES_V1 = 1 + 1 + NUM_FADERS * 5 + NUM_BUTTONS * 2 + NUM_FADERS + NUM_BUTTONS + NAME_LEN
PBYTES_V2 = PBYTES_V1 + NUM_FADERS + NUM_BUTTONS  # 82
PBYTES_V3 = PBYTES_V2 + 2 * NUM_BUTTONS           # 100
PBYTES_V4 = PBYTES_V3 + 2 * NUM_BUTTONS           # 118
PER_LAYER = NUM_FADERS + 3 * NUM_BUTTONS          # 31 (v5 L3/L4 bank)
PBYTES_V5 = PBYTES_V4 + 2 * PER_LAYER             # 180
# v6: per-layer ext bank for L2/L3/L4 (38 B each): fader_min[4]+fader_max[4]+
# fader_curve[4]+fader_invert[4]=16, button_type[9]=9, fader_channel[4]=4,
# button_channel[9]=9  ->  38 * 3 = 114. 180 + 114 = 294. base64 = 392, no pad.
# SoA order per ext layer mirrors struct layer_ext in profile.h (the v6 tail):
# fader_min,fader_max,fader_curve,fader_invert,button_type,fader_channel,button_channel.
PER_EXT  = 4 * NUM_FADERS + NUM_BUTTONS + NUM_FADERS + NUM_BUTTONS   # 38
PBYTES_V6 = PBYTES_V5 + 3 * PER_EXT               # 294
# v8: chord6 tail. chord6[4][9]*6 = 216 + fader_role[4][4] = 16 + chord_flags[2] = 2
# -> 234. 294 + 234 = 528. base64 = 704, no pad (528 % 3 == 0). Each chord6 is the
# packed 6-byte stored chord (spec §2): hdr = (mode<<5)|count, then 5 payload bytes.
# This REPLACES v7's per-PROFILE shared chord_table + button_chord_ix with one inline
# packed chord per (layer, button) - chords now scale like every other control.
CHORD6_BYTES = 6
CHORD6_MAX_EXPLICIT = 5
PER_V8_TAIL = NUM_LAYERS * NUM_BUTTONS * CHORD6_BYTES \
            + NUM_LAYERS * NUM_FADERS + 2                                  # 234
PBYTES_V8 = PBYTES_V6 + PER_V8_TAIL                                        # 528
PBYTES = PBYTES_V8                                # default = full current image
# Byte length of each on-wire profile version. The format is a strict PREFIX
# SUPERSET (each version is a clean tail-append), so encoding an older version =
# build the full v8 image, stamp byte 0 = target, and slice to VSIZE[target].
# Lets sp1ctl WRITE older formats (a v2..v6 firmware rejects a wrong-length
# blob) while the parity selftest still proves v6 + v8. Mirrors the web codec's VSIZE.
# A v7 (444 B) full image is no longer a valid wire length; its v6 prefix (294) is
# still readable. VSIZE keeps the historical prefixes for the read-only Export path.
VSIZE = {1: PBYTES_V1, 2: PBYTES_V2, 3: PBYTES_V3, 4: PBYTES_V4,
         5: PBYTES_V5, 6: PBYTES_V6, 8: PBYTES_V8}
PORT_GLOB = "/dev/cu.usbmodem*"
BAUD = 115200
DEFAULT_TIMEOUT = 2.0

CURVE_NAMES = {0: "linear", 1: "log", 2: "exp"}
CURVE_VALS = {v: k for k, v in CURVE_NAMES.items()}
BTN_NAMES = {0: "none", 1: "note", 2: "cc_toggle", 3: "cc_momentary",
             4: "transport", 5: "profile_switch", 6: "chord"}
BTN_VALS = {v: k for k, v in BTN_NAMES.items()}
ROLE_NAMES = {0: "cc", 1: "chord_depth"}
ROLE_VALS = {v: k for k, v in ROLE_NAMES.items()}


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


def _chord6_pack(c):
    """friendly chord dict -> 6 packed bytes (mirrors firmware chord6_pack)."""
    mode = _rng("chord6.mode", c.get("mode", 0), 0, 2)
    if mode == 0:  # explicit
        notes = c.get("notes", [])
        cnt = min(len(notes), CHORD6_MAX_EXPLICIT)
        b = [((mode << 5) | (cnt & 0x1F))]
        for i in range(CHORD6_MAX_EXPLICIT):
            b.append(_rng("chord6.note", notes[i] if i < cnt else 0, 0, 127))
        return b
    if mode == 1:  # range
        return [(mode << 5),
                _rng("chord6.range_start", c.get("range_start", 0), 0, 127),
                _rng("chord6.range_count", c.get("range_count", 0), 0, 8), 0, 0, 0]
    # mode 2: root+quality
    return [(mode << 5),
            _rng("chord6.root", c.get("root", 0), 0, 127),
            _rng("chord6.quality", c.get("quality", 0), 0, 9), 0, 0, 0]


def _chord6_unpack(b):
    """6 packed bytes -> friendly chord dict (None if empty)."""
    mode  = (b[0] >> 5) & 0x07
    count = b[0] & 0x1F
    if mode == 0:
        if count == 0:
            return None
        return {"mode": 0, "notes": list(b[1:1 + count])}
    if mode == 1:
        if b[2] == 0:
            return None
        return {"mode": 1, "range_start": b[1], "range_count": b[2]}
    if b[2] == 0:
        return None
    return {"mode": 2, "root": b[1], "quality": b[2]}


def encode_profile(p, target_version=PROFILE_VERSION):
    """Friendly-JSON dict -> packed little-endian blob (bytes).

    Builds the full v4 (118-byte) image, then emits the requested
    `target_version` (default = latest = v4, the unchanged parity path). A lower
    target stamps byte 0 = target_version and slices to VSIZE[target_version];
    correct because each version is a clean PREFIX of the next, so the sliced
    bytes are exactly that version's layout. Lets `set --target` write an older
    device's own format (a v2/v3 firmware rejects a wrong-length/version blob).

    Validates ranges client-side and raises ProfileError with a clear message
    before producing any bytes (so `set` refuses bad input up front).
    """
    if not isinstance(p, dict):
        raise ProfileError("profile must be a JSON object")

    if target_version not in VSIZE:
        raise ProfileError(f"unsupported target version {target_version} (expected 1, 2, 3, 4, 5, 6 or 8)")

    # Accept a v1..v8 profile dict; always build the full v8 image (auto-upgrade).
    # A v7 dict is accepted as INPUT (its v6 prefix re-encodes; its retired chord_table
    # tail is dropped - chords must be re-expressed as the new per-button chord6 grid).
    in_version = p.get("version", PROFILE_VERSION)
    if in_version not in (1, 2, 3, 4, 5, 6, 7, 8):
        raise ProfileError(f"unsupported profile version {in_version} (expected 1, 2, 3, 4, 5, 6, 7 or 8)")

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
    out.append(PROFILE_VERSION)  # always emit v8 (full image, sliced for older targets)
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
            btype = _rng(f"buttons[{i}].type", type_in, 0, 6)   # v8: BTN_CHORD=6
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

    # v2: per-control channels appended after the v1 layout; each defaults to the
    # profile-wide channel when the control doesn't override it.
    for i, f in enumerate(faders):
        out.append(_rng(f"faders[{i}].channel", f.get("channel", channel), 0, 15))
    for i, b in enumerate(buttons):
        out.append(_rng(f"buttons[{i}].channel", b.get("channel", channel), 0, 15))

    # v3: per-button HID key + modifier (Keyboard mode, base layer). Every byte
    # 0..255 is a legal HID usage / modifier bitmask (0 = unbound), so no clamp.
    for i, b in enumerate(buttons):
        out.append(_u8(f"buttons[{i}].key", b.get("key", 0)))
    for i, b in enumerate(buttons):
        out.append(_u8(f"buttons[{i}].mod", b.get("mod", 0)))

    # v4: per-button SHIFT-layer HID key + modifier, after the v3 base keymap.
    # Keyboard mode's second keymap (double-tap-PLAY bank). Each defaults to 0.
    for i, b in enumerate(buttons):
        out.append(_u8(f"buttons[{i}].key_shift", b.get("key_shift", 0)))
    for i, b in enumerate(buttons):
        out.append(_u8(f"buttons[{i}].mod_shift", b.get("mod_shift", 0)))

    # v5: the two ADDITIONAL layers (L3, L4) beyond inline-L1 / shift-L2. Read from
    # an optional `layers` array (index 2 = L3, index 3 = L4); each missing field
    # defaults the MIDI banks to the base/shift value and the keyboard banks to 0
    # (unbound), matching the firmware make_default seed.
    layers = p.get("layers") or []
    def _layer(n):
        return layers[n] if (n < len(layers) and isinstance(layers[n], dict)) else {}
    for Ln in (2, 3):                       # L3, L4
        lb = _layer(Ln)
        fcc = lb.get("fader_cc", [f.get("cc", 0) for f in faders])
        bv  = lb.get("button_value", [b.get("value", 0) for b in buttons])
        bk  = lb.get("button_key", [0] * NUM_BUTTONS)
        bm  = lb.get("button_mod", [0] * NUM_BUTTONS)
        for i in range(NUM_FADERS):  out.append(_rng(f"layers[{Ln}].fader_cc[{i}]", fcc[i], 0, 127))
        for i in range(NUM_BUTTONS): out.append(_rng(f"layers[{Ln}].button_value[{i}]", bv[i], 0, 127))
        for i in range(NUM_BUTTONS): out.append(_u8(f"layers[{Ln}].button_key[{i}]", bk[i]))
        for i in range(NUM_BUTTONS): out.append(_u8(f"layers[{Ln}].button_mod[{i}]", bm[i]))

    # v6: per-layer ext banks (L2, L3, L4). Read from optional `ext`[0..2]; missing
    # fields INHERIT L1 (mirrors firmware profile_fill_missing: the v5 "share from
    # L1" model), so a v5 dict auto-upgrades. Same SoA order as struct layer_ext
    # (profile.h): fader_min,fader_max,fader_curve,fader_invert,button_type,
    # fader_channel,button_channel. ext[0]=L2 (shift), ext[1]=L3 (layer[0]),
    # ext[2]=L4 (layer[1]).
    ext_in = p.get("ext") or []
    def _ext(n):
        return ext_in[n] if (n < len(ext_in) and isinstance(ext_in[n], dict)) else {}
    l1_fmin = [f.get("min", 0)    for f in faders]
    l1_fmax = [f.get("max", 127)  for f in faders]
    l1_fcur = [CURVE_VALS.get(f.get("curve", "linear"), f.get("curve", 0))
               if isinstance(f.get("curve", "linear"), str) else f.get("curve", 0)
               for f in faders]
    l1_finv = [1 if f.get("invert", False) in (True, 1) else 0 for f in faders]
    l1_fch  = [f.get("channel", channel) for f in faders]
    l1_btype= [BTN_VALS.get(b.get("type", "none"), b.get("type", 0))
               if isinstance(b.get("type", "none"), str) else b.get("type", 0)
               for b in buttons]
    l1_bch  = [b.get("channel", channel) for b in buttons]
    for Ln in (0, 1, 2):                       # ext[0]=L2, [1]=L3, [2]=L4
        e = _ext(Ln)
        fmin = e.get("fader_min", l1_fmin); fmax = e.get("fader_max", l1_fmax)
        fcur = e.get("fader_curve", l1_fcur); finv = e.get("fader_invert", l1_finv)
        btype = e.get("button_type", l1_btype)
        fch  = e.get("fader_channel", l1_fch); bch = e.get("button_channel", l1_bch)
        for i in range(NUM_FADERS):  out.append(_rng(f"ext[{Ln}].fader_min[{i}]", fmin[i], 0, 127))
        for i in range(NUM_FADERS):  out.append(_rng(f"ext[{Ln}].fader_max[{i}]", fmax[i], 0, 127))
        for i in range(NUM_FADERS):  out.append(_rng(f"ext[{Ln}].fader_curve[{i}]", fcur[i], 0, 2))
        for i in range(NUM_FADERS):  out.append(_rng(f"ext[{Ln}].fader_invert[{i}]", finv[i], 0, 1))
        for i in range(NUM_BUTTONS): out.append(_rng(f"ext[{Ln}].button_type[{i}]", btype[i], 0, 6))   # v8: BTN_CHORD=6
        for i in range(NUM_FADERS):  out.append(_rng(f"ext[{Ln}].fader_channel[{i}]", fch[i], 0, 15))
        for i in range(NUM_BUTTONS): out.append(_rng(f"ext[{Ln}].button_channel[{i}]", bch[i], 0, 15))

    # v8: chord6 tail. `chord` block carries a per-button chord6 grid (4x9 dicts/None),
    # fader_role (4x4), chord_velocity. Default = no chords, role cc, velocity 100
    # (mirrors firmware make_default + profile_fill_missing).
    ch = p.get("chord") or {}
    grid = ch.get("chord6")        # 4x9 of dict|None, or None
    frole = ch.get("fader_role")   # 4x4 or None
    cvel = ch.get("chord_velocity", 100)
    for L in range(NUM_LAYERS):
        for i in range(NUM_BUTTONS):
            cell = (grid[L][i] if grid else None)
            if cell:
                out.extend(_chord6_pack(cell))
            else:
                out.extend([0, 0, 0, 0, 0, 0])   # empty packed chord
    for L in range(NUM_LAYERS):
        for f in range(NUM_FADERS):
            v = (frole[L][f] if frole else 0)
            out.append(_rng(f"fader_role[{L}][{f}]", v, 0, 1))
    out.append(_rng("chord_velocity", cvel, 0, 127))
    out.append(0)   # chord_flags[1] reserved

    if len(out) != PBYTES:
        raise ProfileError(f"internal: encoded {len(out)} bytes, expected {PBYTES}")

    # Default (target_version == 8): byte 0 is already v8 and the full 528-byte
    # image is returned unchanged - the parity fixture holds. Older target: stamp
    # the version byte and slice to that version's length (prefix-superset layout).
    # v7 encode is retired (a v7 full image is no longer a valid wire length); the
    # Export path reads older formats but only writes v6-and-below prefixes or v8.
    if target_version == PROFILE_VERSION:           # v8: full 528-byte image
        return bytes(out)
    if target_version == 7:
        raise ProfileError("v7 encode retired; v8 only (export reads older formats, does not write them)")
    out[0] = target_version
    return bytes(out[:VSIZE[target_version]])


def decode_profile(blob):
    """packed blob (bytes) -> friendly-JSON dict. v8 = 528 B; v6 = 294 B; v5 = 180 B; v4 = 118 B; v3 = 100 B; v2 = 82 B; v1 = 69 B."""
    is_v2 = len(blob) >= PBYTES_V2
    if not is_v2 and len(blob) < PBYTES_V1:
        raise ProfileError(f"profile blob too short: {len(blob)} < {PBYTES_V1}")
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
            "channel": channel,   # v1 default; overwritten below for v2
        })

    buttons = []
    for _ in range(NUM_BUTTONS):
        btype, bval = blob[off:off + 2]
        off += 2
        buttons.append({"type": BTN_NAMES.get(btype, btype), "value": bval,
                        "channel": channel})

    shift_fcc = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
    shift_bv = list(blob[off:off + NUM_BUTTONS]); off += NUM_BUTTONS

    name = blob[off:off + NAME_LEN].split(b"\x00", 1)[0].decode("utf-8", "replace")
    off += NAME_LEN

    # v2: per-control channels follow the name; v1 keeps the profile-wide channel.
    if is_v2:
        for i in range(NUM_FADERS):
            faders[i]["channel"] = blob[off]; off += 1
        for i in range(NUM_BUTTONS):
            buttons[i]["channel"] = blob[off]; off += 1

    # v3: per-button HID key + modifier (base layer) follow the channels; older
    # blobs default to unbound (0/0) so a v1/v2 profile decodes cleanly.
    is_v3 = len(blob) >= PBYTES_V3
    if is_v3:
        for i in range(NUM_BUTTONS):
            buttons[i]["key"] = blob[off]; off += 1
        for i in range(NUM_BUTTONS):
            buttons[i]["mod"] = blob[off]; off += 1
    else:
        for b in buttons:
            b["key"] = 0
            b["mod"] = 0

    # v4: per-button SHIFT-layer key + modifier follow the base keymap; older
    # blobs (v1/v2/v3) stop before them and keep the shift layer unbound (0/0).
    is_v4 = len(blob) >= PBYTES_V4
    if is_v4:
        for i in range(NUM_BUTTONS):
            buttons[i]["key_shift"] = blob[off]; off += 1
        for i in range(NUM_BUTTONS):
            buttons[i]["mod_shift"] = blob[off]; off += 1
    else:
        for b in buttons:
            b["key_shift"] = 0
            b["mod_shift"] = 0

    # v5: the two appended layers (L3, L4). Older blobs stop before them; default
    # the layers to None so the friendly form carries only the banks the device has.
    is_v5 = len(blob) >= PBYTES_V5
    layers = [None, None, None, None]   # L1/L2 live in the base fields; L3/L4 here
    if is_v5:
        for Ln in (2, 3):
            fcc = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
            bv  = list(blob[off:off + NUM_BUTTONS]); off += NUM_BUTTONS
            bk  = list(blob[off:off + NUM_BUTTONS]); off += NUM_BUTTONS
            bm  = list(blob[off:off + NUM_BUTTONS]); off += NUM_BUTTONS
            layers[Ln] = {"fader_cc": fcc, "button_value": bv,
                          "button_key": bk, "button_mod": bm}

    # v6: per-layer ext banks (L2, L3, L4). Older blobs stop before them; default
    # ext to None so the friendly form only carries banks the device actually has.
    # Same SoA order as struct layer_ext (profile.h): fader_min, fader_max,
    # fader_curve, fader_invert, button_type, fader_channel, button_channel.
    is_v6 = len(blob) >= PBYTES_V6
    ext = [None, None, None]
    if is_v6:
        for Ln in (0, 1, 2):
            fmin = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
            fmax = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
            fcur = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
            finv = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
            btype= list(blob[off:off + NUM_BUTTONS]); off += NUM_BUTTONS
            fch  = list(blob[off:off + NUM_FADERS]); off += NUM_FADERS
            bch  = list(blob[off:off + NUM_BUTTONS]); off += NUM_BUTTONS
            ext[Ln] = {"fader_min": fmin, "fader_max": fmax, "fader_curve": fcur,
                       "fader_invert": finv, "button_type": btype,
                       "fader_channel": fch, "button_channel": bch}

    # v8: chord6 grid (4x9 packed) + fader_role (4x4) + chord_flags. Older blobs stop
    # before it; chord=None so the friendly form only carries a chord block when the
    # device actually has the v8 tail. Each chord6 is the packed 6-byte stored chord.
    is_v8 = len(blob) >= PBYTES_V8
    chord = None
    if is_v8:
        grid = [[None] * NUM_BUTTONS for _ in range(NUM_LAYERS)]
        for L in range(NUM_LAYERS):
            for i in range(NUM_BUTTONS):
                cell = _chord6_unpack(list(blob[off:off + 6])); off += 6
                grid[L][i] = cell
        frole = [[0] * NUM_FADERS for _ in range(NUM_LAYERS)]
        for L in range(NUM_LAYERS):
            for f in range(NUM_FADERS):
                frole[L][f] = blob[off]; off += 1
        cvel = blob[off]; off += 1
        _reserved = blob[off]; off += 1
        chord = {"chord6": grid, "fader_role": frole, "chord_velocity": cvel}

    return {
        "format": "sp1-profile",
        "version": version,
        "name": name,
        "channel": channel,
        "faders": faders,
        "buttons": buttons,
        "shift": {"fader_cc": shift_fcc, "button_value": shift_bv},
        "layers": layers,
        "ext": ext,
        "chord": chord,
    }


def default_profile():
    """The firmware's M5.2 default profile (librarian.c make_default).

    v4: factory defaults ship both keyboard maps UNBOUND (key/mod and the
    key_shift/mod_shift shift layer all 0) to match the firmware's make_default and
    the cross-repo parity rule — the device ships in MIDI mode with no keys bound,
    so omitting "key"/"mod"/"key_shift"/"mod_shift" lets them encode as 0.

    v5: omits "layers" entirely, so the two appended layers (L3, L4) encode their
    MIDI fader_cc/button_value banks from the base fader CCs / button values and
    their Keyboard key/mod banks as 0 (unbound), keeping the 180-byte image a clean
    prefix-superset of the v4 fixture.

    0.12.1 three-channel default: faders stay on MIDI ch1 (channel 0), the FRONT
    track buttons (idx 1..4) move to ch2 (channel 1), the SIDE buttons (idx 5..8)
    to ch3 (channel 2), PLAY (idx 0) stays on the profile channel — so a fader and
    a button can never collide on the same (channel, CC). This is a REPRESENTATIVE
    host default (classic fader CCs), not a byte-mirror of the firmware's per-slot
    make_default (P*16+L*4+slot); it just carries the same non-conflicting channel
    split. button index map: 0=Play, 1..4=Track1..4 (front), 5..8=Vol/FWD/RWD (side).
    """
    def btn_ch(i):
        return 1 if 1 <= i <= 4 else 2 if 5 <= i <= 8 else 0  # front ch2 / side ch3 / PLAY ch1
    return {
        "format": "sp1-profile", "version": PROFILE_VERSION,
        "name": "Default", "channel": 0,
        "faders": [{"cc": cc, "min": 0, "max": 127, "curve": "linear", "invert": False}
                   for cc in (7, 74, 71, 76)],
        "buttons": [{"type": "cc_momentary", "value": 20 + i, "channel": btn_ch(i)}
                    for i in range(NUM_BUTTONS)],
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
        # 16 banked NVS slots: bank 0 = MIDI (0..7), bank 1 = Keyboard (8..15).
        self.profiles = [encode_profile(_named_default(i)) for i in range(PROFILES)]
        self.active_within = [0] * NUM_MODES   # per-mode WITHIN-bank active (0..7)
        self.mode = 0          # current device mode: 0 MIDI (default), 1 KEYBOARD
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
                    "fw": self.fw, "profiles": len(self.profiles),
                    "active": self.active_within[self.mode],
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
            # n is a WITHIN-bank index (0..NUM_BANK_PROFILES-1) of the CURRENT mode,
            # NOT a global slot — mirrors protocol.c (bounds against bank_profiles=8,
            # not profiles=16). The other bank is reached by flipping mode.
            n = fields.get("n")
            if not isinstance(n, int) or not (0 <= n < NUM_BANK_PROFILES):
                return err("BAD_INDEX", "bad index")
            self.active_within[self.mode] = n
            return {"t": "setactive_r", "i": rid, "ok": True, "active": n}
        if verb == "getactive":
            return {"t": "getactive_r", "i": rid, "ok": True,
                    "active": self.active_within[self.mode]}
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
            # All 16 global slots, each tagged with its bank (n // NUM_BANK_PROFILES:
            # 0=MIDI, 1=Keyboard), plus the current `mode` and the WITHIN-mode
            # `active` at the top so the web shows both banks and highlights the
            # active one. Mirrors protocol.c list_r{mode,active,profiles:[{n,bank,
            # name,ver}]}.
            entries = []
            for g, blob in enumerate(self.profiles):
                name = blob[53:53 + NAME_LEN].split(b"\x00", 1)[0].decode("utf-8", "replace")
                entries.append({"n": g, "bank": g // NUM_BANK_PROFILES,
                                "name": name, "ver": blob[0]})
            return {"t": "list_r", "i": rid, "ok": True, "mode": self.mode,
                    "active": self.active_within[self.mode], "profiles": entries}
        if verb == "mode":
            # Firmware-parity verb (protocol.c dispatches "mode" — see the module
            # PARITY NOTE). {"t":"mode"} reads the current global device mode;
            # {"t":"mode","v":0|1} sets it (0 MIDI, 1 KEYBOARD). Switching mode
            # flips the active bank; each mode keeps its own within-bank active, so
            # getactive/list re-scope to the new mode's remembered active. Shape
            # (mode_r {v}, BAD_VALUE for v not in 0/1) matches the firmware.
            if "v" in fields:
                v = fields.get("v")
                if not isinstance(v, int) or isinstance(v, bool) or v not in (0, 1):
                    return err("BAD_VALUE", "mode must be 0 or 1")
                self.mode = v
            return {"t": "mode_r", "i": rid, "ok": True, "v": self.mode}
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
                  f"the friendly-JSON codec assumes the v{PROFILE_VERSION} {PBYTES}-byte layout "
                  f"(use --target to write an older device's own format).")
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
        mode = r.get("mode")
        active = r.get("active")
        rows = r.get("profiles", [])
        print(f"device mode: {'KEYBOARD' if mode else 'MIDI'}   active (within): {active}")
        print(f"{'idx':>3}  {'bank':>4}  {'name':<16}  {'ver':>3}  active")
        print(f"{'-'*3}  {'-'*4}  {'-'*16}  {'-'*3}  {'-'*6}")
        for row in rows:
            within = row.get("n", 0) % NUM_BANK_PROFILES
            mark = "*" if (row.get("bank") == mode and within == active) else ""
            bname = "KB" if row.get("bank") else "MIDI"
            print(f"{row.get('n'):>3}  {bname:>4}  {row.get('name', ''):<16}  "
                  f"{row.get('ver', ''):>3}  {mark}")
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
    # --target writes the device's OWN format version (default latest = v4). An
    # older firmware rejects a wrong-length/version blob, so pass its pver here.
    target = getattr(args, "target", None) or PROFILE_VERSION
    blob = encode_profile(p, target)  # raises ProfileError (clear message) on bad input
    data = base64.b64encode(blob).decode("ascii")
    t = t or open_transport(args)
    try:
        t.request("write", n=args.n, data=data)
        print(f"wrote profile {args.n} ({decode_profile(blob)['name']!r}, {len(blob)} bytes, v{blob[0]})")
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

    print("\n1. blob size + field offsets match profile.h v6 layout")
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
    # v3 base keymap + v4 shift keymap. Factory defaults ship UNBOUND (all 0) per
    # the cross-repo parity rule: the device ships in MIDI mode with no keys bound.
    check(blob[82] == 0, "button_key[0] at offset 82 (== 0, factory unbound)")
    check(blob[91] == 0, "button_mod[0] at offset 91 (== 0, no modifier)")
    check(blob[100] == 0, "button_key_shift[0] at offset 100 (== 0, factory unbound)")
    check(blob[109] == 0, "button_mod_shift[0] at offset 109 (== 0, no modifier)")
    # v5 L3/L4: default_profile omits `layers`, so the MIDI fader_cc banks default
    # to the base fader CC (offset 118 == base fader[0].cc == 7) and the keyboard
    # banks default to 0 (unbound). The first 180 bytes are a clean prefix-superset.
    check(blob[118] == 7, "L3 fader_cc[0] at offset 118 (defaults to base fader cc == 7)")
    check(blob[131] == 0, "L3 button_key[0] at offset 131 (== 0, factory unbound)")
    check(blob[149] == 7, "L4 fader_cc[0] at offset 149 (defaults to base fader cc == 7)")
    # v6 ext tail (L2/L3/L4): default_profile omits `ext`, so each ext bank INHERITS
    # L1 (the v5 share-from-L1 model). The 294-byte v6 region is a clean prefix-
    # superset of the 180-byte v5 image. ext[0]=L2 starts at offset 180.
    check(len(blob) == 528, f"v8 blob is exactly 528 bytes (got {len(blob)})")
    check(blob[180] == 0, "ext[0].fader_min[0] at offset 180 (inherits L1 fader min == 0)")
    check(blob[184] == 127, "ext[0].fader_max[0] at offset 184 (inherits L1 fader max == 127)")
    check(blob[196] == BTN_VALS["cc_momentary"],
          "ext[0].button_type[0] at offset 196 (inherits L1 button[0].type == cc_momentary)")
    check(blob[205] == 0, "ext[0].fader_channel[0] at offset 205 (inherits L1 fader ch == 0)")
    check(blob[210] == 1, "ext[0].button_channel[1]=Track1 at offset 210 (inherits L1 button ch == 1)")
    check(blob[293] == 2, "ext[2].button_channel[8]=RWD at offset 293 (last byte of the v6 region, inherits L1 ch == 2)")
    # v8 chord tail (default_profile omits `chord`): chord6 grid all-zero from offset
    # 294 (216 B = no chords); fader_role all-zero from 510 (16 B = role cc); chord
    # velocity 100 at 526; chord_flags[1] reserved = 0 at 527 (the last byte).
    check(blob[294] == 0, "chord6[0][0].hdr at offset 294 (== 0, empty chord)")
    check(blob[510] == 0, "fader_role[0][0] at offset 510 (== 0, role cc)")
    check(blob[526] == 100, "chord_flags[0]=velocity at offset 526 (== 100, default)")
    check(blob[527] == 0, "chord_flags[1]=reserved at offset 527 (== 0, last byte)")
    # 0.12.1 three-channel default: button_channel[9] at offset 73 (v2 layout).
    # PLAY (idx 0) -> ch1 (0); front Track1..4 (idx 1..4) -> ch2 (1); side
    # Vol/FWD/RWD (idx 5..8) -> ch3 (2). Keeps faders (ch1) and buttons off the
    # same (channel, CC).
    check(blob[73] == 0, "button_channel[0]=PLAY at offset 73 (== 0, profile channel)")
    check(blob[74] == 1, "button_channel[1]=Track1 at offset 74 (== 1, front ch2)")
    check(blob[77] == 1, "button_channel[4]=Track4 at offset 77 (== 1, front ch2)")
    check(blob[78] == 2, "button_channel[5]=Vol+ at offset 78 (== 2, side ch3)")
    check(blob[81] == 2, "button_channel[8]=RWD at offset 81 (== 2, side ch3)")

    print("\n2. round-trip friendly-JSON -> blob -> b64 -> blob -> friendly-JSON")
    p_in = {
        "format": "sp1-profile", "version": 1, "name": "OP-XY mix", "channel": 5,
        "faders": [
            {"cc": 7, "min": 0, "max": 127, "curve": "linear", "invert": False, "channel": 0},
            {"cc": 74, "min": 10, "max": 120, "curve": "log", "invert": True, "channel": 1},
            {"cc": 71, "min": 0, "max": 100, "curve": "exp", "invert": False, "channel": 2},
            {"cc": 76, "min": 5, "max": 127, "curve": "linear", "invert": True, "channel": 3},
        ],
        "buttons": [
            {"type": "note", "value": 60, "channel": 4, "key": 0x04, "mod": 0x01, "key_shift": 0x00, "mod_shift": 0x00},
            {"type": "cc_toggle", "value": 64, "channel": 5, "key": 0x05, "mod": 0x02, "key_shift": 0x06, "mod_shift": 0x08},
            {"type": "cc_momentary", "value": 65, "channel": 6, "key": 0x28, "mod": 0x00, "key_shift": 0x07, "mod_shift": 0x08},
            {"type": "transport", "value": 1, "channel": 7, "key": 0x2c, "mod": 0x04, "key_shift": 0x09, "mod_shift": 0x01},
            {"type": "profile_switch", "value": 2, "channel": 8, "key": 0x2b, "mod": 0x08, "key_shift": 0x0a, "mod_shift": 0x01},
            {"type": "none", "value": 0, "channel": 9, "key": 0x29, "mod": 0x05, "key_shift": 0x0b, "mod_shift": 0x05},
            {"type": "note", "value": 62, "channel": 10, "key": 0x50, "mod": 0x00, "key_shift": 0x4a, "mod_shift": 0x00},
            {"type": "cc_toggle", "value": 80, "channel": 11, "key": 0x4f, "mod": 0x00, "key_shift": 0x4d, "mod_shift": 0x00},
            {"type": "cc_momentary", "value": 81, "channel": 12, "key": 0x00, "mod": 0x00, "key_shift": 0x00, "mod_shift": 0x00},
        ],
        "shift": {"fader_cc": [20, 21, 22, 23],
                  "button_value": [30, 31, 32, 33, 34, 35, 36, 37, 38]},
    }
    blob1 = encode_profile(p_in)                 # full v8 image (528 B)
    b64 = base64.b64encode(blob1).decode("ascii")
    blob1_v4 = encode_profile(p_in, 4)           # the v4 prefix (118 B, byte 0 = 4)
    b64_v4 = base64.b64encode(blob1_v4).decode("ascii")
    # Cross-repo v4 parity fixture (must be byte-identical to the web repo's
    # SELFTEST_V4 / codec.test.ts SELFTEST_V4_B64). Base keymap: a,b,Enter,Space,
    # Tab,Esc,Left,Right,unbound with Ctrl,Shift,-,Alt,Gui,Ctrl+Shift modifiers.
    # SHIFT keymap: -,c,d,f,g,h,Home,End,- with -,Cmd,Cmd,Ctrl,Ctrl,Ctrl+Shift,-,-,-.
    # The v5 image is a strict prefix-superset: its first 118 bytes == the v4 image
    # (byte 0 restamped). Assert the literal against the v4 slice.
    PARITY_V4_B64 = ("BAUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhce"
                     "HyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAwQFBgcICQoLDAQFKCwrKVBP"
                     "AAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAAA==")
    check(len(blob1) == 528, f"v8 image is 528 bytes (got {len(blob1)})")
    check(len(blob1_v4) == 118, f"v4 parity slice is 118 bytes (got {len(blob1_v4)})")
    check(b64_v4 == PARITY_V4_B64, "cross-repo v4 parity base64 matches the canonical literal")
    check(blob1[1:118] == blob1_v4[1:], "v5 first 118 bytes == v4 image (prefix superset)")
    check(blob1[82] == 0x04, "parity fixture button_key[0] at offset 82 (== 0x04 'a')")
    check(blob1[84] == 0x28, "parity fixture button_key[2] at offset 84 (== 0x28 Enter)")
    check(blob1[90] == 0x00, "parity fixture button_key[8] at offset 90 (== 0x00 unbound)")
    check(blob1[91] == 0x01, "parity fixture button_mod[0] at offset 91 (== 0x01 Ctrl)")
    check(blob1[96] == 0x05, "parity fixture button_mod[5] at offset 96 (== 0x05 Ctrl+Shift)")
    check(blob1[100] == 0x00, "parity fixture button_key_shift[0] at offset 100 (== 0x00 unbound)")
    check(blob1[101] == 0x06, "parity fixture button_key_shift[1] at offset 101 (== 0x06 'c')")
    check(blob1[106] == 0x4a, "parity fixture button_key_shift[6] at offset 106 (== 0x4a Home)")
    check(blob1[109] == 0x00, "parity fixture button_mod_shift[0] at offset 109 (== 0x00 none)")
    check(blob1[110] == 0x08, "parity fixture button_mod_shift[1] at offset 110 (== 0x08 Cmd)")
    check(blob1[114] == 0x05, "parity fixture button_mod_shift[5] at offset 114 (== 0x05 Ctrl+Shift)")
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

    # 2b. version-aware encode: write an OLDER device's own format. Build the full
    # v8 image, stamp byte 0, slice to VSIZE[target]. Default (no target) == v8.
    check(encode_profile(p_in) == blob1, "encode default == v8 (target arg optional)")
    check(VSIZE == {1: 69, 2: 82, 3: 100, 4: 118, 5: 180, 6: 294, 8: 528},
          "VSIZE = {1:69,2:82,3:100,4:118,5:180,6:294,8:528}")
    for tv, sz in ((1, 69), (2, 82), (3, 100), (4, 118), (5, 180), (6, 294), (8, 528)):
        bv = encode_profile(p_in, tv)
        check(len(bv) == sz, f"encode(p, {tv}) is {sz} bytes")
        check(bv[0] == tv, f"encode(p, {tv}) byte0 == {tv}")
        if tv < 7:
            check(bytes(bv[1:]) == blob1[1:sz], f"encode(p, {tv}) body == v8 image[1:{sz}] (prefix superset)")
    # a v2-target encode then decode round-trips the v1+v2 fields (no v3/v4 tail)
    pv2 = decode_profile(encode_profile(p_in, 2))
    check(pv2["version"] == 2, "v2-target decode reports version 2")
    check(pv2["faders"] == p_in["faders"], "v2-target decode round-trips faders")
    check([b["channel"] for b in pv2["buttons"]] == [b["channel"] for b in p_in["buttons"]],
          "v2-target decode round-trips per-control channels")
    check(all(b["key"] == 0 and b["mod"] == 0 and b["key_shift"] == 0 and b["mod_shift"] == 0
              for b in pv2["buttons"]), "v2-target decode: v3/v4 tail defaults to 0")
    # v7 is a retired wire target (its 444-byte full image is no longer valid); v8 is
    # the only full-image target. Both an unknown target (9) and the retired v7 raise.
    try:
        encode_profile(p_in, 9); check(False, "encode target 9 rejected (unknown)")
    except ProfileError:
        check(True, "encode target 9 rejected (unknown)")
    try:
        encode_profile(p_in, 7); check(False, "encode target 7 rejected (retired)")
    except ProfileError:
        check(True, "encode target 7 rejected (retired)")

    print("\n2c. v5 cross-repo byte-parity: the shared golden 240-char base64")
    p_v5 = {**p_in, "version": 5,
            "layers": [
                None, None,   # L1 inline, L2 shift already in the base fields
                {"fader_cc": [40,41,42,43],
                 "button_value": [60,61,62,63,64,65,66,67,68],
                 "button_key": [0x14,0x15,0x16,0x17,0x18,0x19,0x1a,0x1b,0x1c],
                 "button_mod": [0x01,0x02,0x04,0x08,0x01,0x02,0x04,0x08,0x00]},
                {"fader_cc": [50,51,52,53],
                 "button_value": [70,71,72,73,74,75,76,77,78],
                 "button_key": [0x1e,0x1f,0x20,0x21,0x22,0x23,0x24,0x25,0x26],
                 "button_mod": [0x08,0x04,0x02,0x01,0x08,0x04,0x02,0x01,0x00]},
            ]}
    blob_v5 = encode_profile(p_v5, 5)   # the 180-byte v5 slice (default target is now v6)
    b64_v5 = base64.b64encode(blob_v5).decode("ascii")
    PARITY_V5_B64 = ("BQUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhce"
                     "HyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAwQFBgcICQoLDAQFKCwrKVBP"
                     "AAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYX"
                     "GBkaGxwBAgQIAQIECAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEA")
    check(len(blob_v5) == 180, f"v5 parity blob is 180 bytes (got {len(blob_v5)})")
    check(len(b64_v5) == 240 and not b64_v5.endswith("="),
          "v5 base64 is 240 chars, no padding (180 % 3 == 0)")
    check(b64_v5 == PARITY_V5_B64, "cross-repo v5 parity base64 matches the canonical literal")
    check(blob_v5[118] == 40, "v5 L3 fader_cc[0] at offset 118 (== 40)")
    check(blob_v5[131] == 0x14, "v5 L3 button_key[0] at offset 131 (== 0x14)")
    check(blob_v5[149] == 50, "v5 L4 fader_cc[0] at offset 149 (== 50)")
    check(blob_v5[171] == 0x08, "v5 L4 button_mod[0] at offset 171 (== 0x08)")
    p_v5_out = decode_profile(blob_v5)
    check(encode_profile(p_v5_out, 5) == blob_v5, "v5 re-encode of decoded profile is byte-identical")

    print("\n2d. v6 cross-repo byte-parity: the shared golden 392-char base64")
    # ext[0]=L2 (shift), ext[1]=L3 (layer[0]), ext[2]=L4 (layer[1]). MUST be
    # byte-identical to test_profile.c make_parity_v6 / PARITY_V6_B64.
    p_v6 = {**p_v5, "version": 6,
            "ext": [
                {"fader_min": [0,5,10,15], "fader_max": [127,120,100,90],
                 "fader_curve": [0,1,2,0], "fader_invert": [0,1,0,1],
                 "button_type": [1,2,3,4,5,0,1,2,3], "fader_channel": [1,2,3,4],
                 "button_channel": [0,1,2,3,4,5,6,7,8]},
                {"fader_min": [1,2,3,4], "fader_max": [110,111,112,113],
                 "fader_curve": [1,2,0,1], "fader_invert": [1,0,1,0],
                 "button_type": [2,3,4,5,0,1,2,3,4], "fader_channel": [5,6,7,8],
                 "button_channel": [9,10,11,12,13,14,15,0,1]},
                {"fader_min": [20,0,7,0], "fader_max": [127,64,80,96],
                 "fader_curve": [2,0,1,2], "fader_invert": [1,1,0,0],
                 "button_type": [3,4,5,0,1,2,3,4,5], "fader_channel": [9,10,11,12],
                 "button_channel": [2,3,4,5,6,7,8,9,10]},
            ]}
    blob_v6 = encode_profile(p_v6, 6)
    b64_v6 = base64.b64encode(blob_v6).decode("ascii")
    PARITY_V6_B64 = (
        "BgUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhceHyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAw"
        "QFBgcICQoLDAQFKCwrKVBPAAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYXGBkaGxwBAgQIAQIE"
        "CAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEAAAUKD394ZFoAAQIAAAEAAQECAwQFAAECAwECAwQAAQIDBAUGBwgBAg"
        "MEbm9wcQECAAEBAAEAAgMEBQABAgMEBQYHCAkKCwwNDg8AARQABwB/QFBgAgABAgEBAAADBAUAAQIDBAUJCgsMAgMEBQYHCAkK")
    check(len(blob_v6) == 294, f"v6 parity blob is 294 bytes (got {len(blob_v6)})")
    check(len(b64_v6) == 392 and not b64_v6.endswith("="),
          "v6 base64 is 392 chars, no padding (294 % 3 == 0)")
    check(b64_v6 == PARITY_V6_B64, "cross-repo v6 parity base64 == firmware PARITY_V6_B64")
    check(blob_v6[1:180] == blob_v5[1:180], "v6 first 180 bytes == v5 image (prefix superset)")
    p_v6_out = decode_profile(blob_v6)
    check(encode_profile(p_v6_out, 6) == blob_v6, "v6 re-encode of decoded profile is byte-identical")

    print("\n2f. v8 cross-repo byte-parity: the shared golden 704-char base64")
    # SAME fixture as test_profile.c make_parity_v8 (Task 9): the v6 parity fixture +
    # a fixed chord6 tail. Per-button chord6 on the diagonal: L0 b1 = explicit C-E-G,
    # L1 b2 = range 21..27, L2 b3 = Cmin7 (root 48, quality 5), L3 b4 = explicit C-E-G.
    # fader_role identity diagonal (L%4 == fader idx); velocity 100. MUST be byte-
    # identical to the firmware PARITY_V8_B64 literal (the FIRMWARE is authoritative).
    p_v8 = {**p_v6, "version": 8, "chord": {
        "chord6": [
            [None, {"mode":0,"notes":[60,64,67]}, None, None, None, None, None, None, None],  # L0 b1
            [None, None, {"mode":1,"range_start":21,"range_count":7}, None, None, None, None, None, None],  # L1 b2
            [None, None, None, {"mode":2,"root":48,"quality":5}, None, None, None, None, None],  # L2 b3
            [None, None, None, None, {"mode":0,"notes":[60,64,67]}, None, None, None, None]],   # L3 b4
        "fader_role": [[1,0,0,0],[0,1,0,0],[0,0,1,0],[0,0,0,1]],
        "chord_velocity": 100,
    }}
    blob_v8 = encode_profile(p_v8, 8)
    b64_v8 = base64.b64encode(blob_v8).decode("ascii")
    PARITY_V8_B64 = (
        "CAUHAH8AAEoKeAEBRwBkAgBMBX8AAQE8AkADQQQBBQIAAAE+AlADURQVFhceHyAhIiMkJSZPUC1YWSBtaXgAAAAAAAAAAAECAw"
        "QFBgcICQoLDAQFKCwrKVBPAAECAAQIBQAAAAAGBwkKC0pNAAAICAEBBQAAACgpKis8PT4/QEFCQ0QUFRYXGBkaGxwBAgQIAQIE"
        "CAAyMzQ1RkdISUpLTE1OHh8gISIjJCUmCAQCAQgEAgEAAAUKD394ZFoAAQIAAAEAAQECAwQFAAECAwECAwQAAQIDBAUGBwgBAg"
        "MEbm9wcQECAAEBAAEAAgMEBQABAgMEBQYHCAkKCwwNDg8AARQABwB/QFBgAgABAgEBAAADBAUAAQIDBAUJCgsMAgMEBQYHCAkK"
        "AAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAIBUHAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQDAFAAAAAAAAAAAAAAAAAAAAAAAA"
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAzxAQwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAQAAAA"
        "ABAAAAAAEAAAAAAWQA")
    check(len(blob_v8) == 528, f"v8 parity blob is 528 bytes (got {len(blob_v8)})")
    check(len(b64_v8) == 704 and not b64_v8.endswith("="),
          "v8 base64 is 704 chars, no padding (528 % 3 == 0)")
    check(b64_v8 == PARITY_V8_B64, "cross-repo v8 parity base64 == firmware PARITY_V8_B64")
    check(blob_v8[1:294] == blob_v6[1:294], "v8 first 294 bytes == v6 image (prefix superset)")
    p_v8_out = decode_profile(blob_v8)
    check(encode_profile(p_v8_out, 8) == blob_v8, "v8 re-encode of decoded profile is byte-identical")

    print("\n3. firmware-parity: b64 alphabet is standard + padding matches profile.c")
    # profile.c uses A-Za-z0-9+/ with canonical '=' padding; pad = (3 - pbytes%3)%3.
    # v5 (180 B): 180 % 3 == 0 -> pad == 0, so the 240-char wire form has NO padding.
    pad = (3 - PBYTES % 3) % 3
    check(b64 == base64.b64encode(blob1).decode("ascii"), "standard base64 (A-Za-z0-9+/)")
    check(len(b64) == ((PBYTES + 2) // 3) * 4, f"b64 length == ((pbytes+2)//3)*4 = {((PBYTES+2)//3)*4}")
    check(b64.count("=") == pad and b64.endswith("=" * pad),
          f"canonical padding matches profile.c ({pad} '=' for {PBYTES} bytes)")

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
    check(h["faders"] == 4 and h["buttons"] == 9 and h["profiles"] == 16,
          "hello_r: faders=4 buttons=9 profiles=16")
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
    # mode verb (firmware parity — see the module PARITY NOTE): protocol.c now
    # dispatches "mode" (the branch sits BEFORE `list`, lines 274-287), so these
    # checks assert the byte/verb-exact get/set shape a real SP-1 speaks, not just
    # the host mock. get defaults to 0 (MIDI), set 1 -> KEYBOARD, get reflects,
    # bad-value rejected — mode_r {v} / BAD_VALUE matches the firmware.
    md = mt.request("mode")
    check(md["t"] == "mode_r" and md["v"] == 0, "mode get defaults to 0 (MIDI)")
    ms = mt.request("mode", v=1)
    check(ms["t"] == "mode_r" and ms["v"] == 1, "mode set 1 -> KEYBOARD")
    check(mt.request("mode")["v"] == 1, "mode get reflects the set")
    try:
        mt.request("mode", v=2); check(False, "mode v=2 rejected")
    except ProtocolError as e:
        check("BAD" in str(e), "mode v=2 -> BAD_*")
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
    check(f"pver     : {PROFILE_VERSION}" in out, "cmd_hello prints pver")
    out = run_cmd(cmd_list)
    check("Default" in out and out.count("\n") >= 16, "cmd_list prints 16-row table")
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
    check(bundle["format"] == "sp1-bundle" and len(bundle["profiles"]) == 16,
          "cmd_export_all bundles 16 profiles")
    out = run_cmd(cmd_monitor)
    check("F0=" in out or "B" in out, "cmd_monitor prints mon frames then exits")

    print("\n7. mode-scoped banks: 16 profiles, per-mode active, list carries bank")
    mt2 = MockTransport()
    h2 = mt2.request("hello")
    check(h2["profiles"] == PROFILES == 16, "hello reports 16 profiles")
    # per-mode active is independent: setactive operates on the current mode's bank.
    mt2.request("mode", v=0); mt2.request("setactive", n=3)
    mt2.request("mode", v=1); mt2.request("setactive", n=5)
    mt2.request("mode", v=0)
    check(mt2.request("getactive")["active"] == 3, "MIDI bank remembers within 3")
    mt2.request("mode", v=1)
    check(mt2.request("getactive")["active"] == 5, "KB bank remembers within 5")
    # setactive is bounded by the bank width (0..7), NOT the 16-slot total.
    try:
        mt2.request("setactive", n=8); check(False, "setactive 8 rejected (within-bank only)")
    except ProtocolError as e:
        check("BAD_INDEX" in str(e), "setactive 8 -> BAD_INDEX (within-bank bound)")
    # read/write reach the other bank by GLOBAL index (0..15), independent of mode.
    blob = mt2.request("read", n=15)
    check(base64.b64decode(blob["data"]) == mt2.profiles[15], "read global slot 15 (KB bank)")
    # list returns 16 entries each with a bank tag + a top-level mode/active.
    lr = mt2.request("list")
    check(len(lr["profiles"]) == 16, "list returns all 16 profiles")
    check(lr["mode"] == 1 and lr["active"] == 5, "list reports current mode + within active")
    check(lr["profiles"][0]["bank"] == 0 and lr["profiles"][8]["bank"] == 1,
          "global 0 -> bank 0, global 8 -> bank 1")
    # bank arithmetic mirrors lib_bank.h (bank = global // NUM_BANK_PROFILES).
    check(all(e["bank"] == e["n"] // NUM_BANK_PROFILES for e in lr["profiles"]),
          "every entry bank == n // NUM_BANK_PROFILES")

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
    s.add_argument("--target", type=int, choices=(1, 2, 3, 4, 5, 6, 7),
                   help="profile format version to write (default: latest; pass the device's pver for older firmware)")

    im = sub.add_parser("import", help="alias of set")
    im.add_argument("n", type=int); im.add_argument("file")
    im.add_argument("--target", type=int, choices=(1, 2, 3, 4, 5, 6, 7),
                    help="profile format version to write (default: latest)")

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
