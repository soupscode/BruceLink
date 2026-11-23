# Changelog

All notable changes to BruceLink. Dates are UTC.

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
  5 s, so it cannot keep the StickS3 out.
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
