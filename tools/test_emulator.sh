#!/usr/bin/env bash
# Quick test cycle on a Pebble emulator: reset SPI flash from the SDK template
# (works around the "Waiting for firmware to boot" hang that piles up after
# repeated installs), install the current build, screenshot, then kill the
# emulator cleanly. The qemu cleanup is important because qemu-pebble likes
# to zombie at 100 % CPU when the app crashes.
#
# Usage: ./test_emulator.sh [platform] [out.png]
#        platform: aplite | basalt | chalk | diorite | emery | flint | gabbro
#                  (default: emery — the Pebble Time 2)

set -euo pipefail
PLATFORM="${1:-emery}"
SHOT_PATH="${2:-/tmp/van-face-${PLATFORM}.png}"

# Pebble-tool & toolchain assumptions: $CC etc. are unset (the Nix shell's
# host CC confuses waf into picking host gcc, which doesn't know -mthumb).
unset CC CXX AR AS LD RANLIB
unset CC_FOR_TARGET CXX_FOR_TARGET AR_FOR_TARGET AS_FOR_TARGET LD_FOR_TARGET RANLIB_FOR_TARGET

cleanup() {
  pebble kill >/dev/null 2>&1 || true
  pkill -9 -f qemu-pebble       2>/dev/null || true
  pkill -9 -f "venv/bin/pebble" 2>/dev/null || true
  pkill -9 -f pypkjs             2>/dev/null || true
}
trap cleanup EXIT

# Kill anything stale before we start; otherwise install hangs.
cleanup
sleep 1

# Reset SPI flash from the SDK template. Without this the emulator can be
# stuck in a half-corrupted state from a previous panic and will sit at
# "Waiting for the firmware to boot" forever.
SDK_VERSION=$(pebble sdk list | awk '/active/ {print $1; exit}')
SDK_BASE="$HOME/.pebble-sdk/SDKs/$SDK_VERSION/sdk-core/pebble/$PLATFORM/qemu"
STATE_DIR="$HOME/.pebble-sdk/$SDK_VERSION/$PLATFORM"
rm -rf "$STATE_DIR"
mkdir -p "$STATE_DIR"
bzip2 -dkc "$SDK_BASE/qemu_spi_flash.bin.bz2" > "$STATE_DIR/qemu_spi_flash.bin"

pebble install --emulator "$PLATFORM"
sleep 3
pebble screenshot --emulator "$PLATFORM" "$SHOT_PATH"
echo
echo "screenshot: $SHOT_PATH"
