"""sp1_console.cb_transport - macOS CoreBluetooth "attach to the paired device" transport.

On macOS the SP-1 is normally PAIRED in System Settings (so it works as a BT keyboard
/ MIDI device). While paired, the OS holds the connection and the device STOPS
advertising -- so bleak's scan-based BleTransport can never find it (BleakDeviceNotFound,
empty scans, the whole "keyboard-grab" fight). The correct macOS API is
CoreBluetooth's `retrieveConnectedPeripherals(withServices:)`: it hands back the device
macOS already holds, and the app then opens the custom console GATT service on that SAME
link. LEDs out + button/fader monitor in coexist with the OS keyboard/MIDI use -- no
third mode. Hardware-validated 2026-07-20 (full round-trip on a system-paired unit).

Design: the pure peripheral picker (`pick_feldd`) and the transport class carry NO import
of pyobjc, so they load (and unit-test) anywhere; every CoreBluetooth / libdispatch symbol
is imported lazily inside `connect()` and the delegate, mirroring how BleTransport imports
bleak lazily. The CoreBluetooth session runs on its own serial dispatch queue, so it needs
no main-thread NSRunLoop and slots into the daemon's threading model unchanged.
"""
from __future__ import annotations

import logging
from typing import Optional

from . import protocol
from .transport import Transport

log = logging.getLogger("sp1_console.cb_transport")

# Services macOS reliably knows the feldd offers (so retrieveConnected returns it).
HID_SERVICE_UUID = "00001812-0000-1000-8000-00805f9b34fb"
MIDI_SERVICE_UUID = "03b80e5a-ede8-4b33-a751-6ce34ec4c700"


def pick_feldd(peripherals, name_hint: str = "feldd"):
    """Return the first peripheral whose name contains `name_hint` (case-insensitive),
    else None.

    Safety-critical: retrieveConnectedPeripherals(withServices:) is queried by services
    that MANY devices share (HID, battery), so AirPods / mice / keyboards appear in the
    list too. Never return a non-matching peripheral -- grabbing the wrong one would
    hijack an unrelated device. Accepts either CBPeripheral objects (``.name()``) or
    ``(name, id)`` tuples so the logic is unit-testable without a radio.
    """
    hint = name_hint.lower()
    for p in peripherals:
        name = (p[0] if isinstance(p, tuple) else p.name()) or ""
        if hint in name.lower():
            return p
    return None


_DELEGATE_CLASS = None


def _delegate_class():
    """Build (once) the CBCentralManager/CBPeripheral delegate NSObject subclass. Imports
    pyobjc lazily so this module imports on any platform; only called from connect()."""
    global _DELEGATE_CLASS
    if _DELEGATE_CLASS is not None:
        return _DELEGATE_CLASS

    import objc
    from Foundation import NSObject
    from CoreBluetooth import CBUUID, CBManagerStatePoweredOn

    class _FelddCBDelegate(NSObject):
        # NOTE: every method here is an ObjC selector, so arg-count must match the
        # trailing-underscore count (PyObjC rule). Non-selector helpers live on the
        # transport (plain Python), reached via self._t.
        def initWithTransport_(self, t):
            self = objc.super(_FelddCBDelegate, self).init()
            if self is None:
                return None
            self._t = t
            return self

        def centralManagerDidUpdateState_(self, mgr):
            if mgr.state() != CBManagerStatePoweredOn:
                self._t._on_link_down()
                return
            self.doAttachWithManager_(mgr)

        def doAttachWithManager_(self, mgr):
            t = self._t
            if t._closed:
                return
            svc_uuids = [CBUUID.UUIDWithString_(u) for u in t._connect_services]
            periphs = list(mgr.retrieveConnectedPeripheralsWithServices_(svc_uuids))
            chosen = pick_feldd(periphs, t._name_hint)
            if chosen is None:
                t._schedule_retry(mgr)      # macOS isn't holding a feldd yet
                return
            t._peripheral = chosen
            chosen.setDelegate_(self)
            mgr.connectPeripheral_options_(chosen, None)

        def centralManager_didConnectPeripheral_(self, mgr, peripheral):
            peripheral.discoverServices_(None)

        def centralManager_didFailToConnectPeripheral_error_(self, mgr, peripheral, error):
            self._t._schedule_retry(mgr)

        def centralManager_didDisconnectPeripheral_error_(self, mgr, peripheral, error):
            self._t._on_link_down()
            self._t._schedule_retry(mgr)

        def peripheral_didDiscoverServices_(self, peripheral, error):
            if error:
                log.warning("discoverServices error: %s", error)
                return
            want = CBUUID.UUIDWithString_(self._t._svc)
            svc = next((s for s in peripheral.services() if s.UUID().isEqual_(want)), None)
            if svc is None:
                log.warning("feldd connected but console service not exposed by macOS")
                return
            rx = CBUUID.UUIDWithString_(self._t._rx_uuid)
            tx = CBUUID.UUIDWithString_(self._t._tx_uuid)
            peripheral.discoverCharacteristics_forService_([rx, tx], svc)

        def peripheral_didDiscoverCharacteristicsForService_error_(self, peripheral, service, error):
            if error:
                log.warning("discoverCharacteristics error: %s", error)
                return
            rxu = CBUUID.UUIDWithString_(self._t._rx_uuid)
            txu = CBUUID.UUIDWithString_(self._t._tx_uuid)
            rx = tx = None
            for c in service.characteristics():
                if c.UUID().isEqual_(rxu):
                    rx = c
                elif c.UUID().isEqual_(txu):
                    tx = c
            if tx is not None:
                peripheral.setNotifyValue_forCharacteristic_(True, tx)
            self._t._on_link_up(peripheral, rx)

        def peripheral_didUpdateValueForCharacteristic_error_(self, peripheral, characteristic, error):
            if error or characteristic.value() is None:
                return
            self._t._on_rx(bytes(characteristic.value()))

    _DELEGATE_CLASS = _FelddCBDelegate
    return _DELEGATE_CLASS


class CoreBluetoothTransport(Transport):
    """Attach to the system-connected (paired) feldd and drive its console service.

    The transport core (framing, event/link dispatch) is the unit-tested base; only the
    CoreBluetooth session below needs a radio (hardware-validated, not unit-tested).
    """

    def __init__(self, *, service_uuid: str, rx_uuid: str, tx_uuid: str,
                 name_hint: str = "feldd", connect_service_uuids=None,
                 autostart: bool = True, arm: bool = True, retry_delay_s: float = 2.0) -> None:
        super().__init__()
        self._svc = service_uuid
        self._rx_uuid = rx_uuid
        self._tx_uuid = tx_uuid
        self._name_hint = name_hint
        self._connect_services = list(connect_service_uuids) if connect_service_uuids else [
            HID_SERVICE_UUID, MIDI_SERVICE_UUID, service_uuid]
        self._autostart = autostart
        self._arm = arm                      # send monset/console/mode on link-up (like BleTransport)
        self._retry_delay_s = retry_delay_s
        self._mgr = None
        self._delegate = None
        self._queue = None
        self._peripheral = None
        self._rx_char = None
        self._closed = False

    # ---- Transport contract --------------------------------------------------
    def _write(self, data: bytes) -> None:
        q = self._queue
        if q is None:
            return
        from libdispatch import dispatch_async

        def _do():
            p, rx = self._peripheral, self._rx_char
            if p is None or rx is None:
                return
            from Foundation import NSData
            from CoreBluetooth import CBCharacteristicWriteWithoutResponse
            for chunk in protocol.chunks(data):     # whole frame, chunks in order
                nsd = NSData.dataWithBytes_length_(chunk, len(chunk))
                p.writeValue_forCharacteristic_type_(nsd, rx, CBCharacteristicWriteWithoutResponse)

        dispatch_async(q, _do)                       # serialize onto the CB queue

    def connect(self) -> bool:
        if not self._autostart:
            return False                             # tests drive the core directly
        if self._mgr is not None:
            return True                              # one session per instance
        from libdispatch import dispatch_queue_create
        from CoreBluetooth import CBCentralManager
        self._closed = False
        self._queue = dispatch_queue_create(b"engineering.feldd.console.cb", None)
        self._delegate = _delegate_class().alloc().initWithTransport_(self)
        self._mgr = CBCentralManager.alloc().initWithDelegate_queue_(self._delegate, self._queue)
        return True

    def close(self) -> None:
        self._closed = True
        mgr, p, q = self._mgr, self._peripheral, self._queue
        if mgr is not None and p is not None and q is not None:
            try:
                from libdispatch import dispatch_async
                dispatch_async(q, lambda: mgr.cancelPeripheralConnection_(p))
            except Exception:
                log.exception("cancelPeripheralConnection failed")
        self._on_link_down()
        self._mgr = None

    # ---- callbacks from the delegate (plain Python, run on the CB queue) ------
    def _on_link_up(self, peripheral, rx_char) -> None:
        self._peripheral = peripheral
        self._rx_char = rx_char
        self._reset_frames()
        self._emit_link("up")
        if self._arm:
            self.send(protocol.monset(True))
            self.send(protocol.console(True))
            self.send(protocol.mode_query())

    def _on_rx(self, data: bytes) -> None:
        self._ingest(data)

    def _on_link_down(self) -> None:
        self._rx_char = None
        self._reset_frames()
        self._emit_link("down")

    def _schedule_retry(self, mgr) -> None:
        if self._closed or self._queue is None:
            return
        from libdispatch import dispatch_after, dispatch_time, DISPATCH_TIME_NOW
        when = dispatch_time(DISPATCH_TIME_NOW, int(self._retry_delay_s * 1e9))
        dispatch_after(when, self._queue, lambda: self._delegate.doAttachWithManager_(mgr))
