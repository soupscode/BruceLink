# BruceLink — Troubleshooting history

Significant debugging work reconstructed from `docs/TEST_RESULTS.md`
(especially "Hardware results"), `LOCAL_HANDOFF.md` §6, the ADRs and the git
log. Each issue: symptoms, root cause (flagged where unknown), what was tried,
resolution, source + commits, how to recognise a recurrence.

Evidence levels used below: **owner-reported** (ran on the owner's hardware,
not independently verified by any cloud session), **host-tested** (ran on the
host under ASan+UBSan), **software-verified** (compiled/linked only),
**unconfirmed** (a stated reading the record never proved). The entire git
history is 2026-11-23 → 2026-11-24; commit dates are authoritative for order.

Cross-refs: `MEMORY_OPTIMIZATION.md` (RAM budget and trims), `TESTING.md` (test
suites and the mutation checks), `DECISION_HISTORY.md` (ADR narrative),
`KNOWN_ISSUES.md` (still-open items), `HARDWARE.md` (device facts),
`SESSION_SUMMARY.md` (full timeline).

---

## 1. Connect crashed the Flipper with a BusFault (stack overflow)

**Symptoms.** First run of any BruceLink code on a device (owner's Flipper,
Momentum `mntm-012`, 2026-11-23, over the in-process loopback mock — no radio).
Build `f100604` (the `dev/foundation-v1` tip merged as PR #1, merge `5a8cd04`):
pressing **Connect crashed the Flipper with a BusFault**.

**Root cause (established).** Stack overflow, two independent causes, both on
the Connect/handshake path:
1. The FAP stack was 4 KB. The Connect call chain held two 540-byte frame
   buffers plus the X25519 exchange (`bl_x25519`'s frame alone is 1216 bytes,
   measured with `-fstack-usage`) under the ViewDispatcher's own frames.
2. The 50 ms poll ran in a `FuriTimer` callback. Those execute on the FreeRTOS
   timer-service task, which Momentum gives a 256-word / 1 KB stack
   (`targets/f7/inc/FreeRTOSConfig.h`), and that callback drove the central
   side of the handshake, X25519 included.

**Approaches tried / what failed.** Diagnosed directly from the crash plus the
`-fstack-usage` measurement; no failed intermediate attempts are recorded.

**Resolution (`2410767`).** `stack_size` 4 KB → 8 KB in `application.fam`; poll
from the ViewDispatcher **tick event** (app thread) instead of a `FuriTimer`
(the mutex was removed — everything single-threaded now); handshake paths use a
128-byte buffer (largest handshake frame is 109 bytes) instead of `BL_MAX_MSG`;
Connection screen reflects "connected" on handshake completion. No shared-core
change. Confirmed working on-device at `0d8d272` (owner: "works good"; recorded
`88c2a94`, merge `3115c51`). Scope of that confirmation: UI, tick-driven
polling, the X25519 handshake on the Flipper CPU and request/response — all
against the **in-process mock**, not a radio.

**Source + commits.** `flipper/brucelink/application.fam`,
`flipper/brucelink/brucelink.c`, `flipper/brucelink/brucelink_i.h`,
`flipper/brucelink/transport/bl_transport_loopback.c`. Crash state `f100604` →
fix `2410767` → confirmed `0d8d272`.

**Recurrence.** Any BusFault/hard-fault on Connect or a handshake-heavy path.
Check (a) FAP `stack_size` in `application.fam` (currently 8 KB) and (b) that
nothing drives the handshake from a `FuriTimer` (1 KB timer-service stack).
`bl_x25519` is the largest stack consumer (~1216 B frame); rebuild with
`-fstack-usage` to re-measure. The loopback transport is gone (removed at
`b635b03`), but the sizing applies equally to the BLE path, which has **not**
been stack-profiled on hardware.

---

## 2. Wi-Fi + BLE do not fit in internal RAM on the StickS3 (the central failure)

**Symptoms.** The first over-the-air link worked (recorded `d18907f`): Flipper
`connected`, Device Info correct, Capabilities listed, ~3 s handshake, off/on
recovery. But **Wi-Fi Scan "does not do anything"** (owner). The first report
could not distinguish a lost reply from a failed or slow scan.

Second run, both devices on `72d9efc`: `requesting…` → `no reply (timed out)`;
counters at the timeout `link 1 mtu 247 rx 3/3 drop 0 bad 0 txf 0`, **no
`frag`/`odd` line** — the link was up, the MTU exchange reached the Flipper
(247), and **no reply data arrived at all**. Device Info afterwards also timed
out; toggling BruceLink off "crashes the stick."

**Root cause (established, from the StickS3 serial log).** Not enough internal
RAM for Wi-Fi next to BLE:
```
E wifi:Expected to init 4 rx buffer, actual is 2
E wifi_init: Failed to deinit Wi-Fi (0x3001)
[E] wifiLowLevelInit(): esp_wifi_init 0x101: ESP_ERR_NO_MEM
[BruceLink] wifi scan: result -2 in 55 ms
```
The StickS3 has ~135 kB internal heap free at the menu; the radio stacks
allocate from **internal** RAM in this Bruce build (PSRAM does not help — Bruce
ships the framework libraries prebuilt). With NimBLE up, `esp_wifi_init` runs
out part-way, cannot be torn down, and leaves so little internal RAM that the
BLE stack can no longer send: the "scan failed" reply was produced (55 ms) but
**never left the device**, and nothing worked afterwards. The bridge task was
not blocked in the scan — it was **starved**. Bruce's own 15 kB block check
(`radioHasMemForWifi`) passed, so it is not a sufficient guard here.

**Measured costs (owner serial logs, `docs/TEST_RESULTS.md`; see also ADR 0004
and `MEMORY_OPTIMIZATION.md`):**

| Point | Internal free |
|---|---|
| Main menu, before BLE | 135,083 / 133,247 B |
| BLE up, scanning for Flipper | 75,707 B (BLE start ≈ 57.5 kB) |
| Flipper linked | 61,735 B (another ≈ 14 kB); 47,979 B before the RAM savings |
| Wi-Fi scan peak cost | **35,728 B first / 29,700 B repeat**; lowest free 25,879–25,911; lowest DMA block 16,372 |
| Steady state, Flipper linked | 55,059 B vs 53,248 B (52 kB) guard → **≈ 1.8 kB headroom** |

So BLE + BruceLink cost ≈ 72 kB internal (57.5 + 14), and a full `esp_wifi_init`
did not fit in the ≈ 48 kB left.

**Approaches tried.**
1. `c605fe9` — refuse a Wi-Fi start that would not fit (`wifiStartAllowed`,
   host-tested): first estimate ≥ 80 kB internal + 16 kB DMA block. *"Makes the
   failure clean. It does not make the scan work."*
2. `2b604e4` — free internal RAM: BLE discovery scan runs callback-only
   (`setMaxResults(0)`; NimBLE otherwise keeps every advertiser, unbounded, in
   internal RAM) and the ~4 kB chunk queue moves to PSRAM. Recovered ≈ 13.7 kB
   (47,979 → 61,675 with the Flipper linked). *Wi-Fi still did not fit.*
3. `ee58380` (ADR 0004) — reduced, **transient** Wi-Fi start
   (`BruceLinkWifi.cpp`): bring the driver up directly with minimal buffers
   (2 static RX/TX, 8 management, no A-MPDU, no NVS), scan, then stop and
   deinitialise; guard lowered to 52 kB from the measurements. This **worked**
   (owner: "Success from flipper"; network list shown). But the guard headroom
   was only ≈ 1.8 kB, and after Bruce's own Wi-Fi is used the largest block
   stays ≈ 32 kB and Bruce's 48 kB BLE guard refuses to restart BLE until
   reboot. A pre-start guard cannot rescue a tool (capture, portal, sniffer)
   that allocates as it runs.

**Resolution (`9f37f46`, ADR 0006 — owner's decision).** **One radio at a
time.** A capability that needs Wi-Fi (or a Bruce BLE tool) runs as a **job**:
the StickS3 records the job in RTC memory, restarts with BLE never started,
runs the tool with the full standalone-Bruce heap, restarts, turns BruceLink
back on, and the Flipper fetches the result on the new session. ADR 0004's
inline Wi-Fi path (`BruceLinkWifi.cpp`, `wifiStartAllowed`) was removed. The
Wi-Fi scan job worked on hardware (owner: "that worked"). This is the
architectural resolution of the project's central failure; see ADR 0006 and
`DECISION_HISTORY.md`.

**Source + commits.** `esp32/src/modules/brucelink/BruceBackendImpl.cpp`,
`esp32/lib/BruceLink/NimBleCentralTransport.cpp`,
`esp32/lib/BruceLink/BruceLinkFormat.{cpp,h}`,
`esp32/src/modules/brucelink/BruceLinkWifi.{cpp,h}` (added `ee58380`, removed
`9f37f46`), `esp32/src/modules/brucelink/BruceLinkJobs.{cpp,h}` and
`shared/brucelink_core/src/bl_job.c` (added `9f37f46`). Trail: `d18907f` (first
link) → `72d9efc` (diagnose) → `c605fe9` (guard) → `2b604e4` (savings) →
`ee58380` (transient start) → `9f37f46` (jobs). ADRs 0004, 0006.

**Recurrence.** `ESP_ERR_NO_MEM` / `esp_wifi_init 0x101` in the StickS3 serial
log, "Expected to init N rx buffer", a scan reply produced but never received,
Device Info timing out afterwards (bridge **starved**, not blocked), toggling
off "crashes." Watch the `[BruceLink] module: BLE up` / `Flipper linked` serial
lines for `internal` / `dma-blk` and compare against the baselines above; ≈ 1.8
kB more internal use anywhere in Bruce makes scans refuse (cleanly). Structural
rule (`CLAUDE.md`): **never start Wi-Fi from the bridge task while BLE is up**;
radio-exclusive capabilities run as jobs.

---

## 3. "StickS3 refused: unknown wire error" after the jobs change

**Symptoms.** `9f37f46` merged image on the StickS3; Wi-Fi Scan from the
Flipper returned **`StickS3 refused: unknown wire error`**.

**Root cause (established).** Version skew. The Flipper was still running the
**v0.3** app (no jobs). The StickS3 (`9f37f46`) replied with the new `USE_JOB`
error (16) for a job capability invoked inline; the v0.3 app has no name for
error 16, so it displayed "unknown wire error." The job path itself had not
run.

**Resolution.** Run matching builds — Flipper app **v0.4** (jobs). Then the
full job cycle worked (next entry, and issue 8).

**Source + commits.** `shared/brucelink_core/src/bl_proto.h` (error `USE_JOB`
16), `shared/brucelink_core/src/bl_strings.c`, `flipper/brucelink/brucelink.c`.
Introduced `9f37f46`; recorded `c71959f`.

**Recurrence.** Any unnamed/"unknown wire error" code on the Flipper after a
protocol change = **devices on different builds**. General rule (LOCAL_HANDOFF
§6; reinforced by the `222920f` wire change): both devices must run the same
build. The session transcript and header byte 5 (the request tag) changed at
`222920f`, so devices on opposite sides of that commit do not even complete the
handshake.

---

## 4. App-only / three-part flash does not boot; merged image does

**Symptoms.** Owner flashed `Bruce-1.17-BruceLink-b635b03-…-app-only.bin`
(`firmware.bin`) → StickS3 **did not boot**. The merged image of the same build
at `0x0` **boots** (owner-reported). Same again at `9f37f46`: a three-part
(bootloader / partitions / app) flash **did not boot**; the merged image at
`0x0` boots.

**Root cause: NOT KNOWN.** The docs state this explicitly. Cloud checks that
ruled out a build defect: the delivered app-only file is byte-identical to the
app inside the merged image at `0x10000`; the bootloader and partition table
(`factory` app at `0x10000`, 0x4E0000 bytes; app 0x48DF40) are byte-identical
across every merged image delivered; nothing in the code new-at-boot since the
last booting build (`ee58380`) — the main-menu item's constructor and icon
(first item drawn) and the bridge's static constructors — was found that could
fail. The flash offset and tool the owner used, and what the StickS3 held
before, are not recorded. **An emulator boot test was not run.**

**Approaches tried / what failed.** Source-level comparison ruled out a build
defect (the merged image of the same build boots). Could not reproduce (no
hardware in the cloud; no emulator boot test).

**Resolution (workaround, not a root-cause fix).** Documentation changed to
flash the **merged image** as a single file at `0x0` and stop
recommending `firmware.bin` alone or separate parts (`f3039f7` first advised
three-part to keep NVS; withdrawn at `222920f`; `27650af` records the merged
image booting). Trade-off: flashing the merged image at `0x0` **erases NVS**,
so the StickS3 pairing is lost on each update — pair again after flashing.

**Source + commits.** Builds that exhibited it: `b635b03`, `9f37f46`. Docs:
`f3039f7`, `27650af`, `222920f`; flashing guidance in
`docs/HARDWARE_TESTING.md`.

**Recurrence.** StickS3 does not boot after a non-merged flash. The merged
image flashed as a single file at `0x0` is the known-good path. **Cause remains
unknown** — do not report this as solved. To investigate: run an emulator boot
test, or capture the exact offset, tool and prior flash contents of a failing
non-merged flash.

---

## 5. Pairing completes, but connecting straight after fails for memory

**Symptoms.** `b635b03` merged image. Pairing completed on both devices (owner:
"paired"). **Connecting afterwards failed with a memory error** on the StickS3
(owner: "can't connect - uses too much memory"; exact text and serial not
captured). After a **StickS3 reboot**, Turn on + Connect work with the paired
key, and Device Info + Wi-Fi scan work.

**Root cause (reading — UNCONFIRMED).** After storing the key, `b635b03`
stopped BLE (pairing mode) and started it again (normal mode). Starting BLE
goes through Bruce's `radioHasMemForBle()`, which wants one free 48 kB
DMA-capable block. That held at boot (pairing mode started) but **not** on the
restart: memory allocated while BLE was up (the NVS key-store write happens
then) can sit inside the region BLE gives back and split it. The figures were
not captured, so this is a plausible reading, not a proven cause.

**Resolution (`993437b`).** After a successful pairing the bridge task switches
to normal sessions **in place** — `endPairing` + `setKey` (host-tested in
`tests/c/test_bridge.cpp`) — keeping NimBLE up, so **BLE is not restarted**. A
cancelled/failed pairing returns to the old key the same way (or releases BLE
if BruceLink was not on before). `moduleStop` waits 100 ms after the task exits
so the idle task frees its stack before any radio restarts. A BLE start that
fails for memory now prints the largest free block and the 48 kB needed and
suggests a reboot. **The in-place switch has not run on hardware.**

**Source + commits.** `esp32/src/modules/brucelink/BruceLinkModule.cpp`.
Commit `993437b`. ADR 0005; ADR 0006 adds a "restart into BruceLink" one-shot
RTC flag for when Turn on finds no 48 kB block.

**Recurrence.** "not enough memory for BLE" / "uses too much memory" when
starting BLE after any BLE-up or Wi-Fi-up activity (pairing; Bruce's own Wi-Fi
— M5.6). Read the failure line's largest-free-block vs 48 kB. Fix pattern:
avoid restarting BLE (switch modes in place); otherwise reboot, or use the
"restart into BruceLink" one-shot. Heap fragmentation after BLE/Wi-Fi teardown
is the suspected mechanism and is **not fully root-caused**; the module logs
memory on stop so the next occurrence can be measured.

---

## 6. The split-write / RX Offset false suspicion

**Symptoms.** Part of the Wi-Fi-scan investigation (issue 2): a reply never
arrived, and the first report could not say whether it was lost in transit.

**Suspicion.** The Flipper's RX parser (`bl_gatt_evt`) dropped every
attribute-modified event whose **Offset** field was non-zero. ST documents that
field as how the controller splits one write across events (offset of the
piece, bit 15 = more follow). A reply longer than one chunk starts with a full
244-byte chunk — one byte more than the stock serial profile's 243 — so a split
was plausible and would lose the first chunk of every multi-chunk reply.

**Approach.** Fixed **on suspicion, not evidence** (`72d9efc`): `bl_rx_asm_push`
joins split RX writes; added link counters (`link, mtu, rx, drop, bad, txf,
frag, odd`) so a real occurrence would be visible.

**How it was ruled out.** The second run on `72d9efc` showed `rx 3/3 drop 0 bad
0 txf 0` and **no `frag`/`odd` line** at the timeout — the MTU reached the
Flipper (247) but no reply data arrived at all. The split-write handling was
never involved; **the suspicion was wrong**. The real cause was RAM starvation
(issue 2).

**Source + commits.** `flipper/brucelink/transport/bl_gatt_evt.c`,
`bl_ble_transport.c`, `flipper/brucelink/brucelink.c` (counters). Commit
`72d9efc`. ADR 0001 amendment.

**Recurrence.** If a multi-chunk reply is ever lost, the `frag`/`odd` counters
on the Connection screen show whether this controller actually splits a 244-byte
write (**still not observed on hardware**). The join code is retained and
host-tested (`tests/c/test_gatt_evt.c`). Lesson: use the counters to tell "lost
in transit" from "never produced" **before** fixing.

---

## 7. Inverted GATT return value, and other review findings worth keeping

**`ble_gatt_characteristic_update` returns `true` on failure (inverted).**
Recorded as an implementation hazard in ADR 0001 ("Consequences") and
LOCAL_HANDOFF §6. It has not bitten at runtime — the firmware-facing BLE code
is not host-testable and the notify path is only software-verified — but any
future change to the TX path must honour the inversion. The FAP-defined ST
event structs are flagged the same way.

**Other findings (full tables in `TESTING.md` and `DECISION_HISTORY.md`; the
load-bearing ones):**

| Finding | Where found | Fix / outcome | Regression test |
|---|---|---|---|
| A central that connects but never subscribes holds the only link | BLE-profile review | drop after a timeout: 5 s → **20 s** (too short to subscribe by hand) | `t_gatt_connection_complete`, `t_sub_guard` (`b3c282c`, `b648d78`) |
| Any handshake receive error was terminal (~40 s recovery from one stray frame) | BLE-profile review | non-terminal | `t_client_stray_handshake_frame_ignored` (`b648d78`) |
| Chunk size not capped at 244 for a larger MTU | BLE-profile review | one shared rule `bl_ble_chunk_for_mtu` | `t_ble_chunk_for_mtu` |
| Advertisement exactly 31 bytes, silent rejection | BLE-profile review | name shortened to `BL` (3 bytes margin) | — (build) |
| Re-sending "enable active" reset the rate window; a new session reset it too | 2nd Codex review | window is device transmit history, only elapsed time expires it (`bl_safety.c`) | `t_codex2_reenable_does_not_reset_budget`, `t_codex2_reconnect_does_not_reset_budget` (`f100604`) |
| Unauthenticated peer could hold the only BLE link (connected set before auth) | 2nd Codex review | `needsDisconnect()` + 10 s auth deadline + 30 s peer cooldown; transport single-owner (`loop()` owns the bridge, callbacks only record events) | bridge supervision suite (`f100604`) |
| PSK lost on reconnect; ESTOP did not block `CAP_START`; poll failure left backend open; `capture_` uninitialised (UBSan `0xA5A5A5A5`); rate limit not bounding real frames; Wi-Fi scan length bookkeeping; broadcast BSSID accepted as a target | PR #1 review | reproduced on `31629db`, fixed, regression-tested | `t_pr1_*`, `t_safety_*`, `[textbuf]` (`9f89c4d`) |
| HELLO version range not in the transcript; seal/open before established; request tags (M4.7); gate needs a target + no counter wrap; cancel job before the restart; e-stop cleared only by a session; demo key refuses capture | four-reviewer repo review | each fix fails on the old code (mutation-checked) | listed in `TESTING.md` (`222920f`) |

Notes that are behaviour, not bugs, recorded so they are not "re-fixed":
Momentum's `Connected` event only fires after pairing (there is none), so the
Flipper detects the link from the TX subscription; `bt_disconnect()` stops
advertising for good, so the transport restarts it; the firmware reloads the
default profile on SD remount, so the transport detects that and reports link
down.

---

## 8. Jobs too slow (double boot animation)

**Symptoms.** First Wi-Fi-scan job on hardware (`9f37f46`, app v0.4): the full
cycle **worked** (owner: "that worked") but was **too slow** (total time not
reported).

**Root cause (established).** Bruce's boot animation runs up to 7 s on every
boot (`boot_screen_anim`), and a job restarts **twice**; the scan allowed up to
300 ms per channel.

**Resolution (`8f9285a`, app v0.5).** `bruceLinkBootEarly()` (installed after
`_late_setup_gpio()`) runs a pending job **before** the boot animation, sound,
Wi-Fi-at-startup and serial task. The boot after a job skips animation, sound
and Wi-Fi-at-startup for that boot only (RAM, never saved; `bruceLinkBoot()` at
the end of `setup()` restores them and starts BruceLink). Scan 120 ms/channel;
reply flush 500 → 300 ms; finish screen 400 → 150 ms. The result reports scan
time and stick boot time; the Flipper (v0.5) shows the total ("Took N s").
Owner: **"much faster"** (recorded `f4febb1`). **Per-change figures were not
reported**, so how much each saved is unknown.

**Source + commits.** `esp32/src/modules/brucelink/BruceLinkJobs.cpp`,
`BruceLinkModule.{cpp,h}`, `flipper/brucelink/brucelink.c`,
`tools/install_into_bruce.py` (early-hook position). Commit `8f9285a`; ADR 0006.

**Recurrence.** If a job cycle feels slow, confirm `bruceLinkBootEarly()` runs
before Bruce's boot animation and that the post-job boot skips it. The result's
scan/boot timing and the Flipper's "Took N s" localise where the time goes
(expected remaining cost: BLE reconnect + handshake, ~3 s). Timing figures
remain **unmeasured** end to end.

---

## Diagnostic quick reference

| You see | Likely issue | First check |
|---|---|---|
| BusFault on Connect | #1 | FAP `stack_size` (8 KB); no `FuriTimer`-driven handshake |
| `esp_wifi_init 0x101` / `ESP_ERR_NO_MEM`, scan reply never received | #2 | `internal`/`dma-blk` serial lines vs baselines; use a job |
| "unknown wire error" | #3 | both devices on the same build |
| StickS3 will not boot after flashing | #4 | flash the **merged** image as a single file at `0x0` |
| "uses too much memory" after pairing / after Bruce Wi-Fi | #5 | largest free block vs 48 kB; avoid BLE restart or reboot |
| Multi-chunk reply lost | #2 / #6 | `frag`/`odd` counters (lost vs never produced) |
| Job cycle slow | #8 | early boot hook before the animation; "Took N s" |
