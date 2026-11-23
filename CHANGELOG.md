# Changelog

All notable changes to BruceLink. Dates are UTC.

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
