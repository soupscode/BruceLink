# Testing

Test strategy and the **actual** current results. Terse by design.

Authoritative sources: `docs/TEST_RESULTS.md`, `LOCAL_HANDOFF.md` §2/§3,
`docs/STATUS.md`, `docs/HARDWARE_TESTING.md`, `tests/c/`, `tests/*.py`,
`CLAUDE.md` "Testing rule".

**Testing rule (`CLAUDE.md`):** code that can run on the host (shared core,
bridge core, formatters) must be **host-tested**, not just cross-compiled.
Compile success is not review evidence for logic. Every new defect gets a
regression test reproduced against the old code. Never weaken a test to pass.

---

## 1. Automated suites

### Host C — `tests/c/` (ASan + UBSan, strict warnings)

Built by `make -C tests/c test` into two binaries (`tests/c/Makefile`;
harness `tests/c/test_main.c` + `tests/c/bl_test.h`):

- **`bl_tests`** — shared core + the Flipper `bl_gatt_evt` parser:
  - `test_crypto.c` — SHA-256/HMAC/HKDF/X25519 (RFC/NIST known-answer).
  - `test_protocol_core.c` — frame codec + MAC, tamper, malformed, handshake,
    wrong PSK, replay, fragmentation, COBS.
  - `test_capability.c` — capability registry, pcap bytes + CRC-32, remote
    safety gate (unicast-only target, frame budget/refund), capture lifecycle.
  - `test_integration.c` — simulated lossy transport (bulk, corruption,
    dropped fragment, duplicate/replay).
  - `test_textbuf.c` — bounded text builder (truncation, newline injection…).
  - `test_client.c` — `bl_client` handshake/request/timeout logic.
  - `test_gatt_evt.c` — Flipper BLE event parser (RX, subscribe guard, MTU,
    disconnect, malformed, fuzz). This is Flipper app source compiled for the
    host with the strict core flags.
  - `test_pair.c` — pairing core (ADR 0005): happy path, MITM, tampering,
    commitment, timeouts, wipe.
  - `test_job.c` — job record (ADR 0006): RTC record bounds + CRC, bit-flip
    rejection, crash→interrupted/never-re-run, payload limits, Flipper tracker.
  - `test_files.c` — `bl_files` payloads, names, download tracker.
- **`bl_bridge_tests`** — portable bridge core (`esp32/lib/BruceLink/`
  `BruceLinkBridge.cpp`, `BruceLinkFormat.cpp`, `BruceLinkPeerGuard.cpp`)
  driven by a real Flipper-side session through fragmentation against a
  scripted mock `BruceBackend`; test source `tests/c/test_bridge.cpp`. Covers
  reconnect, ESTOP-vs-capture, rate-window/supervision, demo-key refusal,
  pairing mode, jobs end-to-end across simulated restarts.

`NimBleCentralTransport` is **not** in the host build (needs NimBLE); it is
covered only by the §4 ESP32 compile check.

### Python — `tests/` (reference impl + interop + installer)

Run by `python3 -m unittest discover -s tests -v`:

- `test_protocol.py` — independent Python wire reference and **C↔Python
  interop** (`FrameTests`, `CobsTests`, `PcapTests`, `InteropTests`): frame+MAC,
  COBS, pcap, CRC-32 byte-identical; session handshake and pairing recomputed
  from the C output; files. Fails (not skips) if the C vector tool does not
  build.
- `test_install_into_bruce.py` — installer: menu/boot-hook/RAM-trim patches,
  idempotency, uninstall restores the tree, CRLF kept, upgrade removes old
  Config→Advanced entries, a missing anchor writes nothing.
- `test_demo_key.py` — the public demo key appears in **no** Flipper source and
  (when built) not in the `.fap`.
- `test_wrappers.py` — `tools/gen_wrappers.py --check` is clean and no symlinks
  are tracked.

---

## 2. Current verified baseline — commit `448b798`

Ran **2026-11-24 in the cloud session** (per the handoff brief). This is the
known-good baseline on branch `feat/radio-handoff` (zero commits since; PR #8
open). **None of it is hardware.**

| Check | Result |
|---|---|
| `make -C tests/c test` | core **3681** checks + bridge **448** checks, **0 failed** (ASan+UBSan) |
| `make -C tests/c cppcheck` | clean |
| `python3 -m unittest discover -s tests` | **31** tests OK |
| Flipper FAP (`ufbt`, mntm-012) | **v0.7**, APPCHK pass, Target 7 / API 87.1 |
| ESP32 compile check (`esp32/test_compile`) | SUCCESS |
| Bruce 1.17 + BruceLink (`m5stack-sticks3`) | SUCCESS; static internal RAM **91,260 B** (trims on) |

> **GAP — doc lag.** The inline counts in `docs/TEST_RESULTS.md` are the
> 2026-11-23 snapshot (core 3483 + bridge 378, 28 Python; FAP v0.5/v0.7 per
> entry). The numbers above are the later `448b798` run from the brief and
> supersede them; `docs/TEST_RESULTS.md` has not been re-run-stamped to
> `448b798`. Progression over the session: Python 5→11→20→22→23→28→31; core
> checks 1434→1775→3483→3681.

---

## 3. Regression + mutation discipline

Every host-runnable fix carries a regression test that was **run against the
old code (or a mutant restoring it) and failed there**; critical modules are
additionally mutation-checked. Evidence in `docs/TEST_RESULTS.md`:

- **`bl_pair.c`** — six deliberately broken variants (no commitment check,
  confirm never verified, names/nonce dropped from the transcript, secrets
  kept after failure, reveal before checking the peer key) each fail ≥1 test.
- **`bl_job.c`** — seven broken variants (re-run after crash, RUNNING not
  sealed, auto-start never cleared, fetch on the starting session, no CRC
  check, result id unchecked, deadline without the restart allowance) and
  **seven bridge variants** each fail a test.
- **Demo-key refusal** — the bridge test was written first and failed against
  the previous bridge (9 failed checks: "enable" accepted, 16 frames reached
  the mock backend).
- **PR #1 defects** — reproduced against `31629db` (round 1) and `9f89c4d`
  (round 2), then fixed and regression-tested: e.g. PSK lost on reconnect
  (`t_pr1_reconnect_preserves_psk`), ESTOP not blocking `CAP_START`
  (`t_pr1_estop_blocks_capture_start`), rate limit not bounding real frames
  (64 ops → **1024 frames** vs a 64-frame window; `t_pr1_rate_limit_bounds_
  actual_frames`), re-enable resetting the rate window (320 vs 64 frames;
  `t_codex2_reenable_does_not_reset_budget`).
- **Independent-review fixes** — each paired with a test and the old-code
  behaviour it catches (HELLO version range authenticated, seal/open only when
  established, request tags drop a late reply, gate no-wrap / target-always,
  e-stop cleared only by a session, capture stops on drop/e-stop, destructor
  wipes the key). Full table in `docs/TEST_RESULTS.md` → "Independent review
  fixes".

---

## 4. Host-testable vs not

**Host-testable (SOFTWARE-VERIFIED above):** the shared core (crypto, frame,
session, `bl_client`, pairing, job *record* logic, files, safety gate,
capability registry, pcap+CRC), the portable bridge core
(`BruceLinkBridge/Format/PeerGuard`), the Flipper `bl_gatt_evt` parser, and the
Python-interop wire formatters. Plus cross-compile/link: FAP (`ufbt`), ESP32
bridge (`pio run`), full Bruce firmware.

**Not host-testable — hardware (or device build) only** (per
`docs/TEST_RESULTS.md`): RTC `.rtc_noinit` retention across `esp_restart`; the
boot hook inside Bruce's `setup()`; the Wi-Fi scan / capture jobs and their
restart timing; the Flipper's job screen; the Flipper key file (secure
enclave); both pairing screens; the StickS3 NVS key store; radios, screens;
and the `NimBleCentralTransport` (compile-checked only, never executed — the
Flipper BLE transport's firmware-facing code likewise). Treat BLE transport
logic as **UNVERIFIED** until hardware confirms it.

---

## 5. Manual hardware procedures

Full steps in `docs/HARDWARE_TESTING.md` (every procedure there is UNVERIFIED
until run; record results in `docs/TEST_RESULTS.md` and flip cells in
`docs/CAPABILITY_MATRIX.md`). Summary:

- **§0** back up the StickS3 before any flash; active-RF only in owned/isolated
  labs.
- **§1** detect ports + record firmware versions (`scripts/hil/`).
- **§2** build everything; locally use the real `ufbt` toolchain.
- **§3 / §3b** Flipper app on device; BLE profile alone against a phone scanner
  (no StickS3 needed).
- **§4 / §4a / §4b** StickS3 bridge (install + flash); RAM-trim checks; pairing
  (ADR 0005) incl. refusal, wrong-peer, persistence across reboot, Forget,
  copying the key to another Flipper.
- **§5 / §5b** BLE link (discovery, handshake, Device Info/Capabilities,
  counters, MTU, fragmentation, recovery, link supervision); Wi-Fi scan as a
  job incl. stop-on-stick and crash safety.
- **§6** protocol round-trips, ESTOP, link-loss e-stop.
- **§7** handshake capture as a passive job (owned AP+client).
- **§8** capture files on the Flipper (list, resumable download + CRC-32,
  delete).
- **§9** active-RF gate (authorized lab only; not runnable until active jobs
  exist).
- **§10 / Recovery** failure diagnosis and restore.

---

## 6. Status table (4-way)

Legend: **HW** = hardware-verified (owner-reported, not independently verified
by the cloud session) · **SW** = software-verified (host test or compile) ·
**DES** = assumed/designed only · **PEND** = unverified / hardware-pending.

| Area | Status | Notes / evidence |
|---|---|---|
| Crypto, frame+MAC, session + replay, version negotiation, fragmentation, COBS | SW | core host tests + C↔Python interop |
| `bl_client` handshake/request/timeout logic | SW | `test_client.c`; UI path not host-run |
| Flipper `bl_gatt_evt` parser | SW | `test_gatt_evt.c` |
| Pairing core (ADR 0005) | SW | `test_pair.c` (13) + bridge pairing (5); Python recomputation |
| Job record + boot transitions (ADR 0006) | SW | `test_job.c`; bridge end-to-end across simulated restarts |
| Capability registry, capture session mgr, pcap+CRC | SW | host-tested (mock backend) |
| Remote safety gate (default-off, one target, frame budget, e-stop, refund) | SW | host-tested; **nothing transmits yet**; target scope exists in core only, nothing on the wire sets it |
| Installer (menu item, boot hooks, RAM-trim patches) | SW | `test_install_into_bruce.py`; builds inside Bruce |
| FAP build / ESP32 bridge compile / full Bruce build | SW | APPCHK pass / `[SUCCESS]` / links + fits |
| Flipper app UI over the air (Connect, Device Info, Capabilities, exit) | HW | owner-reported 2026-11-23; v0.2 first over loopback, then first OTA link |
| First OTA link Flipper↔StickS3; **Device Info** correct | HW | owner: "flipper shows connected, device info looks right" |
| Capabilities list over the air | HW | owner-reported |
| Handshake time ~3 s (budget 5 s) | HW | owner-measured by eye |
| Toggle BruceLink off/on on the StickS3 | HW | owner-reported; Flipper recovers |
| Pairing on both devices; key survives StickS3 reboot and reconnects | HW | owner-reported, one run; key-id display, wrong-key refusal, Forget **not** checked |
| Wi-Fi scan as a job (full cycle: restart→scan→restart→auto-on→fetch) | HW | owner: "that worked", later "much faster"; RTC record + boot hook + auto-start exercised |
| RAM-trim build `cb11f31` on device | HW | owner: "all working well"; **no memory figures reported**, step 4a items not itemized |
| Flipper BLE transport firmware-facing code (profile start/stop, notify) | PEND | builds + independent source review only; **never executed**; expect to debug (`LOCAL_HANDOFF.md` §6) |
| `NimBleCentralTransport` | PEND | compile-checked only |
| Review-fix build (`448b798`) on either device | PEND | wire changed — both devices must update together |
| Connect straight after pairing (in-place mode switch); "Pair again" while on | PEND | built, not run |
| Forget (both sides); wrong-key refusal; key-id display | PEND | not checked |
| Throughput / negotiated MTU ≥ 247 / 540-byte fragmentation on air | PEND | not measured |
| Job timing figures (`Took`/scan/boot); stop button on stick; crash during job | PEND | not reported / not run |
| **Handshake (EAPOL) capture as a job** | PEND + **not independently reviewed** | bridge side host-tested, builds in Bruce; **the independent review was stopped by a usage safeguard** (`docs/STATUS.md`); `HARDWARE_TESTING.md` §7 |
| Capture files: list / resumable download+CRC / delete | PEND | `bl_files` + bridge host-tested; Flipper screens build, not host-tested; §8 |
| pcap transfer over BLE | PEND | and **unencrypted** (M4.6) |
| Active-RF ops / deauth hook / rate-limit on air | PEND / DES | **refused on the live link** until ADR 0006 active-job rules are implemented + tested; BruceLink builds no RF frames, forwards to Bruce; `HARDWARE_TESTING.md` §9 |
| Serial/UART transport | DES | COBS layer done + tested; UART driver not wired |
| Generic supervised native-function job (ADR 0007) | DES | **proposed/agreed, not implemented**; shipping code still uses per-tool jobs |
| Link confidentiality (AEAD) | DES | frames authenticated, **not encrypted**, until M4.6 |
| BLE + Wi-Fi coexistence | n/a by design | avoided (ADR 0006): one radio at a time; never start Wi-Fi from the bridge task |
| Coexistence with Bruce's own BLE tools while BruceLink is on | PEND | not checked |

**Say it wherever status is reported:** frames are **authenticated but not
encrypted** until M4.6. The public demo key bridge refuses every active op.

---

## See also

- `BUILD_AND_DEPLOY.md` — how to build and run each suite above.
- `docs/TEST_RESULTS.md` — the raw run output, mutation tables, and the dated
  owner hardware reports.
- `docs/HARDWARE_TESTING.md` — the on-device procedures summarised in §5.
- `docs/STATUS.md` — per-item verified/implemented/designed status.
- `LOCAL_HANDOFF.md` §2/§3/§6 — verified vs not, and known risks.
- `docs/CAPABILITY_MATRIX.md` — Bruce support vs BruceLink exposure.
