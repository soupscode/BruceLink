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
| M4.1 | FAP UI: menu/info/caps/scan/settings | 🟡 | Builds; runs on Flipper via loopback |
| M4.2 | **Flipper BLE peripheral profile** | 🟡 linked with the StickS3 on hardware (owner-reported: `connected`, Device Info OK) | Implemented; builds (APPCHK); event parser + client host-tested. Acceptance: advertise → connect → subscribe → handshake on HW; `HELLO` sent on TX subscription (StickS3 10 s auth deadline) |
| M4.5 | Pin the paired Flipper's BLE address on the StickS3 | ⬜ | After first authenticated session, prefer/only accept that address; unpin from Bruce menu. Closes the address-rotating DoS residual (ADR 0001 §6) |
| M4.3 | Reconnection / connection recovery | 🟡 | Re-handshake on every new link, old keys wiped (host-tested); verify on HW |
| M4.6 | Link confidentiality | ⬜ | Session-layer AEAD (keys already derived); payloads unreadable on air; covers BLE + UART. Until then frames are authenticated but **not encrypted** (ADR 0001 amendment) |
| M4.7 | Match replies to requests | ⬜ | A reply arriving after its request timed out is not shown as the answer to the next one (needs a request id or type check) |
| M4.4 | BLE discovery screen | ⬜ | Lists nearby BruceLink bridges |
| M5.1 | Bridge builds inside Bruce: module, glue, installer | 🟡 builds in Bruce `1.17`; flashed and linked with the Flipper (owner-reported: Device Info, Capabilities, off/on OK); **Wi-Fi scan hangs the bridge on hardware — open, top priority (Wi-Fi next to a live BLE link)** | `tools/install_into_bruce.py` + `pio run -e m5stack-sticks3`; menu toggle starts/stops the bridge on the device; device info + Wi-Fi scan over the air |
| M5.2 | Bruce headless sniffer hooks (patch) | ⬜ | Replace `bruce_brucelink_hooks_unavailable.cpp` with real hooks in Bruce's sniffer; start/stop/status work. Until then capture and active ops report "not available" |
| M5.5 | **Fit Wi-Fi next to BLE on the StickS3** | 🔴 **top priority** | A Wi-Fi scan requested over the BLE link returns networks. Needs: measured internal RAM with BLE up and the cost of Wi-Fi start; then trim (bridge task stack, queue, NimBLE/Wi-Fi configuration) until it fits with margin. Every Wi-Fi feature depends on this |
| M5.4 | BLE ownership inside Bruce | ⬜ | Starting another Bruce BLE feature while BruceLink is on is refused or stops BruceLink cleanly (today only BruceLink's own start checks) |
| M5.3 | Pairing-key provisioning | ⬜ | Key in `/brucelink.json`; setup flow. **Until then both builds use the public demo key (`bl_demo_key.h`): no security against anyone who has the source** |
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
