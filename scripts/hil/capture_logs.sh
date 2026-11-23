#!/usr/bin/env bash
# Timestamped serial log capture from one port. UNVERIFIED.
source "$(dirname "$0")/common.sh"
PORT="${1:-}"; BAUD="${2:-115200}"; [ -n "$PORT" ] || die "usage: $0 <port> [baud]"
OUT="$LOG_DIR/serial-$(basename "$PORT")-$(ts).log"
log "Capturing $PORT @$BAUD -> $OUT (Ctrl-C to stop)"
python3 - "$PORT" "$BAUD" "$OUT" <<'PY'
import sys, time, serial
port, baud, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = serial.Serial(port, baud, timeout=0.2)
with open(out, "w") as f:
    try:
        while True:
            line = s.readline()
            if line:
                t = time.strftime("%H:%M:%S")
                txt = line.decode(errors="replace").rstrip()
                print(f"[{t}] {txt}"); f.write(f"[{t}] {txt}\n"); f.flush()
    except KeyboardInterrupt:
        pass
PY
