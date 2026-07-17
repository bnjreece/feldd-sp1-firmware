"""sp1_console.transport - transport interface + SerialTransport (USB CDC).

The daemon talks to the SP-1 only through `Transport`: connect / send(obj) /
on_event(cb) / on_link(cb) / close. Everything above the seam (State, cockpit,
autopilot, the hooks server) is transport-agnostic, so the same daemon runs over
USB (SerialTransport) or, next, BLE (BleTransport) with nothing above changing.
"""
from __future__ import annotations

import abc
import asyncio
import glob as _glob
import logging
import queue as _queue
import threading
import time
from typing import Callable, List, Optional

from . import protocol

log = logging.getLogger("sp1_console.transport")

# Standard BLE-MIDI service UUID. feldd advertises it (the console service is NOT
# advertised - the 31-byte ADV is full), so the host scans for this, connects,
# then verifies the console service post-connect.
BLE_MIDI_SERVICE = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"


class Transport(abc.ABC):
    """Base: owns the event/link callbacks and the inbound frame reassembler."""

    def __init__(self) -> None:
        self._event_cb: Callable[[dict], None] = lambda o: None
        self._link_cb: Callable[[str], None] = lambda s: None
        self._reasm = protocol.FrameReassembler()
        self._last_link: Optional[str] = None

    # registration --------------------------------------------------------------
    def on_event(self, cb: Callable[[dict], None]) -> "Transport":
        self._event_cb = cb or (lambda o: None)
        return self

    def on_link(self, cb: Callable[[str], None]) -> "Transport":
        self._link_cb = cb or (lambda s: None)
        return self

    # shared plumbing -----------------------------------------------------------
    def _ingest(self, data: bytes) -> None:
        """Feed inbound bytes through the framer; dispatch each complete object."""
        for obj in self._reasm.feed(data):
            try:
                self._event_cb(obj)
            except Exception:
                log.exception("event callback raised")

    def _reset_frames(self) -> None:
        """Drop any half-object so a reconnect starts on a clean boundary."""
        self._reasm = protocol.FrameReassembler()

    def _emit_link(self, state: str) -> None:
        if state == self._last_link:
            return                      # edge-only: no "down" spam on each failed poll
        self._last_link = state
        self._link_cb(state)

    def send(self, obj: dict) -> None:
        self._write(protocol.serialize(obj))

    # subclass contract ---------------------------------------------------------
    @abc.abstractmethod
    def _write(self, data: bytes) -> None: ...

    @abc.abstractmethod
    def connect(self) -> bool: ...

    @abc.abstractmethod
    def close(self) -> None: ...


def _default_open(port: str):
    import serial  # pyserial, imported lazily so tests need no hardware dep
    return serial.Serial(port, 115200, timeout=0.1)


class SerialTransport(Transport):
    """Today's USB CDC behavior, verbatim, plus the reconnect it never had."""

    def __init__(self, port_glob: str, *, glob_fn=_glob.glob, open_fn=_default_open,
                 autostart_reader: bool = True, reconnect: bool = True) -> None:
        super().__init__()
        self._port_glob = port_glob
        self._glob_fn = glob_fn
        self._open_fn = open_fn
        self._autostart = autostart_reader
        self._reconnect = reconnect
        self._ser = None
        self._wlock = threading.Lock()   # serialize writes: reconnect-monset (reader
                                         # thread) vs push_mask (render thread)
        self._stop = threading.Event()
        self._reader: threading.Thread | None = None

    # link ----------------------------------------------------------------------
    def connect(self) -> bool:
        if not self._open_port():
            self._emit_link("down")
            return False
        if self._autostart:
            self._start_reader()
        return True

    def _close_ser(self) -> None:
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
            self._ser = None

    def _open_port(self) -> bool:
        """Open the first matching port (no thread). Emits link-up + arms mon on
        success. Shared by connect() and the in-thread reconnect so a reconnect
        never spawns a second reader thread."""
        ports: List[str] = sorted(self._glob_fn(self._port_glob))
        if not ports:
            log.info("no SP-1 serial port matching %s", self._port_glob)
            return False
        self._close_ser()              # never leak a prior handle on reconnect
        try:
            self._ser = self._open_fn(ports[0])
        except Exception as e:
            log.warning("serial open failed: %s", e)
            return False
        if self._ser is None:
            return False
        log.info("opened SP-1 at %s", ports[0])
        self._reset_frames()               # fresh boundary per link
        self._emit_link("up")
        self.send(protocol.monset(True))   # ask feldd for control events
        return True

    def _write(self, data: bytes) -> None:
        ser = self._ser
        if ser is None:
            return
        try:
            with self._wlock:          # one whole frame per write, never interleaved
                ser.write(data)
        except Exception as e:
            log.warning("serial write error: %s", e)

    # reader --------------------------------------------------------------------
    def _pump_once(self) -> bool:
        """Read one chunk and ingest it. Returns False on empty/error (so the
        loop can back off or trigger a reconnect)."""
        if self._ser is None:
            return False
        try:
            data = self._ser.read(64)
        except Exception as e:
            log.warning("serial read error: %s", e)
            self._close_ser()          # dead fd: drop it so _reader_loop reconnects
            return False
        if data:
            self._ingest(data)
            return True
        return False

    def _start_reader(self) -> None:
        self._stop.clear()
        self._reader = threading.Thread(target=self._reader_loop, daemon=True)
        self._reader.start()

    def _reader_loop(self) -> None:
        while not self._stop.is_set():
            ok = self._pump_once()
            if ok:
                continue
            # nothing read: if the port went away, drop the link and (optionally)
            # re-glob/reopen with a jittered backoff, forever.
            if self._ser is None or not self._port_alive():
                self._emit_link("down")
                if not self._reconnect:
                    return
                self._reopen_with_backoff()
            else:
                time.sleep(0.02)   # idle poll, port still present

    def _port_alive(self) -> bool:
        try:
            return bool(self._glob_fn(self._port_glob))
        except Exception:
            return False

    def _reopen_with_backoff(self) -> None:
        delay = 0.5
        while not self._stop.is_set():
            if self._open_port():   # reuses THIS reader thread; no new thread
                return
            time.sleep(delay)
            delay = min(delay * 2, 30.0)

    def close(self) -> None:
        self._stop.set()
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass


class BleTransport(Transport):
    """Wireless transport over the custom CYW20706 GATT console service (bleak).

    Core (chunked writes, notification reassembly, link reset) is transport-agnostic
    and unit-tested. The scan/connect/subscribe session runs bleak's asyncio loop in
    a dedicated thread; that path needs a real radio + provisioned module and is
    validated by the M3 burner round-trip, not unit tests.
    """

    def __init__(self, *, service_uuid: str, rx_uuid: str, tx_uuid: str,
                 name: Optional[str] = None, address: Optional[str] = None,
                 sink: Optional[Callable[[bytes], None]] = None,
                 autostart_loop: bool = True, reconnect_max_s: float = 30.0) -> None:
        super().__init__()
        self._service_uuid = service_uuid
        self._rx = rx_uuid
        self._tx = tx_uuid
        self._name = name
        self._address = address
        self._sink = sink              # callable(bytes): WHOLE outbound frame
        self._autostart_loop = autostart_loop
        self._reconnect_max_s = reconnect_max_s
        self._stop = threading.Event()
        self._loop: Optional[asyncio.AbstractEventLoop] = None
        self._thread: Optional[threading.Thread] = None
        self._disp: Optional[threading.Thread] = None
        self._rawq: "_queue.Queue" = _queue.Queue()   # inbound bytes -> dispatch thread
        self._outq: Optional[asyncio.Queue] = None     # whole frames (created in loop)
        self._wake: Optional[asyncio.Event] = None     # end the session (disconnect/close)

    # transport-core (unit-tested) ---------------------------------------------
    def _write(self, data: bytes) -> None:
        s = self._sink
        if s is not None:
            s(data)                    # whole frame; the writer coroutine chunks it in order

    def _on_notify(self, data: bytes) -> None:
        self._ingest(bytes(data))      # reassemble + dispatch (on the dispatch thread in prod)

    def _on_disconnect(self) -> None:
        self._reset_frames()           # clear any half object before the next link
        self._emit_link("down")

    def _has_console(self, service_uuids) -> bool:
        fold = self._service_uuid.lower()
        return any(str(u).lower() == fold for u in service_uuids)

    # bleak session (prod path; needs hardware -> M3 round-trip, not unit-tested)
    def connect(self) -> bool:
        if not self._autostart_loop:
            return False               # tests drive the core directly
        if self._thread is not None and self._thread.is_alive():
            return True                # re-entrancy guard: one session per instance
        self._stop.clear()
        self._disp = threading.Thread(target=self._dispatch_loop, daemon=True)
        self._disp.start()
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop_runner, daemon=True)
        self._thread.start()
        return True

    def _dispatch_loop(self) -> None:
        # inbound handlers (handle_button -> tmux subprocess) can block for seconds;
        # run them here, off the asyncio loop, so BLE I/O never freezes.
        while True:
            data = self._rawq.get()
            if data is None:
                return
            try:
                self._on_notify(data)
            except Exception:
                log.exception("BLE dispatch")

    def _loop_runner(self) -> None:
        asyncio.set_event_loop(self._loop)
        try:
            self._loop.run_until_complete(self._session())
        except Exception:
            log.exception("BLE loop crashed")
        finally:
            self._loop.close()

    async def _scan(self, scanner):
        if self._address:
            return await scanner.find_device_by_address(self._address, timeout=10.0)

        def _match(dev, adv):
            if self._name and dev.name == self._name:
                return True
            uuids = [str(u).lower() for u in (adv.service_uuids or [])]
            return BLE_MIDI_SERVICE in uuids

        return await scanner.find_device_by_filter(_match, timeout=10.0)

    async def _writer_task(self, client) -> None:
        # single writer: dequeue a WHOLE frame and write its chunks in order, so
        # chunks of concurrent frames can never interleave on the wire.
        try:
            while True:
                frame = await self._outq.get()
                for chunk in protocol.chunks(frame, protocol.BLE_CHUNK):
                    try:
                        await client.write_gatt_char(self._rx, chunk, response=False)
                    except Exception as e:
                        log.warning("BLE write error: %s", e)
                        break          # drop the rest of this frame; firmware resyncs
        except asyncio.CancelledError:
            pass

    async def _wait_or_wake(self, secs: float) -> None:
        try:
            await asyncio.wait_for(self._wake.wait(), timeout=secs)
        except asyncio.TimeoutError:
            pass

    async def _session(self) -> None:
        from bleak import BleakClient, BleakScanner   # lazy: no hardware dep for tests
        self._outq = asyncio.Queue()
        self._wake = asyncio.Event()
        backoff = 1.0
        while not self._stop.is_set():
            try:
                dev = await self._scan(BleakScanner)
                if dev is None:
                    await self._wait_or_wake(backoff)
                    backoff = min(backoff * 2, self._reconnect_max_s)
                    continue
                self._wake.clear()

                def _dc(_c):
                    self._loop.call_soon_threadsafe(self._wake.set)

                async with BleakClient(dev, disconnected_callback=_dc) as client:
                    if not self._has_console([s.uuid for s in client.services]):
                        raise RuntimeError("connected device has no feldd console service")
                    await client.start_notify(
                        self._tx, lambda _s, d: self._rawq.put(bytes(d)))
                    self._sink = lambda frame: self._loop.call_soon_threadsafe(
                        self._outq.put_nowait, frame)
                    writer = asyncio.create_task(self._writer_task(client))
                    self._reset_frames()
                    self._emit_link("up")
                    self.send(protocol.monset(True))     # arm mon stream
                    self.send(protocol.console(True))     # declare the console session
                    self.send(protocol.mode_query())      # learn current mode
                    backoff = 1.0
                    await self._wake.wait()               # disconnect OR close()
                    writer.cancel()
            except Exception as e:
                log.warning("BLE session ended: %s", e)
            self._sink = None
            self._on_disconnect()
            if self._stop.is_set():
                break
            await self._wait_or_wake(backoff)
            backoff = min(backoff * 2, self._reconnect_max_s)
        self._rawq.put(None)             # stop the dispatch thread

    def close(self) -> None:
        self._stop.set()
        if self._loop is not None and self._wake is not None:
            try:
                self._loop.call_soon_threadsafe(self._wake.set)
            except Exception:
                pass
        self._rawq.put(None)             # unblock the dispatch thread even if no session
