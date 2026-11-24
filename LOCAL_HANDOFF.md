# LOCAL_HANDOFF — read this first in the local hardware session

Written by the cloud session on 2026-11-23; last updated on branch
`feat/pairing` (pairing, BLE-only Flipper app, BruceLink on Bruce's main menu).
The cloud session had **no hardware**. Everything below separates what was
verified from what was not.

## 1. What is implemented

- **Shared protocol core** (`shared/brucelink_core`, C99): crypto
  (SHA-256/HMAC/HKDF/X25519), frame codec + MAC, authenticated session
  (PSK + ephemeral ECDH, replay protection, version negotiation),
  fragmentation, COBS, capability registry, capture session manager,
  remote-operation safety gate, pcap writer + CRC-32.
- **Flipper FAP** (`flipper/brucelink`): native ViewDispatcher UI (Pair,
  Connect, Device Info, Wi-Fi Scan, Capabilities, Forget pairing, Settings,
  About) over a **BLE** transport (FAP-owned GATT peripheral profile). The
  session, the single outstanding request and their timeouts are driven by
  `bl_client` in the shared core. BLE-only since v0.3: the loopback mock is
  in git history (`5fdc650`).
- **ESP32 bridge** (`esp32/lib/BruceLink`): portable bridge core + NimBLE
  central transport.
- **Bruce glue** (`esp32/src/modules/brucelink`): `BruceLinkMenu` (the
  "BruceLink" item on Bruce's main menu), `BruceLinkModule` (on/off, pairing
  screen, bridge task), `BruceLinkKeyStore` (NVS), `BruceBackendImpl`,
  headless hook declarations with "not available" placeholder bodies, and
  `PATCH.md`.
  `tools/install_into_bruce.py` puts all of it into a Bruce 1.17 checkout.
- **Python reference** (`shared/brucelink_protocol.py`) for wire interop.
- **HIL scripts** (`scripts/hil/`), docs, 3 ADRs, capability matrix.

## 2. What has been tested (cloud, verified)

| Check | Result |
|---|---|
| Host C core + Flipper event parser, ASan+UBSan | 2137 checks, 0 failed |
| ESP32 bridge core on host (incl. Flipper client ↔ bridge, pairing mode), ASan+UBSan | 266 checks, 0 failed |
| cppcheck (core C + bridge C++) | clean |
| Python ref + C↔Python interop (incl. pairing) + wrapper + installer checks | 22 tests OK |
| Flipper FAP build (mntm-012), v0.3 BLE-only | APPCHK pass, Target 7 / API 87.1 |
| ESP32-S3 bridge build (NimBLE 2.5.1) | SUCCESS (RAM 10.2 %, Flash 17.2 %) |
| Bruce 1.17 + BruceLink, `m5stack-sticks3` (dependency sources from GitHub) | SUCCESS, 4.83 MB merged image, not flashed |

PR #1 review defects were reproduced on `31629db`, fixed, and regression-tested;
see `docs/TEST_RESULTS.md`.

Full evidence: `docs/TEST_RESULTS.md`.

## 3. What has NOT been tested

Run on hardware so far (owner-reported, `docs/TEST_RESULTS.md`): the Flipper
app (v0.2, loopback and BLE profile against a phone), the first Flipper ↔
StickS3 link (handshake, Device Info, Capabilities, on/off) and the Wi-Fi scan
over the link. **Not yet run:** pairing (both devices), the BLE-only app
(v0.3), the StickS3 main-menu item, throughput, capture and active ops (no
hooks yet), the safety gate under real transmission, BLE+Wi-Fi-promiscuous
coexistence, pcap transfer.

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
cd flipper/brucelink && ufbt launch       # install + run the app (needs a paired StickS3 to connect)
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
- **Wi-Fi does not fit next to BLE on the StickS3** (2026-11-23, serial log in
  `docs/TEST_RESULTS.md`): with NimBLE up, `esp_wifi_init` fails with
  `ESP_ERR_NO_MEM`, and the failed init used to leave the bridge dead until
  reboot. Wi-Fi is now started only when it should fit (`wifiStartAllowed`;
  the clean refusal is confirmed on hardware), and the scan brings the driver
  up small and only for its own duration (ADR 0004, `BruceLinkWifi.cpp`).
  That scan **returned networks on the Flipper** (2026-11-23): four scans,
  4.8 s each, no leak, peak 35.7 kB on the first and 29.7 kB after. In
  steady state only 1.8 kB separates free RAM from the guard's threshold. This is the project's
  main open technical risk (task M5.5): every Wi-Fi feature is meant to be
  driven over the BLE link. The StickS3 logs memory at each step on USB serial
  (`python -m serial.tools.miniterm <port> 115200`, lines `[BruceLink] ...`).
- **Handshake takes about 3 s on hardware** against a 5 s limit (measured by
  eye). The request limit (10 s) is still an estimate: Bruce's Wi-Fi scan
  time is unmeasured. If Connect over BLE
  loops between `authenticating...` and `BLE advertising`, measure first.
- **No link encryption.** GATT is open and unpaired at the BLE level; frames
  are authenticated but readable on air (M4.6).
- **Pairing (ADR 0005) has not run on hardware.** It replaces the demo key on
  both devices: Connect and turning BruceLink on now require it. There is no
  fallback: if pairing fails on the device, nothing connects until it is fixed
  (the v0.2 app and the previous StickS3 build still work with the demo key).
- Capture and active ops are "not available" in this build (placeholder hooks).
- After switching BruceLink off, it may not start again until the StickS3 is
  rebooted (`not enough memory for BLE`), in particular once Bruce's own Wi-Fi
  has been used (M5.6).
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
- The StickS3 keeps the pairing in NVS; flashing the merged image at `0x0`
  erases it (flash bootloader / partitions / firmware at 0x0 / 0x8000 / 0x10000
  to keep it; not `firmware.bin` alone, which failed to boot once). The Flipper's key
  file only decrypts on the Flipper that wrote it.
- A reply that arrives after its request timed out is shown as if it answered
  the next request (no request id yet; M4.7).
- Observation: during the cloud session the capability flag
  `BL_CAP_F_REQUIRES_AUTH` was found renamed to `BL_CAP_F_ACTIVE_RF` in
  `bl_capability.h` and `tests/c/test_capability.c` (semantics unchanged:
  marks RF-transmitting capabilities for the safety gate). The ESP32 bridge was
  updated to match. Flagged in case of concurrent edits.

## 7. Next engineering task

1. Run pairing on hardware (`HARDWARE_TESTING.md` step 4b), then step 5 with
   the paired key. Fix what breaks.
2. Headless sniffer hooks (`PATCH.md`, M5.2), using the reduced Wi-Fi start
   from ADR 0004 — capture is the next user-visible feature.
3. Link confidentiality (M4.6): AEAD with the session keys.
4. Widen the Wi-Fi memory headroom (ADR 0004), re-enabling without reboot
   (M5.6), BLE ownership in Bruce (M5.4).
