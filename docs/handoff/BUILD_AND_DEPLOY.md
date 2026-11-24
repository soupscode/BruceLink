# Build & deploy

Reproducible setup, build, test, artifact and flash procedure for BruceLink.
Terse by design. Commands are repo-relative from the repo root unless noted.

Authoritative sources: `CLAUDE.md` ("Commands"), `LOCAL_HANDOFF.md` §5,
`docs/TEST_RESULTS.md`, `docs/HARDWARE_TESTING.md`, `scripts/hil/`,
`tools/install_into_bruce.py`, `esp32/test_compile/platformio.ini`,
`flipper/brucelink/application.fam`, `esp32/src/modules/brucelink/PATCH.md`.

Scope label: every build/test figure below is **cloud SOFTWARE-VERIFIED**
(compile/link/host test only, ran 2026-11-24 in the cloud session at commit
`448b798`). None is hardware. Hardware flashing/boot results are owner-reported
in `docs/TEST_RESULTS.md` → "Hardware results". See `TESTING.md` for the status
breakdown.

---

## 0. Pinned toolchains and versions

| Component | Pin | Notes |
|---|---|---|
| Flipper firmware | Momentum `mntm-012` (`e1784e7`), API `87.1`, Target 7 | FAP is built against this SDK |
| `ufbt` | 0.2.6 | Flipper build tool |
| StickS3 firmware | Bruce `1.17` (`2b6ba4d`), env `m5stack-sticks3` + BruceLink installer patch | upstream Bruce tree uses its own git submodules |
| NimBLE-Arduino | `2.5.1` | BLE stack (central on StickS3) |
| ESP32 platform | pioarduino `platform-espressif32` `55.03.39` | = arduino-esp32 `3.3.9` / ESP-IDF `5.5.4` |
| Xtensa compiler | `xtensa-esp-elf` 14.2.0 | via the platform |
| ARM compiler | ufbt's real `arm-none-eabi-gcc` locally; **cloud used xPack `arm-none-eabi-gcc` 12.3.1** | substitute only — see §3 |
| Host C/C++ | gcc 13.3.0 (per `docs/TEST_RESULTS.md`) + `cppcheck` | for `tests/c` |
| Python | 3.13 | reference impl + interop + installer tests |

`LITE_VERSION` is **not** defined for `m5stack-sticks3` (it lives in
`[env_light]`, not inherited): all non-LITE Bruce features compile in.

> **Cloud network caveat.** The cloud session runs behind an allowlist that
> blocks arbitrary downloads: `update.flipperzero.one` and the PlatformIO
> registry API are unreachable. §3/§6 give the substitutes used. A **local**
> build with the real toolchains and registry is the reference; redo the
> substituted steps locally.

---

## 1. One-shot: rebuild + retest everything

```
scripts/hil/build_all.sh
```

Runs, in order (see `scripts/hil/build_all.sh`): host C core+bridge
(`make -C tests/c clean test`), `cppcheck`, Python ref+interop
(`python3 -m unittest discover -s tests -v`), FAP (`ufbt`), ESP32 bridge
compile (`pio run` in `esp32/test_compile`). Safe to run anywhere; the
Bruce-full firmware build (§6) is **not** part of it (needs a Bruce checkout).

---

## 2. Host tests (no hardware, no cross-toolchain)

```
make -C tests/c test cppcheck                # core (3681) + bridge (448) checks, ASan+UBSan; cppcheck clean
python3 -m unittest discover -s tests -v     # Python reference + C interop (31 tests)
```

`tests/c/` builds two binaries under AddressSanitizer+UBSan with strict
warnings (`-Wall -Wextra -Werror -Wconversion …`, see `tests/c/Makefile`):
`bl_tests` (shared core + Flipper `bl_gatt_evt` parser) and `bl_bridge_tests`
(portable bridge core driven by a real client session). `cppcheck` runs over
the core C and the portable bridge C++.

After **adding/removing/renaming** a file in `shared/brucelink_core/src` or
`esp32/lib/BruceLink`, regenerate the per-build wrapper sources:

```
python3 tools/gen_wrappers.py                # (re)generate
python3 tools/gen_wrappers.py --check        # exit 1 if stale (run in CI by tests/test_wrappers.py)
```

Wrappers are one-line `#include`s of the shared source, one per device build
tree (`flipper/brucelink/core/`, `esp32/test_compile/src/{core,bridge}/`); no
symlinks (they break Windows checkouts). `tests/test_wrappers.py` fails if they
are stale, so a new core file cannot be host-tested yet silently missing from a
device build.

---

## 3. Flipper FAP

```
(cd flipper/brucelink && ufbt)               # build
(cd flipper/brucelink && ufbt launch)        # build + install + run over USB
```

- App metadata: `flipper/brucelink/application.fam` (appid `brucelink`,
  `fap_version` **0.7**, category Bluetooth, `stack_size=8*1024`).
- Output: `flipper/brucelink/dist/brucelink.fap` (also
  `~/.ufbt/build/brucelink.fap`). Current size at `448b798`: ~60 kB.
- Expect `APPCHK` pass, Target 7 / API 87.1.
- **Locally:** run `ufbt update` normally to fetch the real mntm-012 SDK and
  ARM toolchain.
- **Cloud substitution (do not reuse locally):** `update.flipperzero.one` was
  blocked, so the build used xPack `arm-none-eabi-gcc` 12.3.1 via `FBT_NOENV=1`
  (same GCC major/minor as Flipper toolchain 39). It emits harmless
  `DWARF error: line info` linker warnings against Flipper's prebuilt libs.
- If the Flipper runs a Momentum other than `mntm-012`, rebuild the FAP against
  that SDK (the API must match).

---

## 4. ESP32 bridge compile check (standalone, stub backend)

```
(cd esp32/test_compile && pio run)           # env s3-check, board esp32-s3-devkitc-1
```

Compiles + links the bridge core + NimBLE central transport + shared core with
a **stub** backend (not Bruce, not flashed, not run). Config in
`esp32/test_compile/platformio.ini`: pulls the pioarduino 55.03.39 platform
zip and NimBLE 2.5.1; forces `-include bl_sticks3_ram.h` + `-DBOARD_HAS_PSRAM`
(the PSRAM override, as the StickS3 build does). Expect `[SUCCESS]`.

> **Cloud substitution:** the registry was blocked, so NimBLE is pulled by its
> GitHub tag `https://github.com/h2zero/NimBLE-Arduino.git#2.5.1` (resolves to
> the same 2.5.1). The canonical registry pin is `h2zero/NimBLE-Arduino@2.5.1`.
> `tool-scons` came from PyPI `scons` 4.11.1. Locally the registry works.

---

## 5. Install BruceLink into a Bruce checkout

```
python3 tools/install_into_bruce.py <bruce-checkout>              # install
python3 tools/install_into_bruce.py <bruce-checkout> --uninstall  # reverse
python3 tools/install_into_bruce.py <bruce-checkout> --force      # skip the revision guard
```

The installer (`tools/install_into_bruce.py`; tested by
`tests/test_install_into_bruce.py`) is idempotent and **refuses a tree that is
not Bruce `2b6ba4d`** unless `--force`. It writes **nothing** until every check
(file lists, then each Bruce anchor) passes, so a failed install leaves the
tree untouched. What it does:

1. copies the bridge library to `<bruce>/lib/BruceLink/` (from
   `esp32/lib/BruceLink/*` + `shared/brucelink_core/src/*`) and writes a
   generated `library.json` there.
2. copies the Bruce glue to `<bruce>/src/modules/brucelink/` (from
   `esp32/src/modules/brucelink/`).
3. adds **BruceLink** to Bruce's main menu, first, before Wi-Fi
   (`src/core/main_menu.h` include + member; `src/core/main_menu.cpp` entry) —
   each inside a `#if !defined(LITE_VERSION)` guard.
4. calls `bruceLinkBootEarly()` (after the input task starts) and
   `bruceLinkBoot()` (end of `setup()`) in `src/main.cpp` — the two ADR 0006
   boot hooks that run a pending job and bring the link back after it.
5. applies the StickS3 **RAM trims** (owner's choice; `BRUCELINK_RAM_TRIM`):
   three build flags in `boards/m5stack-sticks3/m5stack-sticks3.ini`
   (`-DBRUCELINK_RAM_TRIM=1`, `-I lib/BruceLink`, `-include bl_sticks3_ram.h`)
   plus `ESPmDNS` in that env's `lib_ignore`, and guarded lines in five Bruce source
   files (plus `webInterface.h`'s include-guard edit): no USB mass storage (`src/core/massStorage.h`), the Wi-Fi cracker's
   hot loop out of IRAM (`src/modules/wifi/wifi_recover.cpp`), NetCut's table
   and the RF decoder's table into PSRAM (`src/modules/wifi/netcut.cpp`,
   `src/modules/rf/protocols/rf_stream_memory.c`), no mDNS
   (`src/core/wifi/webInterface.{h,cpp}`). Every added line ends with
   `// BruceLink` / `; BruceLink`; `--uninstall` strips exactly those.

Details and the full patch list: `esp32/src/modules/brucelink/PATCH.md`.
Other boards built from the same tree are unchanged.

---

## 6. Full Bruce 1.17 + BruceLink firmware (`m5stack-sticks3`)

```
git clone <bruce-repo> bruce && cd bruce
git checkout 2b6ba4d                         # Bruce 1.17
git submodule update --init                  # Bruce's OWN submodules (this repo has none)
cd <brucelink-repo>
python3 tools/install_into_bruce.py <path-to>/bruce
cd <path-to>/bruce && pio run -e m5stack-sticks3
```

- Merged image output: `Bruce-m5stack-sticks3.bin` (from `[merge_bin]`).
  Delivered/renamed as `Bruce-1.17-BruceLink-<commit>-m5stack-sticks3-FULL.bin`.
- Expect `[SUCCESS]`. At the last recorded full build the app fits Bruce's
  `0x4E0000` factory partition (~93 %).
- **Static internal RAM 91,260 B** with the RAM trims on (−42,536 B vs
  untrimmed 133,796 B; +4,092 B IRAM back to the heap; −122 kB flash). See
  `MEMORY_OPTIMIZATION.md` / `docs/TEST_RESULTS.md` RAM entries.

> **Cloud caveat (reproducibility, not result):** the registry is unreachable
> in the cloud, so every registry dependency was fetched from GitHub and given
> registry-style metadata locally; floating Bruce deps were taken at their
> default-branch head on the build date (full list in `docs/TEST_RESULTS.md`
> → "full firmware build"). Bruce's own sources were unchanged apart from the
> installer. **A local build with the real registry is the reference**; the
> cloud build shows the integration compiles, links and fits.

---

## 7. Deploy / flash

### 7.0 Back up first (mandatory)

```
scripts/hil/backup_sticks3.sh                # full 8 MB flash + LittleFS-only; sha256 logged
```

Flashing the merged image **erases NVS** (and with it the pairing). Keep the
backup; `scripts/hil/restore_sticks3.sh <sticks3-full-*.bin>` restores it
(type `YES` to confirm).

### 7.1 Detect ports and record versions

```
scripts/hil/detect_devices.sh                # Flipper VID 0483, StickS3 VID 303a
scripts/hil/firmware_versions.sh             # esptool flash_id + Flipper device_info
```

Override auto-detection with `STICKS3_PORT` / `FLIPPER_PORT`.

### 7.2 Flash the StickS3

Flash the **merged image as a single file at offset `0x0`** (it contains the
bootloader at `0x0`, the partition table at `0x8000` and the app at
`0x10000`). This needs nothing from what was on the device.

**Do NOT** flash either of the following — both were tried on hardware
(2026-11-23) and **neither booted** (cause unknown; see `docs/TEST_RESULTS.md`):
- the app-only `firmware.bin` alone;
- the three parts separately (bootloader / partitions / app at
  `0x0` / `0x8000` / `0x10000`).

Keep the merged image at hand to recover. Re-pair after every StickS3 flash
(NVS erased). Procedure and offsets: `docs/HARDWARE_TESTING.md` §4b.

### 7.3 Install the Flipper FAP

```
(cd flipper/brucelink && ufbt launch)        # installs + runs the app over USB
```

### 7.4 Both devices on the same build

A session wire change (transcript + header byte 5) means a Flipper and a
StickS3 on different sides of it **do not complete the handshake**. Update both
together. After flashing, follow `docs/HARDWARE_TESTING.md` steps 3–9.

### 7.5 Getting prebuilt artifacts (no local build)

The cloud session's artifacts live only in its scratchpad (`out/<commit>/`,
e.g. `brucelink-v0.7-448b798.fap`,
`Bruce-1.17-BruceLink-448b798-m5stack-sticks3-FULL.bin`) and are **not**
committed to `main`. To pull prebuilt binaries by git, use the `test/capture`
branch (= `448b798` + one commit adding `builds/`); it is for fetching the
FAP/firmware only, never to merge to `main`.

---

## See also

- `TESTING.md` — suites, current results, host-testable vs hardware, status table.
- `docs/HARDWARE_TESTING.md` — on-device procedures (steps 0–10).
- `docs/TEST_RESULTS.md` — the actual run evidence and hardware reports.
- `LOCAL_HANDOFF.md` — local-session first-read, §5 commands.
- `esp32/src/modules/brucelink/PATCH.md` — exactly what the installer changes.
- `MEMORY_OPTIMIZATION.md` — the RAM trims and measured costs.
- `docs/decisions/` — ADRs 0001–0006 (and ADR 0007, when written, for the
  generic-job model).
