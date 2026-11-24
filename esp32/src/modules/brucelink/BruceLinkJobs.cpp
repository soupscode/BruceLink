/*
 * BruceLink jobs on the StickS3 (see BruceLinkJobs.h, ADR 0006).
 * Builds only inside the Bruce tree.
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION)
#include "BruceLinkJobs.h"

#include "BruceBackendImpl.h" // logStep
#include "BruceLinkModule.h"

#include <BruceLinkFormat.h>

extern "C" {
#include <bl_capability.h>
#include <bl_textbuf.h>
}

#include <Arduino.h>
#include <WiFi.h>
#include <core/display.h> // drawMainBorderWithTitle / padprintln / printFootnote
#include <esp_attr.h>
#include <esp_system.h>
#include <globals.h> // check(), AnyKeyPress

namespace brucelink {

/* Not touched by the startup code: what the last boot left here is still
 * here after a restart. Validated (magic, CRC, bounds) before any use. */
static RTC_NOINIT_ATTR bl_job_record_t g_job;
static volatile bool g_restart = false;

bl_job_record_t* jobRecordStorage() {
    return &g_job;
}

void jobRestartRequest() {
    g_restart = true;
}

bool jobRestartRequested() {
    return g_restart;
}

static bool timeUp(uint32_t deadline) {
    return (int32_t)(millis() - deadline) >= 0;
}

static void appendFailure(bl_textbuf_t* tb, const char* step, int code, uint32_t took) {
    bl_tb_str(tb, "scan failed: ");
    bl_tb_str(tb, step);
    bl_tb_str(tb, "\ncode ");
    bl_tb_int(tb, code);
    bl_tb_str(tb, " after ");
    bl_tb_uint(tb, took);
    bl_tb_str(tb, " ms");
}

/* Wi-Fi scan with the radio to itself: the calls Bruce's own scan makes
 * (core/wifi/wifi_common.cpp: WiFi.mode(WIFI_MODE_STA), scanNetworks),
 * asynchronous so a button or the time limit can end it. Strongest first. */
static bl_job_outcome_t wifiScanJob(bl_textbuf_t* tb, uint32_t deadline) {
    const uint32_t t0 = millis();
    WiFi.mode(WIFI_MODE_STA);
    const int started = WiFi.scanNetworks(true /* async */, true /* hidden too */);
    if(started == WIFI_SCAN_FAILED) {
        appendFailure(tb, "start", started, millis() - t0);
        return BL_JOB_FAILED;
    }
    int found;
    while((found = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) {
        if(check(AnyKeyPress)) {
            WiFi.scanDelete();
            bl_tb_str(tb, "Stopped with a button on the StickS3");
            return BL_JOB_STOPPED;
        }
        if(timeUp(deadline)) {
            WiFi.scanDelete();
            bl_tb_str(tb, "The scan did not finish within its time limit");
            return BL_JOB_TIME_LIMIT;
        }
        delay(50);
    }
    const uint32_t took = millis() - t0;
    if(found < 0) {
        appendFailure(tb, "scan", found, took);
        WiFi.scanDelete();
        return BL_JOB_FAILED;
    }
    bl_tb_uint(tb, (unsigned)found);
    bl_tb_str(tb, " networks, ");
    bl_tb_uint(tb, took);
    bl_tb_str(tb, " ms\n");

    // Strongest first; the reply holds about 15 lines.
    static const int kMaxSorted = 64;
    int order[kMaxSorted];
    const int n = found < kMaxSorted ? found : kMaxSorted;
    for(int i = 0; i < n; i++) {
        int j = i;
        while(j > 0 && WiFi.RSSI(order[j - 1]) < WiFi.RSSI(i)) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = i;
    }
    for(int k = 0; k < n; k++) {
        const int i = order[k];
        if(!formatWifiApLine(tb, WiFi.SSID(i).c_str(), WiFi.RSSI(i), WiFi.channel(i))) break;
    }
    WiFi.scanDelete();
    return BL_JOB_DONE;
}

static const char* jobName(uint16_t cap_id) {
    switch(cap_id) {
    case BL_CAP_WIFI_SCAN: return "Wi-Fi scan";
    default: return "unknown job";
    }
}

/* RUN boot: the record is already sealed as RUNNING, so a crash in here
 * is reported as "interrupted" and never re-run. Ends with a restart. */
static void runJob(bl_job_record_t* rec) {
    logStep("job: running (BLE not started)");
    drawMainBorderWithTitle("BruceLink job");
    padprintln("");
    padprintln(jobName(rec->cap_id));
    padprintln("for the Flipper.");
    padprintln("");
    padprintln("It restarts when done.");
    printFootnote("Any button: stop");
    (void)check(AnyKeyPress); // a press from before the job does not count

    char text[BL_JOB_RESULT_MAX];
    bl_textbuf_t tb;
    bl_tb_init(&tb, text, sizeof(text));
    const uint32_t deadline = millis() + (uint32_t)rec->max_s * 1000u;
    bl_job_outcome_t outcome;
    switch(rec->cap_id) {
    case BL_CAP_WIFI_SCAN: outcome = wifiScanJob(&tb, deadline); break;
    default:
        bl_tb_str(&tb, "This StickS3 build cannot run that job");
        outcome = BL_JOB_FAILED;
        break;
    }
    bl_job_record_finish(rec, outcome, text, tb.len);
    logStep("job: finished, restarting");
    drawMainBorderWithTitle("BruceLink job");
    padprintln("");
    padprintln(bl_job_outcome_str((uint8_t)outcome));
    padprintln("Restarting...");
    delay(400);
    esp_restart();
}

} // namespace brucelink

void bruceLinkBoot() {
    using namespace brucelink;
    switch(bl_job_on_boot(&g_job)) {
    case BL_JOB_BOOT_RUN: runJob(&g_job); break; // does not return
    case BL_JOB_BOOT_LINK:
        // The job's result is waiting: bring the link back without a menu
        // trip. resume_link is already cleared, so this happens once.
        logStep("job: back from a job, starting BruceLink");
        if(moduleStart() != kModuleStarted) logStep("job: BruceLink did not start");
        break;
    default: break;
    }
}
#endif // !LITE_VERSION
