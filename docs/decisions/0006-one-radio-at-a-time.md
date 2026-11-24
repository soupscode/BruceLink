# ADR 0006 — One radio at a time: tools run as jobs between restarts

Status: Accepted (2026-11-23, owner's decision) — implemented for the Wi-Fi
scan; **the full cycle worked on the owner's devices** (2026-11-23, one run;
too slow at first; "much faster" after the speed-ups below, figures not
reported). Supersedes ADR 0004 (Wi-Fi next to the BLE
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
- **Two hooks in Bruce's `setup()`.** `bruceLinkBootEarly()` right after the
  input task starts: a pending job runs here, before Bruce's boot animation
  (up to 7 s, `boot_screen_anim`), startup sound, Wi-Fi at startup and serial
  task, none of which a job needs. On the boot after a job it skips the
  animation, sound and Wi-Fi at startup for that boot only (RAM, never saved:
  one radio at a time, and the user is waiting). `bruceLinkBoot()` at the end
  of `setup()` puts those settings back and starts BruceLink.
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
- Each job costs two restarts and a reconnect. The first hardware run was
  too slow for the owner (time not reported); it paid Bruce's boot animation
  twice (up to 7 s each) and a 300 ms-per-channel scan. Now skipped and
  120 ms. The result shows the scan time and the stick's boot time, and the
  Flipper the total, so the next run says where the rest goes (expected: the
  BLE reconnect and the handshake, about 3 s). There is no live view or remote
  stop during a job.
- RTC memory is lost on power-off: a job interrupted by power loss leaves no
  record, and the Flipper reports that the stick did not come back in time.
- Bruce's "Wi-Fi at startup" never runs on a job's boots: the job starts
  before it, and the boot after a job skips it. It is back on the next normal
  boot.
- Host-tested: the record (CRC, bounds, every boot transition including the
  crash case), the payloads, the Flipper-side tracker (timeouts, no fetch on
  the starting session), and the bridge end to end across a simulated
  restart. Not host-testable: RTC retention, the restarts, the boot hook,
  the Wi-Fi scan job on the stick.

## Amendment — review fixes (2026-11-23)

- **Cancel before the restart.** Between `JOB_ACCEPTED` and the restart the
  StickS3 keeps serving the link for about 300 ms (the reply has to leave).
  An e-stop or a link loss in that time now cancels the job: the bridge clears
  the PENDING record and the restart request. Only then does the module
  commit (`BruceLinkBridge::commitJob`), release BLE and restart; the link
  drop that follows is the restart itself and cancels nothing. Host-tested
  (`t_job_cancelled_before_the_restart`).
- **What is left of "PENDING has no expiry".** A PENDING record now exists
  only for that 300 ms flush. If the stick crashes or loses power inside it,
  the next boot runs the job (crash) or forgets it (power loss). Harmless for
  a scan. Before active jobs exist, a PENDING record must also carry an
  acceptance time it can be checked against, or be written only at commit.
- **The finish is not atomic.** `bl_job_record_finish` writes about 500 bytes
  and then the CRC; a reset in those microseconds leaves an invalid record.
  The next boot then treats it as no record: no re-run (safe), but no result
  and no automatic restart of BruceLink, so the Flipper reports that the stick
  did not come back in time. Accepted as is; a two-slot record would close it.
- **A job restarts the stick whatever it is doing.** The request comes from
  the Flipper; the stick's own UI is not asked. Do not start a job while using
  the stick for something else (recording, writing a file).
- **No record from an earlier pairing.** Forget pairing and a new pairing
  clear the job record, so a result is never handed to a different Flipper.
- **Restart into BruceLink.** When Turn on finds no 48 kB block for BLE (the
  heap fragments after BLE or Wi-Fi has been used), the stick offers to
  restart and start BruceLink once at the next boot, through the same boot
  path as after a job (a one-shot flag in RTC memory, cleared before use).
- **Capture and active ops** are refused on the live link; they are to run as
  jobs (capture first, task M5.2). Active jobs stay refused (now host-tested,
  `t_active_job_refused`) until the rules above are implemented and tested.
