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


def test_push_mask_dedupes_same_value():
    d, ft = _dev()
    d.push_mask(5)
    d.push_mask(5)
    assert ft.sent.count(P.led(5)) == 1


def test_push_mask_re_renders_after_a_link_cycle():
    d, ft = _dev()
    d.push_mask(5)
    ft.sent.clear()
    d.push_mask(5)
    assert ft.sent == []               # deduped while the link stays up
    ft.cycle()                          # reconnect -> device forgot the mask
    d.push_mask(5)
    assert P.led(5) in ft.sent         # forced re-render of the same mask


def test_release_is_idempotent():
    d, ft = _dev()
    d.release()
    n = len(ft.sent)
    d.release()
    assert len(ft.sent) == n           # second release sends nothing


def test_dry_device_never_builds_a_transport():
    d = feldd_cc.Device("glob", dry=True)
    d.push_mask(5)
    d.release()
    assert d._t is None
