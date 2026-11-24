# BruceLink — Session summary (concept → HEAD 448b798)

Chronological reconstruction to preserve engineering context not obvious from
the final source. Built from the git log (dates authoritative), the state docs
(`docs/STATUS.md`, `docs/roadmap.md`, `LOCAL_HANDOFF.md`, `docs/TEST_RESULTS.md`),
and ADRs 0001–0006. Siblings: `DECISION_HISTORY.md` (why each ADR), `CURRENT_STATE.md`
(what is true now), `TESTING.md` (evidence), `TROUBLESHOOTING_HISTORY.md` (the
debugging sagas in depth).

Evidence tags: **owner-reported** (ran on the owner's hardware), **host-tested**,
**software-verified** (built/linked only), **unverified**.

HEAD is `448b798` on branch `feat/radio-handoff` (PR #8 open → `main`), 2026-11-24.

---

## GAP — the recorded history is one ~21.5-hour span

Every commit is dated **2026-11-23 02:37 → 2026-11-24 00:07** (45 commits, root
`f7486f9`). The project's **conception, requirements and the original scaffold**
predate the first commit and are **not in the git record**. What is known of
that pre-history, reconstructed only from docs:
- An original "v0" scaffold existed with Python codec tests (5 tests, green —
  `docs/TEST_RESULTS.md` "M0") and **stub** C/C++ sources; the FAP was later
  confirmed to compile. This is the "M0 repository audit baseline."
- The decision to target Momentum `mntm-012` + Bruce `1.17` + NimBLE 2.5.1, and
  the BLE role split, were settled in M1 but their deliberation is captured only
  as the finished ADR 0001.

Treat anything about ordering or rationale **before `f7486f9`** as uncertain
(likely lost to context compaction). Also: `docs/TEST_RESULTS.md` dates nearly
every hardware entry "2026-11-23" even where the surrounding commit landed
2026-11-24 — those are reporting-day labels; **git commit order is
authoritative**.

---

## Phase 0 — Concept & scaffold

- **`f7486f9`** (02:37) — "Document BruceLink scaffold and quick start." First
  commit: docs + quick start over a stub scaffold. Roadmap **M0** (audit) done.

## Phase 1 — Foundation + protocol core (Milestones 1–3)

- **`31629db`** (03:55) — "Foundation + phase-2 core: protocol, builds, FAP,
  ESP32 bridge, docs." The portable C99 core lands: crypto
  (SHA-256/HMAC/HKDF/X25519), frame codec + MAC, authenticated session (PSK +
  ephemeral ECDH, replay, version negotiation), fragmentation, COBS, capability
  registry, capture manager, safety gate, pcap + CRC-32. Host-tested + C↔Python
  interop. This becomes the **audit baseline** that PR #1 defects are reproduced
  against.
- **`9f89c4d`** (04:17) — PR #1 review round 1: 6 reported defects + 3 found
  while fixing, each reproduced on `31629db`, fixed, regression-tested (PSK lost
  on reconnect, ESTOP vs `CAP_START`, poll-failure cleanup, uninitialised
  `capture_`, rate limit not bounding real frames, Wi-Fi line bookkeeping,
  broadcast BSSID). host-tested.
- **`f100604`** (04:37) — PR #1 review round 2: the rate-window bypass
  (re-enable / reconnect refilled the transmit budget) and the unauthenticated
  link hold (connected set before auth). Fixes in `bl_safety.c` and the NimBLE
  transport (single-owner restructure, 10 s auth deadline, 30 s peer cooldown).
  host-tested (bridge); transport software-verified.
- **`5a8cd04`** (09:41) — merge **PR #1** (`dev/foundation-v1`). Roadmap **M1–M3**
  done on host; ping/status still 🟡.

## Flipper app, first hardware run (over the loopback mock)

- **`2410767`** (10:05) — **BusFault fix.** First device run (owner's Flipper,
  `f100604`) crashed on Connect: stack overflow. Fix: 8 KB FAP stack, poll from
  the ViewDispatcher tick (not a `FuriTimer`), 128-byte handshake buffers. See
  `TROUBLESHOOTING_HISTORY.md` §1.
- **`0d8d272`** (10:10) — replace repo symlinks with generated wrapper sources
  (Windows checkouts break on symlinks; `tools/gen_wrappers.py`). This is the
  build the owner ran green ("works good").
- **`88c2a94`** (10:13) — record the first hardware result: **FAP runs on a
  Flipper over the loopback mock** (UI, polling, X25519 handshake, request/reply
  against the in-process mock — no radio). owner-reported.
- **`3115c51`** (10:26) — merge **PR #2** (`fix/connect-stack-overflow`).

## Flipper BLE peripheral profile (M4.2) — first radio

- **`18c5f5b`** (10:47) — "Flipper BLE peripheral profile, client timeouts,
  demo-key lock (M4.2, M3.5)." FAP-owned custom 128-bit GATT profile; the demo
  key is locked out as a usable key. ADR 0001.
- **`b648d78`** (11:04) — address an **independent review** of the BLE profile
  (checked against pinned Momentum/ST/NimBLE source): subscription guard,
  shared chunk-size rule, non-terminal handshake errors, 31-byte advertisement
  margin, SD-remount recovery. host-tested parser/client.
- **`3dd48d1`** (11:13) — redraw the Connection screen on transport-status
  change (fixes the "stuck on `BLE advertising`" display defect).
- **`b3c282c`** (11:21) — give a central **20 s** to subscribe before dropping
  it (5 s proved too short to subscribe by hand from a phone scanner).
- **`6c6803d`** (11:21) — record the **phone-scanner run** (owner-reported,
  presumed `b648d78`): the scanner finds the service, connects, and after being
  given the subscribe steps the owner reported "it all works" (HELLO sent as
  notifications). No StickS3 yet.
- **`2741f65`** (11:31) — merge **PR #3** (`feat/flipper-ble-profile`).

## Bruce bridge module + first over-the-air link (Milestone 5)

- **`e44142b`** (11:30) — "Bruce module, installer and transport shutdown (M5.1,
  in progress)." `BruceLinkModule`, `tools/install_into_bruce.py`, bridge
  shutdown path.
- **`59268b0`** (11:48) — bound connect attempts; `moduleStop` reports a task
  that has not exited.
- **`589d93c`** (11:51) — record the **Bruce 1.17 + BruceLink full firmware
  build** (`m5stack-sticks3`), image `Bruce-1.17-BruceLink-59268b0-…` (sha256
  `eaf88e41…9106`). Build + link only.
- **`d18907f`** (13:22) — record the **first over-the-air Flipper ↔ StickS3
  link** (owner-reported): Flipper `connected`, Device Info right, Capabilities,
  ~3 s handshake, off/on recovery — **but Wi-Fi Scan "does not do anything."**
  This opens the central crisis.
- **`dbcdac8`** (13:48) — merge **PR #4** (`feat/bruce-bridge-module`).

## The Wi-Fi/BLE internal-RAM crisis → ADR 0004

See `TROUBLESHOOTING_HISTORY.md` §2 and `MEMORY_OPTIMIZATION.md` for the full
measured saga; the commit trail:
- **`72d9efc`** (13:29) — diagnose the Wi-Fi-scan defect; add link counters;
  join split RX writes on the Flipper (fixed on suspicion — later shown wrong,
  §6). Second run: `no reply`, counters prove **no reply data arrived**.
- **`d1ec476`** (13:59) — bound the StickS3 scan and log each step; record the
  hang.
- **`c605fe9`** (14:07) — refuse a Wi-Fi start that would not fit next to BLE;
  **record the root cause** (`esp_wifi_init` `ESP_ERR_NO_MEM`; ~48 kB free with
  BLE up, ~87 kB taken). "Makes the failure clean. It does not make it work."
- **`2b604e4`** (14:20) — free internal RAM (callback-only BLE scan, chunk queue
  to PSRAM); ~13.7 kB recovered.
- **`ee58380`** (14:41) — **ADR 0004**: reduced, transient Wi-Fi start
  (`BruceLinkWifi.cpp`). Owner: **"Success from flipper"** — network list shown.
  Measured peak 35.7 kB first / 29.7 kB repeat, 4.8 s scan, **but ≈ 1.8 kB
  headroom**.
- **`1848a0f`** / **`fd65775`** / **`5b92344`** (14:5x) — record the first scan
  over BLE, that the post-scan link drop was the owner leaving the app, and
  three repeated scans (no leak; thin guard headroom).
- **`2d220c5`** (15:05) — merge **PR #5** (`fix/wifi-scan-hang`).

## Pairing (ADR 0005, M5.3)

- **`39db5a8`** (15:14) — pairing core: key agreement with a 6-digit comparison
  code (man-in-the-middle resistant). 13 host tests + Python recomputation.
- **`5fdc650`** (15:28) — pairing on both devices: screens, key storage (Flipper
  enclave-encrypted file; StickS3 NVS), **no demo key on hardware**. (This is
  the last commit that still contains the loopback transport — see below.)

## Owner direction: BLE-only + BruceLink on Bruce's home menu

- **`b635b03`** (15:46) — **"BruceLink on Bruce's main menu; Flipper app
  BLE-only."** Two owner-requested changes: (1) **drop the loopback mock** —
  `flipper/.../bl_transport_loopback.c` deleted (survives in history at
  `5fdc650`); BLE is the only transport from v0.3. (2) Move the menu entry from
  Config → Advanced to **Bruce's main menu** (`BruceLinkMenu`), with an installer
  upgrade path. owner-reported: owner used it to pair and turn on.
- **`f3039f7`** (20:26) / **`27650af`** (20:27) — flashing guidance. App-only
  (`firmware.bin`) did not boot; the merged image boots. First advised a
  three-part flash to keep NVS (later withdrawn, §4 of troubleshooting). Cause
  of the non-boot **unknown**.
- **`993437b`** (20:39) — **leave pairing mode without restarting BLE**
  (`endPairing` + `setKey` in place). Fix for the "pairing completes, connect
  fails for memory" hardware failure (§5). Not yet run on hardware.

## Owner decision: one radio at a time → jobs (ADR 0006)

- **`9f37f46`** (21:04) — **ADR 0006.** After repeated StickS3 memory failures
  the owner chose **one radio at a time**: Wi-Fi tools run as **jobs** (RTC
  record, restart with BLE off, run, restart, auto-start BruceLink once; Flipper
  fetches the result). Supersedes ADR 0004; its inline Wi-Fi path removed. Adds
  `bl_job.c`, `BruceLinkJobs.{cpp,h}`, protocol `JOB_*` 0x22–0x25 + error
  `USE_JOB` 16. host-tested (16 mutants caught across core + bridge).
- **`c71959f`** (21:21) — record: `9f37f46` merged image boots; Wi-Fi Scan from
  a **v0.3** Flipper showed "unknown wire error" (version skew, §3).
- **`8f9285a`** (21:32) — faster jobs: run the job before Bruce's boot animation,
  skip the animation on a job boot, 120 ms/channel scan. app v0.5.
- **`f4febb1`** (21:36) — record: owner reports the faster job cycle **"much
  faster"** (figures not reported). First full job cycle had already **worked on
  hardware** at `9f37f46` (owner: "that worked").

## Independent repo review (four reviewers) — wire change

- **`222920f`** (22:24) — fix the independent review's findings across core,
  StickS3 and Flipper. **Wire change**: the handshake transcript and header byte
  5 (request tag, M4.7) changed — **both devices must be updated together**.
  Each host-runnable fix fails on the old code (mutation-checked). static RAM
  back to 133,788 B at this point. Not yet run on hardware.

## RAM trims (owner's choice)

- **`3fac5b5`** (22:45) — free internal RAM on the StickS3 **without losing
  features** (NimBLE host buffers + bridge stack to PSRAM via a forced-include
  header; passive scanning). software-verified.
- **`cb11f31`** (23:16) — **owner chose to drop features**: USB HID, USB mass
  storage, mDNS; move NetCut's and the RF decoder's tables to PSRAM. All guarded
  by `BRUCELINK_RAM_TRIM` (5 Bruce files, `PATCH.md`). Static internal RAM
  **133,796 → 91,260 B (−42,536)**, IRAM −4,092 B, Flash −122 kB. owner-reported
  on-device: "all is working well" (no figures). **`LITE_VERSION` is not set for
  this env** — all non-LITE features still compile in.

## Passive capture + capture-file transfer (Phase 2)

- **`2b2c669`** (23:45) — **handshake capture as a job** (`wifi.sniff.handshake`
  0x0020): runs Bruce's own sniffer with BLE off, as a **station that only
  listens** (passive — nothing transmitted, so the active-job rules do not
  apply). Plus list / download (resumable, CRC-32) / delete of capture files
  over the live link (`bl_files.c`). host-tested; **not yet run on hardware**.
- **`a1a6c04`** (23:49) — capture files in the Python reference; the capture job
  never joins a network (passive guarantee reinforced).
- **`448b798`** (00:07, 2026-11-24) — **HEAD.** Fix a transfer review: requests
  carrying a file name could not be sent. host-tested (`bl_files`, `bl_client`).

---

## Architectural reframe: bridge, not reimplementation

The current (2026-11-24) framing — captured in `CLAUDE.md`, `ARCHITECTURE.md`
and the two upstream inventories `NATIVE_BRUCE_INVENTORY.md` /
`INTEGRATION_INTERFACES.md` — is that BruceLink is an **integration layer /
remote management bridge**: it exposes Bruce's existing functions through
supported interfaces behind auth + safety gates, and **does not reimplement**
Bruce's wireless tools. BruceLink owns discovery, menu nav, config, job
submission, status, cancellation, file transfer, connection and resource
management; Bruce owns tool behaviour.

The agreed next step is **one generic supervised native-function job** (a
per-function allowlist with classes; no raw CLI passthrough / arbitrary exec).
This is being written up as **ADR 0007 — which does not exist yet**. Describe
the generic-job model as **proposed/agreed, not implemented**. The **shipping
code still uses per-tool jobs** (the Wi-Fi scan job and the passive
handshake-capture job). RF-transmitting functions stay **refused** until ADR
0006's active-job rules are implemented and tested.

## Owner direction changes (the forks that shaped the code)

| When | Decision | Effect |
|---|---|---|
| `b635b03` | **Drop the loopback mock**; BLE-only | Loopback deleted (history `5fdc650`); the mock is no longer a supported transport |
| `b635b03` | **Move the menu item to Bruce's home menu** | `BruceLinkMenu`; installer upgrades the old Config → Advanced entry |
| `9f37f46` | **One radio at a time** (after the RAM crisis) | ADR 0006; tools run as jobs; ADR 0004's inline Wi-Fi removed |
| `cb11f31` | **Keep the RAM trims** (accept feature loss) | −42.5 kB static internal RAM; USB HID / mass storage / mDNS dropped |
| `2b2c669` | **Keep capture passive** | Listen-only station; no transmit; Bruce's "deauth while sniffing" not offered |
| current | **Reframe as a bridge, not a reimplementation** | Proposed ADR 0007; generic supervised job (not yet built) |

## Current state (HEAD `448b798`)

- **Verified in the cloud at HEAD** (2026-11-24): host C core **3681** + bridge
  **448** checks (ASan+UBSan), cppcheck clean, **31** Python tests OK; FAP v0.7
  APPCHK pass (Target 7 / API 87.1); ESP32 compile check SUCCESS; Bruce 1.17 +
  BruceLink builds, static internal RAM **91,260 B** with trims. **None is
  hardware.** See `TESTING.md` / `docs/TEST_RESULTS.md`.
- **owner-reported on hardware** (through `cb11f31`): Flipper app v0.2–v0.5, the
  OTA link (handshake, Device Info, Capabilities, off/on), pairing + the paired
  key surviving a reboot, the main-menu item, and the Wi-Fi scan as a job (then
  faster). **Not yet run on any device:** the `222920f` review fixes (both
  devices must update together), capture + capture-file transfer, active ops
  (refused), Forget, throughput, Bruce-feature coexistence.
- **Security posture:** authorization = the authenticated connection (ADR 0002);
  frames are **authenticated but not encrypted** until M4.6; the on-air demo key
  means the hardware runs demonstrate the link, not its security (real devices
  use a paired key). RF-transmitting capabilities remain default-off and
  refused. See `KNOWN_ISSUES.md` and `NEXT_STEPS.md`.
