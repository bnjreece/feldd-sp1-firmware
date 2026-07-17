"""Tests for BleTransport's transport-core: whole-frame writes, notification
reassembly, link-drop frame reset, console-service detection. The bleak
scan/connect/subscribe session (single-writer ordering + dispatch thread + close
handling) needs a real radio + module and is validated by the M3 burner round-trip,
not here; these lock the byte-level behavior the wire depends on.
"""
from sp1_console import protocol as P
from sp1_console.transport import BleTransport

SVC = "fe1ddcc0-0001-4b1e-9c81-1a2b3c4d5e01"
RX = "fe1ddcc0-0002-4b1e-9c81-1a2b3c4d5e01"
TX = "fe1ddcc0-0003-4b1e-9c81-1a2b3c4d5e01"


def _ble(sink):
    return BleTransport(name="feldd SP-1", service_uuid=SVC, rx_uuid=RX, tx_uuid=TX,
                        sink=sink, autostart_loop=False)


def test_write_enqueues_the_whole_frame_not_interleavable_chunks():
    # the single writer coroutine chunks whole frames in order (chunk correctness
    # is protocol.chunks' job); _write must hand it ONE complete frame, so two
    # concurrent frames can never interleave on the wire.
    out = []
    t = _ble(out.append)
    obj = P.led(mask=0xFF, bri=[100, 90, 80, 70, 60, 50, 40, 30])
    t.send(obj)
    assert out == [P.serialize(obj)]


def test_write_before_connect_is_dropped_not_raised():
    t = BleTransport(service_uuid=SVC, rx_uuid=RX, tx_uuid=TX, sink=None,
                     autostart_loop=False)
    t.send(P.led(mask=5))              # sink None -> dropped (re-renders on link-up)


def test_notifications_reassemble_across_chunks_and_dispatch():
    got = []
    t = _ble(lambda d: None)
    t.on_event(got.append)
    line = P.serialize({"t": "mon", "k": "b", "ix": 3, "s": 1})
    for chunk in P.chunks(line, 20):
        t._on_notify(chunk)
    assert got == [{"t": "mon", "k": "b", "ix": 3, "s": 1}]


def test_link_drop_resets_frame_state():
    got = []
    t = _ble(lambda d: None)
    t.on_event(got.append)
    t._on_notify(b'{"t":"mon","k":"b","ix')     # partial object arrives
    t._on_disconnect()                           # link drops
    line = P.serialize({"t": "mon", "k": "f", "ix": 0, "v": 1})
    for chunk in P.chunks(line, 20):
        t._on_notify(chunk)
    assert got == [{"t": "mon", "k": "f", "ix": 0, "v": 1}]


def test_disconnect_fires_link_down():
    seen = []
    t = _ble(lambda d: None)
    t.on_link(seen.append)
    t._on_disconnect()
    assert seen == ["down"]


def test_has_console_service_detects_presence_case_insensitively():
    t = _ble(lambda d: None)
    assert t._has_console([SVC.upper(), "00001812-0000-1000-8000-00805f9b34fb"]) is True
    assert t._has_console(["00001812-0000-1000-8000-00805f9b34fb"]) is False
