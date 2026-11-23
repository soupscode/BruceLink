# Engineering Control Center

Single-page dashboard of team roles, subsystem health, firmware compatibility,
test coverage, and hardware-dependent verification. Owner: TPM; the Lead
Engineer gates every "verified" mark. Updated 2026-11-23.

## Team and workflow

| Role | Owns | This session |
|---|---|---|
| TPM | `TASKS.md`, milestones, scope | Backlog + phase-2 intake |
| Systems Architect | ADRs, feasibility | Momentum BLE + Bruce audits; ADR 0001–0003 |
| Flipper Engineer | FAP | UI, transport abstraction, loopback |
| ESP32 / Bruce Engineer | bridge, glue | Bridge core, NimBLE transport, Bruce glue + patch |
| Protocol & Security | wire protocol, auth | v1 frame, session, gate |
| QA | tests, simulators | 1434-assertion suite, fault-injection, interop |
| Lead / Release Gate | acceptance | Reviews below |

Loop: Plan → Research → Delegate → Implement → Verify → Review → Correct →
Integrate. Research audits ran in parallel; implementation stayed sequential
where files overlapped.

## Subsystem health

| Subsystem | Code | Host tests | Builds | HW |
|---|---|---|---|---|
| Crypto | ✅ | ✅ RFC KATs | ✅ host/ARM/Xtensa | — |
| Frame / session / replay | ✅ | ✅ | ✅ | — |
| Fragmentation / COBS | ✅ | ✅ | ✅ | — |
| Capability registry | ✅ | ✅ | ✅ | — |
| Capture manager | ✅ | ✅ (mock) | ✅ | — |
| PCAP + CRC-32 | ✅ | ✅ + interop | ✅ | — |
| Safety gate | ✅ | ✅ | ✅ | — |
| Flipper UI + loopback | ✅ | — | ✅ APPCHK | — |
| Flipper BLE transport | ✅ | parser + client ✅; radio path — | ✅ APPCHK | — |
| ESP32 bridge core | ✅ | ✅ 88 checks (host) | ✅ SUCCESS | — |
| ESP32 NimBLE transport | ✅ | — (needs NimBLE) | ✅ SUCCESS | — |
| Bruce glue + patch | written | — | ⬜ (Bruce tree) | — |

## Capture subsystem tracker (Phase 2)

| Item | Status |
|---|---|
| Bruce sniffer/EAPOL/pcap audited | ✅ (source-verified) |
| Session manager + gating + failure paths | ✅ host |
| PCAP format validation | ✅ byte-exact + interop |
| Transfer integrity (CRC-32) | ✅ primitive; resumable transfer ⬜ |
| Session lifecycle tests | ✅ |
| Simulated connection-failure tests | ✅ |
| Headless Bruce hooks | 📐 `PATCH.md` |
| Coexistence / transfer-channel measurement | ⬜ HW |

## Firmware compatibility

| Component | Pinned | Compatibility |
|---|---|---|
| Momentum | `mntm-012` / API 87.1 | FAP built + APPCHK'd against it |
| Bruce | `1.17` (`2b6ba4d`) | Audited; glue written vs its symbols |
| StickS3 board | `m5stack-sticks3` | 8 MB flash, 3 MB LittleFS, no SD, PSRAM |
| NimBLE-Arduino | 2.5.1 | Bridge built against it |
| ESP-IDF | 5.5.4 (arduino-esp32 3.3.9) | Bridge built against it |

## Hardware-dependent verification queue

Ordered: BLE peripheral profile → pairing/MTU → protocol round-trips → Bruce glue
in-tree → handshake capture → coexistence → transfer → active-RF gate (lab).
Procedures: `docs/HARDWARE_TESTING.md`.

## Lead Engineer review log

- **Accepted:** shared core (crypto KATs, tamper/replay, fault injection, interop);
  FAP + bridge build evidence. Builds were run, not assumed.
- **Rejected / corrected during the session:**
  - COBS test fed 600 B (> 540 B frame max) → capped at `BL_MAX_MSG`.
  - Aggregate test counter was per-TU `static` (always "0 checks") → made global
    so the pass/fail tally and exit code are real.
  - fbt recursive glob double-linked the core → single `*.c` pattern.
  - Per-operation arm-token in the safety gate → **removed** per owner direction;
    replaced with connection-level authorization.
  - ESP32 bridge used a stale flag name and lacked `<stdio.h>` → fixed; rebuilt.
- **Not accepted as done:** anything on-air; BLE transport (stub); Bruce glue
  (uncompiled in-tree). These stay 🟡/⬜.

### PR #1 external review (2026-11-23) — gaps in the internal review

The internal gate accepted `31629db` with the bridge core only *compile*-checked.
Independent review (project owner + Codex) found six real defects there, all
reproduced against `31629db` before fixing (evidence: `docs/TEST_RESULTS.md`):

| Finding | Fix |
|---|---|
| P1: PSK wiped by `bl_session_close` on disconnect → every reconnect fails auth | Provisioned key kept in `psk_`, separate from the disposable session |
| ESTOP didn't block `CAP_START` | Capture start goes through the gate; refused while latched |
| Poll failure left radio + pcap file open | `bl_capture_poll` stops the backend exactly once (`backend_active`); cleanup paths call idempotent `bl_capture_stop` |
| `capture_` uninitialised (constructor; no-backend builds) | Initialised in ctor and `begin()`; null backend = permanently idle |
| Rate limit charged 1 frame/op while ops send many (16× over) | Frame budget: reserve `min(16, window remaining)`, pass as hard cap to the backend, refund unused; overrun → e-stop |
| Wi-Fi scan summed `snprintf` returns (SIZE_MAX at cap 0, cut lines) | Bounded `bl_textbuf` + `formatWifiApLine`; bridge clamps backend lengths |

Found while fixing: broadcast/multicast BSSIDs passed the "no broadcast" check
(it only tested for NULL); untrusted SSIDs could inject lines; the bridge never
learned the negotiated MTU. All fixed with tests.

### PR #1 second Codex review (`9f89c4d`)

| Finding | Fix |
|---|---|
| Re-sending "enable active" cleared the rate window (320 frames vs limit 64) | Window is transmit history; only time expires it. Also closed the reconnect variant found while fixing |
| Impostor advertising the public UUID could hold the only connection | Bridge link supervision (10 s handshake deadline, immediate drop on failed proof) + transport per-peer cooldown |

Found while fixing: the bridge was mutated from two FreeRTOS tasks, and
subscribing before `onConnect()` could lose an immediate `HELLO`, which with
the new deadline would have blocklisted the real Flipper. The transport now has
a single owner. One lesson from my own first-round fix: the frame budget
was correct per op but never tested against *administrative* resets of the
window. The new tests drive the adversarial sequence (enable/op pairs,
reconnect cycles) rather than the happy path.

**Process change:** portable code is now host-tested, not just compile-checked.
`make test` runs the bridge core under ASan+UBSan (`bl_bridge_tests`), and
cppcheck covers it. Compile success alone is no longer accepted as review
evidence for logic that can run on the host.
