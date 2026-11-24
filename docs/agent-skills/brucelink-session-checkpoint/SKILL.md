---
name: brucelink-session-checkpoint
description: Capture the BruceLink working state at the end of a session, or recover it cold at the start, so work transfers cleanly between agents and humans without losing context. Invoke before pausing, before a risky step, or when picking up with no prior context.
---

# BruceLink — Session Checkpointing & Context Recovery

Paths are repo-root-relative. Detail: `docs/handoff/CURRENT_STATE.md`, `docs/handoff/SESSION_SUMMARY.md`, `docs/handoff/HANDOFF.md`.

## When to invoke
- **Checkpoint:** before pausing work, before a risky/irreversible step, or at a milestone.
- **Recover:** starting cold on the repo with no prior session context.

## Required project context
- Git state and the handoff docs; knowledge of where any uncommitted artifacts live.

## References
- `docs/handoff/CURRENT_STATE.md`, `SESSION_SUMMARY.md`, `HANDOFF.md`, `DECISION_HISTORY.md`.

## Procedure
**Checkpoint:**
1. Record branch, HEAD, `git status`, and what changed this session.
2. State what is verified vs pending, and the single next action.
3. Update `CURRENT_STATE.md`; if a decision was made, add it to `DECISION_HISTORY.md` or a new ADR.
4. Note any uncommitted work and where artifacts are; do **not** discard uncommitted work or overwrite functional code.
5. Keep documentation commits separate from functional ones.

**Recover:**
1. Read `CURRENT_STATE.md` → `SESSION_SUMMARY.md` → the last few `git log` entries.
2. Run the build-validation gate to confirm the baseline reproduces.
3. Resume from the recorded next action.

## Verification / acceptance
- A reader with no prior context can state the HEAD, what is done vs pending, and the next action.
- The gate reproduces the baseline (`brucelink-build-validation`).

## Common mistakes & pitfalls
- Leaving uncommitted work undocumented.
- Committing docs and functional code together.
- Claiming hardware state that was only owner-reported.
- Overwriting working code during recovery instead of building on the baseline.

## Expected output
A checkpoint note (state + next action) and/or a clean recovery summary.
