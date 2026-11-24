# AGENTS.md — BruceLink

Model-independent instructions for any coding agent or human working in this
repository. Compatible with agents that read `AGENTS.md` / the `SKILL.md`
convention. The Claude-specific version of these engineering rules is in
`CLAUDE.md`; **the two must stay consistent — if you change an engineering rule,
change both.** Deep detail lives in `docs/handoff/`.

## What BruceLink is

A Flipper Zero (Momentum) native remote frontend / management **bridge** for an
M5StickS3 running **Bruce** firmware, over an **authenticated BLE link**. It
**reuses Bruce's existing functions** — it is not a reimplementation and not an
independent attack framework. Flipper = BLE peripheral + session initiator;
StickS3 = BLE central + responder.

## Authoritative documentation (read in this order)

1. `docs/handoff/HANDOFF.md` — the start-here entry point and document index.
2. `docs/handoff/PROJECT_OVERVIEW.md`, `CURRENT_STATE.md`, `ARCHITECTURE.md`.
3. `docs/decisions/` — ADRs 0001–0006 (the binding design decisions); ADR 0007
   (integration layer) is pending.
4. `CLAUDE.md` — the same engineering rules, Claude's format.
5. `docs/agent-skills/` — portable, independently-loadable SKILL.md procedures
   (onboarding, build validation, regression analysis, ADR compliance, etc.).

Do not Glob/Grep for answers the handoff already gives; load the one relevant doc.

## Existing decisions (summary — detail in `docs/handoff/DECISION_HISTORY.md`)

- Authorization **is** the authenticated connection (ADR 0002); no per-function tokens.
- Active-RF capabilities are default-off behind a safety gate (ADR 0003).
- One radio at a time: Wi-Fi tools run as BLE-off restart jobs (ADR 0006).
- Pairing yields per-pair keys; the public demo key is refused on a device (ADR 0005).
- BruceLink is a bridge; Bruce owns tool behaviour; one generic supervised
  native-function job is the target (ADR 0007, pending).

## Coding conventions

- Shared protocol core (`shared/brucelink_core/src`): pure **C99**,
  allocation-free, no platform deps; identical code runs on both devices. Change
  it only with tests.
- Expose a Bruce feature as **a capability + a backend method** (today) /
  **an allowlist entry for the generic job** (target) — never a new transport or
  message type per tool.
- Flipper app pulls the core via generated wrappers; after adding/removing a
  core/bridge source file run `python3 tools/gen_wrappers.py`.
- Bound all payloads ≤ 512 B. Everything Bruce-specific lives behind the backend;
  the bridge core never includes Bruce or NimBLE headers.

## Build & test requirements

Minimum gate before proposing a change (detail in
`docs/handoff/BUILD_AND_DEPLOY.md`):
```bash
make -C tests/c test cppcheck            # core 3681 + bridge 448 checks, 0 failed; cppcheck clean
python3 -m unittest discover -s tests    # 31 tests OK
(cd flipper/brucelink && ufbt)           # FAP APPCHK pass
(cd esp32/test_compile && pio run)       # SUCCESS
```
Full firmware: `python3 tools/install_into_bruce.py <bruce-2b6ba4d-checkout>` then
`pio run -e m5stack-sticks3`. Pins: Momentum `mntm-012`/API 87.1, Bruce `1.17`
(`2b6ba4d`), NimBLE `2.5.1`, pioarduino `55.03.39`.

## Hardware & memory constraints

- StickS3: single 2.4 GHz radio; Wi-Fi + BLE do not fit in internal RAM together
  → Wi-Fi tools are BLE-off jobs only; never start Wi-Fi from the bridge task.
- RAM trims recover ~46 kB internal (default on; `BRUCELINK_RAM_TRIM`); steady
  headroom is thin (~1.8 kB) — watch internal-RAM growth. See
  `docs/handoff/MEMORY_OPTIMIZATION.md`.
- Flash the **single merged image at `0x0`** (app-only / three-part flashes did
  not boot); flashing erases NVS → re-pair.

## Regression prevention

- Host-test host-runnable code; compile success is not evidence of logic.
- Every new defect gets a regression test that **fails on the old code** before
  the fix. Never weaken a test to make it pass.
- Both devices must run the same build when the wire changes.

## Making incremental changes safely

- Work on a branch off `448b798` (the known-good baseline). Descriptive commits;
  run the gate before committing. **Do not merge unverified changes to `main`.**
- Keep documentation commits separate from functional ones.
- Never claim untested functionality works; list hardware-dependent verification
  explicitly; active-RF testing only in owned, isolated, authorized labs.

## Maintaining documentation

When you change behaviour, update the affected `docs/handoff/` doc(s) in the same
change (`CURRENT_STATE`, `TESTING`, `FEATURE_MATRIX`, `KNOWN_ISSUES`,
`NEXT_STEPS`, and a new ADR or `DECISION_HISTORY` for decisions), append hardware
results to `docs/TEST_RESULTS.md`, and keep `CLAUDE.md` and this file consistent.

## Security boundaries (never relax as a side effect)

Authenticated-connection authorization; active-RF default-off + gate; active
ops/jobs refused until ADR 0006's active-job rules exist and are tested; one
radio at a time; demo-key bridge refuses active ops; no unauthenticated control
or unrestricted command execution; key never in `bruce.conf`/LittleFS/logs/
screen; link authenticated-not-encrypted until M4.6.
