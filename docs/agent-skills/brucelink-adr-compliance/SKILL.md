---
name: brucelink-adr-compliance
description: Check a proposed or existing BruceLink change against the project's architectural decisions and safety boundaries before it merges. Invoke in review, or before implementing anything touching transport, jobs, the safety gate, or Bruce integration.
---

# BruceLink — Architectural Decision Compliance

Paths are repo-root-relative. Detail: `docs/decisions/`, `docs/handoff/ARCHITECTURE.md`, `docs/handoff/DECISION_HISTORY.md`.

## When to invoke
- Reviewing any change, especially to transport, session, jobs, safety, or the Bruce glue.
- Before implementing something that might cross a boundary.

## Required project context
- ADRs 0001–0006 (0007 pending) and the CLAUDE.md "Architecture rules" and "Security rules".

## References
- `docs/decisions/0001`..`0006`, `docs/handoff/ARCHITECTURE.md` (§5 invariants), `docs/handoff/DECISION_HISTORY.md`, `CLAUDE.md`.

## Procedure
Map the change to the relevant ADR(s), then check each invariant:
1. **Authorization = the authenticated connection** (ADR 0002). No per-function authorization tokens.
2. **Active-RF default-off + safety gate** (ADR 0003): session enable, e-stop, link-loss auto-stop, rate limit, one specific unicast target, refund on failed transmit. BruceLink builds no RF frames.
3. **Active ops/jobs refused** (`CAPABILITY_DISABLED`) until ADR 0006's active-job rules exist and are tested.
4. **One radio at a time** (ADR 0006): no Wi-Fi while the link is up; never start Wi-Fi from the bridge task; a job is never re-run after reset.
5. **Core discipline**: `shared/brucelink_core` stays pure C99, allocation-free, no platform deps; a feature is a capability + backend method (or, under ADR 0007, an allowlist entry), **not** a new transport or message type per tool.
6. **Payloads ≤ 512 B**; key never in `bruce.conf`/LittleFS/logs/screen (key id ok); demo-key bridge refuses active ops.
7. **Link authenticated-not-encrypted until M4.6** — the caveat must remain wherever status is reported.
Flag every divergence; if a change genuinely needs to alter a decision, propose an explicit ADR amendment — do not weaken a boundary silently.

## Verification / acceptance
- Every touched invariant is satisfied, or there is an explicit, justified ADR amendment.
- No safety boundary weakened as a side effect; no test disabled to pass.

## Common mistakes & pitfalls
- Adding a per-function auth token "for convenience".
- Enabling an active op/job without the gate and the ADR 0006 rules.
- Starting Wi-Fi beside BLE, or from the bridge task.
- Forking Bruce instead of patching via `tools/install_into_bruce.py`.

## Expected output
A compliance checklist with pass/flag per invariant, and any required ADR amendment called out.
