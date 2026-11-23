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
| Ping/pong, device info, status, errors | 🟡 | host-tested; device info confirmed over the air (owner-reported) |
| Timeouts / cancellation (ESTOP) | 🟡 | ESTOP + link-loss path in bridge; app-side handshake/request timeouts host-tested (`bl_client`), UI path not yet run on a device |
| Serial transport | 📐 | COBS layer done & tested; UART driver not wired |
| BLE transport (Flipper peripheral profile) | 🟡 | implemented; host-tested parser/client; run on hardware 2026-11-23 (owner-reported): against a phone scanner, then linked with the StickS3 — handshake completes, Device Info works |
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
| BruceBackend glue (device info, Wi-Fi scan, capture, active op) | 🟡 builds inside Bruce `1.17`; flashed 2026-11-23, **Device Info and Capabilities work over the air (owner-reported)**; **Wi-Fi scan works over the air (owner-reported, 2026-11-23, four scans, no leak)** with the reduced, transient driver start (ADR 0004): 4.8 s each; guard headroom only 1.8 kB in steady state; capture / active op unavailable (placeholder hooks) |
| Bruce module + menu toggle + installer | 🟡 runs on the StickS3 (owner-reported: link established); off/on cycling and coexistence with other Bruce features not yet checked |
| Headless sniffer hooks (Bruce patch) | 📐 declared; placeholder bodies make capture and active ops report "not available" |
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
| BLE/Wi-Fi coexistence evaluation | 🔴 2026-11-23: with NimBLE up, `esp_wifi_init` fails with `ESP_ERR_NO_MEM` on the StickS3 (135 kB internal heap at the menu; radios do not use PSRAM in this build). 🟡 ADR 0004: 62 kB free with the Flipper linked; a reduced, transient Wi-Fi scan fits (peak 35.7 kB, lowest free 25.9 kB) and returned networks on hardware. Repeated use and the other Wi-Fi features still to do |

## Hardware

Owner-reported on 2026-11-23, in order: the Flipper app over its loopback mock;
the Flipper BLE profile against a phone scanner; and the first real link,
Flipper ↔ StickS3 (Bruce 1.17 + BruceLink), reaching `connected` with a correct
Device Info reply. Details and what was not checked: `docs/TEST_RESULTS.md`.
Everything else (Wi-Fi scan and capabilities over the air, capture, recovery,
throughput, coexistence) is still pending. See `LOCAL_HANDOFF.md`.
