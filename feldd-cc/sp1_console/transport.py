"""sp1_console.transport - transport interface + SerialTransport (USB CDC).

The daemon talks to the SP-1 only through `Transport`: connect / send(obj) /
on_event(cb) / on_link(cb) / close. Everything above the seam (State, cockpit,
autopilot, the hooks server) is transport-agnostic, so the same daemon runs over
USB (SerialTransport) or, next, BLE (BleTransport) with nothing above changing.
"""
from __future__ import annotations

import abc
import glob as _glob
import logging
import threading
import time
from typing import Callable, List

from . import protocol

log = logging.getLogger("sp1_console.transport")


class Transport(abc.ABC):
    """Base: owns the event/link callbacks and the inbound frame reassembler."""

    def __init__(self) -> None:
        self._event_cb: Callable[[dict], None] = lambda o: None
        self._link_cb: Callable[[str], None] = lambda s: None
        self._reasm = protocol.FrameReassembler()

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
        self._stop = threading.Event()
        self._reader: threading.Thread | None = None

    # link ----------------------------------------------------------------------
    def connect(self) -> bool:
        ports: List[str] = sorted(self._glob_fn(self._port_glob))
        if not ports:
            log.info("no SP-1 serial port matching %s", self._port_glob)
            self._emit_link("down")
            return False
        try:
            self._ser = self._open_fn(ports[0])
        except Exception as e:
            log.warning("serial open failed: %s", e)
            self._emit_link("down")
            return False
        if self._ser is None:
            self._emit_link("down")
            return False
        log.info("opened SP-1 at %s", ports[0])
        self._reset_frames()               # fresh boundary per link
        self._emit_link("up")
        self.send(protocol.monset(True))   # ask feldd for control events
        if self._autostart:
            self._start_reader()
        return True

    def _write(self, data: bytes) -> None:
        if self._ser is None:
            return
        try:
            self._ser.write(data)
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
            if self.connect():   # re-arms link up + monset + fresh frames
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
