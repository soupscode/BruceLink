# BruceLink — verified implementation status

Distinguishes **verified** progress (a build or test actually ran and passed)
from **claimed/designed** work. Updated 2026-11-23. Evidence: `docs/TEST_RESULTS.md`.

Legend: ✅ verified · 🟡 implemented, not verified on hardware · 📐 designed only · ⬜ not started

## Foundation (Milestones 0–3)

| Item | State | Evidence |
|---|---|---|
| Repository audit | ✅ | scaffold inventoried; original 5 Python tests ran green |
| Upstream research + pins | ✅ | Momentum `mntm-012`, Bruce `1.17`, NimBLE 2.5.1 pinned; ADRs 0001–0006 |
| BLE architecture decision | ✅ (design) | ADR 0001 — verified against Momentum/Bruce source |
| Reproducible host build + tests | ✅ | `tests/c` Makefile, ASan+UBSan |
| Flipper FAP build (ufbt, mntm-012 SDK) | ✅ | APPCHK pass, Target 7 / API 87.1 |
| ESP32-S3 bridge build (NimBLE 2.5.1) | ✅ | PlatformIO SUCCESS, RAM 10.1 % / Flash 17.1 % |
| ESP32 bridge core host tests | ✅ | `bl_bridge_tests`, ASan+UBSan, incl. the Flipper client driven against the bridge (counts per run in `TEST_RESULTS.md`) |
| PR #1 review defects, round 1 (6 + 3 found while fixing) | ✅ | reproduced on `31629db`, fixed, regression-tested |
| PR #1 review defects, round 2 (2 + 2 found while fixing) | ✅ bridge / 🟡 transport | rate bypass reproduced on `9f89c4d`; link supervision host-tested; NimBLE transport compile-checked only |
| Crypto (SHA-256/HMAC/HKDF/X25519) | ✅ | RFC/NIST known-answer tests pass |
| Frame codec + MAC | ✅ | round-trip, tamper, malformed tests |
| Authenticated session + replay protection | ✅ | handshake, wrong-PSK, replay, cross-key, header tamper, counter end, downgrade (version range in the transcript), strict states and wipe on failure (review 2026-11-23) |
| Request tags (M4.7) | ✅ host | a late reply is dropped, not taken for the next request's answer; both devices must run the same build |
| Version negotiation | ✅ | highest-common proto; no-overlap fails closed |
| Fragmentation / reassembly | ✅ | sizes incl. 540 B over MTU 23 & 244; gap detection |
| COBS serial framing | ✅ | round-trip + stream resync after garbage |
| Simulated transport (fault injection) | ✅ | clean bulk, corruption, dropped fragment, duplicate |
| C↔Python interop | ✅ | frame+MAC+tag, COBS, pcap, CRC32 byte-identical; session handshake (transcript, confirm tags, keys) and pairing recomputed from the C output; fails, not skips, if the C tool does not build |
| Ping/pong, device info, status, errors | 🟡 | host-tested; device info confirmed over the air (owner-reported) |
| Timeouts / cancellation (ESTOP) | 🟡 | ESTOP + link-loss path in bridge; app-side handshake/request timeouts host-tested (`bl_client`), UI path not yet run on a device |
| Serial transport | 📐 | COBS layer done & tested; UART driver not wired |
| BLE transport (Flipper peripheral profile) | 🟡 | implemented; host-tested parser/client; run on hardware 2026-11-23 (owner-reported): against a phone scanner, then linked with the StickS3 — handshake completes, Device Info works |
| Link confidentiality | ⬜ | frames authenticated, not encrypted (ADR 0001 amendment; M4.6) |
| Pairing (ADR 0005) | 🟡 | core: 13 host tests incl. man in the middle + Python recomputation; bridge pairing mode: 5 host tests. **Ran on hardware (owner-reported, 2026-11-23): pairing completed on both devices; after a StickS3 reboot the paired key connects and Device Info + Wi-Fi scan work.** Connecting straight after pairing failed for lack of memory on the StickS3's BLE restart (fix: switch modes without restarting BLE; built, not yet run). Key-id display, refusal and Forget not yet checked. "Pair again" while on now switches in place (review fix; not yet run). The Flipper app contains no demo key; the StickS3 knows it only to refuse it |

## Native application (Milestone 4)

| Item | State |
|---|---|
| Connection screen / menu / settings / about | ✅ ran on a Flipper (v0.2 to v0.5, owner-reported, 2026-11-23: v0.5 ran the Wi-Fi scan job). The review fixes (job tracking, Back, pairing feedback) build; not yet run |
| Device info, capabilities, Wi-Fi scan views | 🟡 builds; rendered from protocol replies |
| BLE discovery screen | ⬜ |
| Loopback transport (mock StickS3 in the app) | removed in v0.3 on the owner's request; BLE is the only transport (git history: `5fdc650`) |
| Responsive on disconnect / recovery | 🟡 live connection status, timeouts, re-handshake on every new link (host-tested logic) |

## Bruce integration (Milestone 5)

| Item | State |
|---|---|
| BruceBackend glue (device info, Wi-Fi scan, capture, active op) | 🟡 builds inside Bruce `1.17`; **Device Info and Capabilities work over the air (owner-reported)**; the Wi-Fi scan runs as a job (ADR 0006) and worked on hardware (row below); capture and active ops are refused on the live link (they need Wi-Fi; to run as jobs) |
| Jobs: tools between restarts with BLE off (ADR 0006) | 🟡 record + boot transitions + payloads + Flipper tracker host-tested (7 mutations caught); bridge end to end across simulated restarts incl. crash and power loss (7 mutations caught); **the full Wi-Fi scan cycle worked on hardware** (owner-reported, 2026-11-23: RTC record kept, both restarts, auto-start, result fetched); the faster build (no boot animation on job restarts, 120 ms scan, timing in the result) is "much faster" (owner-reported; figures not reported) |
| Bruce module + installer | 🟡 runs on the StickS3 (owner-reported: link established, off/on works); coexistence with other Bruce features not yet checked |
| BruceLink on Bruce's main menu (icon, Turn on/off, Pair, Forget) | 🟡 runs on the StickS3 (owner used it to pair and turn on, 2026-11-23); installer host-tested (incl. upgrade from the Config → Advanced entries, nothing written on a failed check); Forget not yet checked |
| Headless sniffer hooks (Bruce patch) | 📐 declared; placeholder bodies make capture and active ops report "not available" |
| Pairing-key provisioning | 🟡 pairing (ADR 0005) ran on the StickS3; key kept in NVS across a reboot and used for the session (owner-reported, one run). Key-id display, refusal of a wrong key and Forget not yet checked |

## Phase 2 — capture & research capabilities

| Item | State |
|---|---|
| Capability registry + CAPS | ✅ host-tested |
| Capture session manager | ✅ host-tested (mock backend) |
| PCAP writer (linktype 105) + CRC-32 | ✅ host-tested + interop |
| Generalized command adapter (CMD_REQ) | 🟡 in bridge core |
| Remote safety gate (connection-level) | 🟡 host-tested (default-off, one specific target always, frame budget, no wrap, e-stop on link loss and cleared only by a new session, refund after a failed transmit; the review's surviving mutants are now caught). Nothing transmits yet: no active op is wired into Bruce. The target scope exists in the core only (nothing on the wire sets it) |
| Resumable PCAP transfer | 📐 |
| BLE/Wi-Fi coexistence | Avoided by design (ADR 0006, owner's decision): nothing uses Wi-Fi while the link is up. Background: with NimBLE up a full `esp_wifi_init` ran out of memory on the StickS3; ADR 0004's reduced scan fitted with 1.8 kB to spare |

## Hardware

Owner-reported on 2026-11-23, in order: the Flipper app over its loopback mock;
the Flipper BLE profile against a phone scanner; the first real link with a
correct Device Info reply; capabilities and the Wi-Fi scan over the air;
pairing, and the paired key reconnecting after a reboot; the Wi-Fi scan as a
job, then faster. Details: `docs/TEST_RESULTS.md`. Not yet run on hardware: the
review fixes of 2026-11-23 (both devices must be updated together), Forget,
recovery after a failed handshake, throughput, coexistence with Bruce's other
features, capture and active ops (not implemented). Frames are authenticated,
not encrypted. See `LOCAL_HANDOFF.md`.
