# Known Issues, Limitations & Open Questions

> Outstanding bugs, incomplete work, technical debt, compatibility concerns, and
> unresolved questions at `448b798`. "By design" items are boundaries, not bugs.
> Fixes are prioritized in `NEXT_STEPS.md`.

## Incomplete / unverified

- **Capture-file download & delete not hardware-verified.** `bl_files` and the
  Flipper screens are host-tested/built; the end-to-end transfer has not run on
  hardware. (`TESTING.md`)
- **Handshake-capture job not independently reviewed.** The earlier review was
  stopped by a usage safeguard; the capture + file path still needs an
  independent review. (`NEXT_STEPS.md` #6)
- **Review-fix build not yet run on both devices together.** The wire changed
  (transcript + header byte 5); a Flipper and StickS3 on different sides of the
  change will not complete the handshake — update both.

## By design (boundaries, not bugs)

- **Link is authenticated but NOT encrypted** (M4.6). Frames are readable on air.
- **Active-RF ops and jobs are refused** (`CAPABILITY_DISABLED`) until ADR 0006's
  active-job rules exist and are tested. Deauth/evil-portal/karma/brucegotchi/
  netcut/responder/wifite are all refused today.
- **One radio at a time.** No Wi-Fi runs while the BLE link is up; Wi-Fi tools are
  BLE-off jobs only.
- **Demo-key bridge refuses every active op.** The public demo key is never a real
  key on a device.
- **Full raw pcap is SD-gated** on a bare StickS3 (refused without SD).

## Hardware / platform limitations

- **Re-enable without reboot fails after Bruce's own Wi-Fi** (M5.6): once Bruce's
  Wi-Fi has run, BLE may not restart until the stick is rebooted
  (`radioHasMemForBle` wants a 48 kB DMA block). (`TROUBLESHOOTING_HISTORY.md`)
- **Thin steady-state RAM headroom:** ~55,059 B free vs the 52 kB guard threshold
  (~1.8 kB). About 1.8 kB more internal-RAM use anywhere in Bruce would make Wi-Fi
  jobs refuse (cleanly). (`MEMORY_OPTIMIZATION.md`, ADR 0004)
- **Flashing:** the app-only `firmware.bin` did **not** boot (cause unknown); flash
  the merged image as a single file at `0x0` (the three-part separate flash also failed to boot). Merged-image flashing erases NVS →
  re-pair after each update. The Flipper key file only decrypts on the Flipper
  that wrote it.
- **Momentum `Connected` event only fires after pairing**, and there is no BLE
  pairing — the Flipper detects the link from the TX subscription instead.
- **`ble_gatt_characteristic_update` returns true on failure** (inverted) — known
  Momentum quirk, handled in the transport.

## Technical debt (flagged by the 2026-11-24 reassessment)

- **Unused capture/active-op machinery in the shared core:** `bl_capture.{c,h}`,
  `bl_pcap.c` (only its CRC-32 is used), the target allowlist in `bl_safety`, and
  the placeholder hook files `bruce_brucelink_hooks.h` /
  `bruce_brucelink_hooks_unavailable.cpp`. Candidates for removal under ADR 0007
  (not yet removed; removal needs tests).
- **Scan + capture jobs are BruceLink-authored, not Bruce-native launches.** The
  scan job calls the Arduino Wi-Fi API directly; the capture job drives Bruce's
  sniffer with its own hop loop. Convergence to the generic supervised launch is
  pending (ADR 0007).
- **RAM trims edit 5 Bruce source files** (`PATCH.md`) — an upstream-compat cost;
  to become installer-configurable (`NEXT_STEPS.md` #3).
- **Bruce menu order/labels are not a stable API** — the future menu-walk launcher
  must track `optionsJSON` and tolerate upstream changes.
- **Deauth hook is a placeholder.** If/when `bruce_brucelink_deauth(target,
  channel, max_frames)` is implemented it **must count frames, not calls**
  (Bruce's helpers send 3–12 frames per call) and stop at `max_frames` — exceeding
  it trips the e-stop by design.

## Held / deferred

- Explicit zeroing of `num_HS` / `num_EAPOL` / `packet_counter` in `handshakeJob`
  was held so the build under test did not change; may be moot if the capture job
  is re-based on a Bruce-native launch.

## Open questions

- Exact cause of the app-only / three-part flash boot failure (not established).
- Full cost of a Wi-Fi start on this build (only the scan peak is measured).
- Behaviour of the rest of Bruce while BruceLink is on (coexistence with Bruce's
  own BLE tools not checked).
- Job timing figures (`Took` / scan / boot) were reported "much faster" but not
  itemized.
