#!/usr/bin/env bash
# Detect connected Flipper and StickS3 serial ports. UNVERIFIED.
source "$(dirname "$0")/common.sh"
log "Serial devices:"; ls -l /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | tee -a "$LOG_DIR/hil.log" || true
FL=$(find_port_by_vid "$FLIPPER_VID" || true)
ES=$(find_port_by_vid "$ESP32S3_VID" || true)
log "Flipper port : ${FL:-<not found>}"
log "StickS3 port : ${ES:-<not found>}"
[ -n "${FL:-}" ] && echo "FLIPPER_PORT=$FL"
[ -n "${ES:-}" ] && echo "STICKS3_PORT=$ES"
