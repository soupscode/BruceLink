# Architecture

> Existing architecture as shipped at `448b798`, plus the agreed *proposed*
> direction (the generic supervised native-function interface, to be ADR 0007).
> Design-level ADRs live in `docs/decisions/`; this doc is the integrated
> picture. See `CODEBASE_MAP.md` for file-level detail, `PROTOCOL.md` for the
> wire, `INTEGRATION_INTERFACES.md` for how Bruce is driven.

## 1. System shape

```
  Flipper Zero (Momentum)                    M5StickS3 (Bruce + BruceLink)
  ┌───────────────────────────┐              ┌────────────────────────────────┐
  │ BruceLink FAP             │   BLE GATT   │ BruceLink bridge (NimBLE central)│
  │  - ViewDispatcher UI      │◄────────────►│  - NimBleCentralTransport        │
  │  - bl_client (session)    │  authenticated│  - BruceLinkBridge (session)     │
  │  - BLE peripheral profile │   frames      │  - BruceBackendImpl ──► Bruce FW │
  │  = session INITIATOR      │              │  = session RESPONDER             │
  └───────────────────────────┘              └────────────────────────────────┘
        shared/brucelink_core (identical C99 protocol code on both ends)
```

- **Flipper = BLE peripheral + session initiator.** **StickS3 = BLE central +
  session responder** (ADR 0001). The StickS3 scans for and connects to the
  Flipper's advertised GATT service; the Flipper drives the session.
- Both ends compile the **same** `shared/brucelink_core` C99 code, so protocol
  behaviour is byte-identical. On the Flipper the core is pulled in via the
  generated one-line wrappers under `flipper/brucelink/core/` (see
  `CODEBASE_MAP.md`).

## 2. Component ownership

| Concern | Owner | Where |
|---|---|---|
| Protocol, crypto, session, pairing, jobs, files, safety | shared core | `shared/brucelink_core/src` |
| Flipper UI, views, request orchestration | FAP | `flipper/brucelink` |
| BLE peripheral profile (Flipper side) | FAP transport | `flipper/brucelink/transport` |
| BLE central transport (StickS3 side) | bridge | `esp32/lib/BruceLink/NimBleCentralTransport` |
| Session/bridge dispatch, safety enforcement, file ops | bridge core | `esp32/lib/BruceLink/BruceLinkBridge` |
| Mapping requests to Bruce behaviour | Bruce glue | `esp32/src/modules/brucelink/BruceBackendImpl` |
| Jobs (RTC record, boot hooks) | Bruce glue | `esp32/src/modules/brucelink/BruceLinkJobs` |
| On-device UI, on/off, pairing screen, bridge task | Bruce glue | `esp32/src/modules/brucelink/BruceLinkModule`, `BruceLinkMenu` |
| **Tool behaviour (Wi-Fi scan, sniffer, …)** | **Bruce firmware** | upstream `src/modules/wifi/*`, etc. |

**Intent (owner, 2026-11-24):** BruceLink is an integration/management layer.
Bruce owns tool behaviour; BruceLink owns discovery, menu navigation,
configuration, job submission, status, cancellation, error handling, file
transfer, connection management, and resource management. BruceLink must not
reimplement Bruce's tools.

## 3. Communication flow (a request)

1. Flipper `bl_client` seals a request frame (12-byte header incl. request tag,
   payload ≤ 512 B, 16-byte MAC) once the session is established.
2. Transport chunks it to the negotiated MTU; StickS3 `NimBleCentralTransport`
   reassembles.
3. `BruceLinkBridge::onFrame` opens/authenticates, runs the safety gate for
   active ops, dispatches to `BruceBackendImpl` (device info, capabilities, file
   ops) or records a job.
4. The response echoes the request tag; the Flipper drops any reply whose tag is
   not the pending request's. Frames are **authenticated, not encrypted** (M4.6).

## 4. State machines & lifecycles

### Session (`bl_session`, both ends)
`INIT → HELLO exchange → key confirm → ESTABLISHED`; seals/opens only when
ESTABLISHED; wrong-PSK / replay / version-no-overlap / tamper → `FAILED` and all
secrets wiped. Link loss drops to a state that requires a fresh authenticated
session to recover. Version range is part of the authenticated transcript
(downgrade-resistant).

### Pairing (`bl_pair`, ADR 0005)
Commitment → reveal → mutual confirm; both ends derive the same key id. Flipper
stores the key in a secure-enclave-encrypted file; StickS3 in NVS. The public
**demo key** is only ever present to be *refused* on a device; a demo-key bridge
refuses every active op.

### Job (`bl_job` + `BruceLinkJobs`, ADR 0006) — "one radio at a time"
```
 (live link) JOB_ACCEPTED ──► commit ──► [esp_restart, BLE OFF] ──► RUNNING
                 │                                                      │
         e-stop / link loss                                     tool runs, result
         before commit → cancel                                 written to RTC
         (record cleared, no restart)                                  │
                                              [esp_restart, BLE ON] ◄───┘
                                                     │
                                      FINISHED; Flipper fetches the result;
                                      record is cleared — a job is NEVER re-run
```
The job record lives in `.rtc_noinit` RTC slow memory (survives `esp_restart`,
not initialised by startup). `bruceLinkBootEarly()` runs a PENDING job with BLE
off; `bruceLinkBoot()` brings BLE back. A crash mid-job → INTERRUPTED, never
re-run. Current jobs: `wifi.scan`, `wifi.sniff.handshake` (see `FEATURE_MATRIX.md`).

## 5. Security boundaries (invariant — do not weaken)

- **Authorization = the authenticated connection** (ADR 0002). No per-function
  authorization tokens.
- **Active-RF capabilities are default-off** and must pass the safety gate
  (`bl_safety`): session-level enable, e-stop (latched on link loss, cleared only
  by a new authenticated session), link-loss auto-stop, fixed-window rate limit,
  one specific unicast target (no broadcast/multicast), refund after a failed
  transmit. BruceLink **builds no RF frames** — it forwards to Bruce's existing
  primitives.
- **Active ops and active jobs are refused** (`CAPABILITY_DISABLED`) until ADR
  0006's active-job rules (gate at acceptance, duration cap, on-stick stop, no
  re-run) are implemented and tested.
- **One radio at a time** (ADR 0006): nothing starts Wi-Fi while the BLE link is
  up; Wi-Fi tools run only as BLE-off jobs. Never start Wi-Fi from the bridge
  task.
- Payloads bounded ≤ 512 B. Key never in `bruce.conf` / LittleFS / logs / screen
  (key id is fine). No unauthenticated control or unrestricted command execution.

## 6. Proposed direction — generic supervised native-function interface (→ ADR 0007)

> **Status: agreed, NOT implemented.** Shipping code uses per-tool jobs. This
> section records the target so new work converges on it.

One generic **supervised native-function job** replaces per-tool job code:
- **BruceLink owns the lifecycle**: discovery of what Bruce offers, config
  staging (live over BLE via `settings`/menu), job acceptance against a
  **per-function allowlist with classes** (passive vs active-RF), the BLE-off
  restart, launching the Bruce function, the duration cap, the on-stick stop
  button, and result retrieval (`storage read` of pcaps/creds, or `display dump`).
- **Bruce owns the function.** BruceLink launches it through a *supported*
  interface — the serial CLI where one exists, else the `options`/`nav`/
  `optionsJSON` menu-walk (the only route to deauth/portal/karma/etc.), else
  `startApp` — never raw CLI passthrough or arbitrary command execution.
- **Capabilities come from discovery**, not a hardcoded table.
- **Safety stays firmware-side and independent of the host app.** Active-RF
  functions remain refused until the ADR 0006 active-job rules exist and are
  tested. Exposing a transmitting tool is gated on those rules, regardless of
  what the Flipper asks.

Key constraint from `INTEGRATION_INTERFACES.md`: Bruce's menu order/labels are
**not a stable API**, and most offensive tools have no CLI — so the menu-walk
launcher must track `optionsJSON` state and tolerate upstream menu changes.

## 7. Design invariants (quick checklist for any change)
- Shared core stays pure C99, allocation-free, no platform deps; change it only with tests.
- New Bruce feature = a capability + backend method (today) / an allowlist entry for the generic job (target) — **not** a new transport or message type per tool.
- Host-runnable code is host-tested, not just cross-compiled.
- Both devices must run the same build when the wire changes.
- A job is never re-run after reset.
