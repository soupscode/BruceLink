# Hardware & board/firmware constraints

The two devices BruceLink runs on, and the board, radio, storage and flashing
constraints that shape the design. Reference for the engineering handoff.

> **Companions.** Radio facts and the per-capability RF classification:
> `NATIVE_BRUCE_INVENTORY.md`. How Wi-Fi tools are driven and why each is
> radio-exclusive: `INTEGRATION_INTERFACES.md` §3. The memory work that makes
> the link fit: `MEMORY_OPTIMIZATION.md`. Boot/flash failures in detail:
> `TROUBLESHOOTING_HISTORY.md`. Flashing procedure: `BUILD_AND_DEPLOY.md`.
> Decisions: `docs/decisions/0001-ble-transport.md`,
> `docs/decisions/0004-radio-memory-on-the-sticks3.md`,
> `docs/decisions/0006-one-radio-at-a-time.md`.

## Evidence legend

Every number below carries one of these tags. Do not promote one tier to
another.

- **[HW]** measured on the owner's physical devices (owner-reported serial logs
  in `docs/TEST_RESULTS.md` → "Hardware results"); not independently reproduced
  by the cloud session.
- **[MAP]** read from the PlatformIO build map / computed from the build, not
  measured on a device.
- **[SW]** software-verified (host test or cross-compile only).
- **[SRC]** a fixed source/design fact (header, `.fam`, ADR), not a measurement.

## Two devices at a glance

| | Flipper Zero | M5StickS3 |
|---|---|---|
| Firmware | Momentum `mntm-012` (`e1784e7`), API 87.1 | Bruce `1.17` (`2b6ba4d`), env `m5stack-sticks3`, + BruceLink installer patch |
| BLE role | peripheral (advertises) | central (scans, connects, subscribes) |
| Session role | initiator (sends `HELLO`) | responder | 
| SoC | STM32WB (core 2 = `ble_light` copro stack) | ESP32-S3 + 8 MB PSRAM |
| BruceLink code | FAP (`flipper/brucelink`), BLE only | bridge lib + Bruce glue (`esp32/`) |
| Key storage | enclave-encrypted file on SD | NVS |
| Radios in play | BLE only (BruceLink never uses the sub-GHz/NFC/etc. radios) | single 2.4 GHz (Wi-Fi **or** BLE, never both — ADR 0006) |

Roles are fixed by ADR 0001: the Flipper is peripheral-only (Momentum ships the
*light* copro stack with `GAP_PERIPHERAL_ROLE`; no central API is exported at
API 87.1), so the ESP32-S3 must be the central. The security-layer role split is
independent of the BLE role: the Flipper initiates the authenticated session
handshake once the link is up. [SRC]

## Flipper Zero

**Role.** BLE peripheral + session initiator. It advertises a custom 128-bit
GATT service (name `BL`, service `a1b20001-7a9c-4b1d-9c2e-5f6a7b8c9d00`, RX
`…0002` write, TX `…0003` notify), and sends `HELLO` as soon as the StickS3
subscribes to TX. Link-up is detected from the TX subscription, **not** the
firmware's `Connected` status (which only fires after BLE pairing, and BruceLink
uses `GapPairingNone` / no bonding). [SRC, ADR 0001]

**Firmware.** Momentum `mntm-012` (`e1784e7`), API **87.1**. The FAP is built
against this SDK; a different Momentum needs a rebuild (`LOCAL_HANDOFF.md` §4).
The independent review checked the profile against this exact Momentum + ST
`stm32wb_copro` `133182d` (Cube 1.20.0) + NimBLE-Arduino 2.5.1. [SW]

**The FAP** (`flipper/brucelink/application.fam`): [SRC]

| Field | Value | Note |
|---|---|---|
| `apptype` | `FlipperAppType.EXTERNAL` | loadable `.fap`, not built into firmware |
| `fap_category` | `Bluetooth` | where it lands in the Momentum apps menu |
| `entry_point` | `brucelink_app` | |
| `fap_version` | `0.7` | current |
| `stack_size` | `8 * 1024` (8 KB) | see below |
| `sources` | `["*.c"]` | fbt globs recursively; `core/*.c` are generated one-line wrappers that `#include` the shared core so device and host build identical protocol code — no symlinks (break Windows checkouts), regen with `tools/gen_wrappers.py` |

**Why 8 KB of stack.** The X25519 handshake keeps ~1.4 KB of field elements on
the stack and runs *under* the GUI ViewDispatcher frames. A 4 KB stack
overflowed: **Connect crashed the Flipper with a BusFault** on the first
hardware run (2026-11-23, build `f100604`, PR #1 as merged). Fixed by raising the
stack in `2410767`; `0d8d272` then ran Connect with no crash. [HW] The comment in
`application.fam` records this. Do not lower the stack.

**Key storage.** The ADR 0005 session pairing key is written to a file under the
app's data dir (`apps_data/brucelink/pairing.key`), encrypted with the Flipper's
**device-unique secure-enclave key**, so it decrypts only on the Flipper that
wrote it (copying it to another Flipper is detected and refused —
`HARDWARE_TESTING.md` step 4b.8). Separately, BruceLink points the BLE-bond key
store at `.brucelink.keys` (`bt_keys_storage_set_storage_path`) so its bonds
never touch the phone bond. The key is **never** in `bruce.conf`, on LittleFS, in
logs or on screen (the key id is fine). [SRC, ADR 0001/0005; security rules in
`CLAUDE.md`]

## M5StickS3

**Role.** BLE central + session responder. It scans for the Flipper's service,
connects (no BLE pairing), subscribes to TX, and answers the handshake with the
shared protocol core. The service UUID is public, so a BLE connection is never
trusted on its own: the bridge must reach an authenticated session within 10 s
(`kAuthTimeoutMs`) or the peer is dropped and put on a 30 s cooldown
(`PeerCooldown`). [SRC, ADR 0001]

**SoC.** ESP32-S3 with 8 MB PSRAM. **Single 2.4 GHz radio; no 5 GHz** — every
channel list is 1–11/1–12 (`sniffer.h:36-38`, non-C5 branch;
`NATIVE_BRUCE_INVENTORY.md` §0). The C5-only Dual-Band Analyzer is compiled out
on this board. [SRC]

**Board.** 2-button board (`HAS_2_BUTTONS`) with a TFT display. The on-stick job
screen ("Any button: stop") relies on there being physical buttons. [SRC]

**`LITE_VERSION` is NOT defined** for `m5stack-sticks3` (it lives in
`[env_light]`, which this board does not inherit). All non-LITE Bruce features
compile in — the StickS3 is a full-feature Bruce build, which is why its RAM is
tight. [SRC, `NATIVE_BRUCE_INVENTORY.md` §0]

**Internal-RAM budget.** This is the governing constraint. At Bruce's main menu
about **133 kB** of internal RAM is free [HW: 133,247 / 133,431 / 135,083 B
across runs]. The BLE link costs about **72 kB** of it: ~57.5 kB to start and
~14 kB more once the Flipper links [HW, see table below]. PSRAM does **not**
help the radio stacks: this Bruce build's framework allocates NimBLE and Wi-Fi
from internal RAM, and Bruce ships the framework libraries prebuilt, so that is
not BruceLink's to reconfigure (`docs/decisions/0004`). Only one heap region
(~170 kB at boot, before Bruce's own allocations) can hold BLE's required 48 kB
block; ~98 kB of the "free" figure is in smaller regions [MAP]. See
`MEMORY_OPTIMIZATION.md` for how the link was made to fit.

**One radio at a time (ADR 0006).** Wi-Fi + BLE do not fit together: with NimBLE
up, a full `esp_wifi_init` ran out of internal RAM part-way, could not be torn
down, and left the BLE stack unable to send [HW, 2026-11-23]. So Wi-Fi tools run
as **BLE-off restart jobs**, never next to the link. Never start Wi-Fi from the
bridge task.

**Key storage.** The pairing key is kept in **NVS** (`BruceLinkKeyStore`), never
in `bruce.conf` (Bruce prints it to serial on save) or on LittleFS (Bruce's file
browser and web UI expose it). The key survives a reboot in NVS — confirmed on
hardware: after pairing and a reboot, Connect works with the paired key [HW].
Flashing the merged image erases NVS → re-pair (see Flashing below). [SRC/HW]

## Radio constraints

**Wi-Fi and BLE cannot coexist** on the StickS3 (above, ADR 0006). **Every**
Bruce Wi-Fi capability brings up the radio — even the passive diagnostics need
promiscuous/STA mode; there is no Wi-Fi capability that avoids the radio
(`INTEGRATION_INTERFACES.md` §3, verified by `esp_wifi_init`/`WiFi.mode` usage in
each module). Therefore every Wi-Fi capability is **radio-exclusive ⇒ must run as
a `BL_CAP_F_JOB` job** (scan, sniffer/handshake, deauth, beacon spam, evil
portal, karma, brucegotchi, netcut, channel analyzer, jam detect, AP info, scan
hosts, wardriving, webui, TCP/SSH, …).

**What *can* run live over BLE** (radio stays off): menu navigation/observation
(`optionsJSON`, `nav`, `options`, `display *`), `settings` read/write, and
`storage` file reads (read back pcaps/results). That is the live supervision +
result-retrieval surface. [`INTEGRATION_INTERFACES.md` §3]

**Measured RAM costs** (owner serial logs, `docs/TEST_RESULTS.md`, builds
`ee58380`/`2b604e4` after the BLE memory savings) [HW]:

| Point | Internal free | Largest DMA block |
|---|---|---|
| Main menu (before BLE) | ~133,247 B | 59,380 B |
| BLE up, scanning for Flipper | ~75,707 B | 31,732 B |
| Flipper linked | ~61,735 B | 31,732 B |
| Wi-Fi scan, first, lowest point | 25,911 B | 16,372 B |
| Wi-Fi scan, repeat, lowest point | 25,319–25,879 B | 16,372–17,396 B |
| Steady state, linked | **55,059 B** | 31,732 B |

Derived [HW]: **BLE start ≈ 57.5 kB** (133,247→75,707), **+ linked ≈ 14 kB**
(75,707→61,735), **scan peak cost ≈ 35.7 kB first / ≈ 29.7 kB repeat**, scan
4.8 s with BLE active. Steady-state 55,059 B free sits against the 53,248-byte
(52 kB) scan guard threshold → **≈ 1.8 kB headroom** (ADR 0004): about 1.8 kB
more internal use anywhere in Bruce would make scans refuse (cleanly). The scan
guard and transient Wi-Fi start of ADR 0004 are superseded by ADR 0006 (jobs);
the measurements remain the basis for the budget.

**Re-enable limit** [HW]: after Bruce's own Wi-Fi has been used, the largest DMA
block stays ~32 kB and Bruce's BLE guard (`radioHasMemForBle`, wants a 48 kB
block) refuses to start BLE again, so BruceLink cannot be re-enabled until
reboot. The module offers "restart into BruceLink" in that case (ADR 0006).

## Storage

- **SD vs LittleFS.** Capture tools write to SD when present, else LittleFS. The
  handshake-capture job writes Bruce's own files:
  `/BrucePCAP/handshakes/HS_<MAC>[_<SSID>].pcap`, one append-and-close per write
  (so a restart after the job loses nothing written). [SRC, ADR 0006 amendment;
  `NATIVE_BRUCE_INVENTORY.md` §3]
- **Full raw pcap is SD-gated** on a bare StickS3 (no SD): refused by design
  (`LOCAL_HANDOFF.md` §6). Handshakes-only capture still works to LittleFS.
- Capture files cross the BLE link **unencrypted** (M4.6 not done) — capture
  only networks you own.
- Pairing key storage is NVS, not SD/LittleFS (above).

## Flashing & partitions

**Partition layout** (verified byte-identical across delivered merged images in
the cloud) [SW]: bootloader at `0x0`, partition table at `0x8000`, **factory app
at `0x10000`**, factory app partition size **`0x4E0000`**. The app is ~0x48DF40
and fits the factory partition (~93 %).

**Flash the merged image.** The build delivers a merged
`Bruce-1.17-BruceLink-<commit>-m5stack-sticks3-FULL.bin` containing all three
regions. Flash it as a single blob at `0x0`. This **boots** (owner-reported,
`59268b0` / `b635b03` / `9f37f46` merged images) [HW].

**Non-merged flashes did NOT boot** (cause unknown; see
`TROUBLESHOOTING_HISTORY.md`) [HW]:
- the app-only `…-app-only.bin` (`firmware.bin`) flashed alone — `b635b03`,
  2026-11-23. The delivered app-only file is byte-identical to the app inside
  the merged image, so the build is fine; the failure is in how/where it was
  written.
- the three regions flashed **separately** at `0x0` / `0x8000` / `0x10000`
  (bootloader / partitions / app) — `9f37f46`, 2026-11-23.

So the merged image at `0x0` is the only verified-booting method. The docs no
longer recommend `firmware.bin` alone (`LOCAL_HANDOFF.md` §6,
`HARDWARE_TESTING.md` §4b).

**Flashing the merged image erases NVS**, and the pairing key lives in NVS, so
**you must re-pair after every StickS3 update**. Back up full flash before any
flash (`scripts/hil/backup_sticks3.sh`). A method that keeps NVS would be the
separate-parts flash — but that did not boot, so it is not available until the
boot failure is understood. [HW/SRC]

## Measured-vs-computed summary & GAPS

- The RAM figures in "Radio constraints" are **[HW]** (owner serial logs). The
  ~133 kB / 72 kB / 57.5 kB / 14 kB / 35.7 kB / 29.7 kB / 1.8 kB values are all
  owner-measured, not reproduced by the cloud session.
- The partition offsets/sizes and app-vs-merged byte-identity are **[SW]**
  (checked in the cloud), not re-flashed by this session.
- **GAP:** the cost of a Wi-Fi *start* on this build was only ever captured as a
  peak-cost delta (35.7 kB), not as an isolated init figure with a clean reboot
  between measurements (ADR 0004 "Test B" was run without a reboot).
- **GAP:** why the app-only and separate-parts flashes do not boot is unknown;
  no emulator boot test was run. Details in `TROUBLESHOOTING_HISTORY.md`.
- **GAP:** the Flipper's exact Momentum version on the owner's device was not
  confirmed in the hardware runs (`docs/TEST_RESULTS.md`).
- **GAP:** StickS3 board revision is not recorded (to be captured by
  `scripts/hil/firmware_versions.sh`).
