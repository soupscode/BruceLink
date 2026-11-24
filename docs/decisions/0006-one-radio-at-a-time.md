# ADR 0006 — One radio at a time: tools run as jobs between restarts

Status: Accepted (2026-11-23, owner's decision) — implemented for the Wi-Fi
scan; **not yet run on hardware**. Supersedes ADR 0004 (Wi-Fi next to the BLE
link). Amends ADR 0003 for active operations.

## Context

The StickS3 has about 133 kB of internal RAM free at Bruce's main menu. The
radio stacks can only use internal RAM in Bruce's build; the 8 MB of PSRAM does
not help them. The BruceLink BLE link takes about 72 kB of it: 57.5 kB to
start and 14 kB more with the Flipper connected (*measured*, ADR 0004).

Bruce's tools are written to own the radio memory. A full Wi-Fi start next to
the link ran out of memory part-way and killed the bridge (ADR 0004). The
reduced, transient Wi-Fi start that replaced it works for a scan with about
1.8 kB to spare, but tools that allocate as they run (captures, portals,
sniffers) cannot be made safe that way: a memory guard can refuse a tool
before it starts, not rescue one that runs out half-way. Restarting BLE after
a session also needs a fresh 48 kB block, which the heap may no longer have
(hardware, 2026-11-23: paired, then "not enough memory").

Options considered (owner chose the third):
1. A separate lightweight firmware: frees the most RAM, but means copying
   Bruce's tools out of Bruce and keeping the copies in sync. That is the
   fork this project exists to avoid.
2. A wired UART link (task M3.6): no radio for the link, live control kept,
   but the stick has to be within cable reach.
3. **Wireless, one radio at a time**: the link and a tool never share the
   radio memory.

## Decision

A capability that needs a radio or memory the link cannot spare runs as a
**job**:

```
Flipper                            StickS3
JOB_START cap, max_s, args  ─────▶ check: authenticated session, known
                                    job capability, max_s in range, not
                                    e-stopped, not the demo key
                            ◀───── JOB_ACCEPTED job_id, max_s
                                    write the job record (RTC memory), restart
            (link drops)            boot: BLE never started, run the tool
                                    (button on the stick stops it; hard
                                    limit max_s), store the result, restart
                                    boot: start BruceLink (BLE) with the key
            (link returns, new session)
JOB_RESULT_REQ job_id      ─────▶
                            ◀───── JOB_RESULT job_id, outcome, text
```

- **Restart on both sides of the job.** Each tool starts from a fresh heap,
  the same memory standalone Bruce gives it, and the link starts from a fresh
  heap afterwards. A restart is the only way back to a known memory layout.
- **Job record** in RTC memory that is not initialised at boot
  (`RTC_NOINIT_ATTR`). It survives software restarts, panics and watchdog
  resets, and is lost on power-off. It holds a magic number, a version, a
  CRC-32, the job (id, capability, max duration, up to 32 bytes of
  arguments), its state and the result (up to 480 bytes of text). An invalid
  record counts as no record.
- **No re-run, ever.** The state goes from *pending* to *running* (sealed
  before the tool starts) to *finished*. A boot that finds *running* means
  the stick reset during the job (crash, watchdog, brown-out). The job is
  then marked *interrupted* and not run again, so a tool that crashes the
  stick cannot start a crash loop, and a transmitting tool cannot be
  restarted by a reset.
- **Auto-start once.** The job's last restart starts BruceLink at the next
  boot. The flag is cleared before BLE is started, so a later reboot (or a
  crash while starting BLE) leaves BruceLink off as usual.
- **Results** are kept until the next job or power-off, and are only given
  out over an authenticated session for the matching job id. Fetching twice
  is fine (a lost reply can be re-requested).
- **Protocol** (v1 additions, generic, not per tool): `JOB_START` 0x22,
  `JOB_ACCEPTED` 0x23, `JOB_RESULT_REQ` 0x24, `JOB_RESULT` 0x25; error
  `USE_JOB` (16) for a job capability invoked inline. Outcomes: done,
  failed, stopped on the stick, time limit, interrupted. Layouts in
  `shared/protocol.md`.
- **Flipper** keeps advertising through the drop and fetches the result as
  soon as a new session is up. It gives up at `max_s` plus 60 s for two
  restarts and the reconnect (`BL_JOB_RESTART_ALLOWANCE_MS`; to be set from
  measurement). It never fetches on the session that started the job.
- **Refused:** jobs on a bridge holding the public demo key (a job restarts
  the device), while e-stopped, and active (RF-transmitting) jobs, until the
  rules below are implemented and tested.
- **What stays inline** (answered on the live link): device info, status,
  capabilities, results, pairing. The Wi-Fi scan becomes a job, and the
  inline Wi-Fi path (`BruceLinkWifi.cpp`, `wifiStartAllowed`) is removed.
  Nothing starts Wi-Fi while the link is up.

### Active (RF-transmitting) jobs — rules for when they are added (amends ADR 0003)

ADR 0003's gate assumes a live link: session enable, e-stop over the link,
auto-stop on link loss. During a job the link is down on purpose, so for
active jobs:

- The gate is applied **when the job is accepted**: real paired key (not the
  demo key), active ops enabled in that session, not e-stopped, a specific
  target (no broadcast), rate limits.
- **Hard duration cap** (`max_s`, bounded per capability), enforced on the
  stick, replaces "stop when the link drops".
- **Stop button on the stick** replaces the remote e-stop: any button ends
  the job, and the screen says so for the whole run.
- **No re-run after a reset** (above).
- Testing only in owned or authorized, isolated lab environments.

## Consequences

- Tools get the same memory as standalone Bruce; a tool that runs out of
  memory fails as it would in Bruce, not by taking the link down with it.
- Each job costs two restarts and a reconnect: time not yet measured (Bruce's
  boot animation adds to it unless "instant boot" is set). There is no live
  view or remote stop during a job.
- RTC memory is lost on power-off: a job interrupted by power loss leaves no
  record, and the Flipper reports that the stick did not come back in time.
- If Bruce's "Wi-Fi at startup" is on, Wi-Fi is connecting while a job runs
  and when BLE comes back. Bruce's own BLE memory check then frees Wi-Fi if
  it has to.
- Host-tested: the record (CRC, bounds, every boot transition including the
  crash case), the payloads, the Flipper-side tracker (timeouts, no fetch on
  the starting session), and the bridge end to end across a simulated
  restart. Not host-testable: RTC retention, the restarts, the boot hook,
  the Wi-Fi scan job on the stick.
