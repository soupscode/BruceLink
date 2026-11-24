/*
 * BruceLink jobs on the StickS3 (ADR 0006): tools that need the radio to
 * themselves run between two restarts, with BLE never started.
 *
 *   Flipper: JOB_START  -> bridge writes the job into jobRecordStorage()
 *                          and calls jobRestartRequest()
 *   bridge task         -> sends the reply, releases BLE, restarts
 *   boot (bruceLinkBoot, end of Bruce's setup):
 *       RUN  -> run the tool (any button stops it; its time limit ends it),
 *               store the result, restart
 *       LINK -> start BruceLink with the stored key; the Flipper fetches
 *               the result over the new session
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

} // namespace brucelink
