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
2. In BruceLink: **Settings → Transport: Loopback** (the default), then
   **Connect** → expect `loopback (built-in mock)` / `connected` / `DEMO KEY:
   not secure`, then **Device Info** showing the mock text ("BruceLink mock…").
3. **Capabilities** and **Wi-Fi Scan** should render the mock replies.
4. Back out; confirm the app exits cleanly (no hang, no leak warning).

This validates the UI, tick-driven polling, session handshake and
request/response on real Flipper hardware **without** the StickS3.

### 3b. Flipper BLE profile alone (no StickS3 needed)

Checks the part of the BLE transport that needs only a Flipper and a phone.
1. **Settings → Transport: BLE**, then **Connect**. Expect `BLE advertising` /
   `waiting for StickS3`. The Flipper's own Bluetooth link to the phone app
   drops while BruceLink holds the radio; that is expected.
2. On a phone, open a BLE scanner (nRF Connect or similar). Expect a device
   named **BL** advertising service `a1b20001-7a9c-4b1d-9c2e-5f6a7b8c9d00`.
3. Connect from the scanner. Expect the service with two characteristics:
   `…0002` (write, write without response) and `…0003` (notify).
4. First connect and do nothing: after about 20 s the Flipper should drop
   the scanner (a central that never subscribes) and advertise again. Then
   reconnect and enable notifications on `…0003`. The Flipper should show `BLE linked` /
   `authenticating...` and the scanner should receive a few notifications (the
   `HELLO`, in 20-byte pieces or one larger one). After 5 s with no answer the
   Flipper drops the link by itself and must be **advertising again** (visible
   in the scanner). This is the re-advertise path; note whether it works.
5. Back out of BruceLink. The Flipper's normal Bluetooth (phone app) must work
   again; with Bluetooth switched off in Flipper settings it must stay off.
6. Repeat Connect → Back five times; no crash, no hang.

## 4. StickS3 bridge (Bruce + patch)

1. Back up (step 0).
2. Apply `esp32/src/modules/brucelink/PATCH.md` to a Bruce `1.17` checkout;
   build env `m5stack-sticks3`; flash.
3. `scripts/hil/capture_logs.sh $STICKS3_PORT 115200` and toggle BruceLink in
   the menu. Expect NimBLE init + scanning logs.

## 5. BLE link (implements ADR 0001)

The Flipper transport is implemented but has never run on a radio; do step 3b
first. Both devices use the public demo key until M5.3, so this proves the
link, not its security; the bridge refuses active ops in that state.
- Verify advertising of the 128-bit service + name `BL`. If nothing shows up,
  look for `set_discoverable failed` in the Flipper log (`ufbt cli` → `log`).
- Verify the StickS3 central discovers, connects (no pairing), subscribes, and
  that the Flipper shows `BLE linked` then `connected` within about a second.
- Device Info / Capabilities / Wi-Fi Scan over the air; `STATUS` shows `demo:1`.
- Flipper-side recovery: power the StickS3 off mid-handshake or mid-request.
  Expect `no reply (timed out)` after 10 s for a request, `link lost` when the
  link drops, and a fresh handshake when the StickS3 returns.
- **Measure** (the timeouts are estimates): time from `BLE linked` to
  `connected` (two X25519 operations on the Flipper; budget 5 s), and how long
  Bruce takes to answer a Wi-Fi scan (budget 10 s).
- Questions the source could not settle (from the independent review):
  does the subscription event fire on every reconnect; does the MTU event
  fire on the Flipper with the negotiated value (otherwise chunks stay at
  20 bytes: slow but working); how quickly advertising resumes after a
  Flipper-initiated drop; does remounting the SD card while linked produce
  `BLE taken by system` rather than a crash.
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
wrong Flipper API version (rebuild FAP), Bruce patch not applied, Flipper not
advertising after a dropped link (ADR 0001 amendment), MTU too small (check
negotiated value), DMA starvation dropping Wi-Fi (`radioHasMemForBle`).

## Recovery

`scripts/hil/restore_sticks3.sh <sticks3-full-*.bin>` restores the pre-test
image (type `YES` to confirm). For the Flipper, reinstall firmware via qFlipper
if needed; the BruceLink app stores its BLE keys separately
(`.brucelink.keys`), so the phone bond is unaffected.
