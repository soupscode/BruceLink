# Changelog

All notable changes to BruceLink. Dates are UTC.

## [Unreleased] — dev/foundation-v1 (2026-11-23)

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
