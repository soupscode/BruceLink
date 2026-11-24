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

## 3. Flipper app on device

1. `cd flipper/brucelink && ufbt launch` (installs + runs over USB).
2. Unpaired, the menu starts with **Pair**. **Connect** must say
   `Not paired with a StickS3. Use Pair first.`; **Device Info** must say
   `not connected`.
3. Back out; confirm the app exits cleanly (no hang, no leak warning).

The app is BLE-only since v0.3. Until v0.2 it also had a loopback transport (a
mock StickS3 inside the app, with the public demo key); that is how the UI was
first run on a Flipper (2026-11-23). It is in git history (`5fdc650`).

### 3b. Flipper BLE profile alone (no StickS3 needed)

Checks the part of the BLE transport that needs only a Flipper and a phone.
Passed on v0.2 (2026-11-23, `TEST_RESULTS.md`). Since v0.3 **Connect** needs a
pairing, so run it after step 4b, or use **Pair** for steps 1–3 (it advertises
the same service; a subscribed scanner then receives a `PAIR_COMMIT`, and the
5-second drop in step 4 does not apply: pairing waits up to 90 s).
1. **Connect**. Expect `BLE advertising` /
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
2. Get a Bruce `1.17` checkout (`git checkout 2b6ba4d`, `git submodule update
   --init`) and run `python3 tools/install_into_bruce.py <that checkout>` from
   the BruceLink repo. Build env `m5stack-sticks3` (`pio run -e m5stack-sticks3`)
   and flash (`-t upload`). See `esp32/src/modules/brucelink/PATCH.md` for what
   the installer changes and what this build can and cannot do.
3. On the StickS3, **BruceLink** is the first item of the main menu (before
   Wi-Fi), with a two-device icon whose dots are filled while it is on. Open
   it: the title line says `BruceLink OFF | not paired`. **Turn on** must say
   `BruceLink: not paired. Pair with Flipper first` (pair in step 4b).
   Once paired, **Turn on** says `BruceLink ON > looking for Flipper (not
   encrypted)` and the title shows `BruceLink ON | <Flipper name> XXXX`.
   - `BLE in use`: turn off Bruce's BLE API (or whichever BLE feature is on).
   - Turn it off and on a few times; no crash, no reboot.
   - Config → Advanced must no longer have BruceLink entries (an older install
     put them there; the installer removes them).
4. With BruceLink on, check the rest of Bruce still works (menus, a Wi-Fi scan
   from Bruce's own menu). Starting another Bruce BLE feature while BruceLink
   is on is expected to misbehave: turn BruceLink off first.

## 4a. RAM trims (`PATCH.md`)

The StickS3 build drops USB HID, USB mass storage and mDNS, runs the Wi-Fi
cracker from flash and keeps NetCut's and the RF decoder's tables in PSRAM.
With BruceLink off:

1. On the USB serial log, note `internal` and `dma-blk` from the first
   `[BruceLink]` line after boot, after **Turn on** (`module: BLE up`) and
   after the Flipper links (`Flipper linked`). Compare with the same lines
   from an older build if you have them.
2. Still working: a BLE keyboard (BadBLE), the WebUI (reach it by the IP on
   the screen; `bruce.local` is gone), NetCut (device list fills), an RF
   decode if you have a source, the cracker (slower).
3. Gone, as intended: Files → Mass Storage, USB BadUSB / clicker / U2F.

## 4b. Pairing (ADR 0005)

Needs the step 4 build on the StickS3 and the Flipper app. Flash the merged
image at `0x0`. It erases NVS and with it the pairing, so pair again after
every StickS3 update. Do not flash the parts separately (`firmware.bin` alone,
or bootloader / partitions / firmware at `0x0` / `0x8000` / `0x10000`): both
were tried on 2026-11-23 and neither booted (`TEST_RESULTS.md`; cause unknown).
If you want to find out why, that is a separate experiment: keep the merged
image at hand to recover.

1. Flipper: **Pair**. Expect the instructions screen; the Flipper advertises.
2. StickS3: **BruceLink → Pair with Flipper**.
   Within a few seconds both show a 6-digit code; the Flipper also shows the
   StickS3's name and vibrates.
3. Check the codes match. Answer **Yes** on both, in either order.
4. Expect `Paired with <name>, key XXXX` on both, with the **same key id**. The
   StickS3 then says `BruceLink ON > looking for Flipper` (it switches to
   normal mode without restarting BLE; serial: `module: pairing mode -> normal,
   BLE kept up`); on the Flipper press Back, then **Connect**: `connected`
   within a few seconds, status `Key XXXX, not encrypted`.
   - If a start ever says `Not enough memory for BLE: largest block N kB,
     needs 48`, note N and the `[RAM]` / `[BruceLink]` serial lines, then
     reboot and **Turn on**.
5. Refusal: pair again and answer **No** on one side. Both must report it and
   keep the old pairing (Connect still works with it).
6. Wrong peer: on a StickS3 that is not in pairing mode, the Flipper's Pair
   screen just keeps waiting; nothing is stored.
7. Persistence: reboot both; **Connect** works without pairing again. Then
   **Forget pairing** on the Flipper: Connect says `Not paired` and **Pair**
   moves to the top of the menu. **Forget pairing** on the StickS3 too:
   **Turn on** says `not paired`.
8. Copy `apps_data/brucelink/pairing.key` to another Flipper (if you have
   one): it must report the pairing as from another Flipper, not use it.
9. Record the StickS3 serial lines `[BruceLink] module: ... stack-left N`
   during pairing: the task stack was cut to 10 kB on the strength of 4.3 kB
   measured use.

## 5. BLE link (implements ADR 0001)

Needs the StickS3 build from step 4 with BruceLink turned on, and the Flipper
app on **Connect**, both paired (step 4b). Order does not
matter: the StickS3 keeps scanning, the Flipper keeps advertising.
- Verify advertising of the 128-bit service + name `BL`. If nothing shows up,
  look for `set_discoverable failed` in the Flipper log (`ufbt cli` → `log`).
- Verify the StickS3 central discovers, connects (no pairing), subscribes, and
  that the Flipper shows `BLE linked` then `connected` within a few seconds
  (about 3 s measured; the Flipper gives up at 5 s, the StickS3 at 10 s).
- Device Info / Capabilities over the air; `STATUS` shows `demo:0`. Wi-Fi Scan
  is a job: step 5b.
- Flipper-side recovery: power the StickS3 off mid-handshake or mid-request.
  Expect `no reply (timed out)` after 10 s for a request, `link lost` when the
  link drops, and a fresh handshake when the StickS3 returns.
- **Counters.** The Flipper's Connection screen (press Connect while linked;
  it no longer restarts the link) and the `no reply (timed out)` message show
  `link N mtu M rx chunks/frames`, `drop`, `bad`, `txf`, and, if any occurred,
  `frag` (writes that arrived in pieces) and `odd` (pieces that could not be
  joined, with the raw Offset field and length of the last one). Note them
  when something fails. `mtu 0` means the MTU event never reached the Flipper
  (it then sends 20-byte chunks).
- **Measure** (the timeouts are estimates): time from `BLE linked` to
  `connected` (two X25519 operations on the Flipper; budget 5 s).
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

## 5b. Jobs: Wi-Fi scan with BLE off (ADR 0006)

Paired and connected (step 4b). Keep the StickS3's USB serial open: each step
logs `[BruceLink] job: ...` with the memory figures.

1. Flipper: **Wi-Fi Scan**. Expect `Running on the StickS3...` with a seconds
   counter. The StickS3 restarts, shows **BruceLink job / Wi-Fi scan for the
   Flipper / Any button: stop**, scans, shows `done / Restarting...`, restarts
   again and turns BruceLink on by itself (main-menu icon dots filled).
2. The Flipper reconnects, shows `StickS3 is back. Fetching the result...`,
   then the networks (strongest first) and vibrates. The result starts with
   `Took N s` (Flipper, whole cycle) and `n networks (scan X ms, boot Y ms)`
   (stick: the scan, and Bruce's boot before the job). **Note all three.**
   Neither restart should show Bruce's boot animation, and the stick should
   not join Wi-Fi on the boot after the job even if "Wi-Fi at startup" is on;
   a normal reboot afterwards must play it and join as usual.
3. Compare with Bruce's own scan on the StickS3: same networks, roughly.
4. **Stop on the stick:** start a scan and press a StickS3 button while it
   shows "Any button: stop". The Flipper must report `stopped on the StickS3`.
5. **Crash safety** (optional): during the job boot, press the StickS3's reset
   (or pull USB power on a stick without battery). After it comes back with
   BruceLink on, the Flipper must report `interrupted ... not run again`, and
   the stick must not start the scan again. With power fully cut (RTC lost) the
   Flipper reports that the StickS3 no longer has the job, or gives up at 80 s.
6. A normal reboot afterwards must leave BruceLink **off** (auto-start is once,
   after a job only).
7. Back out of the Wi-Fi Scan screen while a job runs, then open Wi-Fi Scan
   again: it must show the same job, not start another. Back out again and
   wait until it has finished: opening Wi-Fi Scan must show that result once;
   the time after, it starts a new scan.
8. If "Wi-Fi at startup" is on in Bruce: check the job still runs and BruceLink
   comes back (Bruce's BLE memory check may disconnect Wi-Fi to make room).

## 6. Protocol functions over BLE

- PING/PONG, DEVICE_INFO, CAPS, STATUS round-trips.
- ESTOP from the Flipper → STATUS shows `estop:1`.
- Link loss → bridge e-stops and stops any capture (`onDisconnect`).

## 7. Handshake capture as a job (passive; an AP and client you own)

Needs both devices on the same build (Flipper 0.7) and a paired, connected link.

1. Flipper: **Handshake Capture**: Duration 1 min, Channel = your AP's channel,
   **Start capture**. Expect the job screen as with the Wi-Fi scan; the StickS3
   restarts and shows "Handshake capture" with the channel, `HS` / `EAPOL`
   counts, packets and seconds left.
2. While it runs, reconnect a client to your AP (toggle its Wi-Fi). `EAPOL`
   should rise; `HS` once a full handshake is in.
3. At the end the StickS3 restarts and the Flipper shows the counts and the new
   file, e.g. `HS_<MAC>_<SSID>.pcap 1234 B new`. Report the `Took` figure.
4. Again with Channel **Hop** and 2 min: counts are lower per channel; note them.
5. Press a button on the stick mid-capture: outcome `stopped on the stick`,
   with what was captured so far.
6. LittleFS full: the job ends early with "LittleFS is full" (or refuses to
   start). Delete files from the Flipper (step 8) to recover.

## 8. Capture files on the Flipper

1. **Captures**: the list shows each file by SSID (or MAC) and size.
2. Pick one, **Download**: progress in bytes, then "Saved to the SD card ...
   CRC-32 checked". On a PC the file is in `apps_data/brucelink/captures/` and
   opens in Wireshark (linktype 105) with the EAPOL frames.
3. Start a larger download and walk out of range (or turn BruceLink off and
   on): it continues where it stopped once the link is back (Connect again if
   needed), and still ends with the CRC check.
4. **Delete** a file (confirm with Yes): it disappears from the list on the next
   **Captures**.
5. Capture and transfer never overlap: the capture runs with BLE off.

## 9. Active-RF gate (authorized lab only)

- With active ops **disabled**: a deauth `CMD_REQ` must return `not_authorized`.
- Not runnable yet: the StickS3 refuses every active op until active jobs
  exist (ADR 0006). When they do: enable active ops over the authenticated
  session; deauth to one specific BSSID succeeds; broadcast is refused.
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
