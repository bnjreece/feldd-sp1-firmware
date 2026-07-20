"""Tests for the macOS CoreBluetooth attach transport's pure logic.

The CoreBluetooth session itself needs a real radio + a system-paired SP-1 and is
validated on hardware (2026-07-20 round-trip), not unit-tested. But peripheral
SELECTION is pure and safety-critical: retrieveConnectedPeripherals(withServices:)
is queried by services many devices share (HID, battery), so the picker must return
ONLY a feldd and never latch onto, say, AirPods. That logic is tested here.
"""
from sp1_console.cb_transport import pick_feldd


def test_prefers_the_feldd_by_name():
    picked = pick_feldd([("Benjamin's AirPods Max", "a"), ("feldd", "f"), ("Magic Keyboard", "k")])
    assert picked == ("feldd", "f")


def test_name_match_is_case_insensitive():
    assert pick_feldd([("FELDD SP-1", "x")]) == ("FELDD SP-1", "x")


def test_returns_none_when_no_feldd_present():
    # AirPods/mice expose battery + other shared services and WILL show up in the
    # retrieveConnected query; the picker must refuse them, not grab the first one.
    assert pick_feldd([("Benjamin's AirPods Max", "a"), ("Magic Mouse", "m")]) is None


def test_empty_is_none():
    assert pick_feldd([]) is None


def test_skips_unnamed_peripherals():
    assert pick_feldd([(None, "x"), ("feldd", "f")]) == ("feldd", "f")


def test_first_feldd_wins_when_several():
    assert pick_feldd([("feldd", "1"), ("feldd SP-1", "2")]) == ("feldd", "1")


def test_accepts_objects_with_name_method():
    class P:
        def __init__(self, n): self._n = n
        def name(self): return self._n
    a, b = P("AirPods"), P("feldd")
    assert pick_feldd([a, b]) is b
