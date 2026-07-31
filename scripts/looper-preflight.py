#!/usr/bin/env python3
"""Read-only SP-1 source/image safety preflight. This never contacts a device."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path

APP_BASE = 0x00020000
APP_SIZE = 0x000BF000
APP_END = APP_BASE + APP_SIZE
STORAGE_BASE = 0x000F7000
STORAGE_SIZE = 0x00008000
RESERVED_BASE = 0x000FF000
SRAM_BASE = 0x20000000
SRAM_END = 0x20040000


class Check:
    def __init__(self) -> None:
        self.failed = False

    def require(self, condition: bool, message: str) -> None:
        mark = "PASS" if condition else "FAIL"
        print(f"{mark}  {message}")
        if not condition:
            self.failed = True


def text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8")
    except OSError as exc:
        raise SystemExit(f"cannot read {path}: {exc}") from exc


def check_source(repo: Path, check: Check) -> None:
    board = repo.parent / "marisko" / "boards" / "arm" / "sp1"
    kconfig = text(board / "Kconfig.defconfig")
    dts = text(board / "sp1.dts")
    overlay = text(repo / "firmware" / "app" / "app.overlay")
    sysbuild = text(repo / "firmware" / "app" / "sysbuild.conf")
    main = text(repo / "firmware" / "app" / "src" / "main.c")

    check.require(
        bool(re.search(r"config\s+ROM_START_OFFSET\s+default\s+0x20000", kconfig)),
        "link offset remains 0x20000",
    )
    check.require(
        "reg = <0x00000000 0x00020000>;" in dts,
        "bootloader partition remains 0x00000..0x1ffff",
    )
    check.require(
        "reg = <0x00020000 0x000BF000>;" in dts,
        "application partition remains 0x20000..0xdefff",
    )
    check.require(
        "reg = <0x000F7000 0x00008000>;" in overlay,
        "NVS remains 0xf7000..0xfefff",
    )
    check.require(
        STORAGE_BASE + STORAGE_SIZE == RESERVED_BASE and APP_END <= STORAGE_BASE,
        "application and NVS end before reserved page 0xff000",
    )
    check.require(
        "SB_CONFIG_PARTITION_MANAGER=n" in sysbuild,
        "NCS Partition Manager remains disabled",
    )
    check.require(
        "buttons_dfu_held()" in main and "enter_dfu();" in main,
        "Track 1+4 runtime DFU escape remains wired",
    )
    check.require(
        "feed_wdt();" in main and "for (;;)" in main,
        "main loop retains watchdog feeding",
    )


def check_image(path: Path, check: Check) -> None:
    try:
        data = path.read_bytes()
    except OSError as exc:
        raise SystemExit(f"cannot read {path}: {exc}") from exc
    check.require(len(data) >= 8, "image contains an ARM vector table")
    if len(data) < 8:
        return

    stack, reset = struct.unpack_from("<II", data)
    reset_addr = reset & ~1
    digest = hashlib.sha256(data).hexdigest()
    print(f"INFO  image={path}")
    print(f"INFO  bytes={len(data)} sha256={digest}")
    print(f"INFO  initial_sp=0x{stack:08x} reset=0x{reset:08x}")

    check.require(SRAM_BASE <= stack <= SRAM_END, "initial stack pointer is in nRF52840 SRAM")
    check.require((reset & 1) == 1, "reset vector has the Thumb bit set")
    check.require(APP_BASE <= reset_addr < APP_END, "reset handler points inside the app partition")
    check.require(len(data) <= APP_SIZE, "stripped image fits entirely in the app partition")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify SP-1 source layout and a stripped app .bin without flashing."
    )
    parser.add_argument("image", type=Path, help="stripped application binary to inspect")
    args = parser.parse_args()

    repo = Path(__file__).resolve().parent.parent
    check = Check()
    check_source(repo, check)
    check_image(args.image.resolve(), check)
    print("RESULT", "FAIL — do not flash" if check.failed else "PASS — eligible for later hardware review")
    return 1 if check.failed else 0


if __name__ == "__main__":
    sys.exit(main())
