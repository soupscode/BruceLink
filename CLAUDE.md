# BruceLink — agent instructions

Native Momentum Flipper Zero remote frontend for an M5StickS3 running Bruce,
over an authenticated BLE link. BruceLink **reuses Bruce's existing functions**;
it is not a reimplementation.

## Start here

- `LOCAL_HANDOFF.md` — what's implemented / tested / not, and the next task.
- `docs/STATUS.md` — verified status. `TASKS.md` — backlog.
- `docs/decisions/` — ADRs (BLE transport, protocol+auth, capture+safety).
- `docs/CAPABILITY_MATRIX.md` — what Bruce supports vs what BruceLink exposes.

## Pinned versions

Momentum `mntm-012` (API 87.1) · Bruce `1.17` (`2b6ba4d`, env `m5stack-sticks3`)
· NimBLE-Arduino 2.5.1 · pioarduino 55.03.39.

## Layout

- `shared/brucelink_core/` — portable C99 protocol core (used by FAP + bridge + tests).
- `flipper/brucelink/` — FAP; `brucelink_core/` is a symlink to the shared core.
- `esp32/lib/BruceLink/` — bridge core + NimBLE transport (portable / NimBLE only).
- `esp32/src/modules/brucelink/` — Bruce glue (builds only in the Bruce tree) + `PATCH.md`.
- `esp32/test_compile/` — standalone ESP32-S3 compile check (stub backend).
- `tests/c/` — host C tests (ASan+UBSan). `tests/test_protocol.py` — Python + interop.
- `scripts/hil/` — hardware-in-the-loop scripts.

## Commands

```
make -C tests/c test cppcheck                # core + bridge-core tests (ASan/UBSan) + static analysis
python3 -m unittest discover -s tests -v     # Python ref + C interop
(cd flipper/brucelink && ufbt)               # build FAP
(cd esp32/test_compile && pio run)           # build ESP32 bridge
scripts/hil/build_all.sh                     # everything
```

## Architecture rules

- The shared core stays pure C99, allocation-free, no platform deps. Both devices
  run byte-identical protocol code. Change it only with tests.
- Flipper = BLE peripheral + session initiator. StickS3 = BLE central + session
  responder (ADR 0001).
- Everything Bruce-specific lives behind `BruceBackend`; the bridge core never
  includes Bruce or NimBLE headers.
- New Bruce features: add a capability (`bl_capability.h`) + a backend method.
  No new transport or message type per tool.

## Security rules

- Authorization is the **authenticated connection** (ADR 0002). Do not add
  per-function authorization tokens.
- RF-transmitting capabilities (`BL_CAP_F_ACTIVE_RF`) are **default-off** and pass
  the remote safety gate: session-level enable, e-stop, link-loss auto-stop, rate
  limit, optional target scope (ADR 0003). BruceLink builds no RF frames; it
  forwards to Bruce's existing primitives.
- Never broadcast an active op. Never expose unauthenticated control or
  unrestricted command execution. Bound all payloads (≤ 512 B).
- Never store the pairing key in `bruce.conf` (Bruce prints it to serial).
- Active-RF testing only in owned/authorized, isolated lab environments.

## Testing rule

Code that can run on the host (shared core, bridge core, formatters) must be
**host-tested**, not just cross-compiled. Compile success is not review evidence
for logic. New defects get a regression test reproduced against the old code.

## Definition of done

Code exists · relevant tests pass · builds succeed · errors handled · docs
updated · independently reviewed · hardware-dependent verification explicitly
listed. **Never claim untested functionality works**; never mark a capability
hardware-verified from simulation. Never weaken a test to make it pass.

## Git

Develop on a branch; descriptive commits; run tests before committing; PRs carry
build/test results, limitations, and hardware-verification needs. Do not merge
unverified changes to `main`.
