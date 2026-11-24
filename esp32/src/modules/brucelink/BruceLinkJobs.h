/*
 * BruceLink jobs on the StickS3 (ADR 0006): tools that need the radio to
 * themselves run between two restarts, with BLE never started.
 *
 *   Flipper: JOB_START  -> bridge writes the job into jobRecordStorage()
 *                          and calls jobRestartRequest()
 *   bridge task         -> keeps serving the link while the reply leaves
 *                          (an e-stop or a link loss now cancels the job),
 *                          commits (BruceLinkBridge::commitJob), releases
 *                          BLE, restarts
 *   boot (bruceLinkBootEarly, right after the input task starts):
 *       RUN  -> run the tool (any button stops it; its time limit ends it),
 *               store the result, restart
 *       LINK -> skip the boot animation for this boot; at the end of
 *               setup (bruceLinkBoot) start BruceLink with the stored key,
 *               and the Flipper fetches the result over the new session
 *
 * The record lives in RTC memory that is not initialised at boot: it
 * survives restarts, panics and watchdog resets, not a power-off. The
 * transitions (and "never run a job twice") are in the host-tested core
 * (bl_job_on_boot). Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#pragma once

extern "C" {
#include <bl_job.h>
}

namespace brucelink {

/* The record (RTC_NOINIT). */
bl_job_record_t* jobRecordStorage();
/* Ask the bridge task to restart into the job (after its reply is sent). */
void jobRestartRequest();
bool jobRestartRequested();
/* The bridge cancelled the job before the restart (e-stop, link loss). */
void jobRestartCancel();
/* Restart now and start BruceLink once at the next boot, skipping the boot
 * animation and Wi-Fi at startup like the boot after a job. For when the
 * heap is too fragmented for BLE: a restart gives it back in one piece. */
void restartIntoBruceLink();

} // namespace brucelink
