#!/bin/bash
# One-time Zephyr/NCS workspace setup for the SP-1 controller firmware (plan M0.1).
# Idempotent-ish: re-running skips finished steps. Logs everything; long step is
# `west update` (multi-GB). Apple Silicon (arm64) assumed.
set -u
WS="$HOME/bnjmn/sp-1/.zephyr-ws"
SDK_VER="0.17.0"
ARCH="aarch64"   # Apple Silicon
log(){ echo "[$(date +%H:%M:%S)] $*"; }
mkdir -p "$WS"

log "=== 1/6 brew host deps ==="
for pkg in cmake ninja gperf ccache dfu-util dtc wget; do
  if brew list "$pkg" >/dev/null 2>&1; then log "  $pkg present"; else
    log "  installing $pkg"; brew install "$pkg" >>"$WS/brew.log" 2>&1 || { log "  BREW FAIL $pkg (see brew.log)"; }
  fi
done

log "=== 2/6 west (pip) ==="
if command -v west >/dev/null 2>&1; then log "  west present: $(west --version)"; else
  pip3 install --break-system-packages --user west >>"$WS/pip.log" 2>&1 || pip3 install --break-system-packages west >>"$WS/pip.log" 2>&1
  export PATH="$HOME/Library/Python/$(python3 -c 'import sys;print(f"{sys.version_info.major}.{sys.version_info.minor}")')/bin:$PATH"
  command -v west >/dev/null 2>&1 && log "  west installed" || { log "  WEST INSTALL FAILED (see pip.log)"; exit 1; }
fi

log "=== 3/6 clone sdk-nrf v3.3.0 ==="
if [ -d "$WS/nrf/.git" ]; then log "  nrf clone present"; else
  git clone --depth 1 https://github.com/nrfconnect/sdk-nrf --branch v3.3.0 "$WS/nrf" >>"$WS/clone.log" 2>&1 \
    && log "  cloned" || { log "  CLONE FAILED (see clone.log)"; exit 1; }
fi

log "=== 4/6 west init + update (LONG: multi-GB) ==="
cd "$WS" || exit 1
[ -d "$WS/.west" ] || west init -l nrf >>"$WS/west.log" 2>&1
log "  west update starting (this is the multi-GB pull; be patient)"
if west update >>"$WS/west.log" 2>&1; then log "  west update OK"; else log "  WEST UPDATE FAILED (see west.log)"; exit 1; fi
west zephyr-export >>"$WS/west.log" 2>&1
log "  installing python requirements"
pip3 install --break-system-packages -r zephyr/scripts/requirements.txt >>"$WS/pip.log" 2>&1
pip3 install --break-system-packages -r nrf/scripts/requirements.txt >>"$WS/pip.log" 2>&1

log "=== 5/6 Zephyr SDK $SDK_VER (cross-compiler) ==="
SDK_DIR="$WS/zephyr-sdk-$SDK_VER"
if [ -d "$SDK_DIR" ]; then log "  SDK present"; else
  TARBALL="zephyr-sdk-${SDK_VER}_macos-${ARCH}_minimal.tar.xz"
  URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VER}/${TARBALL}"
  log "  downloading $TARBALL"
  if wget -q "$URL" -O "$WS/$TARBALL" >>"$WS/sdk.log" 2>&1; then
    log "  extracting"; tar xf "$WS/$TARBALL" -C "$WS" >>"$WS/sdk.log" 2>&1 && rm -f "$WS/$TARBALL"
    log "  running setup.sh (installs arm-zephyr-eabi toolchain)"
    (cd "$SDK_DIR" && ./setup.sh -t arm-zephyr-eabi -c) >>"$WS/sdk.log" 2>&1 \
      && log "  SDK installed" || log "  SDK setup.sh issue (see sdk.log)"
  else log "  SDK DOWNLOAD FAILED (see sdk.log) - the minimal tarball name/URL may have changed for $SDK_VER; check https://github.com/zephyrproject-rtos/sdk-ng/releases/tag/v${SDK_VER}"; fi
fi

log "=== 6/6 verify: board visible ==="
export ZEPHYR_SDK_INSTALL_DIR="$SDK_DIR"
if west boards --board-root "$HOME/bnjmn/sp-1/reference/related-firmware/marisko" 2>/dev/null | grep -qx sp1; then
  log "  SUCCESS: board 'sp1' is visible. Workspace ready."
  log "  Build the firmware with:"
  log "    cd $WS && ZEPHYR_SDK_INSTALL_DIR=$SDK_DIR \\"
  log "      west build -b sp1 -d build $HOME/bnjmn/sp-1/firmware/app -- \\"
  log "      -DBOARD_ROOT=$HOME/bnjmn/sp-1/reference/related-firmware/marisko"
else
  log "  board 'sp1' NOT yet visible - check the logs in $WS/*.log"
fi
log "=== DONE ==="
