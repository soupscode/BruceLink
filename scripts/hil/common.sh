#!/usr/bin/env bash
# Shared helpers for BruceLink hardware-in-the-loop scripts.
# STATUS: UNVERIFIED — written in the cloud session; first run on real hardware
# must confirm port detection, VID/PIDs and esptool behaviour.
set -euo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
LOG_DIR="${LOG_DIR:-$REPO_ROOT/hil-logs}"
mkdir -p "$LOG_DIR"
ts() { date -u +%Y%m%dT%H%M%SZ; }
log() { echo "[$(date -u +%H:%M:%S)] $*" | tee -a "$LOG_DIR/hil.log"; }
die() { log "ERROR: $*"; exit 1; }
# USB IDs (verify on first run):
#   Flipper Zero (CDC)       VID 0483 PID 5740
#   ESP32-S3 native USB-CDC  VID 303a (PID 1001 typical)
FLIPPER_VID=0483; ESP32S3_VID=303a
find_port_by_vid() {  # $1 = VID (lowercase hex)
  for dev in /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem*; do
    [ -e "$dev" ] || continue
    if command -v udevadm >/dev/null 2>&1; then
      v=$(udevadm info -q property -n "$dev" 2>/dev/null | sed -n 's/^ID_VENDOR_ID=//p')
      [ "${v,,}" = "$1" ] && { echo "$dev"; return 0; }
    fi
  done
  return 1
}
