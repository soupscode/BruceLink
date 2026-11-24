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
#include <bl_files.h>
#include <bl_textbuf.h>
}

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <core/sd_functions.h> // checkLittleFsSizeNM
#include <esp_heap_caps.h>
#include <esp_wifi.h>
#include <modules/wifi/sniffer.h> // Bruce's sniffer: callback, storage, counters
#include <core/display.h> // drawMainBorderWithTitle / padprintln / printFootnote
#include <esp_attr.h>
#include <esp_system.h>
#include <globals.h> // check(), AnyKeyPress

// Bruce's sniffer globals that sniffer.h does not declare (global scope).
extern uint32_t packet_counter; // sniffer.cpp
extern volatile bool littleFsSpaceAvailable; // sniffer.cpp: its callback stops writing when false

namespace brucelink {

/* Not touched by the startup code: what the last boot left here is still
 * here after a restart. Validated (magic, CRC, bounds) before any use. */
static RTC_NOINIT_ATTR bl_job_record_t g_job;
static volatile bool g_restart = false;
/* "Start BruceLink once at the next boot" (restartIntoBruceLink), kept in
 * the same RTC memory as the job record. Any other value means no. */
static RTC_NOINIT_ATTR uint32_t g_autostart;
static const uint32_t kAutostartMagic = 0x424C4153u; // "BLAS"

/* This boot's decision (bruceLinkBootEarly), acted on again at the end of
 * setup (bruceLinkBoot). */
static bl_job_boot_t g_boot = BL_JOB_BOOT_NONE;
static int g_saved_instant_boot = 0;
static int g_saved_wifi_at_startup = 0;

/* Active scan time per channel. Arduino's default allows up to 300 ms (about
 * 4 s for 14 channels); 120 ms finds the same networks in under 2 s, and is
 * what BruceLink's earlier scan used on this hardware. */
static const uint32_t kScanMsPerChannel = 120;
/* Show the outcome on the stick's screen this long before restarting. */
static const uint32_t kFinishShowMs = 150;

bl_job_record_t* jobRecordStorage() {
    return &g_job;
}

void jobRestartRequest() {
    g_restart = true;
}

bool jobRestartRequested() {
    return g_restart;
}

void jobRestartCancel() {
    g_restart = false;
}

void restartIntoBruceLink() {
    logStep("restarting into BruceLink (heap too fragmented for BLE)");
    g_autostart = kAutostartMagic;
    delay(50);
    esp_restart();
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
static bl_job_outcome_t wifiScanJob(bl_textbuf_t* tb, uint32_t deadline, uint32_t boot_ms) {
    const uint32_t t0 = millis();
    WiFi.mode(WIFI_MODE_STA);
    const int started =
        WiFi.scanNetworks(true /* async */, true /* hidden too */, false /* active */, kScanMsPerChannel);
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
    // Where the time went on the stick, for tuning: the scan itself and how
    // long Bruce took to boot before the job could start.
    bl_tb_uint(tb, (unsigned)found);
    bl_tb_str(tb, " networks (scan ");
    bl_tb_uint(tb, took);
    bl_tb_str(tb, " ms, boot ");
    bl_tb_uint(tb, boot_ms);
    bl_tb_str(tb, " ms)\n");

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

/* ---- handshake capture (BL_CAP_SNIFF_HANDSHAKE) ----------------------
 *
 * Bruce's own sniffer, headless: its promiscuous callback, its writer task
 * and its "handshakes only" mode, which saves each AP's EAPOL frames (and a
 * beacon) to /BrucePCAP/handshakes/HS_<MAC>[_<SSID>].pcap on LittleFS, one
 * open-append-close per write (so the restart that follows loses nothing
 * written). Differences from Bruce's sniffer screen: the radio is a station
 * that only listens (Bruce's starts a hidden AP, which sends beacons), the
 * channel hops by itself when asked to, and it ends at max_s or a button. */

/* Long enough to see a beacon (~100 ms apart) and part of a 4-way handshake,
 * short enough to visit all 12 channels every ~5 s. */
static const uint32_t kHopDwellMs = 400;
static const uint32_t kCaptureRedrawMs = 1000;
static const int kMaxFileSnap = 32;

struct FileSnap {
    char name[BL_FILE_NAME_MAX + 1];
    uint32_t size;
};

static int snapCaptureFiles(FileSnap* out, int max) {
    int n = 0;
    File dir = LittleFS.open(kCaptureDir);
    if(!dir || !dir.isDirectory()) return 0;
    for(File f = dir.openNextFile(); f && n < max; f = dir.openNextFile()) {
        const char* nm = f.name();
        if(!f.isDirectory() && nm && bl_file_name_ok(nm, strlen(nm))) {
            snprintf(out[n].name, sizeof(out[n].name), "%s", nm);
            out[n].size = (uint32_t)f.size();
            n++;
        }
        f.close();
    }
    dir.close();
    return n;
}

static void drawCapture(uint8_t fixed, uint8_t ch, uint32_t left_s) {
    char line[48];
    drawMainBorderWithTitle("BruceLink job");
    padprintln("");
    padprintln("Handshake capture");
    if(fixed) snprintf(line, sizeof(line), "Channel %u", (unsigned)ch);
    else snprintf(line, sizeof(line), "Channel %u (hopping)", (unsigned)ch);
    padprintln(line);
    snprintf(line, sizeof(line), "HS %d  EAPOL %d", num_HS, num_EAPOL);
    padprintln(line);
    snprintf(line, sizeof(line), "%lu packets, %lu s left", (unsigned long)packet_counter, (unsigned long)left_s);
    padprintln(line);
    printFootnote("Any button: stop");
}

static bl_job_outcome_t handshakeJob(
    const bl_job_record_t* rec,
    bl_textbuf_t* tb,
    uint32_t deadline,
    uint32_t boot_ms) {
    const uint8_t fixed = rec->args_len >= 1 ? rec->args[0] : 0; // 0: hop
    const size_t nch = sizeof(all_wifi_channels) / sizeof(all_wifi_channels[0]);
    if(!checkLittleFsSizeNM()) {
        bl_tb_str(tb, "LittleFS is full: no room for captures. Delete some from the Flipper.");
        return BL_JOB_FAILED;
    }
    // What was there before, to report what this run added (heap: the
    // job's boot has memory to spare, and nothing static is kept).
    FileSnap* before = static_cast<FileSnap*>(heap_caps_calloc(kMaxFileSnap, sizeof(FileSnap), MALLOC_CAP_8BIT));
    const int nbefore = before ? snapCaptureFiles(before, kMaxFileSnap) : 0;

    if(!sniffer_prepare_storage(&LittleFS, false)) {
        free(before);
        bl_tb_str(tb, "capture failed: the sniffer's writer could not start");
        return BL_JOB_FAILED;
    }
    sniffer_set_mode(SnifferMode::HandshakesOnly);
    sniffer_reset_handshake_cache();
    littleFsSpaceAvailable = true;

    const uint32_t t0 = millis();
    WiFi.mode(WIFI_MODE_STA); // as the scan job; no network is joined
    size_t hop = 0;
    uint8_t ch = fixed ? fixed : all_wifi_channels[0];
    esp_err_t err = esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
    if(err == ESP_OK) err = esp_wifi_set_promiscuous_rx_cb(sniffer);
    if(err == ESP_OK) err = esp_wifi_set_promiscuous(true);
    if(err != ESP_OK) {
        free(before);
        bl_tb_str(tb, "capture failed: promiscuous mode, code ");
        bl_tb_int(tb, (int)err);
        return BL_JOB_FAILED;
    }

    bl_job_outcome_t outcome = BL_JOB_DONE;
    const char* note = nullptr;
    uint32_t last_hop = millis(), last_draw = 0;
    while(!timeUp(deadline)) {
        if(check(AnyKeyPress)) {
            outcome = BL_JOB_STOPPED;
            break;
        }
        const uint32_t now = millis();
        if(!fixed && now - last_hop >= kHopDwellMs) {
            hop = (hop + 1) % nch;
            ch = all_wifi_channels[hop];
            esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE);
            last_hop = now;
        }
        if(now - last_draw >= kCaptureRedrawMs) {
            // Bruce's own loop does this check: its callback stops saving
            // when LittleFS is out of room.
            littleFsSpaceAvailable = checkLittleFsSizeNM();
            if(!littleFsSpaceAvailable) {
                note = "Stopped early: LittleFS is full.";
                break;
            }
            drawCapture(fixed, ch, (uint32_t)(deadline - now) / 1000u);
            last_draw = now;
        }
        delay(20);
    }
    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(nullptr);
    sniffer_wait_for_flush(2000);
    delay(300); // the writer may still be in its last append
    const uint32_t took = millis() - t0;
    WiFi.mode(WIFI_OFF);

    bl_tb_int(tb, num_HS);
    bl_tb_str(tb, " handshakes, ");
    bl_tb_int(tb, num_EAPOL);
    bl_tb_str(tb, " EAPOL, ");
    bl_tb_uint(tb, packet_counter);
    bl_tb_str(tb, " packets\n");
    if(fixed) {
        bl_tb_str(tb, "channel ");
        bl_tb_uint(tb, fixed);
    } else {
        bl_tb_str(tb, "hopping 1-");
        bl_tb_uint(tb, all_wifi_channels[nch - 1]);
    }
    bl_tb_str(tb, ", ");
    bl_tb_uint(tb, took / 1000u);
    bl_tb_str(tb, " s (boot ");
    bl_tb_uint(tb, boot_ms);
    bl_tb_str(tb, " ms)\n");
    if(note) {
        bl_tb_str(tb, note);
        bl_tb_str(tb, "\n");
    }
    // New or grown files: what the Flipper can download now.
    FileSnap* after = static_cast<FileSnap*>(heap_caps_calloc(kMaxFileSnap, sizeof(FileSnap), MALLOC_CAP_8BIT));
    const int nafter = after ? snapCaptureFiles(after, kMaxFileSnap) : 0;
    int changed = 0;
    for(int i = 0; i < nafter; i++) {
        uint32_t old = 0;
        bool existed = false;
        for(int j = 0; j < nbefore; j++) {
            if(strcmp(before[j].name, after[i].name) == 0) {
                existed = true;
                old = before[j].size;
                break;
            }
        }
        if(existed && old == after[i].size) continue;
        bl_tb_str(tb, after[i].name);
        bl_tb_str(tb, existed ? " +" : " ");
        bl_tb_uint(tb, after[i].size - (existed ? old : 0));
        bl_tb_str(tb, existed ? " B\n" : " B new\n");
        changed++;
    }
    if(changed == 0) bl_tb_str(tb, "No new handshake data.\n");
    bl_tb_str(tb, "LittleFS ");
    bl_tb_uint(tb, (unsigned long)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024u));
    bl_tb_str(tb, " kB free");
    free(before);
    free(after);
    return outcome;
}

static const char* jobName(uint16_t cap_id) {
    switch(cap_id) {
    case BL_CAP_WIFI_SCAN: return "Wi-Fi scan";
    case BL_CAP_SNIFF_HANDSHAKE: return "Handshake capture";
    default: return "unknown job";
    }
}

/* RUN boot: the record is already sealed as RUNNING, so a crash in here
 * is reported as "interrupted" and never re-run. Ends with a restart. */
static void runJob(bl_job_record_t* rec) {
    const uint32_t boot_ms = millis(); // since the app started
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
    case BL_CAP_WIFI_SCAN: outcome = wifiScanJob(&tb, deadline, boot_ms); break;
    case BL_CAP_SNIFF_HANDSHAKE: outcome = handshakeJob(rec, &tb, deadline, boot_ms); break;
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
    delay(kFinishShowMs);
    esp_restart();
}

} // namespace brucelink

void bruceLinkBootEarly() {
    using namespace brucelink;
    g_boot = bl_job_on_boot(&g_job);
    // Asked for once, honoured once: cleared before anything can fail.
    const bool autostart = g_autostart == kAutostartMagic;
    g_autostart = 0;
    if(g_boot == BL_JOB_BOOT_NONE && autostart) g_boot = BL_JOB_BOOT_LINK;
    switch(g_boot) {
    case BL_JOB_BOOT_RUN:
        // Before Bruce's boot animation (up to 7 s), sound, Wi-Fi at
        // startup and serial task: none of them are needed for the job.
        runJob(&g_job); // does not return
        break;
    case BL_JOB_BOOT_LINK:
        // This boot only brings the link back for the job's result. Skip
        // the boot animation and sound, and Bruce's Wi-Fi at startup (one
        // radio at a time: BLE starts at the end of setup). RAM only, put
        // back in bruceLinkBoot(); nothing is saved.
        g_saved_instant_boot = bruceConfig.instantBoot;
        g_saved_wifi_at_startup = bruceConfig.wifiAtStartup;
        bruceConfig.instantBoot = 1;
        bruceConfig.wifiAtStartup = 0;
        break;
    default: break;
    }
}

void bruceLinkBoot() {
    using namespace brucelink;
    if(g_boot != BL_JOB_BOOT_LINK) return;
    g_boot = BL_JOB_BOOT_NONE;
    bruceConfig.instantBoot = g_saved_instant_boot;
    bruceConfig.wifiAtStartup = g_saved_wifi_at_startup;
    // The job's result is waiting (or the user asked for a restart into
    // BruceLink): bring the link back without a menu trip. Both flags are
    // already cleared, so this happens once.
    logStep("boot: starting BruceLink");
    if(moduleStart() != kModuleStarted) logStep("job: BruceLink did not start");
}
#endif // !LITE_VERSION
