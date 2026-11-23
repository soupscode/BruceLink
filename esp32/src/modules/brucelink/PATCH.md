# BruceLink inside Bruce 1.17

BruceLink is a remote frontend; it adds the **minimum** new Bruce code and reuses
everything else. Target: a Bruce `1.17` (`2b6ba4d`) checkout, env `m5stack-sticks3`.
Status of each part (built / run on hardware) is in `docs/STATUS.md`.

## Installing (what exists today)

```
python3 tools/install_into_bruce.py /path/to/bruce-firmware     # from the BruceLink repo
cd /path/to/bruce-firmware && pio run -e m5stack-sticks3
```

The installer (idempotent; `--uninstall` reverses it; host-tested) does three things:

- copies `esp32/lib/BruceLink/` + `shared/brucelink_core/src/` to Bruce's
  `lib/BruceLink/` (bridge core, NimBLE central transport, shared protocol core).
  NimBLE-Arduino 2.5.1 is already a Bruce dependency.
- copies this directory's sources to Bruce's `src/modules/brucelink/`
  (Bruce's `build_src_filter` compiles `src/` automatically):
  - `BruceLinkModule.{h,cpp}` — start/stop, pairing screen; a task that owns the
    bridge and transport.
  - `BruceLinkKeyStore.{h,cpp}` — the pairing key in NVS.
  - `BruceBackendImpl.{h,cpp}` — the `BruceBackend`: calls Bruce's own functions.
  - `BruceLinkWifi.{h,cpp}` — Wi-Fi scan next to the live BLE link: a reduced,
    transient driver start, because Bruce's full start does not fit (ADR 0004).
  - `bruce_brucelink_hooks.h` — the headless hooks BruceLink needs from Bruce.
  - `bruce_brucelink_hooks_unavailable.cpp` — placeholder hook bodies (below).
- adds **two lines** (plus an include) to `src/core/menu_items/ConfigMenu.cpp`:
  **Config → Advanced → Toggle BruceLink** and **BruceLink pairing**, next to
  Bruce's own "Toggle BLE API", which the toggle is modelled on.

That is the whole change to Bruce so far: no existing Bruce function is modified.

### What this build does and does not do

- Works without further Bruce changes: device info, Wi-Fi scan, capabilities, status.
- **Capture and active operations report "not available".** They need the sniffer
  hooks in the next section, which are not written yet;
  `bruce_brucelink_hooks_unavailable.cpp` makes every one of them refuse.
- **Pairing** (ADR 0005): **Config → Advanced → BruceLink pairing**. The key is
  kept in NVS (`BruceLinkKeyStore`), never in `bruce.conf` or on LittleFS. Toggle
  BruceLink refuses to start without one. Flashing the merged image at `0x0`
  erases NVS and the pairing; flash `firmware.bin` at `0x10000` to keep it.
- One BLE stack, one owner: the toggle refuses to start while another Bruce BLE
  feature (BLE API, BadBLE, scanners) has NimBLE initialised, and those features
  will fail or disturb the link if started while BruceLink is on. Turn BruceLink
  off first.
- The Wi-Fi scan runs on the bridge task. If Bruce has Wi-Fi up it scans through
  that; otherwise it starts the driver itself for the duration of the scan and
  releases it. It refuses if one of Bruce's own Wi-Fi tools holds the driver.
- After BruceLink has been switched off it may refuse to start again until
  reboot (`not enough memory for BLE`), especially once Bruce's Wi-Fi was used.

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

3. **Pairing key provisioning.** Store the 32-byte key in `/brucelink.json` on
   LittleFS (NOT `bruce.conf`, which Bruce prints to serial on save —
   `config.cpp:456`). First run generates one with `esp_fill_random` and shows a
   setup code; the Flipper is provisioned with the same key out of band.

4. **(Active ops only) gate call-sites.** `bruce_brucelink_deauth(target, channel,
   max_frames)` calls Bruce's existing `send_raw_frame`/`wsl_bypasser_send_raw_frame`
   (`wifi_atks.cpp:140-147`) and **must stop before exceeding `max_frames`**,
   returning the count actually transmitted. Bruce's helpers emit several frames
   per call (`send_raw_frame` ×3, `sendDeauthToAP` ×12), so the hook counts
   frames rather than calls. The remote safety gate (enable, e-stop, unicast
   target, optional scope, frame budget) is enforced in the bridge *before* this
   is reached; no change to Bruce's transmit code itself.

## Coexistence note

`sniffer_start_headless` brings up Wi-Fi promiscuous while the NimBLE link is
held. Bruce configures no `esp_coex` preferences. Measure BLE stability + capture
loss during concurrent operation on hardware; if poor, pause BLE notifications
during capture or add an `esp_coex_preference_set` call (see ADR 0003).

## Flashing

Bruce ships a merged `Bruce-m5stack-sticks3.bin`. Back up full flash first
(`docs/HARDWARE_TESTING.md`). The patched build flashes the same way.
