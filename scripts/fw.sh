#!/bin/bash
# SP-1 controller firmware dev wrapper - one command for build/package/inspect.
# Hides the long west incantation + the sysbuild/SDK/PATH gotchas.
#   ./scripts/fw.sh build      pristine build for the sp1 board
#   ./scripts/fw.sh inc        incremental build (no -p)
#   ./scripts/fw.sh bin        build + produce the flashable sp1_firmware.bin
#   ./scripts/fw.sh info       size + link-address sanity of the current build
#   ./scripts/fw.sh test       run the host unit tests
#   ./scripts/fw.sh monitor    open the SP-1 CDC serial port (protocol/monitor stream)
#   ./scripts/fw.sh clean      remove the build dir
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
WS="${FELDD_ZEPHYR_WS:-$ROOT/.zephyr-ws}"
SDK="${ZEPHYR_SDK_INSTALL_DIR:-$WS/zephyr-sdk-0.17.0}"
APP="$ROOT/firmware/app"
BOARD_ROOT="${FELDD_BOARD_ROOT:-$(cd "$ROOT/.." && pwd)/marisko}"
BUILD="$WS/build"
ELF="$BUILD/app/zephyr/zephyr.elf"        # NCS sysbuild path (note the app/ segment)
BIN_OUT="$APP/sp1_firmware.bin"
export PATH="$SDK/arm-zephyr-eabi/bin:$PATH"
OBJCOPY="$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-objcopy"
OBJDUMP="$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-objdump"

require_build_env(){
  [ -d "$WS/.west" ] || {
    echo "missing west workspace: $WS"
    echo "set FELDD_ZEPHYR_WS or install the NCS v3.3.0 workspace there"
    exit 1
  }
  [ -x "$OBJCOPY" ] || {
    echo "missing Zephyr ARM SDK: $SDK"
    echo "set ZEPHYR_SDK_INSTALL_DIR to a Zephyr SDK with arm-zephyr-eabi"
    exit 1
  }
  [ -d "$BOARD_ROOT/boards/arm/sp1" ] || {
    echo "missing SP-1 board root: $BOARD_ROOT"
    echo "set FELDD_BOARD_ROOT to the marisko checkout"
    exit 1
  }
  command -v west >/dev/null 2>&1 || {
    echo "west is not installed or not on PATH"
    exit 1
  }
}

do_build(){ local p="$1"; require_build_env; cd "$WS" || exit 1
  ZEPHYR_SDK_INSTALL_DIR="$SDK" west build -b sp1 -d build "$APP" $p -- -DBOARD_ROOT="$BOARD_ROOT"; }

do_bin(){
  [ -f "$ELF" ] || { echo "no ELF - run a build first"; exit 1; }
  "$OBJCOPY" -O binary --gap-fill 0xFF --remove-section=.debug_* --remove-section=.comment \
    --remove-section=.ARM.attributes "$ELF" /tmp/sp1_full.bin || exit 1
  dd if=/tmp/sp1_full.bin of="$BIN_OUT" bs=1 skip=131072 2>/dev/null    # strip the 0x20000 pad
  rm -f /tmp/sp1_full.bin
  echo "flashable image -> $BIN_OUT  ($(wc -c <"$BIN_OUT") bytes)"
  echo "flash it at https://solderless.engineering (Chrome): enter bootloader (power off, hold track 1+4, plug USB), upload this .bin"
}

do_info(){
  require_build_env
  [ -f "$ELF" ] || { echo "no ELF - run a build first"; exit 1; }
  echo "== sections (vector table must be VMA 0x00020000) =="
  "$OBJDUMP" -h "$ELF" | grep -iE 'Idx|vector|text|rom_start' | head
  echo "== size =="; "$SDK/arm-zephyr-eabi/bin/arm-zephyr-eabi-size" "$ELF"
}

case "${1:-}" in
  build)   do_build "-p" ;;
  inc)     do_build "" ;;
  bin)     do_build "-p" && do_bin ;;
  info)    do_info ;;
  test)    cd "$ROOT/firmware/test" && make clean >/dev/null 2>&1; make test ;;
  monitor) port=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)
           [ -n "$port" ] && { echo "opening $port (ctrl-a k to quit screen)"; screen "$port" 115200; } \
             || echo "no /dev/cu.usbmodem* - is the SP-1 plugged in and flashed?" ;;
  clean)   rm -rf "$BUILD" && echo "cleaned $BUILD" ;;
  *) sed -n '2,12p' "$0" ;;
esac
