---
name: brucelink-memory-regression
description: Detect and analyze internal-RAM regressions on the StickS3 build — guarding the ~46 kB of trims and the thin Wi-Fi-job headroom. Invoke when a change could affect RAM, when a Wi-Fi job starts refusing, or to re-verify the trims.
---

# BruceLink — Embedded Memory Regression Analysis

Paths are repo-root-relative. Detail: `docs/handoff/MEMORY_OPTIMIZATION.md`, `docs/handoff/HARDWARE.md`.

## When to invoke
- A change could affect internal RAM (new buffers, NimBLE/Bruce config, a trim).
- A Wi-Fi job begins refusing ("would not fit next to BLE").
- Re-verifying the trims after a dependency or Bruce bump.

## Required project context
- The StickS3 RAM facts: single 2.4 GHz radio; ~133 kB internal free at menu; BLE link ~72 kB; PSRAM cannot hold the radio stacks; one radio at a time (ADR 0004/0006).
- The trims live behind `BRUCELINK_RAM_TRIM` (`esp32/src/modules/brucelink/PATCH.md`) and the forced include `esp32/lib/BruceLink/bl_sticks3_ram.h`.

## References
- `docs/handoff/MEMORY_OPTIMIZATION.md`, `docs/handoff/HARDWARE.md`, `docs/decisions/0004-radio-memory-on-the-sticks3.md`, `docs/decisions/0006-one-radio-at-a-time.md`, `docs/TEST_RESULTS.md`.

## Procedure
1. Build the trimmed Bruce firmware; read **static internal RAM** from the PlatformIO output / map.
2. Compare to the baseline **91,260 B** (trimmed; untrimmed ≈ 133,796 B). Any increase must be justified.
3. If on hardware, capture the `[BruceLink] module:` serial lines (`internal` / `dma-blk`) at: BLE up, Flipper linked, Wi-Fi scan begin/peak/after-release. Derive BLE cost (~57.5 kB), linked (+~14 kB), scan peak (~35.7 kB first / ~29.7 kB repeat).
4. Check steady-state headroom vs the 52 kB guard (baseline ≈ 55,059 B free ≈ 1.8 kB margin).
5. Label every figure as map-computed `[MAP]` or stick-measured `[HW]`.

## Verification / acceptance
- Static internal RAM ≤ baseline, or the increase is justified and documented.
- On hardware: Wi-Fi jobs still start; steady headroom not eroded below the guard.

## Common mistakes & pitfalls
- Confusing the "largest DMA block" (a separate ~32 kB region) with the main region's block.
- Expecting PSRAM to hold radio stacks — it does not.
- A mere +1.8 kB internal bump anywhere in Bruce makes scans refuse (cleanly).
- Reporting a map-computed number as if measured on hardware.

## Expected output
A before/after RAM table (static `[MAP]`, plus `[HW]` figures if available), a verdict, and any new refusal risk against the guard.
