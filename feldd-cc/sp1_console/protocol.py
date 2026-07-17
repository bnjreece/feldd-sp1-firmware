"""sp1_console.protocol - the SP-1 console wire codec (transport-agnostic).

The single contract the SP-1 firmware and every host client agree on:
  * verbs OUT  -> compact JSON, byte-identical to today's feldd-cc `led`/`mon`
  * events IN  <- brace-framed JSON (works over a USB byte stream OR BLE
                  notification chunks, no newline dependency, string/escape aware)
  * chunking   -> split writes/notifications to the 20-byte BLE ATT_MTU cap

Device shape (stated once): 9 ladder buttons + 4 faders + 8 LEDs. The dot-dot
(function) button is reserved (power/wake/Fn), never a console input; there is no
scrubber (feldd-cc's "scrubber" is just its host-side name for fader 1).
"""
from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Iterator, Optional, Union

NUM_BUTTONS = 9   # PLAY=0, T1-T4=1-4, VolUp=5, VolDn=6, FWD=7, RWD=8
NUM_FADERS = 4    # no scrubber
NUM_LEDS = 8      # track row 0-3, side/play row 4-7

BLE_CHUNK = 20    # ATT_MTU 23 -> 20 usable bytes per write/notification


# --------------------------------------------------------------- verb builders
def led(mask: int, bri: Optional[list] = None,
        blink: Optional[int] = None, ms: Optional[int] = None) -> dict:
    """LED mask command. `bri`/`blink`/`ms` are the v1 per-LED-duty + breathe
    extensions; omit them and the object is byte-identical to today's verb, so a
    v1-only device and a breathe-capable host stay compatible both ways."""
    if ms is not None and blink is None:
        raise ValueError("led(): ms requires blink (ms is the blink period)")
    obj: dict = {"t": "led", "mask": mask}
    if bri is not None:
        obj["bri"] = list(bri)
    if blink is not None:
        obj["blink"] = blink
        if ms is not None:
            obj["ms"] = ms
    return obj


def led_index(ix: int, on: bool) -> dict:
    return {"t": "led", "ix": ix, "on": bool(on)}


def led_release() -> dict:
    return {"t": "led", "release": True}


def monset(on: bool = True) -> dict:
    return {"t": "monset", "on": bool(on)}


def mode_query() -> dict:
    return {"t": "mode"}


def console(on: bool) -> dict:
    """Declare/clear a console session (host-driven mode toggle, no gesture): while
    set, the device mutes HID keystrokes so console typing can't double."""
    return {"t": "console", "on": bool(on)}


# --------------------------------------------------------------- serialize/chunk
def serialize(obj: dict) -> bytes:
    """Compact JSON bytes, exactly as feldd-cc has always written them."""
    return json.dumps(obj, separators=(",", ":")).encode()


def chunks(data: bytes, size: int = BLE_CHUNK) -> list:
    """Split a byte string into <=size pieces (for the 20-byte BLE cap)."""
    return [data[i:i + size] for i in range(0, len(data), size)]


# --------------------------------------------------------------- reassembly
class FrameReassembler:
    """Brace-depth JSON framer with string/escape awareness and junk resync.

    Reuses the exact framing logic that has always worked on feldd-cc's USB CDC
    stream, so it needs no firmware framing change; because it tracks brace depth
    (not newlines) it also spans BLE notification chunk boundaries transparently.
    `feed()` yields each complete JSON object as it closes.
    """

    # Bound the buffer + guarantee resync, exactly like the firmware's feed_byte
    # LINE_CAP: the SP-1 CDC TX discards bytes when its FIFO is full, so a
    # congestion-truncated frame (dropped closing quote/brace) is a designed
    # reality. Without a cap such a frame would grow _buf unbounded and wedge the
    # framer until the link drops.
    _CAP = 1024

    def __init__(self) -> None:
        self._buf = bytearray()
        self._depth = 0
        self._instr = False
        self._esc = False

    def feed(self, data: bytes) -> Iterator[dict]:
        for b in data:
            c = chr(b)
            self._buf.append(b)
            if len(self._buf) > self._CAP:
                self._buf.clear()          # drop-and-resync (mirrors firmware LINE_CAP)
                self._depth = 0
                self._instr = False
                self._esc = False
                continue
            if self._instr:
                if self._esc:
                    self._esc = False
                elif c == "\\":
                    self._esc = True
                elif c == '"':
                    self._instr = False
                continue
            if c == '"':
                if self._depth > 0:        # a bare quote at depth 0 is junk, not a string
                    self._instr = True
            elif c == "{":
                self._depth += 1
            elif c == "}":
                self._depth -= 1
                if self._depth <= 0:
                    self._depth = 0
                    try:
                        obj = json.loads(bytes(self._buf).decode("utf-8", "ignore"))
                        if isinstance(obj, dict):
                            yield obj
                    except Exception:
                        pass
                    self._buf.clear()
                    continue
            if self._depth == 0 and c not in "{} \r\n\t":
                self._buf.clear()   # resync on junk between objects


# --------------------------------------------------------------- typed events
@dataclass
class ButtonEvent:
    ix: int
    pressed: bool


@dataclass
class FaderEvent:
    ix: int
    value: int   # 0-127 (7-bit)


@dataclass
class ModeEvent:
    value: int   # 0 = MIDI, 1 = Keyboard


Event = Union[ButtonEvent, FaderEvent, ModeEvent]


def parse_event(obj: dict) -> Optional[Event]:
    """Map a `mon` frame to a typed event; anything else (replies, etc.) -> None."""
    if obj.get("t") != "mon":
        return None
    k = obj.get("k")
    if k == "b":
        return ButtonEvent(ix=int(obj.get("ix", -1)), pressed=obj.get("s") == 1)
    if k == "f":
        return FaderEvent(ix=int(obj.get("ix", -1)), value=int(obj.get("v", 0)))
    if k == "mode":            # firmware emits k="mode" (config_cdc_fmt.c), not "m"
        return ModeEvent(value=int(obj.get("v", 0)))
    return None
