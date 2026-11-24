# Next Steps — prioritized engineering backlog

> Ordered by dependency and value. Each item: scope, dependencies, expected
> outcome, acceptance criteria, verification. The authorization and safety
> boundaries in `ARCHITECTURE.md` §5 are **preserved** across all of these.
> Work on a branch off `448b798`; do not merge unverified changes to `main`.

## P0 — architecture consolidation (agreed, in flight)

### 1. ADR 0007 + doc/ADR revisions
- **Scope:** write ADR 0007 (integration-layer architecture / generic supervised
  native-function job); revise CLAUDE.md and ADRs 0003/0006 to match.
- **Depends on:** nothing (design is agreed).
- **Outcome:** a single source of truth that Bruce owns tools and BruceLink is a
  bridge.
- **Acceptance:** ADR 0007 committed; CLAUDE.md "Architecture rules" no longer
  implies per-tool code; ADRs 0003/0006 reference the generic job and the
  active-job rules.
- **Verify:** doc review (the independent review pass).

### 2. Generic supervised native-function job
- **Scope:** one job type that launches/supervises a Bruce function via a
  supported interface (CLI where it exists, else `options`/`nav`/`optionsJSON`
  menu-walk, else `startApp`); bridge owns lifecycle + a per-function allowlist
  with classes (passive/active); capabilities by discovery. Replace the per-tool
  job code incrementally (keep `wifi.scan`, `wifi.sniff.handshake` working).
- **Depends on:** #1.
- **Outcome:** new Bruce tools exposed by adding an allowlist entry, not new
  firmware code.
- **Acceptance:** scan + passive capture run through the generic path with no
  regression; menu-walk launcher tolerates upstream menu-label changes (tracks
  `optionsJSON`); active functions still refused.
- **Verify:** host tests (allowlist, lifecycle, refusals) that fail on the old
  code; then hardware for scan + capture.

### 3. Make RAM trims installer-configurable (default on)
- **Scope:** expose the `BRUCELINK_RAM_TRIM` trims as an installer/build option;
  keep them ON by default.
- **Depends on:** nothing.
- **Outcome:** the ~46 kB is kept by default but a builder can opt out per trim.
- **Acceptance:** installer flag toggles the patches; default build is byte-equivalent to today; `tests/test_install_into_bruce.py` covers both modes.
- **Verify:** host (installer tests) + a trimmed and an untrimmed Bruce build.

## P1 — close verification gaps

### 4. Evaluate native Sniffer vs the passive capture job
- **Scope:** assess launching Bruce's native Sniffer as a *separate* capability;
  document differences in channel handling, TX behaviour (hidden AP), memory,
  output format. **Do not remove** the working passive capture until a
  replacement is proven equivalent without regressions.
- **Depends on:** #2 (clean way to launch it).
- **Acceptance:** a written comparison + a decision; working code untouched until then.
- **Verify:** hardware A/B once a candidate exists.

### 5. Hardware-verify the shipped-but-unconfirmed paths
- **Scope:** capture-file download/delete end to end; the review-fix build on
  **both** devices together (wire changed); Forget; connect-straight-after-pairing.
- **Depends on:** local hardware session (`docs/HARDWARE_TESTING.md`).
- **Acceptance:** each path exercised on hardware, results appended to `docs/TEST_RESULTS.md`.
- **Verify:** hardware (owner).

### 6. Independent review of the handshake-capture job
- **Scope:** the capture job has had **no** independent review (a usage safeguard
  stopped the earlier attempt). Get an independent review of the capture +
  file-transfer path.
- **Depends on:** nothing.
- **Acceptance:** review findings triaged; each host-runnable fix has a regression test.

## P2 — confidentiality & feature expansion

### 7. Link encryption (M4.6)
- **Scope:** AEAD over the session keys so frames are encrypted, not just
  authenticated. Update every "authenticated, not encrypted" caveat when done.
- **Depends on:** nothing (keys exist).
- **Acceptance:** frames unreadable on air; interop + replay tests; both devices updated together.
- **Verify:** host (crypto/interop) + hardware link.

### 8. Active-job rules, then expose active tools
- **Scope:** implement ADR 0006's active-job rules (gate at acceptance, duration
  cap, on-stick stop, no re-run) and test them; only then expose any
  transmitting Bruce tool (deauth/portal/karma/etc.) through the generic job,
  still default-off behind the safety gate.
- **Depends on:** #2, #7 recommended.
- **Acceptance:** active job refused without the rules; with them, bounded,
  stoppable on the stick, never re-run; demo-key bridge still refuses.
- **Verify:** host (gate/limits/refusals, mutation-checked) + hardware in an
  owned, isolated, authorized lab only.

### 9. StickS3 BLE lifecycle
- **Scope:** re-enable BLE without a reboot after Bruce's own Wi-Fi has been used
  (M5.6); BLE ownership/coexistence handling in Bruce (M5.4).
- **Depends on:** nothing hard; interacts with ADR 0006.
- **Acceptance:** BruceLink can be turned off and on without a reboot in the
  common path; documented limits otherwise.
- **Verify:** hardware.

## Notes
- Hardware-only items stay **explicitly unverified** until run locally.
- Do not weaken a test to make it pass; new defects get a regression test that
  fails on the old code.
