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
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"web": 2}}))
    st.set_state("x", "/home/u", "working", pane="%5", tmux="web")
    assert st.sessions["x"]["led"] == 2          # pinned by name
    st.set_state("y", "/home/u", "working", pane="%6", tmux="other")
    assert st.sessions["y"]["led"] == 0          # unpinned -> first free


def test_pin_mixed_name_and_path():
    st = fc.State(cfg(sessions={"mode": "multi", "assign": {"web": 0, "/proj/x": 1}}))
    st.set_state("a", "/home/u", "working", tmux="web")          # name match -> 0
    st.set_state("b", "/proj/x/sub", "working", tmux="zzz")      # path match -> 1
    assert st.sessions["a"]["led"] == 0 and st.sessions["b"]["led"] == 1


def test_active_target_carries_pane():
    st = fc.State(cfg(sessions={"mode": "multi"}))
    st.set_state("a", "/home/u", "needs", pane="%9", tmux="web")  # needs -> active
    assert st.active == "a"
    assert st.active_target() == ("%9", "/home/u")


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


def test_nudge_play_types_word_then_enter_tmux():
    # dangerous-mode recipe: Play = ["continue", "Enter"] is passed straight to
    # tmux send-keys (the word is literal text, Enter is the key) -- no new daemon code.
    st = fc.State(cfg(buttons={"play": ["continue", "Enter"]}))
    st.set_state("a", "/x", "working", pane="%1", tmux="t")
    sent = []
    orig = fc.send_keys_to
    fc.send_keys_to = lambda pane, cwd, *keys: sent.append(list(keys))
    try:
        fc.handle_button(st, fc.PLAY)
    finally:
        fc.send_keys_to = orig
    assert sent == [["continue", "Enter"]]


def test_nudge_play_via_osascript():
    st = fc.State(cfg(buttons={"play": ["continue", "Enter"]}, input={"backend": "osascript"}))
    st.set_state("a", "/x", "working", pane="%1")
    sent = []
    orig = fc.send_osascript
    fc.send_osascript = lambda keys: sent.append(list(keys))
    try:
        fc.handle_button(st, fc.PLAY)
    finally:
        fc.send_osascript = orig
    assert sent == [["continue", "Enter"]]
    assert fc._osa_fragment("continue") == 'keystroke "continue"'   # literal word, not a keycode


def test_permission_defaults():
    c = cfg()
    assert c["permission"]["enabled"] is True
    assert c["permission"]["timeout_s"] == 90


def test_await_decision_focuses_and_blinks():
    st = fc.State(cfg())
    slot = st.await_decision("a", "/x", "%1", "web")
    assert st.active == "a"                       # the awaiting session grabs focus
    assert st.sessions["a"]["state"] == "needs"   # ...and blinks
    assert st.sessions["a"]["pane"] == "%1"
    assert st.pending.get("a") is slot
    assert not slot["ev"].is_set()                # still waiting on a press


def test_play_resolves_allow_without_typing_enter():
    st = fc.State(cfg())
    slot = st.await_decision("a", "/x", "%1", "web")
    sent = []
    orig = fc.send_keys_to
    fc.send_keys_to = lambda pane, cwd, *keys: sent.append(list(keys))
    try:
        fc.handle_button(st, fc.PLAY)             # Play -> ALLOW, not a literal Enter
    finally:
        fc.send_keys_to = orig
    assert slot["result"] == "allow" and slot["ev"].is_set()
    assert sent == []                             # did NOT type Enter into the pane
    assert "a" not in st.pending                  # consumed
    assert st.sessions["a"]["state"] == "working"


def test_vold_resolves_deny():
    st = fc.State(cfg())
    slot = st.await_decision("a", "/x", "%1", "web")
    fc.handle_button(st, fc.VOLD)                 # Vol- -> DENY
    assert slot["result"] == "deny" and slot["ev"].is_set()
    assert "a" not in st.pending
    assert st.sessions["a"]["state"] == "idle"


def test_resolve_active_noop_when_nothing_pending():
    st = fc.State(cfg())
    st.set_state("a", "/x", "working", pane="%1", tmux="t")
    assert st.resolve_active("allow") is False    # no pending -> Play does its normal action
    sent = []
    orig = fc.send_keys_to
    fc.send_keys_to = lambda pane, cwd, *keys: sent.append(list(keys))
    try:
        fc.handle_button(st, fc.PLAY)             # ...so a real Enter goes through
    finally:
        fc.send_keys_to = orig
    assert sent == [["Enter"]]


def test_resolve_active_only_touches_the_active_session():
    st = fc.State(cfg(sessions={"mode": "multi"}))
    slot_a = st.await_decision("a", "/x", "%1", "web")   # active = a
    slot_b = st.await_decision("b", "/y", "%2", "api")   # active = b (latest)
    assert st.resolve_active("allow") is True            # resolves b only
    assert slot_b["result"] == "allow" and slot_b["ev"].is_set()
    assert slot_a["result"] is None and not slot_a["ev"].is_set()
    assert "a" in st.pending                             # a still waiting


def test_cancel_pending_reverts_and_does_not_fire():
    st = fc.State(cfg())
    slot = st.await_decision("a", "/x", "%1", "web")
    st.cancel_pending("a")                        # timeout path
    assert "a" not in st.pending
    assert not slot["ev"].is_set()                # never resolved
    assert st.sessions["a"]["state"] == "working" # reverted off the blink


# ---- v3 cockpit ----------------------------------------------------------
def test_cockpit_allocates_eight_leds():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    for sid in "abcdefgh":
        st.set_state(sid, "/p/%s" % sid, "working")
    assert {st.sessions[s]["led"] for s in "abcdefgh"} == set(range(8))
    assert st.led_mask(time.time()) == 0xFF


def test_cockpit_pin_to_high_led_sticks():
    st = fc.State(cfg(sessions={"mode": "cockpit", "assign": {"web": 6}}))
    st.set_state("x", "/home/u", "working", tmux="web")
    assert st.sessions["x"]["led"] == 6          # a cockpit pin can be 0..7
    st.set_state("y", "/home/u", "working", tmux="other")
    assert st.sessions["y"]["led"] == 0          # unpinned -> first free, 6 reserved


def test_fader_pickup_grabs_after_crossing():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    st.fader_target[0] = 64
    assert st.fader_grab(0, 60) is None      # below target, not crossed -> ignored
    assert st.fader_grab(0, 70) == 70        # crossed 64 -> grabs and tracks
    assert st.fader_grab(0, 72) == 72        # grabbed -> tracks directly


def test_fader_pickup_grabs_immediately_when_no_target():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    assert st.fader_grab(2, 40) == 40        # no prior target -> grab on first move


def test_reset_fader_grab_forces_repickup():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    st.fader_grab(0, 40)                      # grabbed at 40
    st.reset_fader_grab(0)
    st.fader_target[0] = 90                   # underlying moved
    assert st.fader_grab(0, 41) is None       # must re-pick-up past 90
    assert st.fader_grab(0, 95) == 95


def test_play_held_shifts_track_to_bank_two():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    for sid in "abcdefgh":
        st.set_state(sid, "/p/%s" % sid, "working")   # a..h on leds 0..7
    fc.handle_button(st, fc.PLAY, True)                # Play DOWN -> shift armed
    fc.handle_button(st, fc.TRK1, True)               # Track1 shifted -> led 4 = session e
    fc.handle_button(st, fc.TRK1, False)
    fc.handle_button(st, fc.PLAY, False)              # Play UP, chorded -> NO play action
    assert st.active == "e"


def test_play_tap_fires_on_release_without_chord():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    st.set_state("a", "/x", "working", pane="%1", tmux="t")
    sent = []
    orig = fc.send_keys_to
    fc.send_keys_to = lambda p, c, *k: sent.append(list(k))
    try:
        fc.handle_button(st, fc.PLAY, True)
        assert sent == []                              # nothing on press (deferred)
        fc.handle_button(st, fc.PLAY, False)           # tap completes -> normal Play
    finally:
        fc.send_keys_to = orig
    assert sent == [["Enter"]]


def test_cockpit_track_unshifted_is_bank_one():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    for sid in "abcdefgh":
        st.set_state(sid, "/p/%s" % sid, "working")
    fc.handle_button(st, fc.TRK1, True)               # no Play held -> led 0 = session a
    assert st.active == "a"


def test_track_jump_switches_tmux_client():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    st.set_state("a", "/x", "working", pane="%1", tmux="web")   # led 0
    st.set_state("b", "/y", "working", pane="%2", tmux="api")   # led 1
    calls = []
    orig = fc.tmux_focus
    fc.tmux_focus = lambda tmuxname, pane: calls.append((tmuxname, pane))
    try:
        fc.handle_button(st, 2, True)        # Track 2 -> led 1 -> session b -> focus api
    finally:
        fc.tmux_focus = orig
    assert st.active == "b" and calls == [("api", "%2")]


def test_track_jump_only_in_cockpit():
    st = fc.State(cfg(sessions={"mode": "multi"}))
    st.set_state("a", "/x", "working", pane="%1", tmux="web")
    st.set_state("b", "/y", "working", pane="%2", tmux="api")
    calls = []
    orig = fc.tmux_focus
    fc.tmux_focus = lambda tmuxname, pane: calls.append(tmuxname)
    try:
        fc.handle_button(st, 2, True)        # multi mode: select only, no tmux switch
    finally:
        fc.tmux_focus = orig
    assert st.active == "b" and calls == []


def test_volplus_jumps_to_next_needs():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    st.set_state("a", "/x", "needs", pane="%1", tmux="w1")   # led 0
    st.set_state("b", "/y", "needs", pane="%2", tmux="w2")   # led 1, active=b (auto-focus)
    calls = []
    orig = fc.tmux_focus
    fc.tmux_focus = lambda n, p: calls.append(n)
    try:
        fc.handle_button(st, fc.VOLU, True)      # next needer after b wraps to a
    finally:
        fc.tmux_focus = orig
    assert st.active == "a" and calls == ["w1"]


def test_volplus_noop_outside_cockpit():
    st = fc.State(cfg(sessions={"mode": "multi"}))
    st.set_state("a", "/x", "needs", pane="%1", tmux="w1")
    calls = []
    orig = fc.tmux_focus
    fc.tmux_focus = lambda n, p: calls.append(n)
    try:
        fc.handle_button(st, fc.VOLU, True)      # multi: Vol+ runs its config action, no jump
    finally:
        fc.tmux_focus = orig
    assert calls == []


def test_fader_scroll_goes_to_percent():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    st.set_state("a", "/x", "working", pane="%9", tmux="w")
    calls = []
    orig = fc.tmux_scroll
    fc.tmux_scroll = lambda pane, pct: calls.append((pane, pct))
    try:
        fc.fader_scroll(st, 127)             # full throw -> 100%
    finally:
        fc.tmux_scroll = orig
    assert calls == [("%9", 100)]


def test_fader_scroll_respects_pickup():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    st.set_state("a", "/x", "working", pane="%9", tmux="w")
    st.fader_target[0] = 64
    calls = []
    orig = fc.tmux_scroll
    fc.tmux_scroll = lambda p, pc: calls.append((p, pc))
    try:
        fc.fader_scroll(st, 60)              # below target -> ignored (no grab yet)
        fc.fader_scroll(st, 70)              # crossed -> scrolls
    finally:
        fc.tmux_scroll = orig
    assert calls == [("%9", round(70 * 100 / 127))]


def test_handle_fader_dispatches_by_ix():
    st = fc.State(cfg(sessions={"mode": "cockpit"}))
    seen = []
    orig = fc.fader_scroll
    fc.fader_scroll = lambda state, v: seen.append(("scroll", v))
    try:
        fc.handle_fader(st, 0, 100)          # ix 0 = scroll by default
    finally:
        fc.fader_scroll = orig
    assert seen == [("scroll", 100)]


def test_handle_fader_noop_when_not_cockpit():
    st = fc.State(cfg(sessions={"mode": "multi"}))
    seen = []
    orig = fc.fader_scroll
    fc.fader_scroll = lambda state, v: seen.append(v)
    try:
        fc.handle_fader(st, 0, 100)
    finally:
        fc.fader_scroll = orig
    assert seen == []                         # faders only act in cockpit mode


def main():
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
    print("all feldd-cc tests passed")


if __name__ == "__main__":
    main()
