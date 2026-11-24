# Decision History

> Consolidates the architectural decisions and why alternatives were rejected or
> abandoned. The authoritative ADRs are in `docs/decisions/`; read them for full
> context + consequences. This doc is the index + the reasoning, including
> decisions not yet captured as ADRs.

## Accepted ADRs (`docs/decisions/`)

| ADR | Decision | Core reasoning | Rejected alternative |
|---|---|---|---|
| **0001** BLE transport | Flipper = BLE peripheral + session initiator; StickS3 = BLE central + responder | Works within Momentum's GATT model and Bruce's NimBLE; no cable | USB/serial and an in-app loopback mock (removed v0.3, in git history) |
| **0002** Protocol & auth | Authorization **is** the authenticated connection; PSK + ephemeral X25519 session | One auth boundary, simple to reason about and test | Per-function authorization tokens (rejected — complexity, no added safety) |
| **0003** Capture & safety | Active-RF capabilities default-off behind a safety gate (enable, e-stop, link-loss stop, rate limit, specific target) | Make dangerous ops opt-in and bounded; BruceLink forwards to Bruce's primitives, builds no RF | Always-on or ungated active ops |
| **0004** Radio memory on the StickS3 | Transient, reduced Wi-Fi start to fit beside BLE | First attempt to run Wi-Fi next to BLE within RAM limits | Full `esp_wifi_init` beside BLE (ran out of internal RAM on hardware) |
| **0005** Pairing | PSK from a pairing exchange; Flipper enclave-encrypted file, StickS3 NVS; public demo key refused on device | Real per-pair keys; no shared secret in the repo on a device | Shipping the demo key as a real key (refused — it is a test-only key) |
| **0006** One radio at a time | Wi-Fi tools run as jobs between BLE-off restarts; never start Wi-Fi from the bridge task; a job is never re-run | Wi-Fi + BLE do not fit in internal RAM on the StickS3 (seen on hardware); avoid coexistence entirely | BLE/Wi-Fi coexistence (abandoned after hardware OOM; superseded ADR 0004's "fit it in" approach) |

## Decided, not yet an ADR

- **ADR 0007 — integration-layer architecture (pending).** BruceLink is a
  management bridge; Bruce owns tool behaviour; a single generic supervised
  native-function job replaces per-tool job code; capabilities come from
  discovery; policy is a per-function allowlist with classes, not CLI
  passthrough; safety stays firmware-side. Agreed 2026-11-24; to be written up,
  with CLAUDE.md and ADRs 0003/0006 revised to match. See `ARCHITECTURE.md` §6.

- **ADR 0002 is partially superseded by the shipping code (reconcile when ADR
  0007 lands).** Two drifts found 2026-11-24 — the **code is authoritative**:
  (a) the session transcript now prepends `hello_min || hello_max` (the
  2026-11-23 downgrade fix in `bl_session.{h,c}`), which ADR 0002's written
  transcript predates; (b) header byte 5, called "reserved (0)" in ADR 0002's
  frame table, is the **request tag** (`BL_OFF_TAG`). `PROTOCOL.md` documents the
  real wire; amend ADR 0002 during the ADR revision. No functional change — the
  code already behaves correctly.

## Decisions on the three open questions (owner, 2026-11-24)

1. **Capture:** keep the working passive capture job as-is; evaluate Bruce's
   native Sniffer as a *separate* capability; do not remove working code until a
   replacement is proven equivalent without regressions; document the
   differences (channel handling, TX behaviour, memory, output format).
2. **RAM trims:** keep them on by default, but make them installer/build
   configurable; do not give back the ~46 kB.
3. **Architecture:** proceed with ADR 0007 + doc/ADR revisions on a dev branch;
   preserve `448b798` as the known-good baseline.

## Approaches rejected or abandoned (lessons)

- **Loopback/mock transport** — removed at the owner's request once BLE worked
  (v0.3).
- **Wi-Fi + BLE coexistence** — abandoned; the hardware proved it does not fit
  (ADR 0006).
- **App-only / three-part flashing** — abandoned; the app-only `firmware.bin` did
  not boot on hardware (cause unknown); flash the **merged** image at
  `0x0` as a single file instead, and re-pair (see `TROUBLESHOOTING_HISTORY.md`).
- **Maintaining a Bruce fork** — avoided; integration is via
  `tools/install_into_bruce.py` patches (reversible with `--uninstall`).
- **Per-tool job/backend code for every Bruce feature** — being superseded by the
  generic supervised job (ADR 0007), to minimise custom firmware and stay
  upstream-compatible.
- **Building new attack tooling / deauth-assisted capture in BruceLink** —
  explicitly out of scope under the bridge reframe; BruceLink exposes Bruce's
  existing tools, it does not add new ones.
