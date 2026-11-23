#!/usr/bin/env bash
# Restore a full-flash backup. DESTRUCTIVE: overwrites the StickS3. UNVERIFIED.
source "$(dirname "$0")/common.sh"
IMG="${1:-}"; [ -f "$IMG" ] || die "usage: $0 <sticks3-full-*.bin>"
ES="${STICKS3_PORT:-$(find_port_by_vid "$ESP32S3_VID" || true)}"
[ -n "${ES:-}" ] || die "StickS3 not found (set STICKS3_PORT)"
read -r -p "Overwrite ALL flash on $ES with $IMG? type YES: " ok
[ "$ok" = "YES" ] || die "aborted"
esptool.py --chip esp32s3 -p "$ES" -b 921600 write_flash 0x0 "$IMG"
log "Restore complete."
