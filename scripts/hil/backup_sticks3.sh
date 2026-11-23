#!/usr/bin/env bash
# Full 8 MB flash backup of the StickS3 BEFORE any flashing. UNVERIFIED.
source "$(dirname "$0")/common.sh"
ES="${STICKS3_PORT:-$(find_port_by_vid "$ESP32S3_VID" || true)}"
[ -n "${ES:-}" ] || die "StickS3 not found (set STICKS3_PORT)"
OUT="$LOG_DIR/sticks3-full-$(ts).bin"
log "Reading 8 MB flash from $ES -> $OUT"
esptool.py --chip esp32s3 -p "$ES" -b 921600 read_flash 0x0 0x800000 "$OUT"
sha256sum "$OUT" | tee -a "$LOG_DIR/hil.log"
log "LittleFS-only backup (config/pcaps, 0x4F0000+3MB):"
esptool.py --chip esp32s3 -p "$ES" -b 921600 read_flash 0x4F0000 0x300000 "$LOG_DIR/sticks3-littlefs-$(ts).bin"
log "Backup complete. Keep $OUT safe before flashing."
