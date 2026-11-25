# Hardware-in-the-loop testing

Procedures for the **local** Claude Code session with both devices on USB.
Every procedure here is **unverified** until run; record results in
`docs/TEST_RESULTS.md` and flip cells in `docs/CAPABILITY_MATRIX.md`.

Scripts: `scripts/hil/`. `hil.py` (Python + pyserial; esptool 5 for the
StickS3 backup) is the verified path **on Windows** (owner's PC, 2026-11-24).
It is written for Linux and macOS too but has not run with the devices there,
in particular its no-reset opening of the StickS3 port, whose line order
differs on those systems. The `.sh` scripts are Linux-only (`udevadm`,
`/dev/ttyACM*`) and unverified; the `.ps1` scripts are Windows-only read-only
helpers. Logs go to `hil-logs/` (git-ignored). `hil.py` finds the devices by
USB ID; pass `--port` to choose one (`check`: `--flipper-port` /
`--sticks3-port`; it only checks devices it detects). The `.sh` scripts read
`STICKS3_PORT` / `FLIPPER_PORT`. Its parsers (Flipper CLI replies, the app's
code digest, the partition table, the app descriptor, `verify-flash` output,
the boot banner, the read-only command allowlist), the backup-folder rule and
the port's DTR/RTS order are host-tested by `tests/test_hil.py`, which needs
no device and no pyserial.

## 0. Safety first

- **Back up both devices before any flash.** Both commands only read, into a
  `<dir>` that must be **outside any git checkout** (`hil.py` refuses one
  inside: an ignored file there is one `git add -f` from a commit, and a
  worktree's ignored files vanish with the worktree):
  - `python scripts/hil/hil.py sticks3-backup <dir>`: the whole flash
    (`esptool read-flash`), then `esptool verify-flash` of the file against
    the chip, the partition table and the app's identity. The StickS3
    **reboots** (each esptool step enters download mode and ends in a reset,
    so Bruce starts between the read and the verify); nothing is written.
  - `python scripts/hil/hil.py flipper-backup <dir>`: the BruceLink app and
    the files directly in `apps_data/brucelink/` (not subfolders; a file name
    with a space is not supported), each checked against the Flipper's own
    MD5. (`qFlipper-cli backup <file>` saves the Flipper's internal memory;
    never run `qFlipper-cli` with no arguments, which starts a firmware
    update.)

  Flashing Bruce's merged image wipes NVS, and with it the pairing. Backups
  hold pairing keys (StickS3 NVS; the Flipper's enclave-encrypted key file):
  never share them.
- **Serial logs can hold secrets.** Bruce prints its whole `bruce.conf` to
  serial at boot, which can include Wi-Fi and web UI credentials. Keep
  `hil-logs/` local; quote only the lines you need.
- Active-RF tests (deauth, beacon, probe) **only** on owned/explicitly
  authorized, isolated lab networks. Keep active ops disabled otherwise.

## 1. Detect hardware & versions

None of these write to either device, and none resets it (verified on
Windows; on Linux/macOS the StickS3 port opening is not yet verified):
```
python scripts/hil/hil.py detect                # ports by VID/PID (Flipper 0483:5740, StickS3 303A:*)
python scripts/hil/hil.py flipper-info          # firmware/API/target, BruceLink files + app MD5 (Flipper CLI)
python scripts/hil/hil.py sticks3-cli info      # Bruce `info` / `free` / `uptime` (read-only, opened without a reset)
python scripts/hil/hil.py sticks3-log --seconds 30   # serial log without resetting the stick
python scripts/hil/hil.py check --fap-ref flipper/brucelink/dist/brucelink.fap --sticks3-cli --log-seconds 5
```
`check` is the repeatable regression check: both devices present, Flipper on
the pinned Momentum / API / target, app installed and a `pairing.key` present
(the StickS3 side of the pairing is not checked), the installed app the
**same code** as a reference build (every ELF section's header and bytes,
so `.bss` sizes count too, but not `.gnu_debuglink`, which only names the
build's debug file), Bruce's CLI answering, and with
`--log-seconds` no ESP32 boot banner in the log (a reset). It writes a JSON
report to `hil-logs/` and exits 1 on any mismatch; an exception (a port that
vanishes, a timeout) also exits non-zero, without a report.

Windows only, PnP/CLI read-only: `scripts/hil/detect_devices.ps1`,
`scripts/hil/flipper_info.ps1`. Linux only (unverified): `detect_devices.sh`,
`firmware_versions.sh` (`esptool flash_id` reboots the stick).

Record: Momentum version string, Bruce commit, StickS3 board revision, ports.
Confirm the Flipper is on `mntm-012` (API 87.1) or rebuild the FAP for its API.
A locally built Bruce reports `Bruce vdev` / `Homebrew` over its CLI, so the
StickS3's build is identified from a flash backup (`sticks3-backup` prints the
partition table and app; compare the app region with a reference image).

## 2. Build everything (also runs in the cloud)

```
scripts/hil/build_all.sh
```
Locally, fetch the pinned Momentum SDK (a plain `ufbt update` fetches the
official firmware's SDK instead), ideally in its own ufbt home:
```
UFBT_HOME=~/.ufbt-brucelink ufbt update --hw-target=f7   --url=https://github.com/Next-Flip/Momentum-Firmware/releases/download/mntm-012/flipper-z-f7-sdk-mntm-012.zip
```
Confirm the FAP still passes APPCHK. On Windows, run PlatformIO from
PowerShell or cmd, not Git Bash: pioarduino's `idf_tools.py` refuses MSYS
(`MSys/Mingw is not supported`) and the Xtensa compiler is then missing.

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

## 7. Capture (passive)

- `CAP_START` mode 0 (handshake) on a known channel of an **owned** AP;
  reconnect a client to produce a 4-way handshake; STATUS counts rise.
- `CAP_STOP`; confirm a `/BrucePCAP/handshakes/HS_*.pcap` exists on LittleFS and
  opens in Wireshark (linktype 105).
- `CAP_START` mode 1 (raw) on a bare StickS3 must be **refused**
  (`no_resource`) — SD-gated upstream.
- Fill LittleFS: STATUS must surface the error and capture must stop.

## 8. Coexistence & transfer

- Not applicable any more: capture runs as a job with BLE off (ADR 0006),
  never while the link is up.
- Transfer a pcap over BLE; verify CRC-32 matches the device-side value; drop
  the link mid-transfer and confirm resume from offset.

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

Restoring the StickS3 **writes its whole flash**, so it needs the owner's
go-ahead:
```
python -m esptool --chip esp32s3 -p <port> write-flash 0x0 <backup>/sticks3-full.bin
python -m esptool --chip esp32s3 -p <port> verify-flash 0x0 <backup>/sticks3-full.bin
```
(`scripts/hil/restore_sticks3.sh <sticks3-full-*.bin>` is the Linux wrapper;
type `YES` to confirm.) For the Flipper, reinstall firmware via qFlipper
if needed; the BruceLink app stores its BLE keys separately
(`.brucelink.keys`), so the phone bond is unaffected.
