# LOCAL_HANDOFF — read this first in the local hardware session

Written by the cloud session on 2026-11-23; last updated on branch
`feat/radio-handoff` (jobs, then the fixes from the independent review).
The cloud session had **no hardware**. Everything below separates what was
verified from what was not.

## 1. What is implemented

- **Shared protocol core** (`shared/brucelink_core`, C99): crypto
  (SHA-256/HMAC/HKDF/X25519), frame codec + MAC, authenticated session
  (PSK + ephemeral ECDH, replay protection, version negotiation, request
  tags), pairing (ADR 0005), jobs (ADR 0006), fragmentation, COBS,
  capability registry, capture session manager, remote-operation safety
  gate, pcap writer + CRC-32.
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
  screen, bridge task), `BruceLinkKeyStore` (NVS), `BruceLinkJobs` (the job
  record in RTC memory, the boot hooks, the Wi-Fi scan job),
  `BruceBackendImpl` (capture and active ops refused on the live link), and
  `PATCH.md`. The installer also applies the StickS3 RAM trims (owner's
  choice: no USB HID / mass storage, no mDNS; `PATCH.md`).
  `tools/install_into_bruce.py` puts all of it into a Bruce 1.17 checkout.
- **Python reference** (`shared/brucelink_protocol.py`): wire bytes, the
  session handshake and pairing, recomputed from the C output.
- **HIL scripts** (`scripts/hil/`), docs, ADRs 0001–0006, capability matrix.

## 2. What has been tested (cloud, verified)

| Check | Result |
|---|---|
| Host C core + Flipper event parser, ASan+UBSan | 3483 checks, 0 failed |
| ESP32 bridge core on host (incl. Flipper client ↔ bridge, pairing mode, jobs), ASan+UBSan | 378 checks, 0 failed |
| cppcheck (core C + bridge C++) | clean |
| Python ref + C↔Python interop (frames, handshake, pairing) + demo-key, wrapper and installer checks | 28 tests OK |
| Flipper FAP build (mntm-012), v0.5 | APPCHK pass, Target 7 / API 87.1 |
| ESP32-S3 bridge build (NimBLE 2.5.1) | SUCCESS (RAM 10.2 %, Flash 17.3 %) |
| Bruce 1.17 + BruceLink, `m5stack-sticks3` | SUCCESS (static RAM 133,788 B) |

These are the counts after the review fixes (2026-11-23); every fix that runs
on the host has a test that fails on the old code (mutation-checked).

PR #1 review defects were reproduced on `31629db`, fixed, and regression-tested;
see `docs/TEST_RESULTS.md`.

Full evidence: `docs/TEST_RESULTS.md`.

## 3. What has NOT been tested

Run on hardware so far (owner-reported, `docs/TEST_RESULTS.md`): the Flipper
app (v0.2 to v0.5), the Flipper ↔ StickS3 link (handshake, Device Info,
Capabilities, on/off), pairing on both devices and the paired key after a
reboot, the StickS3 main-menu item, and the Wi-Fi scan as a job (then
faster). **Not yet run:** the review fixes of 2026-11-23 (the handshake
changed: update both devices together), connecting straight after pairing,
Forget, "Pair again" while on, throughput, capture and active ops (refused
until they run as jobs), BLE+Wi-Fi-promiscuous coexistence, pcap transfer.

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
- **One radio at a time** (ADR 0006, owner's decision, 2026-11-23). Wi-Fi and
  BLE do not fit together on the StickS3 (with NimBLE up a full
  `esp_wifi_init` ran out of memory; ADR 0004's reduced scan fitted with
  1.8 kB to spare). Tools now run as **jobs**: the StickS3 restarts, runs the
  tool with BLE off, restarts and turns BruceLink back on; the Flipper fetches
  the result. The Wi-Fi scan cycle works on the owner's devices and was
  "much faster" after the speed-ups (figures not reported). An e-stop or link
  loss before the restart cancels the job (review fix, host-tested).
  `HARDWARE_TESTING.md` step 5b.
- **Handshake takes about 3 s on hardware** against a 5 s limit (measured by
  eye). The request limit (10 s) is still an estimate: Bruce's Wi-Fi scan
  time is unmeasured. If Connect over BLE
  loops between `authenticating...` and `BLE advertising`, measure first.
- **No link encryption.** GATT is open and unpaired at the BLE level; frames
  are authenticated but readable on air (M4.6).
- **Pairing (ADR 0005) ran on hardware** and replaces the demo key on both
  devices: Connect and turning BruceLink on require it.
- Capture and active ops are refused on the live link (they need Wi-Fi; ADR
  0006); handshake capture is next, as a job.
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
  erases it, so pair again after each update. Flashing the parts separately
  (which would keep NVS) did not boot either time it was tried (cause
  unknown), so it is not recommended. The Flipper's key file only decrypts on
  the Flipper that wrote it.
- Requests carry a tag (header byte 5) that the response echoes; the Flipper
  drops a reply whose tag is not the pending request's (M4.7, host-tested).
  Both devices must run the same build: the session transcript changed too.

## 7. Next engineering task

1. Flash the review-fix build on **both** devices (the handshake changed) and
   re-run steps 4b (pairing, then connecting straight after it) and 5b (the
   job; report the `Took` / scan / boot figures).
2. Handshake capture as a job (M5.2), the owner's next pick, then getting the
   file to the Flipper. Then BLE tools as jobs.
3. Measure the RAM trims on the stick (`HARDWARE_TESTING.md` step 4a) and
   check Bruce's remaining features.
4. Link confidentiality (M4.6): AEAD with the session keys.
5. Re-enabling without reboot (M5.6), BLE ownership in Bruce (M5.4).
