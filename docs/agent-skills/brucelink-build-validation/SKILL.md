---
name: brucelink-build-validation
description: Reproducibly validate that BruceLink builds and its host tests pass, comparing against the known-good baseline, before a commit/PR or after dependency or build-flag changes. Invoke to prove a change is green or to reproduce the baseline.
---

# BruceLink — Reproducible Build Validation

Paths are repo-root-relative. Full detail: `docs/handoff/BUILD_AND_DEPLOY.md`.

## When to invoke
- Before committing or opening a PR.
- After changing dependencies, build flags, or the installer.
- To reproduce the `448b798` known-good baseline from scratch.

## Required project context
- Pinned toolchains: `ufbt` 0.2.6 (Momentum `mntm-012`/API 87.1), PlatformIO platform pioarduino `55.03.39`, NimBLE-Arduino `2.5.1`, Python 3.13, a host C compiler.
- The repo at the commit under test. Network note: the cloud sandbox allowlist blocks the PlatformIO registry — NimBLE is fetched by its GitHub `2.5.1` tag; locally the registry works.

## References
- `docs/handoff/BUILD_AND_DEPLOY.md`, `docs/handoff/TESTING.md`, `docs/handoff/CURRENT_STATE.md`.

## Procedure
1. If core/bridge source files were added/removed: `python3 tools/gen_wrappers.py` (then confirm `tests/test_wrappers.py` is happy).
2. Host tests: `make -C tests/c test cppcheck`.
3. Python: `python3 -m unittest discover -s tests`.
4. Flipper FAP: `(cd flipper/brucelink && ufbt)`.
5. ESP32 bridge compile check: `(cd esp32/test_compile && pio run)`.
6. (Optional, full firmware) `python3 tools/install_into_bruce.py <bruce-2b6ba4d-checkout>` then `pio run -e m5stack-sticks3`.
7. Compare every result to the baseline below.

## Verification / acceptance (baseline at `448b798`)
- `make test`: **core 3681 + bridge 448 checks, 0 failed**; `cppcheck` clean.
- Python: **31 tests OK**.
- FAP: APPCHK pass, Target 7 / API 87.1 (v0.7).
- ESP32 compile: SUCCESS.
- Full Bruce build: SUCCESS, static internal RAM ≈ **91,260 B** (trimmed).
- Any deviation is investigated and explained, not ignored. No test weakened to pass.

## Common mistakes & pitfalls
- Treating compile success as proof of logic — it is not (see `TESTING.md`).
- Stale generated wrappers after a core change.
- Forgetting both devices must run the same build when the wire changes.
- Reporting a build "passes" without the numeric comparison.

## Expected output
A pass/fail table of each target vs the baseline, with any deltas explained.
