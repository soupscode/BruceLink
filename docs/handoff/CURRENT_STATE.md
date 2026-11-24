# Current State — snapshot at `448b798`

> Exact project snapshot as of 2026-11-24. Pairs with `TESTING.md` (evidence),
> `KNOWN_ISSUES.md` (what's broken/incomplete), `NEXT_STEPS.md` (what's next).

## Git

- **Remote:** `origin` = github.com/soupscode/brucelink
- **Active branch:** `feat/radio-handoff`
- **HEAD:** `448b798` — "Fix the transfer review: requests with a file name could not be sent"
- **HEAD == the known-good baseline.** `448b798` is the baseline; there are **zero
  commits after it** on this branch. Working tree is **clean** and in sync with
  `origin/feat/radio-handoff`.
- **Open PR:** #8 (`feat/radio-handoff` → `main`), "Jobs, review fixes, RAM trims → main". **Not merged** (do not merge unverified changes to `main`).
- **No git submodules** in this repo. Dependencies are pinned via `ufbt` (Flipper) and PlatformIO platform/`lib_deps` (ESP32).

### Recent history (newest first)
```
448b798  Fix the transfer review: requests with a file name could not be sent
a1a6c04  Capture files in the Python reference; capture job never joins a network
2b2c669  Handshake capture as a job; list, download and delete capture files
cb11f31  StickS3: drop USB HID/mass storage and mDNS, move tables to PSRAM
3fac5b5  StickS3: free internal RAM without losing features
222920f  Fix the independent review's findings (core, StickS3, Flipper, docs)
f4febb1  Docs: faster jobs confirmed on hardware (owner-reported)
8f9285a  Jobs: skip Bruce's boot animation, shorter scan, timing in the result
```

### Other branches
- `test/capture` = `448b798` + `0b523ff` (adds prebuilt binaries under `builds/`; for pulling the FAP/firmware via `git pull`, **not** for `main`).
- Historical feature branches: `dev/foundation-v1`, `feat/bruce-bridge-module`, `feat/flipper-ble-profile`, `feat/pairing`, `fix/connect-stack-overflow`, `fix/wifi-scan-hang` (all pushed to `origin`).

## Firmware versions (the pins)

| Component | Version |
|---|---|
| Flipper | Momentum `mntm-012` (`e1784e7`), API `87.1` |
| StickS3 | Bruce `1.17` (`2b6ba4d`), env `m5stack-sticks3` + BruceLink installer patch |
| ESP32 BLE lib | NimBLE-Arduino `2.5.1` |
| ESP32 platform | pioarduino `55.03.39` (arduino-esp32 `3.3.9` / ESP-IDF `5.5.4`) |
| ufbt | `0.2.6` |

## Build status (cloud session, 2026-11-24 — no hardware)

| Target | Result |
|---|---|
| Host C core + bridge (`make -C tests/c test`, ASan+UBSan) | **core 3681 + bridge 448 checks, 0 failed** |
| `make -C tests/c cppcheck` | clean |
| Python (`python3 -m unittest discover -s tests`) | **31 tests OK** |
| Flipper FAP (`ufbt`, mntm-012) | v0.7, APPCHK pass, Target 7 / API 87.1 |
| ESP32 compile check (`esp32/test_compile`) | SUCCESS |
| Bruce 1.17 + BruceLink (`m5stack-sticks3`) | SUCCESS; static internal RAM **91,260 B** (trimmed) |

## Completed (shipped in `448b798`)

- Shared protocol core (C99): crypto, frame+MAC, authenticated session (PSK + X25519, replay, version negotiation, request tags), pairing (ADR 0005), jobs (ADR 0006), capture-file transfer (`bl_files`), fragmentation, COBS, capability registry, safety gate.
- Flipper FAP (BLE-only): Pair, Connect, Device Info, Wi-Fi Scan, Capabilities, Capture, Files (list/download/delete), Forget, Settings, About.
- ESP32 bridge core + NimBLE central transport; Bruce glue + installer (`tools/install_into_bruce.py`): main-menu item, two boot hooks, RAM trims.
- Jobs: Wi-Fi scan (`wifi.scan`) and passive handshake capture (`wifi.sniff.handshake`) run between BLE-off restarts.
- RAM trims: −42,536 B static internal RAM, −4,092 B IRAM (default on).

## Outstanding (not done)

- **ADR 0007** (integration-layer architecture / generic supervised native-function job) — agreed, not yet written.
- **Generic supervised native-function job** — agreed direction; current code uses per-tool jobs.
- **Make RAM trims installer-configurable** (default on) — agreed, not implemented.
- **Native Sniffer** as a separate capability vs the passive capture job — to evaluate; no removal of working code first.
- **Link encryption (M4.6)** — frames authenticated, not encrypted. Not started.
- **Active-RF jobs** (deauth/portal/karma/etc.) — refused until ADR 0006 active-job rules exist and are tested.
- This **engineering handoff** (`docs/handoff/`) and the **agent skills** (`docs/agent-skills/`) — in progress.

## Verification status (summary — detail in `TESTING.md`)

- **Hardware-verified (owner-reported):** Flipper app over its mock; BLE profile vs a phone scanner; first OTA link; Device Info; Capabilities; pairing + paired key after reboot; the Wi-Fi scan job (then faster); and the RAM-trim build (owner: "all working well").
- **Software-verified (host/compile only):** everything in the Build status table; `bl_files` transfer path; the review fixes of 2026-11-23; the job/safety/pairing logic.
- **Not verified:** the review-fix build on *both* devices together (wire changed — update both); the passive handshake-capture job on hardware (host-tested and built; an informal owner remark that capture "is running" was never recorded in `docs/TEST_RESULTS.md` and the capture build has no hardware report — treated as pending, not verified); capture-file download/delete end to end; active ops (refused); link under load; coexistence with Bruce's own tools while BruceLink is on.
- **Not independently reviewed:** the handshake-capture job (a usage safeguard stopped that review).

## Delivered artifacts (cloud scratchpad, not committed except on `test/capture`)
`out/448b798/Bruce-1.17-BruceLink-448b798-m5stack-sticks3-FULL.bin`,
`out/448b798/brucelink-v0.7-448b798.fap`, with SHA256SUMS. Flash the single **merged** image
file at `0x0` (which lays bootloader@`0x0` / partition-table@`0x8000` /
app@`0x10000`); the three-part separate flash and the app-only `firmware.bin`
both failed to boot on hardware. Flashing erases NVS → re-pair.
