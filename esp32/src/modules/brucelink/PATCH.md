# Bruce 1.17 patch plan for BruceLink

BruceLink is a remote frontend; it adds the **minimum** new Bruce code and reuses
everything else. This is the complete list of changes to a Bruce `1.17` (`2b6ba4d`)
checkout for the `m5stack-sticks3` env. None of it is verified on hardware yet.

## New files (no upstream edits)

- `src/modules/brucelink/BruceBackendImpl.{h,cpp}` — the BruceBackend (this dir).
- `src/modules/brucelink/bruce_brucelink_hooks.{h,cpp}` — the headless hooks.
- `src/modules/brucelink/BruceLinkModule.{h,cpp}` — owns the
  `NimBleCentralTransport` + `BruceLinkBridge`, started/stopped by a menu toggle.
- `lib/BruceLink/` — the bridge core + NimBLE transport + shared protocol core
  (this repo's `esp32/lib/BruceLink`). Bruce's `build_src_filter` compiles `src/`
  automatically; add `lib/BruceLink` via a `lib_deps`/`lib_extra_dirs` entry or by
  dropping it in Bruce's `lib/`. NimBLE-Arduino 2.5.1 is already a Bruce dep.

## Minimal upstream edits

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

2. **Menu entry** to toggle the BruceLink bridge, next to Bruce's own
   "Toggle BLE API" (`src/core/menu_items/ConfigMenu.cpp:197-201`) or in
   `OthersMenu`. Calls `BruceLinkModule::start()/stop()`.

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
