# LOCAL_HANDOFF — read this first in the local hardware session

Written by the cloud session on 2026-11-23; last updated on branch
`feat/flipper-ble-profile` (Flipper BLE profile + timeouts).
The cloud session had **no hardware**. Everything below separates what was
verified from what was not.

## 1. What is implemented

- **Shared protocol core** (`shared/brucelink_core`, C99): crypto
  (SHA-256/HMAC/HKDF/X25519), frame codec + MAC, authenticated session
  (PSK + ephemeral ECDH, replay protection, version negotiation),
  fragmentation, COBS, capability registry, capture session manager,
  remote-operation safety gate, pcap writer + CRC-32.
- **Flipper FAP** (`flipper/brucelink`): native ViewDispatcher UI (Connect,
  Device Info, Wi-Fi Scan, Capabilities, Settings, About), a transport
  abstraction, a **loopback** transport (mock StickS3 running the real
  protocol) and a **BLE** transport (FAP-owned GATT peripheral profile),
  selectable in Settings. The session, the single outstanding request and
  their timeouts are driven by `bl_client` in the shared core.
- **ESP32 bridge** (`esp32/lib/BruceLink`): portable bridge core + NimBLE
  central transport.
- **Bruce glue** (`esp32/src/modules/brucelink`): `BruceLinkModule` (menu
  toggle + bridge task), `BruceBackendImpl`, headless hook declarations with
  "not available" placeholder bodies, and `PATCH.md`.
  `tools/install_into_bruce.py` puts all of it into a Bruce 1.17 checkout.
- **Python reference** (`shared/brucelink_protocol.py`) for wire interop.
- **HIL scripts** (`scripts/hil/`), docs, 3 ADRs, capability matrix.

## 2. What has been tested (cloud, verified)

| Check | Result |
|---|---|
| Host C core + Flipper event parser, ASan+UBSan | 1775 checks, 0 failed |
| ESP32 bridge core on host (incl. Flipper client ↔ bridge), ASan+UBSan | 188 checks, 0 failed |
| cppcheck (core C + bridge C++) | clean |
| Python ref + C↔Python interop + wrapper check | 13 tests OK |
| Flipper FAP build (mntm-012) | APPCHK pass, Target 7 / API 87.1 |
| ESP32-S3 bridge build (NimBLE 2.5.1) | SUCCESS (RAM 10.1 %, Flash 17.1 %) |
| Bruce 1.17 + BruceLink, `m5stack-sticks3` (dependency sources from GitHub) | SUCCESS, 4.83 MB merged image, not flashed |

PR #1 review defects were reproduced on `31629db`, fixed, and regression-tested;
see `docs/TEST_RESULTS.md`.

Full evidence: `docs/TEST_RESULTS.md`.

## 3. What has NOT been tested

The Flipper app has run on a device over its loopback transport only (first run
found and fixed a stack overflow; `docs/TEST_RESULTS.md`), and that was the
build **before** the BLE transport and the `bl_client` rewrite: the current app
has not run on a device at all. Still untested: the whole Flipper BLE profile
(advertising, subscription, MTU, notify, re-advertising after a drop, handing
the radio back on exit); throughput; `BruceBackendImpl` (never
compiled in the Bruce tree); the Bruce patch; on-air scan/capture/TX; the safety
gate under real transmission; BLE+Wi-Fi coexistence; pcap transfer.

## 4. Required firmware versions

| Component | Version |
|---|---|
| Flipper | Momentum **`mntm-012`** (`e1784e7`), API **87.1** |
| StickS3 | Bruce **`1.17`** (`2b6ba4d`), env `m5stack-sticks3` + BruceLink patch |
| ESP32 BLE lib | NimBLE-Arduino **2.5.1** |
| ESP32 platform | pioarduino 55.03.39 (arduino-esp32 3.3.9 / IDF 5.5.4) |

If the Flipper runs a different Momentum, rebuild the FAP against that SDK.

## 5. Commands to run

```
scripts/hil/build_all.sh                  # rebuild + retest everything
scripts/hil/detect_devices.sh             # find ports
scripts/hil/firmware_versions.sh          # record versions
scripts/hil/backup_sticks3.sh             # BACK UP BEFORE FLASHING
cd flipper/brucelink && ufbt launch       # run app (loopback works w/o StickS3)
```
Then follow `docs/HARDWARE_TESTING.md` steps 3–9.

### Cloud-only toolchain substitutions (redo locally with the real tools)

- ARM: used xPack `arm-none-eabi-gcc` 12.3.1 + `FBT_NOENV=1` because
  `update.flipperzero.one` was blocked. Locally just run `ufbt update` normally.
  (Harmless `DWARF error` link warnings came from the substitute.)
- PlatformIO registry was blocked: `tool-scons` came from PyPI scons 4.11.1 and
  NimBLE from its GitHub tag. Locally the registry works; `esp32/test_compile/
  platformio.ini` already uses the GitHub NimBLE URL (same 2.5.1).

## 6. Known failures / risks

- **The Flipper BLE transport is unproven.** It builds and its portable parts
  are host-tested, but the firmware-facing code (profile start/stop, event
  handler, notify) was written against the Momentum source and never executed.
  Expect to debug it. Start with `HARDWARE_TESTING.md` step 3b (needs only a
  Flipper and a phone).
- **Wi-Fi Scan hangs the StickS3's bridge task** (2026-11-23). Device Info and
  Capabilities work until a scan is requested; afterwards nothing answers and
  toggling BruceLink off misbehaves. The Flipper side is fine (counters show
  no reply data arrived). Starting Wi-Fi next to the live BLE link is the
  suspect; this is the project's main open technical risk, because every
  Wi-Fi feature is meant to be driven over that link. Get the StickS3 serial
  log (`python -m serial.tools.miniterm <port> 115200`): the scan logs each
  step as `[BruceLink] wifi scan: ...` with heap and DMA-block figures.
- **Handshake takes about 3 s on hardware** against a 5 s limit (measured by
  eye). The request limit (10 s) is still an estimate: Bruce's Wi-Fi scan
  time is unmeasured. If Connect over BLE
  loops between `authenticating...` and `BLE advertising`, measure first.
- **No link encryption and a public key.** GATT is open and unpaired; frames
  are authenticated but readable on air (M4.6). Both builds use the demo key
  (M5.3), so the bridge refuses all active ops (`STATUS` shows `demo:1`).
- The Bruce build with BruceLink **compiles and links but has never been
  flashed**. Expect first-run problems in `BruceLinkModule` (task, NimBLE
  ownership, memory) rather than in the protocol.
- Capture and active ops are "not available" in this build (placeholder hooks).
- Momentum's `Connected` event only fires after pairing completes, and there
  is no pairing: the Flipper detects the link from the TX subscription instead.
- `bt_disconnect()` stops advertising for good; the transport restarts it.
- `ble_gatt_characteristic_update` returns **true on failure** (inverted).
- When implementing the Bruce hook `bruce_brucelink_deauth(target, channel,
  max_frames)`: it **must count frames, not calls** (Bruce's helpers send 3–12
  frames per call) and stop at `max_frames`. Exceeding it trips the bridge's
  e-stop by design. Test with `HARDWARE_TESTING.md` step 9.
- BLE + Wi-Fi-promiscuous coexistence is unmanaged upstream (no `esp_coex`).
- Full raw pcap is SD-gated on a bare StickS3 (by design, refused).
- The demo pairing key (`bl_demo_key.h`) is compiled into both devices; real
  key provisioning (`/brucelink.json`) is not implemented.
- A reply that arrives after its request timed out is shown as if it answered
  the next request (no request id yet; M4.7).
- Observation: during the cloud session the capability flag
  `BL_CAP_F_REQUIRES_AUTH` was found renamed to `BL_CAP_F_ACTIVE_RF` in
  `bl_capability.h` and `tests/c/test_capability.c` (semantics unchanged:
  marks RF-transmitting capabilities for the safety gate). The ESP32 bridge was
  updated to match. Flagged in case of concurrent edits.

## 7. Next engineering task

1. The link works on hardware (2026-11-23, owner-reported: `connected`, Device
   Info correct). Finish `HARDWARE_TESTING.md` step 5: Capabilities, Wi-Fi
   Scan, timings, recovery. Then key provisioning (M5.3), which every active
   op waits on, and the headless sniffer hooks (`PATCH.md`, M5.2). The
   Flipper profile has passed the phone-scanner check (`HARDWARE_TESTING.md`
   step 3b, owner-reported; see `docs/TEST_RESULTS.md` for what was and was
   not confirmed).
2. `HARDWARE_TESTING.md` step 5: the full link with the StickS3.
3. Key provisioning (M5.3) — required before any active op can be enabled —
   then link confidentiality (M4.6).
4. Run `HARDWARE_TESTING.md` steps 5–9; update the capability matrix HW column.
