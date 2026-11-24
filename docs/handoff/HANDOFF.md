# BruceLink — Engineering Handoff (START HERE)

> The entry point for any developer or coding assistant picking up BruceLink.
> It is model-independent: you do not need the originating chat session, any
> Anthropic-specific tooling, or that session's context to use it. Load the
> deeper docs selectively (see the reading order) — you do **not** need the whole
> repo or history in context at once.

## What BruceLink is (30 seconds)

A Flipper Zero (Momentum) native remote frontend / management **bridge** for an
M5StickS3 running **Bruce** firmware, over an **authenticated BLE link**.
BruceLink exposes Bruce's *existing* wireless tools through supported interfaces,
behind authorization and safety gates — it does **not** reimplement them.
Flipper = BLE peripheral + session initiator; StickS3 = BLE central + responder.
Full framing: `PROJECT_OVERVIEW.md`.

## Current status (one line)

At commit `448b798` (branch `feat/radio-handoff`, the known-good baseline):
device info, capabilities, Wi-Fi scan (job) and passive handshake capture (job)
+ capture-file transfer are shipped; the architecture is being consolidated to a
generic supervised native-function bridge (ADR 0007, pending). Full detail:
`CURRENT_STATE.md`.

## Reading order

**Everyone, first:**
1. `HANDOFF.md` (this file) and `/AGENTS.md` (root; model-independent repo rules).
2. `PROJECT_OVERVIEW.md` — what/why, how the goal evolved.
3. `CURRENT_STATE.md` — exact snapshot (git, builds, verification).
4. `ARCHITECTURE.md` — components, ownership, state machines, security boundaries, the proposed generic job.

**Then by task:**
- Changing code → `CODEBASE_MAP.md`, then the area doc.
- Protocol / wire / crypto → `PROTOCOL.md`.
- Exposing a Bruce capability → `FEATURE_MATRIX.md`, `NATIVE_BRUCE_INVENTORY.md`, `INTEGRATION_INTERFACES.md`.
- Board / radio / RAM → `HARDWARE.md`, `MEMORY_OPTIMIZATION.md`.
- Building / flashing / testing → `BUILD_AND_DEPLOY.md`, `TESTING.md`.
- Debugging / "why is it like this" → `TROUBLESHOOTING_HISTORY.md`, `DECISION_HISTORY.md`.
- Planning / what's broken → `KNOWN_ISSUES.md`, `NEXT_STEPS.md`.
- Context / chronology → `SESSION_SUMMARY.md`.
- How to work here → `AGENT_WORKFLOW.md`.
- Reusable procedures → `../agent-skills/` (portable SKILL.md skills).

## Document index

| Doc | Purpose |
|---|---|
| `HANDOFF.md` | This entry point: orientation, reading order, verification. |
| `PROJECT_OVERVIEW.md` | Objectives, evolution, how Flipper/StickS3/Bruce relate. |
| `CURRENT_STATE.md` | Git, versions, build status, completed/outstanding, verification. |
| `ARCHITECTURE.md` | Existing + proposed architecture, ownership, lifecycles, invariants. |
| `CODEBASE_MAP.md` | Dirs, files, symbols, entry points, "where to change X". |
| `FEATURE_MATRIX.md` | Native Bruce vs BruceLink parity (availability, board, RF, path, status, verification). |
| `NATIVE_BRUCE_INVENTORY.md` | Upstream Bruce Wi-Fi capabilities (source, deps, build inclusion). |
| `INTEGRATION_INTERFACES.md` | Bruce's supported control interfaces (CLI, menu-walk, startup, config). |
| `PROTOCOL.md` | Wire format, session, pairing, jobs, file transfer, error semantics. |
| `HARDWARE.md` | Flipper + StickS3, radio/storage/flash constraints. |
| `MEMORY_OPTIMIZATION.md` | The ~46 kB of RAM trims: why, where, measured, trade-offs, config. |
| `DECISION_HISTORY.md` | ADRs + reasoning + rejected/abandoned approaches. |
| `TROUBLESHOOTING_HISTORY.md` | The real debugging saga, per issue, with commits. |
| `BUILD_AND_DEPLOY.md` | Reproducible setup, build, test, artifact, flash. |
| `TESTING.md` | Suites, current baseline, verified/assumed split, hardware procedures. |
| `KNOWN_ISSUES.md` | Bugs, incomplete work, debt, limitations, open questions. |
| `NEXT_STEPS.md` | Prioritized backlog with acceptance + verification. |
| `AGENT_WORKFLOW.md` | How to work here (context, tests, git, review, docs). |
| `SESSION_SUMMARY.md` | Chronological reconstruction of the development. |

## Independently verify the repository (no trust required)

```bash
git clone https://github.com/soupscode/brucelink && cd brucelink
git checkout feat/radio-handoff        # HEAD should be 448b798
git log -1 --format='%h %s'            # 448b798 ...

# Host tests (need gcc + make; ASan/UBSan)
make -C tests/c test cppcheck          # expect: core 3681 + bridge 448 checks, 0 failed; cppcheck clean
python3 -m unittest discover -s tests  # expect: 31 tests OK

# Builds (need the pinned toolchains — see BUILD_AND_DEPLOY.md)
(cd flipper/brucelink && ufbt)         # expect: APPCHK pass, Target 7 / API 87.1
(cd esp32/test_compile && pio run)     # expect: SUCCESS
```
The full Bruce + BruceLink firmware build (installer into a Bruce `2b6ba4d`
checkout, then `pio run -e m5stack-sticks3`) is in `BUILD_AND_DEPLOY.md`; expect a
successful link and static internal RAM ≈ 91,260 B with the trims. **No cloud
result involves hardware;** hardware results are owner-reported in
`docs/TEST_RESULTS.md` and labelled as such.

## Non-negotiables (carry these into every change)

- **Safety boundaries** (do not weaken): authorization = the authenticated
  connection; active-RF default-off + safety gate; active ops/jobs **refused**
  until ADR 0006's active-job rules exist and are tested; one radio at a time;
  demo-key bridge refuses active ops; payloads ≤ 512 B; key never in
  `bruce.conf`/logs/screen; link authenticated-but-not-encrypted until M4.6. See
  `ARCHITECTURE.md` §5 and `/CLAUDE.md`.
- **Honesty**: never claim untested functionality works; never mark a capability
  hardware-verified from simulation; distinguish verified / software-verified /
  assumed / unverified. Host-test host-runnable code; new defects get a
  regression test that fails on the old code; never weaken a test to pass it.
- **Git**: branch off `448b798`; do not merge unverified changes to `main`; keep
  documentation commits separate from functional ones.

## What may still need manual export from the originating session

This handoff is reconstructed from the repository, git history, the ADRs, the two
upstream inventories, and the owner-reported hardware results in
`docs/TEST_RESULTS.md`. It does **not** include the raw originating chat
transcript. If you need verbatim decision wording or intermediate reasoning not
captured here, export that from the original session separately. Pre-2026-11-23
chronology is thin and flagged as a gap in `SESSION_SUMMARY.md`.
