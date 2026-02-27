#!/usr/bin/env bash
# ─── Overhead Tracker — build / upload helper ─────────────────────────────────
#
# Usage (run from project root):
#   ./build.sh               → compile + auto-detect port + upload via USB
#   ./build.sh compile       → compile only (error-check)
#   ./build.sh upload        → upload last build via USB (auto-detect port)
#   ./build.sh upload COM5   → upload last build to a specific port
#   ./build.sh monitor       → open serial monitor on auto-detected port
#   ./build.sh monitor COM5  → open serial monitor on specific port
#   ./build.sh ota           → compile + OTA upload (overhead-tracker.local)
#   OVERHEAD_TRACKER_IP=x.x.x.x ./build.sh ota  → OTA to specific IP
#
# ──────────────────────────────────────────────────────────────────────────────

set -e

ARDUINO_CLI="/c/Program Files/Arduino IDE/resources/app/lib/backend/resources/arduino-cli.exe"
CONFIG_FILE="C:/Users/maxim/.arduinoIDE/arduino-cli.yaml"
SKETCH="tracker_live_fnk0103s/tracker_live_fnk0103s.ino"
FQBN="esp32:esp32:esp32"
BUILD_DIR="/tmp/overhead-tracker-build"
BAUD=115200
ESPOTA="/c/Users/maxim/AppData/Local/Arduino15/packages/esp32/hardware/esp32/3.3.7/tools/espota.exe"
OTA_HOST="${OVERHEAD_TRACKER_IP:-overhead-tracker.local}"
OTA_PORT=3232
BIN_FILE="$BUILD_DIR/tracker_live_fnk0103s.ino.bin"

CMD="${1:-all}"

die() { echo -e "\n[ERROR] $*" >&2; exit 1; }
info() { echo -e "\n>>> $*"; }

# ── Compile ──────────────────────────────────────────────────────────────────
run_compile() {
  info "COMPILE  ($FQBN)"
  "$ARDUINO_CLI" compile \
    --config-file "$CONFIG_FILE" \
    --fqbn        "$FQBN" \
    --build-path  "$BUILD_DIR" \
    "$SKETCH"
  info "Compile OK — binary in $BUILD_DIR"
}

# ── Port detection ────────────────────────────────────────────────────────────
detect_port() {
  powershell -Command "
    Get-WMIObject Win32_SerialPort |
    Where-Object { \$_.Caption -match 'CH340|CP210|UART|USB Serial|USB-SERIAL' } |
    Select-Object -First 1 -ExpandProperty DeviceID
  " 2>/dev/null | tr -d '\r\n'
}

resolve_port() {
  local port="${1:-}"
  if [ -z "$port" ]; then
    info "Detecting port..."
    port=$(detect_port)
    [ -n "$port" ] || die "No ESP32 serial port found. Is it plugged in?"
    echo "    Found: $port"
  fi
  echo "$port"
}

# ── Upload ───────────────────────────────────────────────────────────────────
run_upload() {
  local port
  port=$(resolve_port "${2:-}")
  info "UPLOAD → $port"
  "$ARDUINO_CLI" upload \
    --config-file "$CONFIG_FILE" \
    --fqbn        "$FQBN" \
    --port        "$port" \
    --input-dir   "$BUILD_DIR" \
    "$SKETCH"
  info "Upload complete."
}

# ── OTA Upload ────────────────────────────────────────────────────────────────
run_ota() {
  run_compile
  info "OTA UPLOAD → $OTA_HOST:$OTA_PORT"
  "$ESPOTA" -i "$OTA_HOST" -p "$OTA_PORT" -f "$BIN_FILE" -r
  info "OTA upload complete."
}

# ── Serial monitor ────────────────────────────────────────────────────────────
run_monitor() {
  local port
  port=$(resolve_port "${2:-}")
  info "MONITOR → $port @ ${BAUD} baud  (Ctrl-C to exit)"
  "$ARDUINO_CLI" monitor \
    --config-file "$CONFIG_FILE" \
    --port        "$port" \
    --config      "baudrate=$BAUD"
}

# ── Dispatch ──────────────────────────────────────────────────────────────────
case "$CMD" in
  compile)          run_compile ;;
  upload)           run_upload  "$@" ;;
  ota)              run_ota ;;
  monitor)          run_monitor "$@" ;;
  all|*)            run_compile && run_upload "$@" ;;
esac
