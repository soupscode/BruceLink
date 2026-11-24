# BruceLink inside Bruce 1.17

BruceLink is a remote frontend; it adds the **minimum** new Bruce code and reuses
everything else. Target: a Bruce `1.17` (`2b6ba4d`) checkout, env `m5stack-sticks3`.
Status of each part (built / run on hardware) is in `docs/STATUS.md`.

## Installing (what exists today)

```
python3 tools/install_into_bruce.py /path/to/bruce-firmware     # from the BruceLink repo
cd /path/to/bruce-firmware && pio run -e m5stack-sticks3
```

The installer (idempotent; `--uninstall` reverses it; host-tested) does five things:

- copies `esp32/lib/BruceLink/` + `shared/brucelink_core/src/` to Bruce's
  `lib/BruceLink/` (bridge core, NimBLE central transport, shared protocol core).
  NimBLE-Arduino 2.5.1 is already a Bruce dependency.
- copies this directory's sources to Bruce's `src/modules/brucelink/`
  (Bruce's `build_src_filter` compiles `src/` automatically):
  - `BruceLinkMenu.{h,cpp}` — the "BruceLink" item on Bruce's main menu (icon
    + submenu).
  - `BruceLinkModule.{h,cpp}` — the submenu (turn on/off, pair, forget),
    pairing screen; a task that owns the bridge and transport.
  - `BruceLinkKeyStore.{h,cpp}` — the pairing key in NVS.
  - `BruceBackendImpl.{h,cpp}` — the `BruceBackend`: calls Bruce's own functions.
  - `BruceLinkJobs.{h,cpp}` — jobs (ADR 0006): the job record in RTC memory,
    the boot hook that runs a job with BLE off or brings the link back after
    one, and the Wi-Fi scan job (Bruce's own `WiFi.mode` + `scanNetworks`).
  - `bruce_brucelink_hooks.h` — the headless hooks BruceLink needs from Bruce.
  - `bruce_brucelink_hooks_unavailable.cpp` — placeholder hook bodies (below).
- adds **BruceLink to Bruce's main menu**, first, before Wi-Fi: an include and
  a member in `src/core/main_menu.h`, one entry in the item list in
  `src/core/main_menu.cpp`, each inside `#if !defined(LITE_VERSION)` like the
  module itself (9 lines, all ending in `// BruceLink`). Bruce's own "hide
  apps" setting can hide it like any other item. An older version of the
  installer put two entries under Config → Advanced; re-running removes them.
- adds two calls to `setup()` in `src/main.cpp`, plus their include (9 lines,
  same guard and marker): `bruceLinkBootEarly()` right after
  `_late_setup_gpio()` (a job runs there, before the boot animation) and
  `bruceLinkBoot()` right before Bruce's startup app (turns BruceLink back on
  after a job, or after "restart into BruceLink"). On a normal boot both do
  nothing.
- adds two build flags to the StickS3 environment in
  `boards/m5stack-sticks3/m5stack-sticks3.ini` (both ending in `; BruceLink`):
  `-I lib/BruceLink` and `-include bl_nimble_psram.h`. The header puts
  NimBLE's host buffers (about 16 kB, allocated at BLE start) in PSRAM instead
  of internal RAM, for BruceLink and for Bruce's own BLE tools. A `-D` flag
  cannot do this: NimBLE-Arduino reads the prebuilt `sdkconfig.h`, which would
  set internal RAM again. Checked in the compile-check object code (the
  allocator asks for PSRAM, caps `0x404`); not yet run on hardware.

That is the whole change to Bruce so far: no existing Bruce function is modified.

### What this build does and does not do

- Works without further Bruce changes: device info, capabilities, status, and
  the Wi-Fi scan as a job.
- **Capture and active operations report "not available".** They need the sniffer
  hooks in the next section, which are not written yet;
  `bruce_brucelink_hooks_unavailable.cpp` makes every one of them refuse.
- **Pairing** (ADR 0005): **BruceLink → Pair with Flipper**. The key is kept in
  NVS (`BruceLinkKeyStore`), never in `bruce.conf` or on LittleFS. **Turn on**
  refuses without one. Updating the StickS3 currently means the merged image
  at `0x0`, which erases NVS and the pairing: pair again after each update.
  Flashing the parts separately (with or without bootloader and partitions)
  would keep NVS but did not boot either time it was tried (2026-11-23, cause
  unknown; `docs/TEST_RESULTS.md`), so it is not recommended until one boots.
- One BLE stack, one owner: **Turn on** refuses while another Bruce BLE
  feature (BLE API, BadBLE, scanners) has NimBLE initialised, and those features
  will fail or disturb the link if started while BruceLink is on. Turn BruceLink
  off first.
- **Jobs (ADR 0006):** nothing uses Wi-Fi while the link is up. A Wi-Fi scan
  from the Flipper restarts the StickS3, scans with BLE off (screen: "BruceLink
  job", any button stops it), restarts again and turns BruceLink back on by
  itself; the Flipper then fetches the result. The job record is in RTC memory:
  a power-off loses it. A reset during a job marks it interrupted; it is never
  re-run. Bruce's boot animation adds to each restart unless "instant boot" is
  on.
- After BruceLink has been switched off it may refuse to start again until
  reboot (`not enough memory for BLE`); the message shows the largest free
  block and the 48 kB BLE needs.

## Remaining upstream edits (not done: task M5.2)

1. **Expose a headless sniffer start/stop/status** (`src/modules/wifi/sniffer.cpp`
   / `sniffer.h`). Today `sniffer_setup()` is a blocking UI loop and
   `closeRawFile()`/`closeDeauthFile()` are `static`. Add (reusing the existing
   backend — `ensureSnifferBackend`, `sniffer_prepare_storage`, `sniffer_set_mode`,
   `sniffer_wait_for_flush`, the `esp_wifi_*` calls, the queue + writer task):
   ```
   bool sniffer_start_headless(SnifferMode mode, uint8_t channel);
   bool sniffer_set_channel_headless(uint8_t channel);
   void sniffer_stop_headless();           // calls the now-exposed close fns
   void sniffer_get_status(BruceSnifferStatus* out);  // packet_counter, num_EAPOL, num_HS, ...
   ```
   and declare `packet_counter`/`beacon_frames` in `sniffer.h` (today file-scope).
   `bruce_brucelink_*` wrap these 1:1.

2. ~~Menu entry~~ — done by the installer (above).

3. ~~Pairing key provisioning~~ — done: pairing (ADR 0005), key in NVS (not
   `bruce.conf`, which Bruce prints to serial on save, and not LittleFS, which
   Bruce's file browser and web UI expose).

4. **(Active ops only) gate call-sites.** `bruce_brucelink_deauth(target, channel,
   max_frames)` calls Bruce's existing `send_raw_frame`/`wsl_bypasser_send_raw_frame`
   (`wifi_atks.cpp:140-147`) and **must stop before exceeding `max_frames`**,
   returning the count actually transmitted. Bruce's helpers emit several frames
   per call (`send_raw_frame` ×3, `sendDeauthToAP` ×12), so the hook counts
   frames rather than calls. The remote safety gate (enable, e-stop, unicast
   target, optional scope, frame budget) is enforced in the bridge *before* this
   is reached; no change to Bruce's transmit code itself.

## Coexistence note

Superseded by ADR 0006: a capture will run as a job, with BLE off, so Wi-Fi
promiscuous never shares the radio memory with the NimBLE link. The hooks
below still apply; they are called from the job boot instead of the bridge
task.

## Flashing

Bruce ships a merged `Bruce-m5stack-sticks3.bin`. Back up full flash first
(`docs/HARDWARE_TESTING.md`). The patched build flashes the same way.
