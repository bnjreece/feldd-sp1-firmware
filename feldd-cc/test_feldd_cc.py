#!/usr/bin/env python3
"""Host tests for the feldd-cc daemon: config load/merge + state + button logic.
No device, no tmux. Run: python3 test_feldd_cc.py"""
import copy
import json
import os
import tempfile
import time

import feldd_cc as fc


def cfg(**over):
    c = copy.deepcopy(fc.DEFAULTS)
    fc._deep_merge(c, over)
    return c


def test_load_config_missing_is_defaults():
    assert fc.load_config("/no/such/feldd_cc.config.json") == fc.DEFAULTS


def test_load_config_merges_file_over_defaults():
    fd, path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(fd, "w") as f:
        json.dump({"lights": {"blink_hz": 6.0}, "buttons": {"play": ["Escape"]}}, f)
    c = fc.load_config(path)
    os.unlink(path)
    assert c["lights"]["blink_hz"] == 6.0            # overridden
    assert c["lights"]["done_hold_s"] == 0.6         # default kept
    assert c["buttons"]["play"] == ["Escape"]        # overridden
    assert c["buttons"]["vol_minus"] == ["Escape"]   # default kept


def test_hook_event_to_state():
    st = fc.State(cfg())
    fc.apply_hook(st, {"hook_event_name": "PreToolUse", "session_id": "a", "cwd": "/x"})
    assert st.sessions["a"]["state"] == "working"
    fc.apply_hook(st, {"hook_event_name": "Notification", "session_id": "a"})
    assert st.sessions["a"]["state"] == "needs" and st.active == "a"
    fc.apply_hook(st, {"hook_event_name": "Stop", "session_id": "a"})
    assert st.sessions["a"]["state"] == "done"


def test_custom_event_map():
    st = fc.State(cfg(lights={"events": {"PreToolUse": "needs"}}))
    fc.apply_hook(st, {"hook_event_name": "PreToolUse", "session_id": "a"})
    assert st.sessions["a"]["state"] == "needs"


def test_led_mask_working_then_done_then_idle():
    c = cfg()
    st = fc.State(c)
    st.set_state("a", "/x", "working")
    assert st.led_mask(time.time()) == 0x01           # LED 0, solid
    st.set_state("a", "/x", "done")
    assert st.led_mask(time.time()) == 0x01           # flash on
    later = st.sessions["a"]["t"] + c["lights"]["done_hold_s"] + 0.05
    assert st.led_mask(later) == 0x00                  # flash expired
    assert st.sessions["a"]["state"] == "idle"


def test_single_whole_row():
    st = fc.State(cfg(sessions={"mode": "single"}, lights={"whole_row": True}))
    st.set_state("a", "/x", "working")
    assert st.led_mask(time.time()) == 0x0F            # whole front row


def test_multi_allocates_leds_and_track_selects():
    st = fc.State(cfg(sessions={"mode": "multi"}))
    st.set_state("a", "/x", "working")
    st.set_state("b", "/y", "working")
    assert st.sessions["a"]["led"] == 0 and st.sessions["b"]["led"] == 1
    assert st.led_mask(time.time()) == 0b11
    fc.handle_button(st, 2)                             # Track 2 -> led 1 -> session b
    assert st.active == "b"


def test_button_keys_and_nothing():
    st = fc.State(cfg())
    sent = []
    orig = fc.send_keys
    fc.send_keys = lambda cwd, *keys: sent.append(list(keys))
    try:
        fc.handle_button(st, 0)        # play -> Enter
        fc.handle_button(st, 5)        # vol_plus -> None -> nothing
    finally:
        fc.send_keys = orig
    assert sent == [["Enter"]]


def test_single_mode_track_is_noop():
    st = fc.State(cfg(sessions={"mode": "single"}))
    st.set_state("a", "/x", "working")
    fc.handle_button(st, 2)            # Track 2 in single mode: not a named button -> no-op
    assert st.active == "a"            # unchanged, no crash


def test_shell_action():
    st = fc.State(cfg(buttons={"play": {"shell": "echo hi"}}))
    st.set_state("a", "/tmp", "working")
    calls = []
    orig = fc.subprocess.run
    fc.subprocess.run = lambda *a, **k: calls.append((a, k))
    try:
        fc.handle_button(st, 0)
    finally:
        fc.subprocess.run = orig
    assert calls and calls[0][0][0] == "echo hi" and calls[0][1].get("shell") is True


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("all feldd-cc tests passed")


if __name__ == "__main__":
    main()
