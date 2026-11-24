# Agent Workflow — how to work in this repository

> For any coding agent (or human) continuing BruceLink. These are the working
> conventions that produced the current state; they exist to keep changes safe,
> reviewable, and upstream-compatible. The detailed engineering rules live in
> `/CLAUDE.md` and `/AGENTS.md`; this doc is the *how we work*.

## Context management (limited-context friendly)

- Start from `HANDOFF.md` → `PROJECT_OVERVIEW.md` → `CURRENT_STATE.md` →
  `ARCHITECTURE.md`. Load deeper docs **only** for the task at hand (the reading
  order in `HANDOFF.md` maps tasks to docs). You do not need the whole repo or
  the whole history in context at once.
- Treat the handoff docs and the ADRs as the source of truth; re-read the one
  relevant doc rather than reconstructing from memory.
- When upstream Bruce detail matters, the inventories (`NATIVE_BRUCE_INVENTORY.md`,
  `INTEGRATION_INTERFACES.md`) carry file:line citations into the Bruce tree.

## Task decomposition

- Break work to the level of a single reviewable change with its own test.
- A new Bruce feature is exposed by **a capability + backend method** today, and
  by **an allowlist entry for the generic supervised job** under the target
  architecture (ADR 0007) — **not** a new transport or message type per tool.
- Keep the shared core (`shared/brucelink_core`) pure C99, allocation-free, no
  platform deps. Change it only with tests.

## Parallel development (subagents)

- Parallelize across **independent** subject areas with **disjoint file
  ownership** (how this handoff was written: separate agents for
  map/protocol, hardware/memory, build/testing, troubleshooting/session). Never
  have two agents edit the same file concurrently.
- Give each agent a tight brief, the exact files it owns, and the read-only
  sources it needs. Keep investigation agents read-only.

## Testing (non-negotiable)

- Host-testable code (shared core, bridge core, formatters) must be
  **host-tested**, not just cross-compiled. Compile success is not evidence of
  logic.
- Every new defect gets a **regression test reproduced against the old code** (or
  a mutant restoring it) — the test must fail before the fix.
- Never weaken a test to make it pass.
- After adding/removing a core or bridge source file, run
  `python3 tools/gen_wrappers.py` (staleness is caught by `tests/test_wrappers.py`).
- Minimum gate before proposing a change:
  `make -C tests/c test cppcheck` + `python3 -m unittest discover -s tests`, then
  the relevant build (FAP / ESP32 compile / full Bruce). See `BUILD_AND_DEPLOY.md`.

## Independent review

- High-stakes or security-touching work gets an **independent review** by an
  agent/person that did not write it, checking correctness, safety boundaries,
  and verification claims. Triage findings; each host-runnable fix gets a
  regression test.

## Git discipline

- Develop on a branch; descriptive commits; run tests before committing.
- **Do not merge unverified changes to `main`.** PRs carry build/test results,
  limitations, and the explicit hardware-verification needs.
- Keep **documentation commits separate** from functional-code commits.
- Preserve `448b798` as the known-good baseline.

## Honesty / definition of done

- Code exists · relevant tests pass · builds succeed · errors handled · docs
  updated · independently reviewed · hardware-dependent verification explicitly
  listed.
- **Never claim untested functionality works.** Never mark a capability
  hardware-verified from simulation. Distinguish verified / software-verified /
  assumed / unverified everywhere.

## Documentation maintenance

- When you change behaviour, update the affected handoff doc(s) in the same
  change: `CURRENT_STATE.md` (state), `TESTING.md` (results), `KNOWN_ISSUES.md`
  and `NEXT_STEPS.md` (status), `DECISION_HISTORY.md` / a new ADR (decisions),
  `FEATURE_MATRIX.md` (capability status).
- Keep `/CLAUDE.md` and `/AGENTS.md` consistent — if you change an engineering
  rule, change both.
- Append hardware results to `docs/TEST_RESULTS.md` (owner/local session).

## Safety boundaries (never relax as a side effect)

Authorization = the authenticated connection; active-RF default-off + safety
gate; active ops/jobs refused until ADR 0006's rules exist and are tested; one
radio at a time; demo-key bridge refuses active ops; payloads ≤ 512 B; key never
in `bruce.conf`/logs/screen; link authenticated-not-encrypted until M4.6. See
`ARCHITECTURE.md` §5 and `/CLAUDE.md`.
