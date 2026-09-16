#!/usr/bin/env bash
# Compile, flash, and watch the board come up.
#
#   tools/flash-and-watch.sh [PORT] [SECONDS]
#
# Bluetooth Classic plus WiFi does not fit the default partition table, hence
# min_spiffs. DebugLevel=info is what makes the connect/stall/drop lines show up
# on the monitor - at the default level you only see errors.
set -euo pipefail

PORT="${1:-/dev/ttyUSB0}"
SECONDS_TO_WATCH="${2:-30}"
SKETCH="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)/RTKBridge"
FQBN="esp32:esp32:esp32:PartitionScheme=min_spiffs,CPUFreq=240,DebugLevel=info,UploadSpeed=921600"

if ! command -v arduino-cli >/dev/null; then
  echo "arduino-cli not found - install it, then:" >&2
  echo "  arduino-cli core install esp32:esp32" >&2
  exit 1
fi

echo "==> compiling"
arduino-cli compile --fqbn "$FQBN" --warnings all "$SKETCH"

echo "==> flashing to $PORT"
arduino-cli upload --fqbn "$FQBN" -p "$PORT" "$SKETCH"

echo "==> watching for ${SECONDS_TO_WATCH}s (expect a status line every 5s)"
timeout "$SECONDS_TO_WATCH" arduino-cli monitor -p "$PORT" -c baudrate=115200 || true
