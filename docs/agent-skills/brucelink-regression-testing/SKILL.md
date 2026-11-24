---
name: brucelink-regression-testing
description: Add or run host regression tests for a BruceLink change so a fixed defect cannot silently return, following the reproduce-against-old-code discipline. Invoke when fixing a bug or changing the shared core, bridge core, or formatters.
---

# BruceLink — Software Regression Testing

Paths are repo-root-relative. Detail: `docs/handoff/TESTING.md`, `docs/handoff/AGENT_WORKFLOW.md`.

## When to invoke
- Fixing a defect in host-runnable code (shared core, bridge core, formatters, parsers).
- Reviewing a change that should be covered by a test.

## Required project context
- Host tests in `tests/c/` (`-Wall -Wextra -Werror -Wconversion`, ASan+UBSan) and `tests/*.py`.
- The rule (CLAUDE.md "Testing rule"): host-test host-runnable code; a new defect gets a regression test **reproduced against the old code**; never weaken a test.

## References
- `docs/handoff/TESTING.md`, `docs/handoff/AGENT_WORKFLOW.md`, `tests/c/`, `tests/`.

## Procedure
1. Write a test that exercises the defect and run it against the **pre-fix** code (or a mutant restoring the bug). Confirm it **FAILS** — a test that passes on the old code has no regression value.
2. Apply the fix. Confirm the test now passes.
3. Run the full gate: `make -C tests/c test cppcheck` and `python3 -m unittest discover -s tests`.
4. If you added/removed a core or bridge source file, run `python3 tools/gen_wrappers.py`.
5. Keep ASan/UBSan clean; for a protocol change, add/adjust the Python interop vector so C↔Python stays byte-identical.

## Verification / acceptance
- The new test fails on the pre-fix code and passes after the fix (state both).
- Full suite green (report the updated check counts); cppcheck clean.
- No existing test weakened or disabled.

## Common mistakes & pitfalls
- Writing a test that already passes on the old code (proves nothing).
- Cross-compiling only, instead of running on the host.
- Relaxing an assertion to make a suite pass.
- Forgetting the wrapper regen for a core change (`tests/test_wrappers.py` will fail).

## Expected output
Evidence of the failing-then-passing test and the updated suite counts.
