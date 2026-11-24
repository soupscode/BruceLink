---
name: brucelink-handoff-maintenance
description: Keep the docs/handoff/ set and AGENTS.md/CLAUDE.md accurate and non-contradictory as BruceLink changes. Invoke whenever behaviour, state, decisions, verification status, or dependencies change.
---

# BruceLink — Handoff Documentation Maintenance

Paths are repo-root-relative. The doc set + ownership are indexed in `docs/handoff/HANDOFF.md`.

## When to invoke
- After any change to behaviour, state, a decision, verification status, or a dependency.
- Before committing a change that makes an existing doc stale.

## Required project context
- The handoff doc set and which doc owns which concern (`HANDOFF.md` index).

## References
- `docs/handoff/*` (esp. `HANDOFF.md`), `AGENTS.md`, `CLAUDE.md`.

## Procedure
1. Identify which docs the change touches:
   - state → `CURRENT_STATE.md`; test results → `TESTING.md` + append `docs/TEST_RESULTS.md`;
   - capability status → `FEATURE_MATRIX.md`; a decision → a new ADR + `DECISION_HISTORY.md`;
   - bugs/limits/plan → `KNOWN_ISSUES.md` / `NEXT_STEPS.md`;
   - code structure/wire → `CODEBASE_MAP.md` / `PROTOCOL.md`; RAM → `MEMORY_OPTIMIZATION.md`.
2. Update them in the same change; cross-reference by filename (do not duplicate content).
3. Keep `CLAUDE.md` and `AGENTS.md` consistent — if you change a shared engineering rule, change both.
4. Preserve the verified/assumed labels and the safety wording; do not soften a boundary.
5. Confirm the `HANDOFF.md` index still lists every doc.
6. Commit documentation separately from functional code.

## Verification / acceptance
- No doc contradicts another or the code; no stale numbers (e.g. test counts, RAM).
- `CLAUDE.md` ≡ `AGENTS.md` on shared rules; the HANDOFF index is complete.
- Cross-references resolve (filenames exist).

## Common mistakes & pitfalls
- Updating code but not the docs it invalidates.
- Letting `CLAUDE.md` and `AGENTS.md` drift apart.
- Duplicating content across docs instead of cross-referencing.
- Quietly weakening a safety boundary in prose.

## Expected output
The updated docs and a one-line note of what changed where.
