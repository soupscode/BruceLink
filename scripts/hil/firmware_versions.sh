#!/usr/bin/env bash
# Report firmware/chip versions for both devices. UNVERIFIED.
source "$(dirname "$0")/common.sh"
ES="${STICKS3_PORT:-$(find_port_by_vid "$ESP32S3_VID" || true)}"
if [ -n "${ES:-}" ]; then
  log "StickS3 chip/flash ($ES):"
  esptool.py --chip esp32s3 -p "$ES" flash_id 2>&1 | tee -a "$LOG_DIR/versions-$(ts).log"
else log "StickS3 not found"; fi
FL="${FLIPPER_PORT:-$(find_port_by_vid "$FLIPPER_VID" || true)}"
if [ -n "${FL:-}" ]; then
  log "Flipper device_info ($FL) via CLI:"
  # Flipper CLI over CDC: send 'device_info' and read reply.
  python3 - "$FL" <<'PY' 2>&1 | tee -a "$LOG_DIR/versions-$(ts).log"
import sys, time, serial
s = serial.Serial(sys.argv[1], 230400, timeout=1); time.sleep(0.5)
s.write(b"\r\ndevice_info\r\n"); time.sleep(1.0)
print(s.read(8192).decode(errors="replace"))
PY
else log "Flipper not found"; fi
