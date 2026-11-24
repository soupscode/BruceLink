---
name: brucelink-hardware-test-report
description: Record a BruceLink hardware test result accurately — preserving the verified-vs-owner-reported boundary, the device build ids, and the active-RF lab-only rule. Invoke after running anything on the physical Flipper or StickS3.
---

# BruceLink — Hardware Test Reporting

Paths are repo-root-relative. Detail: `docs/handoff/TESTING.md`, `docs/HARDWARE_TESTING.md`.

## When to invoke
- After executing any procedure on the physical Flipper and/or StickS3.
- When updating project status from a hardware observation.

## Required project context
- The evidence boundary: cloud results are software-only; anything on a device is **owner-reported** until independently confirmed.
- Active-RF testing is permitted **only** in an owned, authorized, isolated lab (CLAUDE.md "Security rules").

## References
- `docs/handoff/TESTING.md`, `docs/handoff/HARDWARE.md`, `docs/HARDWARE_TESTING.md`, `docs/TEST_RESULTS.md`.

## Procedure
1. Record the exact firmware build on **each** device (they must match on a wire change) and the test step from `docs/HARDWARE_TESTING.md`.
2. Capture raw evidence: serial logs, RAM figures, what was **observed** vs what is **inferred**.
3. Append a dated entry to `docs/TEST_RESULTS.md` under "Hardware results", labelled **owner-reported**.
4. Update the status cells in `docs/handoff/CURRENT_STATE.md`, `FEATURE_MATRIX.md`, and `TESTING.md` to match — using the 4-way scale (HW owner-reported / software-verified / designed / unverified).
5. Do not upgrade "owner-reported" to "verified" without independent confirmation.

## Verification / acceptance
- The entry contains device build ids, raw observation, and an evidence label; no claim exceeds the evidence.
- Status cells across docs agree.
- Active-RF results, if any, state the owned/authorized/isolated lab context.

## Common mistakes & pitfalls
- Labelling owner-reported as independently verified.
- Omitting which commit was on each device.
- Reporting "it works" without saying what was displayed/measured.
- Pasting a pairing key or secret (key id only).

## Expected output
A dated `docs/TEST_RESULTS.md` entry plus the updated cross-doc status cells.
