# Memory optimization

Why the StickS3 build needs internal RAM trimmed, every significant
optimization, its saving, and its trade-off. The StickS3 is a full-feature Bruce
build (`LITE_VERSION` is **not** defined for `m5stack-sticks3`), so all of
Bruce's wireless tooling compiles in, and the BLE link has to fit alongside it in
~133 kB of internal RAM. PSRAM does not help the radio stacks (framework
allocates them internally, Bruce ships the framework prebuilt), so the only
levers are: move what *can* go to PSRAM, and drop what the StickS3 does not need.

> **Companions.** Board/radio/RAM budget and the measured link costs:
> `HARDWARE.md`. The installer changes in prose: `esp32/src/modules/brucelink/PATCH.md`.
> Decisions: `docs/decisions/0004-radio-memory-on-the-sticks3.md` (the reduced
> Wi-Fi start, superseded) and `docs/decisions/0006-one-radio-at-a-time.md`
> (jobs). Full evidence with the raw numbers: `docs/TEST_RESULTS.md`
> (sections "RAM review", "RAM trims without feature loss", "RAM trims in
> Bruce", "RAM-trim build"). The not-yet-done configurability work:
> `NEXT_STEPS.md`.

## Evidence legend

- **[MAP]** read from the PlatformIO build map / computed from the build. **Not
  measured on the stick** unless also tagged [HW].
- **[HW]** measured on the owner's device (owner-reported serial logs).
- **[SW]** software-verified (host test / compile-check object code).

## Headline result

The trim set, as applied by the installer for `m5stack-sticks3`, versus the same
build without it (baseline `3fac5b5`) [MAP, `docs/TEST_RESULTS.md` "RAM trims in
Bruce"]:

| Metric | Untrimmed (`3fac5b5`) | Trimmed | Change |
|---|---|---|---|
| Static internal RAM (PlatformIO "RAM") | 133,796 B | **91,260 B** | **−42,536 B** |
| IRAM segments in the image | 111,012 B | 106,920 B | −4,092 B (freed IRAM goes to the heap) |
| Flash | 4,778,827 B | 4,656,491 B | **−122 kB** |

Static internal RAM drops 42,536 B and another 4,092 B of IRAM is freed to the
heap, so the build frees **about 46 kB** of internal RAM at boot. These are
**build-map figures, not measured on the stick** (the owner ran the trim build
`cb11f31` and reported "all working well" but gave no memory figures — [HW] only
as a smoke test). The 91,260 B static figure was re-confirmed at the current
baseline `448b798` (`CURRENT_STATE.md`, cloud build). Expected on the stick:
~46 kB more internal heap at boot than `3fac5b5`, on top of NimBLE's host buffers
in PSRAM — to be verified by `HARDWARE_TESTING.md` step 4a.

Two groups: **(a)** BruceLink-side moves with **no feature loss**, and **(b)**
Bruce-side trims that **cost features** and need guarded edits to Bruce source.

## (a) BruceLink-side — no feature loss

These move allocations off internal RAM or reduce transient use. No Bruce feature
is lost.

### a1. NimBLE host buffers → PSRAM
- **What.** NimBLE-Arduino compiles its host from source but takes its settings
  from the framework's prebuilt `sdkconfig.h`, which puts the msys pools and the
  ACL/HCI event buffers (~16 kB, allocated at BLE start) in internal RAM. The
  forced-include header `esp32/lib/BruceLink/bl_sticks3_ram.h` `#undef`s the
  three `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_*` macros and defines
  `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL` (guarded by
  `BOARD_HAS_PSRAM && CONFIG_SPIRAM`), before anything else includes
  `sdkconfig.h`.
- **Why a forced include, not `-D`.** `sdkconfig.h` is `#pragma once`; redefining
  the mode in a header that is included first wins. A `-D` flag cannot: NimBLE
  re-reads `sdkconfig.h`, which would set internal RAM again. GCC applies all
  `-D` before forced includes, which also lets this header undo the board's
  `-DUSB_as_HID` (see b1). The header is wired in by three build flags the
  installer adds to the StickS3 env: `-DBRUCELINK_RAM_TRIM=1`, `-I lib/BruceLink`,
  `-include bl_sticks3_ram.h`.
- **Saving.** ~16 kB more internal free after BLE starts [MAP,
  `docs/TEST_RESULTS.md` "RAM trims without feature loss"].
- **Verified.** [SW] Compile-check object code: `nimble_platform_mem_malloc` /
  `_calloc` ask for caps `0x404` (PSRAM, 8-bit), not `0x804` (internal).
- **Trade-off.** None functionally; the buffers are task-only (no DMA, no ISR),
  which IDF supports with PSRAM in the heap. Applies to Bruce's own BLE tools
  too, not just BruceLink.
- **Naming GAP.** The current header is `bl_sticks3_ram.h`
  (`install_into_bruce.py`, `PATCH.md`). `docs/TEST_RESULTS.md` "RAM trims
  without feature loss" refers to an earlier name `bl_nimble_psram.h` for the
  same forced include; treat them as the same mechanism under the current name.

### a2. Bridge task stack → PSRAM
- **What.** The FreeRTOS bridge task's stack is placed in PSRAM, and its size was
  cut from 12 kB to **10 kB** on the strength of a 4.3 kB measured peak use
  (`HARDWARE_TESTING.md` step 4b.9). Source: `BruceLinkModule` (the task owner,
  `esp32/src/modules/brucelink/`).
- **Saving.** ~3 kB more internal free while BruceLink is on [MAP,
  `docs/TEST_RESULTS.md` "RAM trims without feature loss"]. (The RAM review
  listed the stack as "10 kB while on" as a planning estimate; the implemented
  build's computed figure is ~3 kB net.)
- **Trade-off.** None observed: the task has shown ≥7.9 kB of its stack unused on
  hardware [HW, `stack-left` serial lines].

### a3. Passive BLE scan + chunk queue → PSRAM
- **What.** The scan that looks for the Flipper no longer stores every advertiser
  it sees (NimBLE keeps them all, unbounded, by default), and the RX/TX chunk
  queue lives in PSRAM (ADR 0004 decision 4).
- **Saving.** **~14 kB — measured.** [HW] With the Flipper linked, internal free
  rose 47,979 B → 61,675 B (a 13.7 kB recovery) between `72d9efc` (before) and
  `2b604e4` (BLE memory savings). This is the one group-(a) saving confirmed on
  hardware.
- **Trade-off.** None; fewer transient allocations while scanning.

## (b) Bruce-side — costs features, guarded edits

The owner chose to drop features the RAM review listed, to free internal RAM for
the link (`docs/TEST_RESULTS.md` "RAM trims in Bruce", owner's choice,
2026-11-23). Every change is inserted by `tools/install_into_bruce.py` as lines
**guarded by `#if defined(BRUCELINK_RAM_TRIM)`** (`BRUCELINK_RAM_TRIM` is defined
only in the StickS3 env). No existing Bruce line is modified, other boards built
from the same tree are unaffected, and `--uninstall` removes it all. These are
the **five patched Bruce source files** (plus `webInterface.h`'s include guard
and the forced-include header, which carries the USB-HID undo):

| # | Change | File(s) | Saving | Trade-off (feature lost) |
|---|---|---|---|---|
| b1 | No USB HID **and** no USB mass storage → TinyUSB is not linked at all | `bl_sticks3_ram.h` (`#undef USB_as_HID`) + `src/core/massStorage.h` (`#undef SOC_USB_OTG_SUPPORTED`) | **~31 kB static** [MAP] | BadUSB / USB keyboard / mouse, USB clicker, U2F, USB mass storage. **BLE keyboard and the USB serial console stay.** |
| b2 | No mDNS | `src/core/wifi/webInterface.cpp` + `src/core/wifi/webInterface.h`, **plus `lib_ignore = ESPmDNS`** in the env | ~2.4 kB static + heap [MAP] | WebUI reached by IP, not `bruce.local` |
| b3 | Wi-Fi password cracker's SHA-1/PBKDF2 hot loop out of IRAM, into flash | `src/modules/wifi/wifi_recover.cpp` (`#undef IRAM_ATTR` / redefine empty) | ~4 kB heap (the IRAM that is freed) [MAP] | slower cracking |
| b4 | NetCut's 64-entry device table → PSRAM | `src/modules/wifi/netcut.cpp` | 4.3 kB static [MAP] | none (table is unchanged, just relocated) |
| b5 | RF decoder's 512-entry allocation table → PSRAM | `src/modules/rf/protocols/rf_stream_memory.c` | 4 kB static [MAP] | none |

**The `lib_ignore = ESPmDNS` trick (b2).** `#undef`ing mDNS calls in the source
is not enough: PlatformIO's default LDF links a library's objects whenever a
source *includes* its header (it ignores `#if`), and ESPmDNS's global object kept
the 2 kB mDNS component alive. So the installer also drops the library entirely
via `lib_ignore`. [`PATCH.md`, `install_into_bruce.py`]

**PSRAM relocation mechanism (b4/b5).** A macro turns Bruce's array declaration
into a pointer to an array of the same type (`#define s_devices (*s_devices_ps)`
/ `#define allocated (*rf_alloc_ps)`), allocated once at startup via
`heap_caps_calloc(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)` (falling back to
internal `calloc` if PSRAM fails), so every existing `s_devices[i]` /
`allocated[i]` reference in Bruce still compiles and works. Only a 4-byte pointer
stays in internal RAM. [`install_into_bruce.py` PATCHES]

**Verified in the image** [SW/MAP, `docs/TEST_RESULTS.md`]: no TinyUSB or mDNS
object takes internal RAM (none in the map's DRAM ranges; no `mdns_*` symbol
left); `sha1_transform` is in flash; NetCut's and the RF tables are 4-byte
pointers in internal RAM.

## Owner's decision & the configurability requirement (NEXT STEP)

The feature drops in group (b) are the **owner's explicit choice**
(`docs/TEST_RESULTS.md` "RAM trims in Bruce (owner's choice)"). The agreed
requirement for the handoff is that the trims **stay default-ON** but become
**installer/build configurable**, so a user who wants USB HID / mDNS / fast
cracking back can opt out without hand-editing Bruce.

**This is NOT yet implemented.** The current `tools/install_into_bruce.py`
hard-wires the trims: it always inserts `-DBRUCELINK_RAM_TRIM=1` and the guarded
lines, and its only CLI flags are `--force` and `--uninstall` — there is no
`--no-trim` or per-trim toggle. Treat configurability as a **NEXT STEP**; see
`NEXT_STEPS.md`.

## Hardware RAM-cost measurements

For context — the costs the trims are fighting, measured on the owner's device
after the BLE memory savings (`docs/TEST_RESULTS.md` "Hardware results",
`HARDWARE.md`) [HW]:

| Point | Internal free |
|---|---|
| Main menu (before BLE) | ~133,247 B |
| BLE start | −57.5 kB → ~75,707 B |
| Flipper linked | −14 kB → ~61,735 B |
| Wi-Fi scan, first | peak cost ~35.7 kB (lowest free 25,911 B) |
| Wi-Fi scan, repeat | peak cost ~29.7 kB (lowest free ~25,319 B) |
| Steady state, linked | 55,059 B free vs 52 kB guard → **~1.8 kB headroom** |

The repeated scans begin and end at exactly 55,059 B — **no leak**; the ~6.5 kB
not returned after the first scan is the one-time default event loop. The 1.8 kB
steady-state headroom is why group (a)'s PSRAM moves matter even though jobs
(ADR 0006) now run Wi-Fi with BLE off: every internal byte saved is headroom the
link keeps.

## Verification status & GAPS

- The **−42,536 B / −4,092 B / −122 kB** headline and all group-(b) savings are
  **[MAP]** — build map, not measured on the stick. The only group-(a) saving
  confirmed on hardware is a3 (~14 kB). a1 (~16 kB) and a2 (~3 kB) are **[MAP]**.
- **GAP:** no per-line serial RAM figures from the trim build were reported
  (`cb11f31` was "all working well" with no numbers). `HARDWARE_TESTING.md` step
  4a lists exactly which `internal` / `dma-blk` serial lines to capture to turn
  these [MAP] figures into [HW].
- **GAP:** trim configurability is agreed but unimplemented (above).
- **Cross-check note.** `docs/TEST_RESULTS.md` carries several static-RAM
  readings at different commits (133,236 / 133,788 / 133,796 B untrimmed-ish,
  91,260 B trimmed). The authoritative trim comparison is `3fac5b5` 133,796 B →
  trimmed 91,260 B; 91,260 B is also the current `448b798` figure.
