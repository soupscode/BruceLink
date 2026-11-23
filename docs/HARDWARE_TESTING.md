# Hardware-in-the-loop testing

Procedures for the **local** Claude Code session with both devices on USB.
Every procedure here is **unverified** until run; record results in
`docs/TEST_RESULTS.md` and flip cells in `docs/CAPABILITY_MATRIX.md`.

Scripts: `scripts/hil/` (bash + Python/pyserial + esptool). Logs go to
`hil-logs/` (git-ignored). Set `STICKS3_PORT` / `FLIPPER_PORT` to override
auto-detection.

## 0. Safety first

- **Back up the StickS3 before any flash:** `scripts/hil/backup_sticks3.sh`
  (full 8 MB + LittleFS-only). Flashing Bruce's merged image wipes NVS.
- Active-RF tests (deauth, beacon, probe) **only** on owned/explicitly
  authorized, isolated lab networks. Keep active ops disabled otherwise.

## 1. Detect hardware & versions

```
scripts/hil/detect_devices.sh        # finds Flipper (VID 0483) + StickS3 (VID 303a)
scripts/hil/firmware_versions.sh     # esptool flash_id + Flipper `device_info`
```
Record: Momentum version string, Bruce commit, StickS3 board revision, ports.
Confirm the Flipper is on `mntm-012` (API 87.1) or rebuild the FAP for its API.

## 2. Build everything (also runs in the cloud)

```
scripts/hil/build_all.sh
```
Locally, install the real Flipper toolchain (`ufbt update` with network access)
instead of the xPack substitute; confirm the FAP still passes APPCHK.

## 3. Flipper app on device (loopback first)

1. `cd flipper/brucelink && ufbt launch` (installs + runs over USB).
2. In BruceLink: **Connect** → expect status `connected`, then **Device Info**
   showing the mock text ("BruceLink mock…").
3. **Capabilities** and **Wi-Fi Scan** should render the mock replies.
4. Back out; confirm the app exits cleanly (no hang, no leak warning).

This validates the UI, timer pump, session handshake and request/response on
real Flipper hardware **without** the StickS3.

## 4. StickS3 bridge (Bruce + patch)

1. Back up (step 0).
2. Apply `esp32/src/modules/brucelink/PATCH.md` to a Bruce `1.17` checkout;
   build env `m5stack-sticks3`; flash.
3. `scripts/hil/capture_logs.sh $STICKS3_PORT 115200` and toggle BruceLink in
   the menu. Expect NimBLE init + scanning logs.

## 5. BLE link (implements ADR 0001)

Requires the Flipper BLE transport to be implemented (currently a stub).
- Verify advertising of the 128-bit service + name within 31 bytes.
- Verify the StickS3 central discovers, connects, Just-Works pairs, and that
  Flipper's `Connected` status fires.
- Record negotiated MTU (target ≥ 247) and measure notify throughput.
- Send a 540-byte frame to exercise fragmentation across > 245-byte writes.
- Pull power on the StickS3 mid-session: Flipper must show disconnected, and
  (on reconnect) the session must re-handshake (no replay of old keys).
- **Link supervision** (compile-checked only so far): advertise the BruceLink
  service UUID from a second device (e.g. a phone BLE peripheral app or a spare
  ESP32) that never sends `HELLO`. The StickS3 must drop it after ~10 s, skip it
  for 30 s, and connect to the real Flipper meanwhile. Repeat with a Flipper
  holding the wrong pairing key: the drop must be immediate after its `AUTH`.
- Confirm the real Flipper is never dropped by the 10 s deadline in normal use
  (it must send `HELLO` on subscription), and that a `HELLO` sent immediately
  after subscription is not lost (transport ordering fix).

## 6. Protocol functions over BLE

- PING/PONG, DEVICE_INFO, CAPS, STATUS round-trips.
- ESTOP from the Flipper → STATUS shows `estop:1`.
- Link loss → bridge e-stops and stops any capture (`onDisconnect`).

## 7. Capture (passive)

- `CAP_START` mode 0 (handshake) on a known channel of an **owned** AP;
  reconnect a client to produce a 4-way handshake; STATUS counts rise.
- `CAP_STOP`; confirm a `/BrucePCAP/handshakes/HS_*.pcap` exists on LittleFS and
  opens in Wireshark (linktype 105).
- `CAP_START` mode 1 (raw) on a bare StickS3 must be **refused**
  (`no_resource`) — SD-gated upstream.
- Fill LittleFS: STATUS must surface the error and capture must stop.

## 8. Coexistence & transfer

- Run handshake capture **while** the BLE link is up; measure BLE drops and
  capture loss. Decide whether to pause BLE during capture (ADR 0003).
- Transfer a pcap over BLE; verify CRC-32 matches the device-side value; drop
  the link mid-transfer and confirm resume from offset.

## 9. Active-RF gate (authorized lab only)

- With active ops **disabled**: a deauth `CMD_REQ` must return `not_authorized`.
- Enable active ops over the authenticated session; set a one-BSSID scope;
  deauth to that BSSID succeeds, to another is refused (`not_authorized`).
- Exceed the rate limit → `rate_limited`.
- ESTOP mid-op halts it; link loss halts it.

## 10. Failure diagnosis

Collect `hil-logs/` (serial logs, versions, backups' sha256). Common causes:
wrong Flipper API version (rebuild FAP), Bruce patch not applied, NimBLE pairing
refused (try `GapPairingNone` vs numeric comparison), MTU too small (check
negotiated value), DMA starvation dropping Wi-Fi (`radioHasMemForBle`).

## Recovery

`scripts/hil/restore_sticks3.sh <sticks3-full-*.bin>` restores the pre-test
image (type `YES` to confirm). For the Flipper, reinstall firmware via qFlipper
if needed; the BruceLink app stores its BLE keys separately
(`.brucelink.keys`), so the phone bond is unaffected.
