/*
 * BruceLink ↔ Bruce glue (Bruce 1.17). Builds only inside the Bruce tree.
 * Status: see docs/STATUS.md (compile status in the Bruce tree, hardware).
 * SPDX-License-Identifier: MIT
 */
#if !defined(LITE_VERSION) // like the rest of the module: not in LITE builds
#include "BruceBackendImpl.h"

#include <BruceLinkFormat.h>
#include "BruceLinkJobs.h"

extern "C" {
#include <bl_files.h>
#include <bl_pcap.h> // bl_crc32_*
}

#include <Arduino.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <core/radio_mem.h> // Bruce: radioHasMemForWifi
#include <globals.h> // Bruce: wifiConnected, wifiIP, sdcardMounted, ...

extern int getBattery(); // boards/m5stack-sticks3/interface.cpp:114
extern bool isCharging(); // boards/m5stack-sticks3/interface.cpp:139

namespace brucelink {


size_t BruceBackendImpl::deviceInfo(char* out, size_t cap) {
    // Mirrors Bruce's infoCallback (util_commands.cpp:99) fields. Built with
    // the bounded text builder: the returned length is always the text
    // actually written (never snprintf's would-have-written length).
    bl_textbuf_t tb;
    bl_tb_init(&tb, out, cap);
    bl_tb_str(&tb, "Bruce ");
    bl_tb_str(&tb, BRUCE_VERSION);
    bl_tb_str(&tb, " (");
    bl_tb_str(&tb, GIT_COMMIT_HASH);
    bl_tb_str(&tb, ")\n");
    bl_tb_str(&tb, DEVICE_NAME);
    bl_tb_str(&tb, "\nBatt ");
    bl_tb_int(&tb, getBattery());
    bl_tb_str(&tb, isCharging() ? "% (chg)" : "%");
    bl_tb_str(&tb, "\nHeap "); // internal RAM: what the radios compete for
    bl_tb_uint(&tb, (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    bl_tb_str(&tb, "  PSRAM ");
    bl_tb_uint(&tb, (unsigned long)ESP.getFreePsram());
    bl_tb_str(&tb, "\nDMA blk "); // what gates starting a radio (radio_mem.h)
    bl_tb_uint(&tb, (unsigned long)radioLargestDmaBlock());
    bl_tb_str(&tb, "\nWi-Fi ");
    bl_tb_str(&tb, wifiConnected ? "up " : "down");
    if(wifiConnected) bl_tb_sanitized(&tb, wifiIP.c_str(), 40);
    return tb.len;
}

/* One line on the USB serial log per step, with the memory figures that
 * decide whether a radio can start (see Bruce's core/radio_mem.h). Used to
 * find where a remote operation stops when it does not come back. */
void logStep(const char* what) {
    Serial.printf(
        "[BruceLink] %s | internal %lu dma-blk %lu psram %lu stack-left %lu | %lu ms\n", what,
        (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned long)radioLargestDmaBlock(), (unsigned long)ESP.getFreePsram(),
        (unsigned long)uxTaskGetStackHighWaterMark(nullptr), (unsigned long)millis());
    Serial.flush();
}

bl_job_record_t* BruceBackendImpl::jobRecord() {
    return jobRecordStorage(); // RTC memory: survives the restart (BruceLinkJobs)
}

void BruceBackendImpl::jobRestart() {
    jobRestartRequest(); // the bridge task restarts once the reply has left
}

void BruceBackendImpl::jobCancel() {
    jobRestartCancel(); // e-stop or link loss first: no restart, no job
}

// --- capture and active ops: refused on the live link (ADR 0006) ---------
//
// Both need Wi-Fi, and nothing uses Wi-Fi while the BLE link is up (one radio
// at a time; the two stacks do not fit in the StickS3's internal RAM
// together). They are to run as jobs (task M5.2: handshake capture first).
// Until then the bridge reports capture as disabled and an active op as
// failed, whatever the hooks in bruce_brucelink_hooks.h would do.

bool BruceBackendImpl::captureBackend(const bl_capture_backend_t** be, void** ctx) {
    *be = nullptr;
    *ctx = nullptr;
    return false;
}

int BruceBackendImpl::activeOp(
    uint16_t cap_id,
    const uint8_t* target,
    uint8_t channel,
    uint32_t max_frames) {
    (void)cap_id;
    (void)target;
    (void)channel;
    (void)max_frames;
    return -1; // nothing is transmitted
}

// --- capture files: Bruce's handshake pcaps on LittleFS (bl_files.h) ------
//
// Only regular files directly in kCaptureDir whose names pass
// bl_file_name_ok() are listed or served. The bridge checks every name it
// passes in, so a path is never built from anything else. Runs on the bridge
// task (internal stack), with no radio involved.

const char kCaptureDir[] = "/BrucePCAP/handshakes";

static bool capturePath(const char* name, char* path, size_t cap) {
    if(!bl_file_name_ok(name, strlen(name))) return false;
    const int n = snprintf(path, cap, "%s/%s", kCaptureDir, name);
    return n > 0 && (size_t)n < cap;
}

/* Calls fn(name, size) for each servable file, in directory order, until it
 * returns false. Returns how many it saw. */
template <typename Fn> static int eachCaptureFile(Fn fn) {
    File dir = LittleFS.open(kCaptureDir);
    if(!dir || !dir.isDirectory()) return 0;
    int n = 0;
    for(File f = dir.openNextFile(); f && n < (int)BL_FILE_LIST_MAX; f = dir.openNextFile()) {
        const char* nm = f.name(); // the bare name (arduino-esp32 3.x)
        const bool ok = !f.isDirectory() && nm != nullptr && bl_file_name_ok(nm, strlen(nm));
        const uint32_t size = (uint32_t)f.size();
        if(ok) {
            const bool more = fn(nm, size);
            n++;
            if(!more) {
                f.close();
                break;
            }
        }
        f.close();
    }
    dir.close();
    return n;
}

int BruceBackendImpl::captureFileCount() {
    logStep("files: listing"); // stack-left: LittleFS on the bridge task
    return eachCaptureFile([](const char*, uint32_t) { return true; });
}

bool BruceBackendImpl::captureFileAt(int index, char* name, size_t cap, uint32_t* size) {
    int i = 0;
    bool found = false;
    eachCaptureFile([&](const char* nm, uint32_t sz) {
        if(i++ != index) return true;
        snprintf(name, cap, "%s", nm);
        *size = sz;
        found = true;
        return false;
    });
    return found;
}

bool BruceBackendImpl::captureFileInfo(const char* name, uint32_t* size, uint32_t* crc) {
    char path[sizeof(kCaptureDir) + 1 + BL_FILE_NAME_MAX + 1];
    if(!capturePath(name, path, sizeof(path)) || !LittleFS.exists(path)) return false;
    File f = LittleFS.open(path, FILE_READ);
    if(!f || f.isDirectory()) return false;
    uint32_t c = bl_crc32_init();
    uint8_t buf[256];
    size_t total = 0;
    for(;;) {
        const size_t n = f.read(buf, sizeof(buf));
        if(n == 0) break;
        c = bl_crc32_update(c, buf, n);
        total += n;
    }
    const bool whole = total == f.size();
    f.close();
    logStep("files: info (CRC read)");
    if(!whole) return false; // a read that stopped short: do not vouch for it
    *size = (uint32_t)total;
    *crc = bl_crc32_final(c);
    return true;
}

int BruceBackendImpl::captureFileRead(const char* name, uint32_t offset, uint8_t* buf, size_t len) {
    char path[sizeof(kCaptureDir) + 1 + BL_FILE_NAME_MAX + 1];
    if(!capturePath(name, path, sizeof(path)) || !LittleFS.exists(path)) return -1;
    File f = LittleFS.open(path, FILE_READ);
    if(!f || f.isDirectory()) return -1;
    const size_t size = f.size();
    if(offset > size || !f.seek(offset)) {
        f.close();
        return -1;
    }
    const size_t want = size - offset < len ? size - offset : len;
    const size_t got = want ? f.read(buf, want) : 0;
    f.close();
    return got == want ? (int)got : -1;
}

bool BruceBackendImpl::captureFileDelete(const char* name) {
    char path[sizeof(kCaptureDir) + 1 + BL_FILE_NAME_MAX + 1];
    if(!capturePath(name, path, sizeof(path)) || !LittleFS.exists(path)) return false;
    return LittleFS.remove(path);
}

uint32_t BruceBackendImpl::nowMs() {
    return millis();
}

void BruceBackendImpl::randomBytes(uint8_t* out, size_t len) {
    esp_fill_random(out, len);
}

} // namespace brucelink
#endif // !LITE_VERSION
