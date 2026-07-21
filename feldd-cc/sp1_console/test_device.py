"""Device-adapter tests (the daemon's push_mask/release over a Transport).

Injects a fake transport so the link-up/down re-render contract is exercised with
no hardware. Covers Fable/opus finding #3 (no last_mask reset on link-up).
"""
import feldd_cc
from sp1_console import protocol as P


class FakeTransport:
    def __init__(self):
        self.sent = []
        self._event_cb = lambda o: None
        self._link_cb = lambda s: None

    def on_event(self, cb):
        self._event_cb = cb
        return self

    def on_link(self, cb):
        self._link_cb = cb
        return self

    def send(self, obj):
        self.sent.append(obj)

    def connect(self):
        self._link_cb("up")            # link comes up on connect, like the real ones
        return True

    def close(self):
        pass

    def cycle(self):
        self._link_cb("down")
        self._link_cb("up")


def _dev():
    ft = FakeTransport()
    d = feldd_cc.Device("glob", dry=False, transport=ft)
    d.read_loop(None)                  # registers callbacks + connect (fires 'up')
    return d, ft


def test_push_dedupes_same_frame():
    d, ft = _dev()
    d.push(5)
    d.push(5)
    assert ft.sent.count(P.led(5)) == 1


def test_push_re_renders_after_a_link_cycle():
    d, ft = _dev()
    d.push(5)
    ft.sent.clear()
    d.push(5)
    assert ft.sent == []               # deduped while the link stays up
    ft.cycle()                          # reconnect -> device forgot the mask
    d.push(5)
    assert P.led(5) in ft.sent         # forced re-render of the same frame


def test_push_all_full_brightness_is_a_mask_only_frame():
    d, ft = _dev()
    d.push(5, bri=[100] * 8)            # all full -> byte-identical to a v1 mask-only led
    assert ft.sent[-1] == P.led(5)     # no 'bri' key emitted


def test_release_is_idempotent():
    d, ft = _dev()
    d.release()
    n = len(ft.sent)
    d.release()
    assert len(ft.sent) == n           # second release sends nothing


def test_dry_device_never_builds_a_transport():
    d = feldd_cc.Device("glob", dry=True)
    d.push(5)
    d.release()
    assert d._t is None
