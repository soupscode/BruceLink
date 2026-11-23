#!/usr/bin/env bash
# Run every cloud-verifiable build + test. Safe to run anywhere.
source "$(dirname "$0")/common.sh"
log "== host C core (ASan+UBSan) =="; make -C "$REPO_ROOT/tests/c" clean test
log "== cppcheck ==";                  make -C "$REPO_ROOT/tests/c" cppcheck
log "== python ref + interop ==";      (cd "$REPO_ROOT" && python3 -m unittest discover -s tests -v)
log "== Flipper FAP ==";               (cd "$REPO_ROOT/flipper/brucelink" && ufbt)
log "== ESP32 bridge compile ==";      (cd "$REPO_ROOT/esp32/test_compile" && pio run)
log "All builds/tests completed."
