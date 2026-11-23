# Roadmap

Phase 1 (foundation) first; Phase 2 (capture & research) builds on the same core.
Track detail and acceptance criteria in `TASKS.md`; verified state in `STATUS.md`.

## Phase 1 — Foundation

- **M0** Repository audit — ✅
- **M1** Upstream research, pinned revisions, BLE ADR — ✅
- **M2** Reproducible builds (host, FAP, ESP32) + CI — ✅
- **M3** Communication foundation: crypto, framing, authenticated session,
  replay, fragmentation, COBS, simulated transport — ✅ host; ping/status 🟡
- **M4** Native Flipper app + **BLE peripheral profile** — UI 🟡, BLE ⬜ (next)
- **M5** Bruce integration (glue compiled in Bruce tree, headless hooks, key
  provisioning) — ⬜
- **M6** Hardware testing preparation (HIL scripts, procedures, handoff) — 🟡

## Phase 2 — Capture & research capabilities (scope amendment 2026-11-23)

- Capability registry, capture manager, pcap+CRC, safety gate — ✅ host
- Command adapter — 🟡
- Sniffer/EAPOL via Bruce, resumable pcap transfer, transfer-channel decision,
  coexistence evaluation, capture UI, active-RF forwarding — ⬜ (after BLE on HW)

## Milestone priority

The scope amendment did **not** reorder Phase 1. The Flipper BLE peripheral
profile (M4.2) is the critical path: nothing on air can be verified without it.
