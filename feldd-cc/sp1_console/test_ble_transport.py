"""Tests for BleTransport's transport-core: chunked writes, notification
reassembly, link-drop frame reset. The bleak scan/connect/subscribe path needs a
real radio + module and is validated by the M3 burner round-trip, not here; these
lock the byte-level behavior the wire depends on.
"""
from sp1_console import protocol as P
from sp1_console.transport import BleTransport

SVC = "fe1ddcc0-0001-4b1e-9c81-1a2b3c4d5e01"
RX = "fe1ddcc0-0002-4b1e-9c81-1a2b3c4d5e01"
TX = "fe1ddcc0-0003-4b1e-9c81-1a2b3c4d5e01"


def _ble(writer):
    return BleTransport(name="feldd SP-1", service_uuid=SVC, rx_uuid=RX, tx_uuid=TX,
                        writer=writer, autostart_loop=False)


def test_long_write_is_chunked_to_the_20_byte_cap_and_rejoins():
    sent = []
    t = _ble(sent.append)
    obj = P.led(mask=0xFF, bri=[100, 90, 80, 70, 60, 50, 40, 30])   # > 20 bytes
    t.send(obj)
    assert len(P.serialize(obj)) > 20                # precondition: needs chunking
    assert all(len(c) <= 20 for c in sent)
    assert b"".join(sent) == P.serialize(obj)


def test_short_write_is_a_single_chunk():
    sent = []
    t = _ble(sent.append)
    t.send(P.led(mask=5))
    assert sent == [b'{"t":"led","mask":5}']


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
