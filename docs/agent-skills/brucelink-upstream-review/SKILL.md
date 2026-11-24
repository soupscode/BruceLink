---
name: brucelink-upstream-review
description: Review BruceLink for compatibility against a new or alternate upstream Bruce (or a dependency bump) before adopting it — installer anchors, integration surfaces, build flags, menu stability, and RAM. Invoke when Bruce, Momentum, NimBLE, or the ESP32 platform version changes.
---

# BruceLink — Upstream Dependency & Compatibility Review

Paths are repo-root-relative. Pins are in `docs/handoff/CURRENT_STATE.md`.

## When to invoke
- Considering a new Bruce revision (≠ `2b6ba4d`) or bumping Momentum / NimBLE / the ESP32 platform.

## Required project context
- The pins (Momentum `mntm-012`/API 87.1, Bruce `1.17`/`2b6ba4d`, NimBLE `2.5.1`, pioarduino `55.03.39`).
- The installer patch targets and the "menu order/labels are not a stable API" caveat.

## References
- `docs/handoff/INTEGRATION_INTERFACES.md`, `NATIVE_BRUCE_INVENTORY.md`, `BUILD_AND_DEPLOY.md`, `tools/install_into_bruce.py`, `esp32/src/modules/brucelink/PATCH.md`.

## Procedure
1. **Installer anchors:** diff the new upstream against `2b6ba4d` for every file the installer patches — `src/core/main_menu.{h,cpp}`, `src/main.cpp` (the two boot hooks), the board ini, and the RAM-trim files (`massStorage.h`, `wifi_recover.cpp`, `netcut.cpp`, `rf_stream_memory.c`, `webInterface.cpp` + its header `webInterface.h`). Do the anchors still match?
2. **Integration surfaces:** confirm the `serialDevice` swap, `nav`/`options`/`optionsJSON`, the startup-app registry, and `bruceConfig` still exist as documented.
3. **Menu stability:** if anything relies on the Wi-Fi menu-walk, re-check the menu tree order/labels for this env — they are not a stable API.
4. **Build flags:** re-determine `LITE_VERSION`, `BRUCE_NO_SDR_JAMMER`, and board guards for `m5stack-sticks3`, and the per-capability build inclusion.
5. **Build + test:** `tools/install_into_bruce.py` refuses a non-`2b6ba4d` tree without `--force`; run it, then `pio run -e m5stack-sticks3`, then the host gate. Check RAM against the baseline.
6. **Re-pin + update docs** on success.

## Verification / acceptance
- Installer applies cleanly (or anchors updated and `tests/test_install_into_bruce.py` passes).
- Integration surfaces present; build links and fits; RAM within budget.
- Pins updated in `CURRENT_STATE.md`; both devices rebuilt together.

## Common mistakes & pitfalls
- Assuming the menu order is stable across Bruce versions.
- A patch anchor moved silently → the installer writes nothing (by design) or mis-patches.
- A feature now gated differently (LITE/board) changing capability availability.
- A new floating dependency; forgetting to re-pin; updating one device only.

## Expected output
A compatibility report (what still applies, what broke, required patch/doc updates) and a go/no-go recommendation.
