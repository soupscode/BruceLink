# BruceLink — verified implementation status

Distinguishes **verified** progress (a build or test actually ran and passed)
from **claimed/designed** work. Updated 2026-11-23. Evidence: `docs/TEST_RESULTS.md`.

Legend: ✅ verified · 🟡 implemented, not verified on hardware · 📐 designed only · ⬜ not started

## Foundation (Milestones 0–3)

| Item | State | Evidence |
|---|---|---|
| Repository audit | ✅ | scaffold inventoried; original 5 Python tests ran green |
| Upstream research + pins | ✅ | Momentum `mntm-012`, Bruce `1.17`, NimBLE 2.5.1 pinned; 3 ADRs |
| BLE architecture decision | ✅ (design) | ADR 0001 — verified against Momentum/Bruce source |
| Reproducible host build + tests | ✅ | `tests/c` Makefile, ASan+UBSan |
| Flipper FAP build (ufbt, mntm-012 SDK) | ✅ | APPCHK pass, Target 7 / API 87.1 |
| ESP32-S3 bridge build (NimBLE 2.5.1) | ✅ | PlatformIO SUCCESS, RAM 10.1 % / Flash 17.1 % |
| ESP32 bridge core host tests | ✅ | 188 checks, ASan+UBSan (`bl_bridge_tests`), incl. the Flipper client driven against the bridge |
| PR #1 review defects, round 1 (6 + 3 found while fixing) | ✅ | reproduced on `31629db`, fixed, regression-tested |
| PR #1 review defects, round 2 (2 + 2 found while fixing) | ✅ bridge / 🟡 transport | rate bypass reproduced on `9f89c4d`; link supervision host-tested; NimBLE transport compile-checked only |
| Crypto (SHA-256/HMAC/HKDF/X25519) | ✅ | RFC/NIST known-answer tests pass |
| Frame codec + MAC | ✅ | round-trip, tamper, malformed tests |
| Authenticated session + replay protection | ✅ | handshake, wrong-PSK, replay, cross-key tests |
| Version negotiation | ✅ | highest-common proto; no-overlap fails closed |
| Fragmentation / reassembly | ✅ | sizes incl. 540 B over MTU 23 & 244; gap detection |
| COBS serial framing | ✅ | round-trip + stream resync after garbage |
| Simulated transport (fault injection) | ✅ | clean bulk, corruption, dropped fragment, duplicate |
| C↔Python wire interop | ✅ | frame+MAC, COBS, pcap, CRC32 byte-identical |
| Ping/pong, device info, status, errors | 🟡 | on loopback + bridge core; not on air |
| Timeouts / cancellation (ESTOP) | 🟡 | ESTOP + link-loss path in bridge; app-side handshake/request timeouts host-tested (`bl_client`), UI path not yet run on a device |
| Serial transport | 📐 | COBS layer done & tested; UART driver not wired |
| BLE transport (Flipper peripheral profile) | 🟡 | implemented; builds against mntm-012 (APPCHK); event parser, client and client↔bridge host-tested; **never run on a radio** |
| Link confidentiality | ⬜ | frames authenticated, not encrypted (ADR 0001 amendment; M4.6) |
| Pairing key | ⬜ | both builds use the public demo key until M5.3 |

## Native application (Milestone 4)

| Item | State |
|---|---|
| Connection screen / menu / settings / about | ✅ runs on a Flipper over loopback (owner-reported, 2026-11-23) |
| Device info, capabilities, Wi-Fi scan views | 🟡 builds; rendered from protocol replies |
| BLE discovery screen | ⬜ |
| Transport choice (Loopback / BLE) in Settings | 🟡 builds; not yet run on a device |
| Responsive on disconnect / recovery | 🟡 live connection status, timeouts, re-handshake on every new link (host-tested logic) |

## Bruce integration (Milestone 5)

| Item | State |
|---|---|
| BruceBackend glue (device info, Wi-Fi scan, capture, active op) | 🟡 written vs audited symbols; **not compiled in Bruce tree** |
| Headless sniffer hooks (Bruce patch) | 📐 declared + patch plan (`PATCH.md`) |
| Pairing-key provisioning | 📐 |

## Phase 2 — capture & research capabilities

| Item | State |
|---|---|
| Capability registry + CAPS | ✅ host-tested |
| Capture session manager | ✅ host-tested (mock backend) |
| PCAP writer (linktype 105) + CRC-32 | ✅ host-tested + interop |
| Generalized command adapter (CMD_REQ) | 🟡 in bridge core |
| Remote safety gate (connection-level) | ✅ host-tested (incl. frame budget, unicast-only targets) |
| Resumable PCAP transfer | 📐 |
| BLE/Wi-Fi coexistence evaluation | 📐 needs hardware |

## Hardware

Only the Flipper app over its loopback (mock) transport has run on a device
(2026-11-23, the build before the BLE transport and the client rewrite). Everything involving a radio or the StickS3 is still pending. See `LOCAL_HANDOFF.md`.
