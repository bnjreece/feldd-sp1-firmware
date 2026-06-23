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


def test_button_keys_route_to_active_pane():
    st = fc.State(cfg())
    st.set_state("a", "/x", "working", pane="%1", tmux="t")   # active session has a pane
    sent = []
    orig = fc.send_keys_to
    fc.send_keys_to = lambda pane, cwd, *keys: sent.append((pane, list(keys)))
    try:
        fc.handle_button(st, 0)        # play -> Enter -> the active session's exact pane
        fc.handle_button(st, 5)        # vol_plus -> None -> nothing
    finally:
        fc.send_keys_to = orig
    assert sent == [("%1", ["Enter"])]


def test_pin_by_tmux_session_name():
    # all sessions share cwd (home), so the tmux session NAME disambiguates the pin
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"iamkeen": 2}}))
    st.set_state("x", "/Users/bnjmn", "working", pane="%5", tmux="iamkeen")
    assert st.sessions["x"]["led"] == 2          # pinned by name
    st.set_state("y", "/Users/bnjmn", "working", pane="%6", tmux="other")
    assert st.sessions["y"]["led"] == 0          # unpinned -> first free


def test_pin_mixed_name_and_path():
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"feldd": 0, "/proj/x": 1}}))
    st.set_state("a", "/Users/bnjmn", "working", tmux="feldd")   # name match -> 0
    st.set_state("b", "/proj/x/sub", "working", tmux="zzz")      # path match -> 1
    assert st.sessions["a"]["led"] == 0 and st.sessions["b"]["led"] == 1


def test_active_target_carries_pane():
    st = fc.State(cfg(sessions={"mode": "multi"}))
    st.set_state("a", "/Users/bnjmn", "needs", pane="%9", tmux="feldd")  # needs -> active
    assert st.active == "a"
    assert st.active_target() == ("%9", "/Users/bnjmn")


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


def test_pin_assigns_led_and_reserves_it():
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"/proj/a": 2}}))
    st.set_state("x", "/proj/a", "working")
    assert st.sessions["x"]["led"] == 2          # pinned project -> its LED
    st.set_state("y", "/proj/b", "working")
    assert st.sessions["y"]["led"] == 0          # unpinned -> first free (LED 2 reserved)


def test_pin_matches_subdir():
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"/proj/a": 2}}))
    st.set_state("x", "/proj/a/src/deep", "working")
    assert st.sessions["x"]["led"] == 2          # a cwd UNDER the pinned path matches


def test_pin_reserves_led_from_autofill():
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"/proj/a": 1}}))
    st.set_state("u", "/u", "working")
    st.set_state("v", "/v", "working")
    assert {st.sessions["u"]["led"], st.sessions["v"]["led"]} == {0, 2}   # skip reserved 1


def test_pin_tilde_expansion():
    home_proj = os.path.expanduser("~/projX")
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"~/projX": 3}}))
    st.set_state("x", home_proj, "working")
    assert st.sessions["x"]["led"] == 3


def test_osa_key_translation():
    assert fc._osa_fragment("Enter") == "key code 36"
    assert fc._osa_fragment("Escape") == "key code 53"
    assert fc._osa_fragment("C-c") == 'keystroke "c" using control down'
    assert fc._osa_fragment("x") == 'keystroke "x"'


def test_input_backend_dispatch():
    calls = {"osa": [], "tmux": []}
    o, t = fc.send_osascript, fc.send_keys_to
    fc.send_osascript = lambda keys: calls["osa"].append(list(keys))
    fc.send_keys_to = lambda pane, cwd, *keys: calls["tmux"].append(list(keys))
    try:
        st = fc.State(cfg(input={"backend": "osascript"}))
        st.set_state("a", "/x", "working", pane="%1")
        fc.handle_button(st, 0)                       # play -> osascript
        assert calls["osa"] == [["Enter"]] and calls["tmux"] == []

        st2 = fc.State(cfg(input={"backend": "none"}))
        st2.set_state("a", "/x", "working", pane="%1")
        fc.handle_button(st2, 0)                      # none -> nothing
        assert calls["osa"] == [["Enter"]]            # unchanged

        st3 = fc.State(cfg())                          # default backend = tmux
        st3.set_state("a", "/x", "working", pane="%1")
        fc.handle_button(st3, 0)
        assert calls["tmux"] == [["Enter"]]
    finally:
        fc.send_osascript, fc.send_keys_to = o, t


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("all feldd-cc tests passed")


if __name__ == "__main__":
    main()
