"""Tests for the sp1_console wire codec (transport-agnostic).

The codec is the single contract the SP-1 firmware and every host client agree on:
compact JSON verbs out, brace-framed JSON events in, chunked for a 20-byte BLE MTU.
Byte-for-byte compatible with today's feldd-cc `led`/`mon` JSON (feldd_cc.py).
"""
from sp1_console import protocol as P


# --- verb builders serialize to the exact bytes feldd today speaks -------------

def test_led_mask_is_byte_identical_to_todays_verb():
    assert P.serialize(P.led(mask=5)) == b'{"t":"led","mask":5}'

def test_led_index_on():
    assert P.serialize(P.led_index(3, True)) == b'{"t":"led","ix":3,"on":true}'

def test_led_release():
    assert P.serialize(P.led_release()) == b'{"t":"led","release":true}'

def test_monset_default_on():
    assert P.serialize(P.monset()) == b'{"t":"monset","on":true}'

def test_console_verb():
    assert P.serialize(P.console(True)) == b'{"t":"console","on":true}'

def test_mode_query():
    assert P.serialize(P.mode_query()) == b'{"t":"mode"}'

def test_led_v1_breathe_extension_carries_per_led_duty_and_blink():
    obj = P.led(mask=0xFF, bri=[100, 0, 0, 0, 50, 50, 50, 50], blink=0x0F, ms=800)
    assert obj["mask"] == 0xFF
    assert obj["bri"] == [100, 0, 0, 0, 50, 50, 50, 50]
    assert obj["blink"] == 0x0F and obj["ms"] == 800

def test_led_without_extensions_omits_optional_fields_for_v1_parity():
    # a plain mask must not carry bri/blink so v1-only firmware sees the exact old verb
    assert "bri" not in P.led(mask=5) and "blink" not in P.led(mask=5)


# --- chunking for the 20-byte BLE notification/write cap -----------------------

def test_chunks_never_exceed_cap_and_rejoin_to_original():
    data = P.serialize(P.led(mask=170)) + b"\n" + b"x" * 45
    parts = P.chunks(data, 20)
    assert all(len(p) <= 20 for p in parts)
    assert b"".join(parts) == data

def test_short_message_is_one_chunk():
    assert len(P.chunks(b'{"t":"led","mask":5}', 20)) == 1


# --- brace-framed reassembly (works over USB bytes AND BLE notification chunks) -

def test_reassembles_a_whole_object():
    r = P.FrameReassembler()
    assert list(r.feed(b'{"t":"mon","k":"b","ix":2,"s":1}')) == [
        {"t": "mon", "k": "b", "ix": 2, "s": 1}
    ]

def test_reassembles_across_arbitrary_chunk_boundaries():
    r = P.FrameReassembler()
    line = b'{"t":"mon","k":"f","ix":0,"v":64}'
    got = []
    for i in range(0, len(line), 7):          # 7-byte BLE-ish fragments
        got.extend(r.feed(line[i:i + 7]))
    assert got == [{"t": "mon", "k": "f", "ix": 0, "v": 64}]

def test_two_back_to_back_objects_both_emit():
    r = P.FrameReassembler()
    got = list(r.feed(b'{"t":"mode_r","v":0}{"t":"led_r","active":true,"mask":5}'))
    assert got == [
        {"t": "mode_r", "v": 0},
        {"t": "led_r", "active": True, "mask": 5},
    ]

def test_braces_inside_a_string_do_not_break_framing():
    r = P.FrameReassembler()
    assert list(r.feed(b'{"t":"x","s":"a}{b"}')) == [{"t": "x", "s": "a}{b"}]

def test_junk_between_objects_is_resynced():
    r = P.FrameReassembler()
    got = list(r.feed(b'garbage\n  {"t":"led_r","mask":1}  trailing'))
    assert got == [{"t": "led_r", "mask": 1}]


# --- typed event parsing -------------------------------------------------------

def test_parse_button_event():
    ev = P.parse_event({"t": "mon", "k": "b", "ix": 4, "s": 1})
    assert isinstance(ev, P.ButtonEvent) and ev.ix == 4 and ev.pressed is True

def test_parse_button_release():
    ev = P.parse_event({"t": "mon", "k": "b", "ix": 4, "s": 0})
    assert isinstance(ev, P.ButtonEvent) and ev.pressed is False

def test_parse_fader_event():
    ev = P.parse_event({"t": "mon", "k": "f", "ix": 2, "v": 96})
    assert isinstance(ev, P.FaderEvent) and ev.ix == 2 and ev.value == 96

def test_parse_mode_event():
    ev = P.parse_event({"t": "mon", "k": "m", "v": 1})
    assert isinstance(ev, P.ModeEvent) and ev.value == 1

def test_parse_non_event_returns_none():
    assert P.parse_event({"t": "led_r", "mask": 5}) is None


# --- device shape is stated once, canonically ----------------------------------

def test_device_shape_constants():
    assert P.NUM_BUTTONS == 9   # PLAY, T1-T4, VolUp, VolDn, FWD, RWD (no dot-dot: reserved)
    assert P.NUM_FADERS == 4    # no scrubber
    assert P.NUM_LEDS == 8
