"""Tests for the transport layer: the interface + SerialTransport (USB).

Injectable glob/open so the whole thing exercises without a real serial port.
BleTransport lands next; these lock the seam every transport shares.
"""
from sp1_console import protocol as P
from sp1_console.transport import SerialTransport


class FakeSerial:
    def __init__(self):
        self.written = bytearray()
        self._reads = []
        self.closed = False

    def queue_read(self, data: bytes):
        self._reads.append(data)

    def write(self, data):
        self.written += data

    def read(self, n):
        return self._reads.pop(0) if self._reads else b""

    def close(self):
        self.closed = True


def _st(fs, ports=("/dev/cu.usbmodemX",)):
    return SerialTransport(
        "glob", glob_fn=lambda pat: list(ports), open_fn=lambda port: fs,
        autostart_reader=False,
    )


def test_send_serializes_and_writes_exact_bytes():
    fs = FakeSerial()
    t = _st(fs)
    t.connect()
    t.send(P.led(mask=5))
    assert b'{"t":"led","mask":5}' in bytes(fs.written)


def test_connect_arms_the_monitor_stream():
    fs = FakeSerial()
    t = _st(fs)
    t.connect()
    assert b'{"t":"monset","on":true}' in bytes(fs.written)


def test_connect_fires_link_up():
    fs = FakeSerial()
    seen = []
    t = _st(fs)
    t.on_link(seen.append)
    assert t.connect() is True
    assert seen == ["up"]


def test_no_matching_port_fires_link_down_and_does_not_raise():
    seen = []
    t = SerialTransport("glob", glob_fn=lambda pat: [], open_fn=lambda port: None,
                        autostart_reader=False)
    t.on_link(seen.append)
    assert t.connect() is False
    assert seen == ["down"]


def test_incoming_bytes_dispatch_parsed_objects_to_on_event():
    fs = FakeSerial()
    got = []
    t = _st(fs)
    t.on_event(got.append)
    t.connect()
    fs.queue_read(b'{"t":"mon","k":"b","ix":2,"s":1}')
    t._pump_once()
    assert got == [{"t": "mon", "k": "b", "ix": 2, "s": 1}]


def test_events_reassemble_across_reads():
    fs = FakeSerial()
    got = []
    t = _st(fs)
    t.on_event(got.append)
    t.connect()
    fs.queue_read(b'{"t":"mon","k":"f",')
    fs.queue_read(b'"ix":0,"v":9}')
    t._pump_once()
    t._pump_once()
    assert got == [{"t": "mon", "k": "f", "ix": 0, "v": 9}]


def test_frame_state_resets_on_reconnect():
    # a half-object left in the buffer from a dropped link must not corrupt the
    # first object after reconnect
    fs = FakeSerial()
    got = []
    t = _st(fs)
    t.on_event(got.append)
    t.connect()
    fs.queue_read(b'{"t":"mon","k":"b","ix')    # truncated: link drops mid-object
    t._pump_once()
    t.connect()                                 # reconnect -> fresh frame state
    fs.queue_read(b'{"t":"mon","k":"b","ix":1,"s":1}')
    t._pump_once()
    assert got == [{"t": "mon", "k": "b", "ix": 1, "s": 1}]


def test_close_closes_the_port():
    fs = FakeSerial()
    t = _st(fs)
    t.connect()
    t.close()
    assert fs.closed is True
