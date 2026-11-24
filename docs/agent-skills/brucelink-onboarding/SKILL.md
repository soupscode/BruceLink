---
name: brucelink-onboarding
description: Orient a developer or coding agent new to the BruceLink repository — what it is, which docs to read in what order, and how to confirm the repo is at its known-good baseline — before touching anything. Invoke at the start of any BruceLink task or first contact with the repo.
---

# BruceLink — Onboarding & Repository Orientation

Paths are repo-root-relative.

## When to invoke
- First contact with the BruceLink repo, or returning after time away.
- Before making any change, so you work from the real state, not assumptions.
- When asked "where do I start" / "what is this project".

## Required project context
- A checkout of the repo and the ability to run `git` and the host test suite.
- No hardware required for orientation.

## References (read, don't duplicate)
- `AGENTS.md` (root) and `docs/handoff/HANDOFF.md` — the entry points.
- `docs/handoff/PROJECT_OVERVIEW.md`, `CURRENT_STATE.md`, `ARCHITECTURE.md`.
- `CLAUDE.md` — the engineering + security rules.

## Procedure
1. Read `AGENTS.md`, then `docs/handoff/HANDOFF.md` (it has the full reading order and doc index).
2. Read `PROJECT_OVERVIEW.md` → `CURRENT_STATE.md` → `ARCHITECTURE.md`.
3. Confirm the repo state: `git -C . status` and `git log -1 --format='%h %s'`. Expect branch `feat/radio-handoff`, HEAD `448b798` (the known-good baseline), clean tree.
4. Load only the task-specific docs the HANDOFF reading order points you to; do not ingest the whole repo.
5. Note the non-negotiables from `ARCHITECTURE.md` §5 and `CLAUDE.md` (safety boundaries, honesty, git discipline).

## Verification / acceptance
- You can state, from the docs: what BruceLink is, the current HEAD, the safety boundaries, and which docs cover your task.
- `git` shows the expected baseline and a clean tree (or you have noted any divergence).

## Common mistakes & pitfalls
- Glob/Grep-ing the tree for answers the handoff already gives.
- Assuming `LITE_VERSION` is defined — it is **not** for `m5stack-sticks3`; all non-LITE Bruce features compile in.
- Treating owner-reported hardware results as independently verified.
- Starting edits on `main`, or before confirming the baseline.

## Expected output
A short orientation summary (what BruceLink is, current HEAD/branch, the safety boundaries, the docs relevant to the task) and confirmation the repo is at the known-good baseline (or a note of any divergence).
