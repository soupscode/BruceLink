# Changelog

All notable changes to BruceLink. Dates are UTC.

## [Unreleased] — Review fixes (2026-11-23)

From an independent review of the whole repository (four reviewers: protocol
core, Flipper app, StickS3 glue, cross-cutting). Every fix that can run on the
host has a regression test that fails on the old code. **Not yet run on
hardware.** The handshake changed: **update both devices together.**

### Security / protocol
- The HELLO version range is now in the handshake transcript, so a rewritten
  range (a downgrade) fails the confirm tags. Before, only the selected
  version was.
- Request tags (M4.7): header byte 5 carries a non-zero tag that the response
  echoes; the Flipper drops a late reply instead of taking it for the answer
  to the next request.
- A session only seals and opens frames once established, and wipes its keys
  the moment it fails. Only an `AUTH_OK` ends the wait for it (a repeated
  `HELLO_ACK` used to end the handshake).
- Safety gate: every active op needs one specific target (an op flagged as
  needing none used to pass without one); the rate check cannot wrap; the
  unused duration field is gone; the e-stop is cleared only by a new
  authenticated session, not by a bare link; the demo key refuses captures as
  well as active ops and jobs.
- Jobs: an e-stop or a link loss between `JOB_ACCEPTED` and the restart now
  cancels the job (it used to run at the next boot); the Flipper tracker never
  fetches on the starting session even if a caller skips the check.

### StickS3
- Capture and active ops are refused on the live link (they need Wi-Fi; ADR
  0006). They were already unavailable (placeholder hooks).
- Rescans every 5 s while a Flipper on cooldown is in range, so it is found
  again when the cooldown ends (the scan's duplicate filter hid it).
- "Pair again" while BruceLink is on switches to pairing mode with BLE kept
  up (no BLE restart, which can fail for memory).
- Stops cleanly if another Bruce BLE tool releases NimBLE; checks NimBLE,
  queue and client creation; wipes the key from RAM when its task exits;
  "Forget pairing" fails rather than leaving the key live when the task does
  not stop, and also clears the last job's result (as a new pairing does).
- Installer: checks everything before the first write.

### Flipper (FAP 0.6)
- Job tracking survives link changes and timeouts; Back from text screens;
  pairing feedback and key file write (temp file + rename). See the Flipper
  review fixes in `docs/TEST_RESULTS.md`.

### Tests and docs
- Python reference recomputes the session handshake (transcript, confirm
  tags, keys) from the C vectors; the interop suite fails instead of skipping
  when the C tool does not build. RFC 4231 cases 6–7, RFC 5869 A.2, all seven
  X25519 low-order points; header tamper and counter end; a test that the demo
  key never reaches the Flipper app.
- Withdrawn: flashing the StickS3 in separate parts to keep the pairing (it
  did not boot either time it was tried).

## [Unreleased] — One radio at a time (2026-11-23)

Runs on the owner's devices (2026-11-23: the Wi-Fi scan job worked, and was
"much faster" after the speed-ups below).

### Added
- **Jobs** (ADR 0006, task M5.7): a tool that needs the StickS3's radio memory
  runs between two restarts with BLE off, then BruceLink comes back by itself
  and the Flipper fetches the result. Messages `JOB_START` / `JOB_ACCEPTED` /
  `JOB_RESULT_REQ` / `JOB_RESULT` (0x22–0x25), error `use_job` (16),
  capability flag `JOB`.
  - `bl_job` (shared core): the record kept in RTC memory (magic, CRC,
    bounds), the boot transitions (a job is never re-run after a reset), the
    payloads, and the Flipper-side tracker. 9 tests; 7 deliberate bugs caught.
  - Bridge: accepts jobs (refused with the demo key, while e-stopped, and for
    active capabilities; that last refusal had no test until the review fixes
    above), serves results; 4 end-to-end tests across simulated restarts,
    crash and power loss; 7 deliberate bugs caught.
  - StickS3: `BruceLinkJobs` (record, boot hook, Wi-Fi scan job stoppable with
    any button); the installer adds a `bruceLinkBoot()` call at the end of
    Bruce's `setup()`.
  - Flipper (v0.4): Wi-Fi Scan runs as a job, with a seconds counter through
    the restarts and the result fetched on reconnect.

### Changed
- **The Wi-Fi scan is a job**; nothing uses Wi-Fi while the link is up.
- **Faster jobs** (first hardware run was too slow): the job runs before
  Bruce's boot animation (up to 7 s), the boot after a job skips the
  animation, sound and Wi-Fi at startup, the scan uses 120 ms per channel
  instead of up to 300, and fixed delays were cut (800 → 450 ms). The result
  shows the scan and boot times; the Flipper (v0.5) shows the total.

### Removed
- ADR 0004's inline Wi-Fi path (`BruceLinkWifi.cpp`, `wifiStartAllowed`,
  `formatWifiNoMemory`); ADR 0004 is superseded.

## [Unreleased] — Pairing (2026-11-23)

Builds on both devices; host-tested where it can be; **not yet run on hardware**.

### Added
- **Pairing** (ADR 0005, task M5.3): put both devices into pairing mode, compare
  a 6-digit code on both screens, confirm on each. The devices agree on a fresh
  key over X25519 with a commitment, so a man in the middle gets the codes to
  match with probability 1 in a million per attempt.
  - Flipper: **Pair** / **Forget pairing** in the main menu. The key file is
    encrypted with the Flipper's device-unique key (like the stock U2F app).
  - StickS3: **BruceLink → Pair with Flipper**. The key is kept in NVS, as
    Bruce keeps its U2F secrets.
- `bl_pair` (shared core) with 13 host tests, five bridge pairing-mode tests,
  and an independent Python recomputation of every derived value.
- **BruceLink on Bruce's main menu** (first item, before Wi-Fi), with its own
  icon and one submenu: Turn on / off, Pair with Flipper, Forget pairing, and
  the state and key id in the title line. The installer patches
  `main_menu.{h,cpp}` and removes the Config → Advanced entries of an older
  install.

### Changed
- **Real devices no longer use the public demo key.** Connect and turning
  BruceLink on require a pairing.
- **The Flipper app is BLE-only (v0.3).** The loopback transport (a mock
  StickS3 inside the app, with the demo key) and the Transport setting are
  gone; the app contains no demo key. Until paired, **Pair** is the first menu
  item.
- Status screens say "Key XXXX, not encrypted" instead of "DEMO KEY".
- StickS3 bridge task stack 12 kB → 10 kB (measured peak use 4.3 kB): 2 kB more
  internal RAM next to Wi-Fi (ADR 0004 headroom).
- Starting BruceLink clears the StickS3's peer cooldown list.

### Fixed
- Found by the new tests before it shipped: taking the pairing result twice
  on the bridge handed out an all-zero key the second time.

## [Unreleased] — Bruce module and installer (2026-11-23)

Builds and links inside Bruce 1.17 for the StickS3; not flashed or run.

### Added
- `BruceLinkModule`: **Config → Advanced → Toggle BruceLink** in Bruce, modelled
  on Bruce's "Toggle BLE API". A background task owns the bridge and the NimBLE
  central transport. Refuses to start while another Bruce feature holds BLE.
- `tools/install_into_bruce.py`: installs the bridge library, the glue and the
  one menu line into a Bruce 1.17 checkout; idempotent, `--uninstall` reverses.
- Placeholder headless hooks: capture and active ops report "not available"
  until the sniffer hooks exist (M5.2). Device info, Wi-Fi scan, capabilities
  and status work without them.
- `NimBleCentralTransport::end()` and a 5 s bound on connection attempts.

- The Flipper joins RX writes that the BLE controller reports in more than one
  event (`bl_rx_asm_push`). Previously any event with a non-zero Offset field
  was dropped, which would lose a full-size chunk if the controller splits it.
  Not yet confirmed as the cause of the Wi-Fi scan defect.
- Troubleshooting aids after the first hardware run: link counters on the
  Flipper's Connection screen and in the request-timeout message (chunks,
  frames, drops, MTU); the StickS3 reports why a Wi-Fi scan failed and how long
  it took. Pressing Connect while linked now shows the status instead of
  restarting the link.

- **Wi-Fi scan uses a reduced, transient driver start** (ADR 0004): if Wi-Fi
  is not already up, BruceLink initialises the driver with minimal buffers,
  scans, and releases it. **Returned networks on the Flipper over BLE**
  (owner's devices, four scans in a row, no leak): 4.8 s each, peak 35.7 kB
  internal RAM on the first and 29.7 kB after; the link stayed up.
- **Wi-Fi start guard on the StickS3.** Hardware showed Wi-Fi running out of
  internal RAM next to BLE and taking the bridge down with it. Wi-Fi is now
  started only when at least 52 kB internal RAM and a 16 kB block are free;
  otherwise the request is refused with the figures and the link survives
  (confirmed on hardware with the earlier 80 kB threshold).
- BLE scan on the StickS3 no longer stores every advertiser it sees (NimBLE
  keeps them all by default, without limit, in internal RAM), and the chunk
  queue lives in PSRAM. Both free internal RAM, which is what Wi-Fi needs.
- The StickS3's Wi-Fi scan is bounded (120 ms per channel, 7 s overall); the
  module and the scan log memory figures at each step on the serial port;
  Device Info shows internal heap and the largest DMA block; failed GATT
  writes are logged.

### Fixed
- Wi-Fi scan switched the radio to station mode first, as Bruce's own scan
  does (it failed with the radio off).

## [Unreleased] — Flipper BLE profile, timeouts (2026-11-23)

Not run on hardware. See `docs/STATUS.md` for what is host-tested versus
compile-checked.

### Added
- **Flipper BLE transport** (`transport/bl_ble_transport.c`): a FAP-owned GATT
  peripheral profile (one 128-bit service, RX write + TX notify), replacing
  the stub. Link-up is the central's TX subscription; the radio is handed back
  to the firmware's default profile on exit.
- `bl_gatt_evt`: portable parser for the BLE controller events the profile
  needs; host-tested including malformed and fuzzed input.
- `bl_client` (shared core): session-initiator state machine with a 5 s
  handshake timeout and a 10 s request timeout (M3.5). A stalled handshake is
  recovered by dropping the link and re-linking, not by retransmitting; a
  stray or garbled frame during the handshake is ignored.
- The Flipper drops a central that connects but does not subscribe within
  20 s, so it cannot keep the StickS3 out.
- One chunk-size rule for both ends (`bl_ble_chunk_for_mtu`): the StickS3 no
  longer sends chunks larger than the Flipper accepts if a bigger MTU is
  negotiated.
- **Transport** setting (Loopback / BLE); live connection status; "timed
  out", "link lost", "busy" and "wrong pairing key" reporting.
- `bl_uuid.h` and `bl_demo_key.h`: GATT UUIDs, chunk limits and the demo key
  defined once for both devices.
- Host tests driving the Flipper's client against the real bridge core
  through fragmentation (handshake, requests, lost frames, wrong key, link
  loss).

### Changed
- The transport interface now reports a link id that changes on every new
  link; the app re-handshakes on each change and wipes the previous keys.
- **A bridge provisioned with the public demo key refuses all active ops**
  ("enable" and every RF-transmitting capability return `not_authorized`;
  `STATUS` reports `demo:1`). Before this, a session made with the published
  key could enable and run them.
- ADR 0001 amended: the BLE link is unpaired and unencrypted for bring-up.
  Frames stay authenticated and replay-protected; payloads are readable on
  air until M4.6.

## [Unreleased] — first hardware run fixes (2026-11-23)

### Fixed
- **BusFault on Connect** (first run on a Flipper): stack overflow. App stack
  4 KB → 8 KB; protocol polling moved from a FuriTimer callback (1 KB system
  task stack) to the ViewDispatcher tick on the app thread; handshake buffers
  trimmed to 128 bytes.
- **Windows checkouts could not build**: the app reached the shared core
  through a git symlink. Replaced all three repo symlinks with generated
  one-line wrapper sources (`tools/gen_wrappers.py`, checked by
  `tests/test_wrappers.py`). Device builds are byte-identical to before.

## [Unreleased] — PR #1 second review fixes (2026-11-23)

### Fixed
- **Rate-limit bypass**: re-sending "enable active ops" before each op (or
  reconnecting) cleared the transmit window — 320 frames against a 64-frame
  limit. The window now survives enable/disable, e-stop reset and new
  sessions; only elapsed time expires it.
- **Unauthenticated connection hold**: a peripheral advertising the public
  BruceLink UUID could occupy the StickS3's only connection without the key.
  The bridge now requires an authenticated session within 10 s (immediate on a
  forged proof) and the transport drops the peer and skips it for 30 s.
- Found while fixing: the bridge was driven from both the NimBLE host task and
  the application task; a `HELLO` sent right after subscription could arrive
  before `onConnect()`. NimBLE callbacks now only record events and `loop()` is
  the bridge's single owner; the bridge comes up before subscribing.

### Added
- `BruceLinkBridge::needsDisconnect()` (link supervision) and `kAuthTimeoutMs`.
- `PeerCooldown` (`BruceLinkPeerGuard`): wrap-safe per-peer reconnect cooldown.
- 10 regression tests (bridge suite now 128 checks; core 1527).

## [Unreleased] — PR #1 review fixes (2026-11-23)

### Fixed
- **Bridge lost the pairing key on disconnect** (P1): every reconnect failed
  authentication. The provisioned key is now held separately from the session.
- **ESTOP did not block capture start**: a latched e-stop now refuses
  `CAP_START` (and all radio use) for the rest of the session.
- **Capture poll failure left the radio/file open**: the backend is stopped and
  flushed exactly once on failure; later cleanup cannot double-stop.
- **Uninitialised capture manager** in the bridge constructor / no-backend builds.
- **Rate limit did not bound real transmissions** (one frame charged per op):
  ops now get a frame budget passed to Bruce as a hard cap, with refunds;
  backend overrun trips the e-stop.
- **Wi-Fi scan length bookkeeping** (summed `snprintf` results; `SIZE_MAX` at
  zero capacity): replaced with a bounded text builder.
- Found while fixing: broadcast/multicast BSSIDs passed the active-target check;
  SSIDs could inject lines into scan output; the bridge ignored the negotiated
  BLE MTU; capture mode/channel and active-op channel were not range-checked.

### Added
- `bl_textbuf` (shared core): bounded, printf-free text builder.
- `BruceLinkFormat`: sanitized Wi-Fi result line formatting.
- Host test suite for the ESP32 bridge core (`bl_bridge_tests`, 88 checks,
  ASan+UBSan); cppcheck now covers the bridge core.

### Changed
- `BruceBackend::activeOp` and the `bruce_brucelink_deauth` hook take
  `max_frames` and must not exceed it.

## [Foundation] — dev/foundation-v1 (2026-11-23)

### Added
- **Shared protocol core** (`shared/brucelink_core`, C99, allocation-free):
  SHA-256/HMAC/HKDF/X25519; frame codec with truncated-HMAC MAC; mutually
  authenticated session (PSK + ephemeral X25519, forward secrecy, replay
  protection, version negotiation); BLE fragmentation/reassembly; COBS serial
  framing; capability registry; capture session manager; remote-operation
  safety gate; standards-compliant pcap writer + CRC-32.
- **Host test suite** (`tests/c`): 1434 assertions under ASan+UBSan incl. RFC
  known-answer tests and a fault-injecting simulated transport; cppcheck target.
- **Python reference** + C↔Python wire-interop tests (`tools/bl_vectors.c`).
- **Flipper FAP**: native ViewDispatcher UI, transport abstraction, working
  loopback transport (mock StickS3), BLE transport stub. Builds for mntm-012.
- **ESP32-S3 bridge**: portable bridge core + NimBLE 2.5.1 central transport;
  standalone compile-check project. Builds for ESP32-S3.
- **Bruce glue**: `BruceBackendImpl`, headless hook declarations, `PATCH.md`.
- **HIL scripts** (`scripts/hil/`): detect, versions, backup/restore, build,
  log capture.
- Docs: 3 ADRs, capability matrix, protocol v1, architecture, status, test
  results, hardware testing, engineering control center, local handoff.
- CI: host tests + cppcheck + Python interop + FAP build.

### Changed
- Replaced the v0 experimental protocol sketch with wire protocol v1.
- Safety model: authorization at the connection level (no per-function token),
  per the project owner's direction; active ops gated by a session-level enable
  plus link-loss e-stop, rate limit and optional target scope.

### Known limitations
- Nothing verified on physical hardware. BLE transport is a stub.
  `BruceBackendImpl` not yet compiled in the Bruce tree. See `LOCAL_HANDOFF.md`.
