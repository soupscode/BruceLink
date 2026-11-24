# BruceLink — engineering backlog

Owner: TPM. Distinguishes verified (✅) from claimed. Acceptance criteria are
measurable; "done" requires the Definition of Done in `CLAUDE.md`.
Status keys: ✅ done+verified · 🟡 done, needs hardware · 🔵 in progress ·
⬜ todo · ⛔ blocked.

## Phase 1 — Foundation

| ID | Task | Status | Acceptance criteria |
|---|---|---|---|
| M0 | Repository audit | ✅ | Inventory + baseline tests run |
| M1 | Upstream research, pins, BLE ADR | ✅ | Revisions pinned; ADR 0001–0003 |
| M2 | Reproducible builds + CI | ✅ | Host/FAP/ESP32 build in CI; real build output |
| M3.1 | Crypto + frame + session + replay | ✅ | RFC KATs; tamper/replay tests |
| M3.2 | Fragmentation + COBS | ✅ | 540 B over MTU 23/244; resync |
| M3.3 | Simulated transport + fault injection | ✅ | bulk/corrupt/drop/dup tests |
| M3.4 | Ping, device ID, status, errors | 🟡 | Works on loopback + bridge; needs air |
| M3.5 | App-side timeouts + handshake recovery | 🟡 | Handshake 5 s → drop link and re-link; request 10 s → slot freed, session stays up; stray handshake frames ignored. Host-tested (`bl_client`, client↔bridge); UI path needs a device run |
| M3.6 | Serial (UART) transport driver | ⬜ | COBS over `furi_hal_serial` + Arduino `Serial`; HIL ping |
| M4.1 | FAP UI: menu/info/caps/scan/settings | 🟡 | v0.2 ran on a Flipper (loopback, then BLE); v0.3 is BLE-only (loopback removed), builds, not yet run |
| M4.2 | **Flipper BLE peripheral profile** | 🟡 linked with the StickS3 on hardware (owner-reported: `connected`, Device Info OK) | Implemented; builds (APPCHK); event parser + client host-tested. Acceptance: advertise → connect → subscribe → handshake on HW; `HELLO` sent on TX subscription (StickS3 10 s auth deadline) |
| M4.5 | Pin the paired Flipper's BLE address on the StickS3 (after M5.3) | ⬜ | After first authenticated session, prefer/only accept that address; unpin from Bruce menu. Closes the address-rotating DoS residual (ADR 0001 §6) |
| M4.3 | Reconnection / connection recovery | 🟡 | Re-handshake on every new link, old keys wiped (host-tested); verify on HW |
| M4.6 | Link confidentiality | ⬜ | Session-layer AEAD (keys already derived); payloads unreadable on air; covers BLE + UART. Until then frames are authenticated but **not encrypted** (ADR 0001 amendment) |
| M4.7 | Match replies to requests | ⬜ | A reply arriving after its request timed out is not shown as the answer to the next one (needs a request id or type check) |
| M4.4 | BLE discovery screen | ⬜ | Lists nearby BruceLink bridges |
| M5.1 | Bridge builds inside Bruce: module, glue, installer | 🟡 builds in Bruce `1.17`; flashed and linked with the Flipper (owner-reported: Device Info, Capabilities, off/on OK); Wi-Fi scan works over the air (ADR 0004) | `tools/install_into_bruce.py` + `pio run -e m5stack-sticks3`; menu toggle starts/stops the bridge on the device; device info + Wi-Fi scan over the air |
| M5.2 | Bruce headless sniffer hooks (patch) | ⬜ | Replace `bruce_brucelink_hooks_unavailable.cpp` with real hooks in Bruce's sniffer; start/stop/status work. Until then capture and active ops report "not available" |
| M5.5 | ~~Fit Wi-Fi next to BLE on the StickS3~~ | superseded by M5.7 | ADR 0004's reduced scan worked on hardware with 1.8 kB to spare; the owner chose one radio at a time instead (ADR 0006) |
| M5.7 | **Jobs: one radio at a time** (ADR 0006) | 🟡 Wi-Fi scan cycle works on hardware and is "much faster" after skipping the boot animation (owner-reported); figures not yet reported | Wi-Fi scan as a job: accept → restart → scan with BLE off → restart → BruceLink back → Flipper fetches. HARDWARE_TESTING step 5b. Next: measure the restart time, then move capture (M5.2) and BLE tools onto jobs; active jobs only with ADR 0006's rules |
| M5.6 | Re-enabling BruceLink without a reboot | ⬜ | After BruceLink off (and Bruce's Wi-Fi used) the largest DMA block stays ~32 kB and Bruce's 48 kB BLE guard refuses. Find what fragments the heap (module now logs memory on stop) |
| M5.4 | BLE ownership inside Bruce | ⬜ | Starting another Bruce BLE feature while BruceLink is on is refused or stops BruceLink cleanly (today only BruceLink's own start checks) |
| M5.3 | Pairing (key agreement with a comparison code) | 🟡 implemented, host-tested; on hardware pairing completed and the paired key connects after a reboot; connecting straight after pairing failed (StickS3 BLE restart out of memory; in-place switch built, not yet run) | ADR 0005. Flipper: Pair / Forget pairing, key encrypted with the device-unique key. StickS3: BruceLink item on Bruce's main menu (Turn on/off, Pair with Flipper, Forget pairing), key in NVS. No demo key in the FAP |
| M6 | HIL infrastructure | 🟡 | Scripts written + syntax-checked; run on HW |

## Phase 2 — Capture & research capability expansion

Incorporated from the scope amendment (2026-11-23). Lower priority than
Phase 1 BLE; does not interrupt foundation work.

| ID | Task | Status | Acceptance criteria |
|---|---|---|---|
| P2.1 | Capability registry + CAPS | ✅ | Serialize/validate; FAP renders menus |
| P2.2 | Capture session manager | ✅ | Lifecycle, gating, failure tests (mock) |
| P2.3 | PCAP writer (linktype 105) + CRC-32 | ✅ | Byte-exact + Python interop |
| P2.4 | Generalized command adapter (CMD_REQ) | 🟡 | Bridge dispatch; add features via registry |
| P2.5 | Remote safety gate (connection-level) | ✅ | default-deny/scope/rate/estop/link-loss tests |
| P2.6 | Sniffer/EAPOL via Bruce (handshake mode) | ⬜ | Handshake pcap on LittleFS on HW |
| P2.7 | Resumable PCAP transfer + integrity | ⬜ | Resume from offset; CRC verified |
| P2.8 | Transfer channel decision (BLE/USB/Wi-Fi) | ⬜ | Throughput + coexistence measured on HW |
| P2.9 | BLE/Wi-Fi coexistence evaluation | ⬜ | Loss/drop numbers recorded |
| P2.10 | Capture UI on Flipper | ⬜ | Start/stop/status/list screens |
| P2.11 | Active-RF forwarding (authorized lab) | 🟡 | Gate enforced; forwards to Bruce TX; HW-verified |

## Blockers

- ⛔ All 🟡/HW items: need the local session with physical devices.
- Toolchain hosts blocked in the cloud sandbox (worked around; see
  `LOCAL_HANDOFF.md`). Not a blocker locally.

## Milestone priority changes from the scope amendment

None to Phase 1 ordering: **the BLE link (M4.2) remains the top priority** —
now implemented and waiting for its first run on hardware. Phase 2 items reuse the shared core already built (registry,
capture manager, pcap, gate are done host-side), so they cost little now and
are sequenced after BLE works on hardware.
