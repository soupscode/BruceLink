# LOCAL_HANDOFF — read this first in the local hardware session

Written by the cloud session on 2026-11-23, branch `dev/foundation-v1`.
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
  abstraction, a working **loopback** transport (mock StickS3 running the real
  protocol), and a **BLE transport stub**.
- **ESP32 bridge** (`esp32/lib/BruceLink`): portable bridge core + NimBLE
  central transport.
- **Bruce glue** (`esp32/src/modules/brucelink`): `BruceBackendImpl`, headless
  hook declarations, and `PATCH.md` (the Bruce changes).
- **Python reference** (`shared/brucelink_protocol.py`) for wire interop.
- **HIL scripts** (`scripts/hil/`), docs, 3 ADRs, capability matrix.

## 2. What has been tested (cloud, verified)

| Check | Result |
|---|---|
| Host C core, ASan+UBSan | 1517 checks, 0 failed |
| ESP32 bridge core on host, ASan+UBSan | 88 checks, 0 failed |
| cppcheck (core C + bridge C++) | clean |
| Python ref + C↔Python interop | 11 tests OK |
| Flipper FAP build (mntm-012) | APPCHK pass, Target 7 / API 87.1 |
| ESP32-S3 bridge build (NimBLE 2.5.1) | SUCCESS (RAM 10.1 %, Flash 17.1 %) |

PR #1 review defects were reproduced on `31629db`, fixed, and regression-tested;
see `docs/TEST_RESULTS.md`.

Full evidence: `docs/TEST_RESULTS.md`.

## 3. What has NOT been tested

**Nothing has run on physical hardware.** Specifically untested: the FAP on a
Flipper; BLE advertising/pairing/MTU/throughput; `BruceBackendImpl` (never
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

- **BLE transport not implemented** on the Flipper (stub returns
  not-connected). This is the top next task.
- `BruceBackendImpl.cpp` has **never been compiled** — expect API mismatches
  against the real Bruce headers; fix against Bruce `1.17`.
- Momentum's `Connected` event only fires after pairing completes — the ESP32
  must initiate Just-Works pairing (ADR 0001).
- `ble_gatt_characteristic_update` returns **true on failure** (inverted).
- When implementing the Bruce hook `bruce_brucelink_deauth(target, channel,
  max_frames)`: it **must count frames, not calls** (Bruce's helpers send 3–12
  frames per call) and stop at `max_frames`. Exceeding it trips the bridge's
  e-stop by design. Test with `HARDWARE_TESTING.md` step 9.
- BLE + Wi-Fi-promiscuous coexistence is unmanaged upstream (no `esp_coex`).
- Full raw pcap is SD-gated on a bare StickS3 (by design, refused).
- Demo pairing key is compiled into the loopback path only; real key
  provisioning (`/brucelink.json`) is not implemented.
- Observation: during the cloud session the capability flag
  `BL_CAP_F_REQUIRES_AUTH` was found renamed to `BL_CAP_F_ACTIVE_RF` in
  `bl_capability.h` and `tests/c/test_capability.c` (semantics unchanged:
  marks RF-transmitting capabilities for the safety gate). The ESP32 bridge was
  updated to match. Flagged in case of concurrent edits.

## 7. Next engineering task

1. **Implement the Flipper BLE peripheral profile** (`transport/bl_ble_transport.c`)
   per ADR 0001; verify advertise → connect → pair → handshake with the StickS3.
2. Compile `BruceBackendImpl` inside Bruce `1.17` and implement the headless
   sniffer hooks (`PATCH.md`).
3. Run `HARDWARE_TESTING.md` steps 5–9; update the capability matrix HW column.
