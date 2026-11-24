/*
 * Host tests for the ESP32 bridge core (esp32/lib/BruceLink) — built with
 * the host C++ compiler under ASan + UBSan. A real Flipper-side (central)
 * session drives the bridge through fragmentation exactly as over BLE,
 * against a scripted mock BruceBackend.
 *
 * Tests named t_pr1_* cover defects reported in review of PR #1. Each
 * bridge-level defect (PSK lost on reconnect, ESTOP not blocking
 * CAP_START, poll failure not stopping the backend, uninitialised
 * capture_, rate limit not bounding real frames, broadcast target
 * accepted) was reproduced against the 31629db sources with an
 * old-API harness before fixing. The Wi-Fi line test exercises the new
 * formatter that replaced the 31629db snprintf loop (which returned
 * SIZE_MAX for cap 0, cut lines mid-SSID, and passed '\n' in SSIDs).
 * SPDX-License-Identifier: MIT
 */
#include "BruceLinkBridge.h"
#include "BruceLinkFormat.h"
#include "BruceLinkPeerGuard.h"

#include <new> // placement new (destructor wipe test)

extern "C" {
#include "bl_client.h"
#include "bl_demo_key.h"
#include "bl_job.h"
#include "bl_reasm.h"
#include "bl_textbuf.h"
#include "bl_uuid.h"
}

#include "bl_test.h"

#include <new>
#include <map>
#include <string>
#include <vector>

int bl_tests_run = 0;
int bl_tests_failed = 0;

using brucelink::BruceBackend;
using brucelink::BruceLinkBridge;

/* ------------------------------------------------------------ mock */

struct MockBackend : public BruceBackend {
    uint32_t now = 10000;
    uint32_t rng = 0xB0B;
    bool have_capture = true;
    /* capture */
    int start_calls = 0, stop_calls = 0, poll_calls = 0;
    int start_rc = BL_OK, poll_rc = BL_OK;
    int raw_available = 0;
    /* active op */
    int active_calls = 0;
    uint32_t last_max = 0;
    uint32_t max_seen = 0;
    long total_sent = 0;
    int send_mode = 0; /* 0 = send max_frames, 1 = send fixed, 2 = overrun, 3 = error */
    int send_fixed = 0;
    /* text */
    bool lie_about_length = false;

    size_t deviceInfo(char* out, size_t cap) override {
        bl_textbuf_t tb;
        bl_tb_init(&tb, out, cap);
        bl_tb_str(&tb, "mock-dev");
        return lie_about_length ? cap + 100 : tb.len;
    }
    /* the record a restart keeps (RTC memory on the StickS3) */
    bl_job_record_t rec;
    int restarts = 0;
    MockBackend() {
        bl_job_record_clear(&rec);
    }
    bl_job_record_t* jobRecord() override {
        return &rec;
    }
    void jobRestart() override {
        restarts++;
    }
    int cancels = 0;
    void jobCancel() override {
        cancels++;
    }
    /* capture files, in memory (bl_files.h) */
    std::map<std::string, std::vector<uint8_t>> files;
    std::vector<std::string> asked; /* every name the bridge passed on */
    int read_overrun = 0; /* report this many bytes more than asked */
    int captureFileCount() override {
        return (int)files.size();
    }
    bool captureFileAt(int index, char* name, size_t cap, uint32_t* size) override {
        int i = 0;
        for(auto& kv : files) {
            if(i++ != index) continue;
            snprintf(name, cap, "%s", kv.first.c_str());
            *size = (uint32_t)kv.second.size();
            return true;
        }
        return false;
    }
    bool captureFileInfo(const char* name, uint32_t* size, uint32_t* crc) override {
        asked.push_back(name);
        auto it = files.find(name);
        if(it == files.end()) return false;
        *size = (uint32_t)it->second.size();
        *crc = bl_crc32(it->second.data(), it->second.size());
        return true;
    }
    int captureFileRead(const char* name, uint32_t offset, uint8_t* buf, size_t len) override {
        asked.push_back(name);
        auto it = files.find(name);
        if(it == files.end() || offset > it->second.size()) return -1;
        size_t n = it->second.size() - offset < len ? it->second.size() - offset : len;
        if(n) memcpy(buf, it->second.data() + offset, n);
        return (int)n + read_overrun;
    }
    bool captureFileDelete(const char* name) override {
        asked.push_back(name);
        return files.erase(name) == 1;
    }
    static int cStart(void* c, bl_capture_mode_t, uint8_t) {
        MockBackend* m = static_cast<MockBackend*>(c);
        m->start_calls++;
        return m->start_rc;
    }
    static int cSetCh(void*, uint8_t) {
        return BL_OK;
    }
    static int cPoll(void* c, bl_capture_counts_t* o) {
        MockBackend* m = static_cast<MockBackend*>(c);
        m->poll_calls++;
        if(m->poll_rc != BL_OK) return m->poll_rc;
        o->packets = (uint32_t)(m->poll_calls * 10);
        return BL_OK;
    }
    static int cStop(void* c) {
        static_cast<MockBackend*>(c)->stop_calls++;
        return BL_OK;
    }
    static int cAvail(void* c, bl_capture_mode_t mode) {
        MockBackend* m = static_cast<MockBackend*>(c);
        return mode == BL_CAP_MODE_RAW ? m->raw_available : 1;
    }
    bl_capture_backend_t vt{cStart, cSetCh, cPoll, cStop, cAvail};

    bool captureBackend(const bl_capture_backend_t** be, void** ctx) override {
        if(!have_capture) return false;
        *be = &vt;
        *ctx = this;
        return true;
    }
    int activeOp(uint16_t, const uint8_t*, uint8_t, uint32_t max_frames) override {
        active_calls++;
        last_max = max_frames;
        if(max_frames > max_seen) max_seen = max_frames;
        int sent = 0;
        if(send_mode == 0) sent = (int)max_frames;
        if(send_mode == 1) sent = send_fixed;
        if(send_mode == 2) sent = (int)max_frames + 5;
        if(send_mode == 3) sent = -1; /* Bruce's transmit failed */
        total_sent += sent;
        return sent;
    }
    uint32_t nowMs() override {
        return now;
    }
    void randomBytes(uint8_t* out, size_t len) override {
        for(size_t i = 0; i < len; i++) {
            rng = rng * 1103515245u + 12345u;
            out[i] = (uint8_t)(rng >> 16);
        }
    }
};

/* --------------------------------------------------------- harness */

struct Msg {
    uint8_t type;
    std::vector<uint8_t> payload;
    std::string text() const {
        return std::string(payload.begin(), payload.end());
    }
    uint16_t err() const {
        return payload.size() >= 2 ? (uint16_t)(payload[0] | (payload[1] << 8)) : 0xFFFF;
    }
};

static void central_rng(void* ctx, uint8_t* buf, size_t len) {
    uint32_t* s = static_cast<uint32_t*>(ctx);
    for(size_t i = 0; i < len; i++) {
        *s = *s * 1664525u + 1013904223u;
        buf[i] = (uint8_t)(*s >> 24);
    }
}

struct Harness {
    MockBackend be;
    BruceLinkBridge bridge;
    std::vector<std::vector<uint8_t>> chunks; /* bridge -> flipper */
    bl_session_t central;
    uint8_t psk[BL_KEY_LEN];
    uint32_t crng = 0xC0DE;
    size_t mtu = 64;

    static void tx(void* ctx, const uint8_t* c, size_t n) {
        static_cast<Harness*>(ctx)->chunks.emplace_back(c, c + n);
    }
    Harness() {
        memset(psk, 0x5c, sizeof(psk));
        memset(&central, 0, sizeof(central));
    }
    void begin() {
        bridge.begin(psk, &be, &Harness::tx, this, mtu);
    }
    void toBridge(const uint8_t* f, size_t n) {
        unsigned i = 0;
        int done = 0;
        while(!done) {
            uint8_t c[600];
            size_t cl = 0;
            done = bl_frag_chunk(f, n, mtu, i++, c, sizeof(c), &cl);
            if(done < 0) return;
            bridge.onChunk(c, cl);
        }
    }
    std::vector<std::vector<uint8_t>> drain() {
        std::vector<std::vector<uint8_t>> frames;
        bl_reasm_t r;
        bl_reasm_init(&r);
        for(auto& c : chunks) {
            const uint8_t* fr = nullptr;
            size_t fl = 0;
            if(bl_reasm_push(&r, c.data(), c.size(), &fr, &fl) == BL_OK && fr)
                frames.emplace_back(fr, fr + fl);
        }
        chunks.clear();
        return frames;
    }
    /* Flipper-side connect with the given key (default: the provisioned one). */
    bool connect(const uint8_t* key = nullptr) {
        bridge.onConnect();
        bl_session_init(&central, BL_ROLE_CENTRAL, key ? key : psk, central_rng, &crng);
        uint8_t b[BL_MAX_MSG], o[BL_MAX_MSG];
        size_t ol = 0;
        int n = bl_session_start(&central, b, sizeof(b));
        if(n <= 0) return false;
        toBridge(b, (size_t)n);
        auto fr = drain();
        if(fr.size() != 1) return false; /* HELLO_ACK */
        if(bl_session_handshake_rx(&central, fr[0].data(), fr[0].size(), o, sizeof(o), &ol) != BL_OK)
            return false;
        toBridge(o, ol); /* AUTH */
        fr = drain();
        if(fr.size() != 1) return false; /* AUTH_OK */
        if(bl_session_handshake_rx(&central, fr[0].data(), fr[0].size(), o, sizeof(o), &ol) != BL_OK)
            return false;
        return bl_session_established(&central) && bridge.established();
    }
    std::vector<Msg> request(uint8_t type, const uint8_t* pl = nullptr, uint16_t len = 0) {
        uint8_t b[BL_MAX_MSG];
        int n = bl_session_seal(&central, type, 0, pl, len, b, sizeof(b));
        if(n > 0) toBridge(b, (size_t)n);
        return collect();
    }
    std::vector<Msg> collect() {
        std::vector<Msg> out;
        for(auto& fr : drain()) {
            bl_frame_t f;
            if(bl_session_open(&central, fr.data(), fr.size(), &f) == BL_OK)
                out.push_back(Msg{f.type, std::vector<uint8_t>(f.payload, f.payload + f.payload_len)});
        }
        return out;
    }
    std::vector<Msg> cmd(uint16_t cap, const std::vector<uint8_t>& args = {}) {
        std::vector<uint8_t> p = {(uint8_t)(cap & 0xFF), (uint8_t)(cap >> 8)};
        p.insert(p.end(), args.begin(), args.end());
        return request(BL_MSG_CMD_REQ, p.data(), (uint16_t)p.size());
    }
    std::vector<Msg> capStart(uint8_t mode, uint8_t ch) {
        uint8_t a[2] = {mode, ch};
        return request(BL_MSG_CAP_START, a, 2);
    }
    std::vector<Msg> jobStart(uint16_t cap, uint16_t max_s = 0, const std::vector<uint8_t>& args = {}) {
        uint8_t p[64];
        int n = bl_job_encode_start(cap, max_s, args.data(), args.size(), p, sizeof(p));
        return request(BL_MSG_JOB_START, p, (uint16_t)n);
    }
    std::vector<Msg> jobResult(uint32_t id) {
        uint8_t p[4];
        bl_job_encode_result_req(id, p, sizeof(p));
        return request(BL_MSG_JOB_RESULT_REQ, p, sizeof(p));
    }
    std::string status() {
        auto r = request(BL_MSG_STATUS_REQ);
        return (r.size() == 1 && r[0].type == BL_MSG_STATUS) ? r[0].text() : std::string("?");
    }
};

static bool isError(const std::vector<Msg>& r, uint16_t code) {
    return r.size() == 1 && r[0].type == BL_MSG_ERROR && r[0].err() == code;
}
static bool contains(const std::string& s, const char* sub) {
    return s.find(sub) != std::string::npos;
}

static const uint8_t kTarget[6] = {0x3c, 0x22, 0xfb, 0x10, 0x20, 0x30};
static std::vector<uint8_t> activeArgs(uint8_t ch = 6, const uint8_t* bssid = kTarget) {
    std::vector<uint8_t> a = {ch};
    a.insert(a.end(), bssid, bssid + 6);
    return a;
}

/* ----------------------------------------------------- regressions */

/* P1 (owner + Codex): PSK was stored only in session_.psk, which
 * bl_session_close() wipes on disconnect -> reconnect used an all-zero key. */
static void t_pr1_reconnect_preserves_psk() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    auto r = h.request(BL_MSG_PING, (const uint8_t*)"x", 1);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_PONG);
    for(int i = 0; i < 3; i++) {
        h.bridge.onDisconnect();
        CHECK(h.connect()); /* failed with BL_E_AUTH before the fix */
        r = h.request(BL_MSG_PING, (const uint8_t*)"y", 1);
        CHECK(r.size() == 1 && r[0].type == BL_MSG_PONG);
    }
    /* authentication is still enforced after reconnects */
    h.bridge.onDisconnect();
    uint8_t wrong[BL_KEY_LEN];
    memset(wrong, 0x11, sizeof(wrong));
    CHECK(!h.connect(wrong));
}

/* Codex P2: ESTOP followed by CAP_START restarted capture in-session. */
static void t_pr1_estop_blocks_capture_start() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    auto r = h.request(BL_MSG_ESTOP);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_ESTOP);
    CHECK(isError(h.capStart(BL_CAP_MODE_HANDSHAKE, 6), BL_ERR_ESTOPPED));
    CHECK_EQ_INT(h.be.start_calls, 0);
    /* a radio job is refused too (and nothing restarts); read-only status
     * still served */
    CHECK(isError(h.jobStart(BL_CAP_WIFI_SCAN), BL_ERR_ESTOPPED));
    CHECK_EQ_INT(h.be.restarts, 0);
    CHECK(contains(h.status(), "estop:1"));
    /* the latch lasts for the session; a new session starts reset */
    h.bridge.onDisconnect();
    CHECK(h.connect());
    r = h.capStart(BL_CAP_MODE_HANDSHAKE, 6);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_STATUS);
    CHECK_EQ_INT(h.be.start_calls, 1);
}

/* Codex P2: a poll failure left the backend running/open, and later
 * disconnect/ESTOP cleanup skipped stop because the state was ERROR. */
static void t_pr1_poll_failure_stops_backend_via_tick() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    auto r = h.capStart(BL_CAP_MODE_HANDSHAKE, 6);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_STATUS);
    CHECK_EQ_INT(h.be.start_calls, 1);
    h.be.poll_rc = BL_E_NO_RESOURCE; /* LittleFS full */
    h.bridge.tick();
    CHECK_EQ_INT(h.be.stop_calls, 1); /* was 0 before the fix */
    std::string st = h.status();
    CHECK(contains(st, "capture:error"));
    CHECK(contains(st, "err:-17")); /* BL_E_NO_RESOURCE */
    /* subsequent cleanup paths do not double-stop */
    h.request(BL_MSG_ESTOP);
    h.bridge.onDisconnect();
    CHECK_EQ_INT(h.be.stop_calls, 1);
}

static void t_pr1_poll_failure_stops_backend_via_status() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.capStart(BL_CAP_MODE_HANDSHAKE, 6);
    h.be.poll_rc = BL_E_NO_RESOURCE;
    CHECK(contains(h.status(), "capture:error"));
    CHECK_EQ_INT(h.be.stop_calls, 1);
    /* operator CAP_STOP clears the error without a second backend stop */
    h.request(BL_MSG_CAP_STOP);
    CHECK_EQ_INT(h.be.stop_calls, 1);
    CHECK(contains(h.status(), "capture:idle"));
}

/* Owner P2: capture_ was initialised only when captureBackend() succeeded
 * (and never in the constructor). */
static void t_pr1_capture_init_without_backend() {
    /* Construct over garbage so an uninitialised member would be visible. */
    alignas(BruceLinkBridge) unsigned char storage[sizeof(BruceLinkBridge)];
    memset(storage, 0xA5, sizeof(storage));
    BruceLinkBridge* b = new(storage) BruceLinkBridge();
    b->onDisconnect(); /* before begin(): must be a clean no-op */
    b->tick();
    b->~BruceLinkBridge();

    Harness h;
    h.be.have_capture = false;
    h.begin();
    CHECK(h.connect());
    CHECK(isError(h.capStart(BL_CAP_MODE_HANDSHAKE, 6), BL_ERR_CAPABILITY_DISABLED));
    CHECK(contains(h.status(), "capture:idle"));
    auto r = h.request(BL_MSG_CAP_STOP);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_STATUS);
    h.request(BL_MSG_ESTOP);
    h.bridge.onDisconnect();
    CHECK_EQ_INT(h.be.start_calls, 0);
}

/* Owner P2: the gate was charged 1 frame per op while activeOp() could
 * transmit many, so the frames-per-window limit did not bound real output. */
static void t_pr1_rate_limit_bounds_actual_frames() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    auto r = h.cmd(brucelink::kCtrlEnableActive, {1});
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
    h.be.send_mode = 0; /* backend uses its whole allowance */
    int ok = 0, limited = 0;
    for(int i = 0; i < 20; i++) {
        r = h.cmd(BL_CAP_DEAUTH, activeArgs());
        if(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP) ok++;
        if(isError(r, BL_ERR_RATE_LIMITED)) limited++;
    }
    /* default window: 64 frames; per-op cap 16 -> exactly 4 ops fit */
    CHECK_EQ_INT(ok, 4);
    CHECK_EQ_INT(limited, 16);
    CHECK(h.be.total_sent <= 64); /* was 20 ops x N frames before the fix */
    CHECK(h.be.max_seen <= brucelink::kMaxFramesPerActiveOp);
    CHECK_EQ_INT(h.be.active_calls, 4); /* refused ops never reach Bruce */
    /* next window */
    h.be.now += 1000;
    r = h.cmd(BL_CAP_DEAUTH, activeArgs());
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
}

static void t_pr1_unused_frames_refunded() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.cmd(brucelink::kCtrlEnableActive, {1});
    h.be.send_mode = 1;
    h.be.send_fixed = 4; /* backend sends fewer than allowed */
    int ok = 0;
    for(int i = 0; i < 40; i++) {
        auto r = h.cmd(BL_CAP_DEAUTH, activeArgs());
        if(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP) ok++;
    }
    CHECK_EQ_INT(ok, 16); /* 64 frames / 4 per op */
    CHECK_EQ_INT(h.be.total_sent, 64);
}

static void t_backend_overrun_engages_estop() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.cmd(brucelink::kCtrlEnableActive, {1});
    h.be.send_mode = 2; /* violates the max_frames contract */
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs()), BL_ERR_INTERNAL));
    CHECK(contains(h.status(), "estop:1"));
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs()), BL_ERR_ESTOPPED));
    CHECK_EQ_INT(h.be.active_calls, 1);
}

/* Broadcast/multicast BSSIDs are not specific targets (wire always carries
 * 6 bytes, so the old NULL-only check never fired). */
static void t_active_rejects_broadcast_and_bad_channel() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    /* default-deny: not enabled */
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs()), BL_ERR_NOT_AUTHORIZED));
    h.cmd(brucelink::kCtrlEnableActive, {1});
    const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    const uint8_t mcast[6] = {0x01, 0x00, 0x5e, 0x7f, 0x00, 0x01};
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs(6, bcast)), BL_ERR_BAD_ARG));
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs(6, mcast)), BL_ERR_BAD_ARG));
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs(0)), BL_ERR_BAD_ARG));
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs(15)), BL_ERR_BAD_ARG));
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, {6, 1, 2}), BL_ERR_BAD_ARG)); /* short */
    CHECK_EQ_INT(h.be.active_calls, 0);
    /* link loss disables active ops */
    h.bridge.onDisconnect();
    CHECK(h.connect());
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs()), BL_ERR_NOT_AUTHORIZED));
}

/* Defence in depth for the owner's P2 length-bookkeeping finding: a backend
 * that over-reports its text length must not cause an over-read. */
static void t_backend_text_length_clamped() {
    Harness h;
    h.be.lie_about_length = true;
    h.begin();
    CHECK(h.connect());
    auto r = h.request(BL_MSG_DEVICE_INFO_REQ);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_DEVICE_INFO);
    CHECK(r[0].text() == "mock-dev");
}

static void t_capture_bad_mode_rejected() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    CHECK(isError(h.capStart(9, 6), BL_ERR_BAD_ARG));
    CHECK(isError(h.capStart(BL_CAP_MODE_HANDSHAKE, 0), BL_ERR_BAD_ARG));
    CHECK(isError(h.capStart(BL_CAP_MODE_RAW, 6), BL_ERR_NO_RESOURCE)); /* no SD */
    CHECK_EQ_INT(h.be.start_calls, 0);
    auto r = h.capStart(BL_CAP_MODE_HANDSHAKE, 6);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_STATUS);
    CHECK(isError(h.capStart(BL_CAP_MODE_HANDSHAKE, 6), BL_ERR_BUSY));
}

static void t_mtu_update_used_for_fragmentation() {
    Harness h;
    h.mtu = 20;
    h.begin();
    CHECK(h.connect());
    h.bridge.setMtu(200);
    h.chunks.clear();
    uint8_t b[BL_MAX_MSG];
    uint8_t pl[150];
    memset(pl, 0x41, sizeof(pl));
    int n = bl_session_seal(&h.central, BL_MSG_PING, 0, pl, sizeof(pl), b, sizeof(b));
    h.toBridge(b, (size_t)n); /* request side still uses h.mtu (20) */
    size_t biggest = 0;
    for(auto& c : h.chunks) biggest = c.size() > biggest ? c.size() : biggest;
    CHECK(biggest > 20 && biggest <= 200); /* response used the new MTU */
    auto r = h.collect();
    CHECK(r.size() == 1 && r[0].type == BL_MSG_PONG && r[0].payload.size() == 150);
}

/* Owner P2 (Wi-Fi scan formatting): long SSIDs, tiny buffers, injection. */
static void t_pr1_wifi_line_bounded_and_sanitized() {
    char buf[256];
    bl_textbuf_t tb;
    bl_tb_init(&tb, buf, sizeof(buf));
    std::string longssid(100, 'A');
    CHECK(brucelink::formatWifiApLine(&tb, longssid.c_str(), -70, 11));
    CHECK(std::string(buf) == std::string(32, 'A') + " -70dBm ch11\n");
    CHECK(brucelink::formatWifiApLine(&tb, "x\nFAKE 0dBm ch1", -40, 1));
    CHECK(std::string(buf).find("x?FAKE") != std::string::npos);
    CHECK(brucelink::formatWifiApLine(&tb, "", -1, 3));
    CHECK(std::string(buf).find("<hidden> -1dBm ch3\n") != std::string::npos);
    /* tiny buffer: line refused whole, length stays exact */
    char tiny[10];
    bl_tb_init(&tb, tiny, sizeof(tiny));
    CHECK(!brucelink::formatWifiApLine(&tb, "Home", -50, 6));
    CHECK_EQ_INT(tb.len, 0);
    CHECK(tiny[0] == '\0');
    /* zero-capacity buffer */
    bl_tb_init(&tb, tiny, 0);
    CHECK(!brucelink::formatWifiApLine(&tb, "Home", -50, 6));
    CHECK_EQ_INT(tb.len, 0);
    /* many APs into a full-size payload: len always equals strlen */
    char pay[BL_MAX_PAYLOAD];
    bl_tb_init(&tb, pay, sizeof(pay));
    int lines = 0;
    for(int i = 0; i < 100; i++)
        if(brucelink::formatWifiApLine(&tb, longssid.c_str(), -99, 14)) lines++;
    CHECK(lines > 0 && lines < 100);
    CHECK_EQ_INT(tb.len, strlen(pay));
    CHECK(tb.len < sizeof(pay));
}

/* ------------------------------ PR #1 second Codex review (9f89c4d) */

/* Codex P2: re-sending "enable active" before each op cleared the rate
 * window (reproduced on 9f89c4d: 20 pairs -> 320 frames vs limit 64). */
static void t_codex2_reenable_does_not_reset_budget() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.be.send_mode = 0;
    int ok = 0;
    for(int i = 0; i < 20; i++) { /* constant clock */
        h.cmd(brucelink::kCtrlEnableActive, {1});
        auto r = h.cmd(BL_CAP_DEAUTH, activeArgs());
        if(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP) ok++;
    }
    CHECK_EQ_INT(ok, 4);
    CHECK(h.be.total_sent <= 64);
}

/* Same bypass via a new session (found while fixing; reproduced on
 * 9f89c4d: reconnect inside the window refilled the budget). */
static void t_codex2_reconnect_does_not_reset_budget() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.cmd(brucelink::kCtrlEnableActive, {1});
    for(int i = 0; i < 4; i++) h.cmd(BL_CAP_DEAUTH, activeArgs());
    CHECK_EQ_INT(h.be.total_sent, 64);
    h.bridge.onDisconnect();
    h.be.now += 500; /* still inside the 1000 ms window */
    CHECK(h.connect());
    h.cmd(brucelink::kCtrlEnableActive, {1});
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs()), BL_ERR_RATE_LIMITED));
    CHECK_EQ_INT(h.be.total_sent, 64);
    h.be.now += 500; /* window elapsed */
    auto r = h.cmd(BL_CAP_DEAUTH, activeArgs());
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
}

/* Codex P2: an impostor advertising the public UUID could hold the only
 * connection. Supervision now asks the transport to drop it. */
static void t_codex2_bad_proof_requests_disconnect() {
    Harness h;
    h.begin();
    h.bridge.onConnect();
    CHECK(!h.bridge.needsDisconnect()); /* fresh link, within deadline */
    /* impostor sends a well-formed HELLO, then an AUTH with a forged proof */
    bl_session_init(&h.central, BL_ROLE_CENTRAL, h.psk, central_rng, &h.crng);
    uint8_t b[BL_MAX_MSG];
    int n = bl_session_start(&h.central, b, sizeof(b));
    h.toBridge(b, (size_t)n);
    CHECK_EQ_INT(h.drain().size(), 1); /* bridge answered HELLO_ACK */
    uint8_t forged[32];
    memset(forged, 0x77, sizeof(forged));
    bl_frame_t f = {BL_WIRE_VERSION, BL_MSG_AUTH, 0, 0, sizeof(forged), forged, 0};
    n = bl_frame_encode(&f, nullptr, 0, b, sizeof(b));
    h.toBridge(b, (size_t)n);
    CHECK(!h.bridge.established());
    CHECK(h.bridge.needsDisconnect()); /* immediately, not after the deadline */
    h.bridge.onDisconnect();
    CHECK(!h.bridge.needsDisconnect()); /* link down: nothing to drop */
}

static void t_codex2_silent_peer_times_out() {
    Harness h;
    h.begin();
    h.bridge.onConnect(); /* transport link up, peer never speaks */
    h.be.now += brucelink::kAuthTimeoutMs - 1;
    CHECK(!h.bridge.needsDisconnect());
    h.be.now += 1;
    CHECK(h.bridge.needsDisconnect());
}

static void t_codex2_stalled_handshake_times_out() {
    Harness h;
    h.begin();
    uint8_t wrong[BL_KEY_LEN];
    memset(wrong, 0x11, sizeof(wrong));
    CHECK(!h.connect(wrong)); /* HELLO/HELLO_ACK exchanged, then it stalls */
    CHECK(!h.bridge.needsDisconnect());
    h.be.now += brucelink::kAuthTimeoutMs;
    CHECK(h.bridge.needsDisconnect());
}

static void t_codex2_established_never_dropped() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.be.now += 3600u * 1000u; /* an hour later */
    CHECK(!h.bridge.needsDisconnect());
    auto r = h.request(BL_MSG_PING, (const uint8_t*)"z", 1);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_PONG);
}

static void t_codex2_unprovisioned_drops_immediately() {
    BruceLinkBridge b; /* begin() never called: no key, no backend */
    CHECK(!b.needsDisconnect());
    b.onConnect();
    CHECK(b.needsDisconnect());
    b.onDisconnect();
    CHECK(!b.needsDisconnect());
}

static void t_codex2_auth_deadline_wraps() {
    Harness h;
    h.begin();
    h.be.now = 0xFFFFF000u; /* millis() about to roll over */
    h.bridge.onConnect();
    h.be.now = 0x00001000u; /* +8192 ms across the wrap */
    CHECK(!h.bridge.needsDisconnect());
    h.be.now = 0x00002000u; /* +12288 ms */
    CHECK(h.bridge.needsDisconnect());
}

static void t_peer_cooldown() {
    brucelink::PeerCooldown c;
    const uint8_t a[7] = {1, 2, 3, 4, 5, 6, 0};
    const uint8_t a_rand[7] = {1, 2, 3, 4, 5, 6, 1}; /* same bytes, other type */
    const uint8_t b[7] = {9, 9, 9, 9, 9, 9, 0};
    CHECK(!c.blocked(a, 7, 1000));
    c.penalize(a, 7, 1000, 30000);
    CHECK(c.blocked(a, 7, 1000));
    CHECK(c.blocked(a, 7, 30999));
    CHECK(!c.blocked(a, 7, 31000)); /* expires */
    CHECK(!c.blocked(a_rand, 7, 2000)); /* address type is part of identity */
    CHECK(!c.blocked(b, 7, 2000));
    /* re-penalising never shortens an active block */
    c.penalize(a, 7, 1000, 30000);
    c.penalize(a, 7, 2000, 1000);
    CHECK(c.blocked(a, 7, 20000));
    /* capacity: more peers than slots evicts the soonest-expiring */
    brucelink::PeerCooldown d;
    for(uint8_t i = 0; i < brucelink::PeerCooldown::kSlots + 2; i++) {
        uint8_t id[7] = {0xAA, i, 0, 0, 0, 0, 0};
        d.penalize(id, 7, 1000u + i, 30000);
    }
    uint8_t newest[7] = {0xAA, (uint8_t)(brucelink::PeerCooldown::kSlots + 1), 0, 0, 0, 0, 0};
    uint8_t oldest[7] = {0xAA, 0, 0, 0, 0, 0, 0};
    CHECK(d.blocked(newest, 7, 2000));
    CHECK(!d.blocked(oldest, 7, 2000));
    /* wraparound */
    brucelink::PeerCooldown w;
    w.penalize(a, 7, 0xFFFFFF00u, 30000);
    CHECK(w.blocked(a, 7, 0x00000100u));
    CHECK(!w.blocked(a, 7, 0x00008000u));
    /* degenerate inputs */
    w.penalize(nullptr, 7, 0, 1000);
    w.penalize(a, 0, 0, 1000);
    CHECK(!w.blocked(nullptr, 7, 0));
    uint8_t longid[20] = {0};
    w.penalize(longid, sizeof(longid), 0, 1000); /* clamped to kIdMax */
    CHECK(w.blocked(longid, sizeof(longid), 10));
}

/* ------------------------------------------- client <-> bridge (e2e) */

/* The Flipper's session client (bl_client) against the real bridge core,
 * through fragmentation in both directions: the two halves that meet for
 * the first time over the air. `drop_to_client` loses the bridge's next
 * reply, as a missed notification would. */
struct Link {
    Harness h;
    bl_client_t cl;
    uint32_t rng = 0xF11B;
    uint32_t now = 500;
    size_t flipper_chunk = BL_BLE_CHUNK_DEFAULT; /* Flipper -> StickS3 */
    bool drop_to_client = false;
    std::vector<Msg> msgs; /* authenticated messages the client surfaced */

    Link() {
        memcpy(h.psk, BRUCELINK_DEMO_PSK, BL_KEY_LEN); /* what both builds ship */
        h.begin();
        bl_client_init(&cl, BRUCELINK_DEMO_PSK, central_rng, &rng);
    }
    void toBridge(const uint8_t* f, size_t n) {
        unsigned i = 0;
        int done = 0;
        while(!done) {
            uint8_t c[BL_BLE_CHUNK_MAX];
            size_t cl_len = 0;
            done = bl_frag_chunk(f, n, flipper_chunk, i++, c, sizeof(c), &cl_len);
            if(done < 0) return;
            h.bridge.onChunk(c, cl_len);
        }
    }
    /* Deliver everything the bridge sent; the client's handshake replies
     * go straight back, until both sides fall silent. */
    void pump() {
        for(int guard = 0; guard < 8; guard++) {
            auto frames = h.drain();
            if(drop_to_client) {
                drop_to_client = false;
                frames.clear();
            }
            if(frames.empty()) return;
            for(auto& fr : frames) {
                uint8_t reply[BL_CLIENT_HS_FRAME_MAX];
                size_t rl = 0;
                bl_frame_t m;
                bool have = false;
                (void)bl_client_rx(&cl, fr.data(), fr.size(), reply, sizeof(reply), &rl, &m, &have);
                if(have) msgs.push_back(Msg{m.type, std::vector<uint8_t>(m.payload, m.payload + m.payload_len)});
                if(rl) toBridge(reply, rl);
            }
        }
    }
    /* A new BLE link: bridge sees the connection, Flipper sees the
     * subscription and sends HELLO. */
    void linkUp() {
        h.bridge.onConnect();
        uint8_t hello[BL_CLIENT_HS_FRAME_MAX];
        int n = bl_client_link_up(&cl, now, hello, sizeof(hello));
        if(n > 0) toBridge(hello, (size_t)n);
        pump();
    }
    void linkDown() {
        h.bridge.onDisconnect();
        bl_client_link_down(&cl);
        h.chunks.clear();
    }
    int request(uint8_t type, const uint8_t* pl = nullptr, uint16_t len = 0) {
        uint8_t out[BL_MAX_MSG];
        int n = bl_client_request(&cl, type, pl, len, now, out, sizeof(out));
        if(n > 0) {
            toBridge(out, (size_t)n);
            pump();
        }
        return n;
    }
};

static void t_e2e_client_handshake_and_requests() {
    Link l;
    l.linkUp();
    CHECK(bl_client_ready(&l.cl));
    CHECK(l.h.bridge.established());
    CHECK(!l.h.bridge.needsDisconnect());

    CHECK(l.request(BL_MSG_DEVICE_INFO_REQ) > 0);
    CHECK(l.msgs.size() == 1 && l.msgs[0].type == BL_MSG_DEVICE_INFO);
    CHECK(l.msgs.size() == 1 && l.msgs[0].text() == "mock-dev");

    /* the Wi-Fi scan only runs as a job now (ADR 0006) */
    const uint8_t cap[2] = {(uint8_t)(BL_CAP_WIFI_SCAN & 0xFF), (uint8_t)(BL_CAP_WIFI_SCAN >> 8)};
    CHECK(l.request(BL_MSG_CMD_REQ, cap, sizeof(cap)) > 0);
    CHECK(l.msgs.size() == 2 && l.msgs[1].type == BL_MSG_ERROR && l.msgs[1].err() == BL_ERR_USE_JOB);
    CHECK_EQ_INT(l.h.be.restarts, 0);

    /* the capability list is the largest reply: multi-chunk both ways */
    CHECK(l.request(BL_MSG_CAPS_REQ) > 0);
    CHECK(l.msgs.size() == 3 && l.msgs[2].type == BL_MSG_CAPS);
    CHECK(l.msgs.size() == 3 &&
          bl_caps_validate(l.msgs[2].payload.data(), l.msgs[2].payload.size()) >= 0);
    CHECK_EQ_INT(bl_client_tick(&l.cl, l.now + 60000), BL_CL_EVT_NONE); /* nothing pending */
}

/* M3.5: a lost handshake frame is recovered by restarting the link. The
 * client gives up (5 s) well before the bridge would drop the peer (10 s),
 * so the Flipper is never the one put on the bridge's cooldown list. */
static void t_e2e_lost_handshake_frame_recovers_by_relink() {
    Link l;
    l.drop_to_client = true; /* HELLO_ACK never arrives */
    l.linkUp();
    CHECK(!bl_client_ready(&l.cl));
    CHECK_EQ_INT(bl_client_tick(&l.cl, l.now + BL_CLIENT_HS_TIMEOUT_MS - 1), BL_CL_EVT_NONE);
    l.now += BL_CLIENT_HS_TIMEOUT_MS;
    l.h.be.now += BL_CLIENT_HS_TIMEOUT_MS;
    CHECK_EQ_INT(bl_client_tick(&l.cl, l.now), BL_CL_EVT_HANDSHAKE_TIMEOUT);
    CHECK(!l.h.bridge.needsDisconnect()); /* bridge deadline not reached */
    CHECK(BL_CLIENT_HS_TIMEOUT_MS < brucelink::kAuthTimeoutMs);

    l.linkDown(); /* app: drop_link; both ends see the disconnect */
    l.linkUp(); /* StickS3 reconnects */
    CHECK(bl_client_ready(&l.cl));
    CHECK(l.h.bridge.established());
    CHECK(l.request(BL_MSG_DEVICE_INFO_REQ) > 0);
    CHECK(l.msgs.size() == 1 && l.msgs[0].type == BL_MSG_DEVICE_INFO);
}

/* M3.5: a lost reply frees the request slot after the timeout and leaves
 * the session usable (sequence numbers stay in step). */
static void t_e2e_lost_reply_times_out_session_survives() {
    Link l;
    l.linkUp();
    CHECK(bl_client_ready(&l.cl));
    l.drop_to_client = true;
    CHECK(l.request(BL_MSG_DEVICE_INFO_REQ) > 0);
    CHECK(l.msgs.empty());
    CHECK_EQ_INT(l.request(BL_MSG_CAPS_REQ), BL_E_BUSY); /* one at a time */
    l.now += BL_CLIENT_REQ_TIMEOUT_MS;
    CHECK_EQ_INT(bl_client_tick(&l.cl, l.now), BL_CL_EVT_REQUEST_TIMEOUT);
    CHECK(bl_client_ready(&l.cl));
    CHECK(l.request(BL_MSG_PING, (const uint8_t*)"k", 1) > 0);
    CHECK(l.msgs.size() == 1 && l.msgs[0].type == BL_MSG_PONG);
}

/* A Flipper holding a different key: it learns so from the handshake, and
 * the bridge asks its transport to drop (and cool down) the peer. */
static void t_e2e_wrong_key_reported_on_both_ends() {
    Link l;
    uint8_t other[BL_KEY_LEN];
    memset(other, 0x42, sizeof(other));
    bl_client_init(&l.cl, other, central_rng, &l.rng);
    l.linkUp();
    CHECK(!bl_client_ready(&l.cl));
    CHECK(!l.h.bridge.established());
    CHECK_EQ_INT(l.cl.state, BL_CL_FAILED);
    CHECK_EQ_INT(l.cl.last_error, BL_E_AUTH);
    CHECK_EQ_INT(l.request(BL_MSG_DEVICE_INFO_REQ), BL_E_STATE);
    l.h.be.now += brucelink::kAuthTimeoutMs;
    CHECK(l.h.bridge.needsDisconnect());
}

/* Link loss mid-request: nothing left pending, and a new link works. */
static void t_e2e_link_loss_mid_request() {
    Link l;
    l.linkUp();
    l.drop_to_client = true;
    CHECK(l.request(BL_MSG_DEVICE_INFO_REQ) > 0);
    l.linkDown();
    CHECK_EQ_INT(bl_client_tick(&l.cl, l.now + 60000), BL_CL_EVT_NONE);
    CHECK_EQ_INT(l.request(BL_MSG_PING), BL_E_STATE);
    l.linkUp();
    CHECK(bl_client_ready(&l.cl));
    CHECK(l.request(BL_MSG_PING, (const uint8_t*)"k", 1) > 0);
    CHECK(l.msgs.size() == 1 && l.msgs[0].type == BL_MSG_PONG);
}

/* The demo key is published, so a session made with it proves nothing
 * about who is on the other end. A bridge provisioned with it must never
 * transmit on a remote request: enabling active ops is refused, an active
 * op is refused, and nothing reaches the backend. Passive requests work. */
static void t_demo_key_never_enables_active_ops() {
    Link l;
    l.linkUp();
    CHECK(bl_client_ready(&l.cl));
    const uint8_t enable[3] = {(uint8_t)(brucelink::kCtrlEnableActive & 0xFF),
                               (uint8_t)(brucelink::kCtrlEnableActive >> 8), 1};
    CHECK(l.request(BL_MSG_CMD_REQ, enable, sizeof(enable)) > 0);
    CHECK(l.msgs.size() == 1 && l.msgs[0].type == BL_MSG_ERROR);
    CHECK(l.msgs.size() == 1 && l.msgs[0].err() == BL_ERR_NOT_AUTHORIZED);

    std::vector<uint8_t> deauth = {(uint8_t)(BL_CAP_DEAUTH & 0xFF),
                                   (uint8_t)(BL_CAP_DEAUTH >> 8)};
    auto args = activeArgs();
    deauth.insert(deauth.end(), args.begin(), args.end());
    CHECK(l.request(BL_MSG_CMD_REQ, deauth.data(), (uint16_t)deauth.size()) > 0);
    CHECK(l.msgs.size() == 2 && l.msgs[1].type == BL_MSG_ERROR);
    CHECK(l.msgs.size() == 2 && l.msgs[1].err() == BL_ERR_NOT_AUTHORIZED);
    CHECK_EQ_INT(l.h.be.active_calls, 0);
    CHECK_EQ_INT(l.h.be.total_sent, 0);

    CHECK(l.request(BL_MSG_STATUS_REQ) > 0);
    CHECK(l.msgs.size() == 3 && contains(l.msgs[2].text(), "active:0"));
    CHECK(l.msgs.size() == 3 && contains(l.msgs[2].text(), "demo:1"));
    CHECK(l.request(BL_MSG_DEVICE_INFO_REQ) > 0); /* passive still served */
    CHECK(l.msgs.size() == 4 && l.msgs[3].type == BL_MSG_DEVICE_INFO);
    /* nor a capture (review 2026-11-23: it was allowed) */
    const uint8_t cap[2] = {(uint8_t)BL_CAP_MODE_HANDSHAKE, 6};
    CHECK(l.request(BL_MSG_CAP_START, cap, sizeof(cap)) > 0);
    CHECK(l.msgs.size() == 5 && l.msgs[4].type == BL_MSG_ERROR);
    CHECK(l.msgs.size() == 5 && l.msgs[4].err() == BL_ERR_NOT_AUTHORIZED);
    CHECK_EQ_INT(l.h.be.start_calls, 0);
}

/* Review 2026-11-23: onConnect() reset the gate, so any BLE link, even one
 * that never authenticated, cleared a latched e-stop. Only an established
 * session may. Also: enable, drop, reconnect -> active ops are off again. */
static void t_estop_cleared_only_by_a_session() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    auto r = h.cmd(brucelink::kCtrlEnableActive, {1});
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
    h.bridge.onDisconnect(); /* link loss latches the e-stop */
    CHECK(h.bridge.estopped());
    h.bridge.onConnect(); /* a bare link: no handshake */
    CHECK(h.bridge.estopped());
    h.be.now += brucelink::kAuthTimeoutMs;
    CHECK(h.bridge.needsDisconnect());
    h.bridge.onDisconnect();
    CHECK(h.bridge.estopped());
    CHECK(h.connect()); /* an authenticated session clears it... */
    CHECK(!h.bridge.estopped());
    CHECK(contains(h.status(), "active:0")); /* ...with active ops off */
    CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs()), BL_ERR_NOT_AUTHORIZED));
    CHECK_EQ_INT(h.be.active_calls, 0);
}

/* Review 2026-11-23 (mutation): deleting bl_capture_stop from onDisconnect
 * or from the ESTOP handler passed every test. */
static void t_capture_stops_on_link_loss_and_estop() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    auto r = h.capStart(BL_CAP_MODE_HANDSHAKE, 6);
    CHECK(r.size() == 1 && r[0].type != BL_MSG_ERROR);
    const int before = h.be.stop_calls;
    h.bridge.onDisconnect();
    CHECK_EQ_INT(h.be.stop_calls, before + 1);

    Harness e;
    e.begin();
    CHECK(e.connect());
    r = e.capStart(BL_CAP_MODE_HANDSHAKE, 6);
    CHECK(r.size() == 1 && r[0].type != BL_MSG_ERROR);
    const int b2 = e.be.stop_calls;
    e.request(BL_MSG_ESTOP);
    CHECK_EQ_INT(e.be.stop_calls, b2 + 1);
    CHECK(contains(e.status(), "estop:1"));
}

static uint32_t acceptedId(const std::vector<Msg>& r, uint16_t* max_s); /* jobs section */

/* Review 2026-11-23 (mutation): the active-job refusal was never run (no
 * active capability is a job yet). A build that adds one must refuse it. */
static void t_active_job_refused() {
    static const bl_capability_desc_t caps[] = {
        {0x7001, "test.active.job", BL_OP_ACTIVE, BL_CAP_F_JOB | BL_CAP_F_ACTIVE_RF | BL_CAP_F_REQUIRES_TARGET, 1},
        {0x7002, "test.rf.job", BL_OP_PASSIVE, BL_CAP_F_JOB | BL_CAP_F_ACTIVE_RF, 1},
        {BL_CAP_WIFI_SCAN, "wifi.scan", BL_OP_PASSIVE, BL_CAP_F_JOB, 1},
    };
    static const bl_capability_registry_t reg = {caps, sizeof(caps) / sizeof(caps[0])};
    Harness h;
    h.begin();
    h.bridge.setRegistry(&reg);
    CHECK(h.connect());
    auto r = h.cmd(brucelink::kCtrlEnableActive, {1}); /* even with active ops on */
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
    CHECK(isError(h.jobStart(0x7001), BL_ERR_CAPABILITY_DISABLED));
    CHECK(isError(h.jobStart(0x7002), BL_ERR_CAPABILITY_DISABLED)); /* flag alone is enough */
    CHECK_EQ_INT(h.be.restarts, 0);
    CHECK(!bl_job_record_valid(&h.be.rec));
    CHECK(acceptedId(h.jobStart(BL_CAP_WIFI_SCAN), nullptr) != 0); /* a passive job still runs */
    h.bridge.setRegistry(nullptr);
}

/* Review 2026-11-23 (mutation): the refund after a failed transmit was not
 * observable. Four failed ops reserve the whole 64-frame window unless
 * they are refunded; the fifth must still get its 16 frames. */
static void t_failed_active_op_is_refunded() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    auto r = h.cmd(brucelink::kCtrlEnableActive, {1});
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
    h.be.send_mode = 3;
    for(int i = 0; i < 4; i++) CHECK(isError(h.cmd(BL_CAP_DEAUTH, activeArgs()), BL_ERR_INTERNAL));
    h.be.send_mode = 0;
    r = h.cmd(BL_CAP_DEAUTH, activeArgs());
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
    CHECK_EQ_INT((long)h.be.max_seen, 16);
}

/* The destructor wipes the provisioned key from the bridge's memory. */
static void t_destructor_wipes_key() {
    alignas(BruceLinkBridge) static unsigned char mem[sizeof(BruceLinkBridge)];
    MockBackend be;
    uint8_t psk[BL_KEY_LEN];
    memset(psk, 0xC7, sizeof(psk));
    BruceLinkBridge* b = new(mem) BruceLinkBridge();
    b->begin(psk, &be, &Harness::tx, nullptr, 64);
    auto has_key = [&]() {
        for(size_t i = 0; i + BL_KEY_LEN <= sizeof(mem); i++)
            if(memcmp(mem + i, psk, BL_KEY_LEN) == 0) return true;
        return false;
    };
    CHECK(has_key());
    b->~BruceLinkBridge();
    CHECK(!has_key());
}

/* A module that stops its task wipes the bridge's key: no session after. */
static void t_clear_key() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.bridge.onDisconnect();
    h.bridge.clearKey();
    CHECK(!h.connect());
    CHECK(h.bridge.needsDisconnect());
}

/* ...and a bridge with its own key is unaffected. */
static void t_real_key_reports_not_demo() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    CHECK(contains(h.status(), "demo:0"));
    auto r = h.cmd(brucelink::kCtrlEnableActive, {1});
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CMD_RESP);
    CHECK(contains(h.status(), "active:1"));
}

/* ---------------------------------------------------- jobs (ADR 0006) */

static uint32_t acceptedId(const std::vector<Msg>& r, uint16_t* max_s = nullptr) {
    uint32_t id = 0;
    uint16_t ms = 0;
    if(r.size() != 1 || r[0].type != BL_MSG_JOB_ACCEPTED) return 0;
    if(bl_job_parse_accepted(r[0].payload.data(), r[0].payload.size(), &id, &ms) != BL_OK) return 0;
    if(max_s) *max_s = ms;
    return id;
}

/* ---------------------------------------- capture files (bl_files.h) */

static void t_files_list_read_delete() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    for(int i = 0; i < 9; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "HS_%012d.pcap", i);
        h.be.files[nm] = std::vector<uint8_t>((size_t)(100 * i), (uint8_t)i);
    }
    h.be.files["bruce.conf"] = std::vector<uint8_t>(10, 'k'); /* never listed or served */
    uint8_t req[BL_MAX_PAYLOAD];
    /* page 0: indexes 0..6 */
    int n = bl_files_encode_list_req(0, req, sizeof(req));
    auto r = h.request(BL_MSG_CAP_LIST, req, (uint16_t)n);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CAP_LIST);
    bl_file_entry_t e[BL_FILE_LIST_PAGE];
    uint8_t total = 0, start = 0;
    size_t count = 0;
    CHECK(r.size() == 1 &&
          bl_files_parse_list(r[0].payload.data(), r[0].payload.size(), &total, &start, e, 7, &count) == BL_OK);
    CHECK_EQ_INT(total, 10);
    CHECK_EQ_INT((long)count, 7);
    CHECK(strcmp(e[0].name, "HS_000000000000.pcap") == 0);
    /* page 1: indexes 7..9; bruce.conf (index 9) is left out */
    n = bl_files_encode_list_req(7, req, sizeof(req));
    r = h.request(BL_MSG_CAP_LIST, req, (uint16_t)n);
    CHECK(r.size() == 1 &&
          bl_files_parse_list(r[0].payload.data(), r[0].payload.size(), &total, &start, e, 7, &count) == BL_OK);
    CHECK_EQ_INT((long)count, 2);
    CHECK_EQ_INT((long)e[1].size, 800);
    CHECK(strcmp(e[1].name, "HS_000000000008.pcap") == 0);

    /* a whole download through the Flipper's tracker */
    bl_xferc_t x;
    CHECK_EQ_INT(bl_xferc_start(&x, "HS_000000000005.pcap", 20), BL_OK);
    std::vector<uint8_t> got;
    for(int guard = 0; guard < 10 && bl_xferc_active(&x); guard++) {
        uint8_t type = 0;
        n = bl_xferc_next(&x, &type, req, sizeof(req));
        CHECK(n > 0);
        r = h.request(type, req, (uint16_t)n);
        CHECK(r.size() == 1);
        if(r.size() != 1) break;
        const uint8_t* d = nullptr;
        size_t dl = 0;
        uint32_t at = 0;
        CHECK(bl_xferc_on_response(&x, r[0].type, r[0].payload.data(), r[0].payload.size(), &d, &dl, &at));
        got.insert(got.end(), d, d + dl);
    }
    CHECK_EQ_INT(x.state, BL_XFER_DONE);
    CHECK(got == h.be.files["HS_000000000005.pcap"]);

    /* delete, then it is gone */
    n = bl_files_encode_name("HS_000000000005.pcap", 20, req, sizeof(req));
    r = h.request(BL_MSG_CAP_DELETE, req, (uint16_t)n);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CAP_DELETE && r[0].payload.empty());
    CHECK(h.be.files.count("HS_000000000005.pcap") == 0);
    CHECK(isError(h.request(BL_MSG_CAP_DELETE, req, (uint16_t)n), BL_ERR_NOT_FOUND));
    CHECK(isError(h.request(BL_MSG_CAP_FILE_INFO, req, (uint16_t)n), BL_ERR_NOT_FOUND));
}

/* Only names that pass bl_file_name_ok ever reach the backend. */
static void t_files_refuse_bad_names_and_demo_key() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    h.be.files["HS_A.pcap"] = std::vector<uint8_t>(10, 1);
    const char* bad[] = {"../bruce.conf", "bruce.conf", "/HS_A.pcap", "a/../HS_A.pcap", ".pcap", "HS_A.pcap/"};
    for(const char* b : bad) {
        std::vector<uint8_t> p = {(uint8_t)strlen(b)};
        p.insert(p.end(), b, b + strlen(b));
        CHECK(isError(h.request(BL_MSG_CAP_FILE_INFO, p.data(), (uint16_t)p.size()), BL_ERR_BAD_ARG));
        CHECK(isError(h.request(BL_MSG_CAP_DELETE, p.data(), (uint16_t)p.size()), BL_ERR_BAD_ARG));
        std::vector<uint8_t> q = {0, 0, 0, 0, 16, 0};
        q.insert(q.end(), p.begin(), p.end());
        CHECK(isError(h.request(BL_MSG_CAP_XFER_DATA, q.data(), (uint16_t)q.size()), BL_ERR_BAD_ARG));
    }
    CHECK(h.be.asked.empty());
    /* a read past the end, an oversized ask, a backend that overruns */
    uint8_t req[BL_MAX_PAYLOAD];
    int n = bl_files_encode_read_req(11, 16, "HS_A.pcap", 9, req, sizeof(req));
    CHECK(isError(h.request(BL_MSG_CAP_XFER_DATA, req, (uint16_t)n), BL_ERR_NOT_FOUND));
    n = bl_files_encode_read_req(10, 16, "HS_A.pcap", 9, req, sizeof(req));
    auto r = h.request(BL_MSG_CAP_XFER_DATA, req, (uint16_t)n);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_CAP_XFER_DATA && r[0].payload.size() == 4); /* at the end */
    req[4] = 0xFF; /* max_len 0x01FF = 511 > BL_FILE_CHUNK_MAX */
    req[5] = 0x01;
    CHECK(isError(h.request(BL_MSG_CAP_XFER_DATA, req, (uint16_t)n), BL_ERR_BAD_ARG));
    h.be.read_overrun = 1;
    n = bl_files_encode_read_req(0, 4, "HS_A.pcap", 9, req, sizeof(req));
    CHECK(isError(h.request(BL_MSG_CAP_XFER_DATA, req, (uint16_t)n), BL_ERR_INTERNAL));

    /* the public demo key gets no capture files */
    Harness d;
    memcpy(d.psk, BRUCELINK_DEMO_PSK, BL_KEY_LEN);
    d.begin();
    CHECK(d.connect());
    d.be.files["HS_A.pcap"] = std::vector<uint8_t>(10, 1);
    n = bl_files_encode_list_req(0, req, sizeof(req));
    CHECK(isError(d.request(BL_MSG_CAP_LIST, req, (uint16_t)n), BL_ERR_NOT_AUTHORIZED));
    n = bl_files_encode_name("HS_A.pcap", 9, req, sizeof(req));
    CHECK(isError(d.request(BL_MSG_CAP_FILE_INFO, req, (uint16_t)n), BL_ERR_NOT_AUTHORIZED));
    CHECK(isError(d.request(BL_MSG_CAP_DELETE, req, (uint16_t)n), BL_ERR_NOT_AUTHORIZED));
    /* nor a read by a guessed name (review 2026-11-23: untested) */
    n = bl_files_encode_read_req(0, 16, "HS_A.pcap", 9, req, sizeof(req));
    CHECK(isError(d.request(BL_MSG_CAP_XFER_DATA, req, (uint16_t)n), BL_ERR_NOT_AUTHORIZED));
    CHECK(d.be.files.size() == 1);
    CHECK(d.be.asked.empty());
}

/* The handshake capture is a job; its channel argument is bounded. */
static void t_sniff_job() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    CHECK(isError(h.cmd(BL_CAP_SNIFF_HANDSHAKE), BL_ERR_USE_JOB));
    CHECK(isError(h.jobStart(BL_CAP_SNIFF_HANDSHAKE, 301), BL_ERR_BAD_ARG));
    CHECK(isError(h.jobStart(BL_CAP_SNIFF_HANDSHAKE, 60, {14}), BL_ERR_BAD_ARG));
    CHECK(isError(h.jobStart(BL_CAP_SNIFF_HANDSHAKE, 60, {6, 1}), BL_ERR_BAD_ARG));
    CHECK_EQ_INT(h.be.restarts, 0);
    uint16_t ms = 0;
    CHECK(acceptedId(h.jobStart(BL_CAP_SNIFF_HANDSHAKE, 0, {6}), &ms) != 0);
    CHECK_EQ_INT(ms, 60);
    CHECK_EQ_INT(h.be.rec.args_len, 1);
    CHECK_EQ_INT(h.be.rec.args[0], 6);
    Harness g;
    g.begin();
    CHECK(g.connect());
    CHECK(acceptedId(g.jobStart(BL_CAP_SNIFF_HANDSHAKE, 300, {0}), &ms) != 0); /* hop, the longest */
    CHECK_EQ_INT(ms, 300);
}

static void t_job_refusals() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    CHECK(isError(h.jobStart(0x7777), BL_ERR_UNKNOWN_CAPABILITY));
    CHECK(isError(h.jobStart(BL_CAP_DEVICE_INFO), BL_ERR_BAD_ARG)); /* answered inline */
    CHECK(isError(h.jobStart(BL_CAP_DEAUTH), BL_ERR_BAD_ARG)); /* not a job (yet) */
    CHECK(isError(h.jobStart(BL_CAP_WIFI_SCAN, 61), BL_ERR_BAD_ARG)); /* over its limit */
    CHECK(isError(h.jobStart(BL_CAP_WIFI_SCAN, 0, {1}), BL_ERR_BAD_ARG)); /* takes no args */
    CHECK(isError(h.request(BL_MSG_JOB_START, (const uint8_t*)"\x10", 1), BL_ERR_BAD_ARG));
    CHECK(isError(h.cmd(BL_CAP_WIFI_SCAN), BL_ERR_USE_JOB));
    CHECK(isError(h.jobResult(1), BL_ERR_NOT_FOUND)); /* nothing stored */
    CHECK(isError(h.request(BL_MSG_JOB_RESULT_REQ, (const uint8_t*)"\x01\x02", 2), BL_ERR_BAD_ARG));
    CHECK_EQ_INT(h.be.restarts, 0);
    CHECK(!bl_job_record_valid(&h.be.rec)); /* nothing written */

    /* the public demo key: no job (a job restarts the device) */
    Harness d;
    memcpy(d.psk, BRUCELINK_DEMO_PSK, BL_KEY_LEN);
    d.begin();
    CHECK(d.connect());
    CHECK(isError(d.jobStart(BL_CAP_WIFI_SCAN), BL_ERR_NOT_AUTHORIZED));
    CHECK_EQ_INT(d.be.restarts, 0);
    CHECK(!bl_job_record_valid(&d.be.rec));
}

static void t_job_accept_and_busy() {
    Harness h;
    h.begin();
    CHECK(h.connect());
    uint16_t ms = 0;
    const uint32_t id = acceptedId(h.jobStart(BL_CAP_WIFI_SCAN), &ms);
    CHECK(id != 0);
    CHECK_EQ_INT(ms, 20); /* the capability's default */
    CHECK_EQ_INT(h.be.restarts, 1);
    CHECK(bl_job_record_valid(&h.be.rec));
    CHECK_EQ_INT(h.be.rec.state, BL_JOB_S_PENDING);
    CHECK_EQ_INT(h.be.rec.job_id, id);
    CHECK_EQ_INT(h.be.rec.cap_id, BL_CAP_WIFI_SCAN);
    /* before the restart happens: one job at a time, no result yet */
    CHECK(isError(h.jobStart(BL_CAP_WIFI_SCAN), BL_ERR_BUSY));
    CHECK(isError(h.jobResult(id), BL_ERR_BUSY));
    CHECK_EQ_INT(h.be.restarts, 1);
    CHECK_EQ_INT(h.be.rec.job_id, id); /* untouched */
    /* an explicit limit is kept */
    Harness h2;
    h2.begin();
    CHECK(h2.connect());
    CHECK(acceptedId(h2.jobStart(BL_CAP_WIFI_SCAN, 60), &ms) != 0);
    CHECK_EQ_INT(ms, 60);
}

/* The whole job, Flipper client + tracker against the bridge, across the
 * StickS3's two restarts (a new bridge each boot, the record carried over
 * as RTC memory would carry it). */
struct JobLink {
    Harness* h = nullptr;
    bl_client_t cl;
    bl_jobc_t job;
    uint32_t rng = 0x10B;
    uint32_t now = 1000;
    uint8_t key[BL_KEY_LEN];
    std::vector<Msg> msgs;
    const char* text = nullptr;
    size_t text_len = 0;
    std::string result;

    JobLink() {
        memset(key, 0x6b, sizeof(key)); /* a paired key */
        h = new Harness();
        memcpy(h->psk, key, BL_KEY_LEN);
        h->begin();
        bl_client_init(&cl, key, central_rng, &rng);
        bl_jobc_init(&job);
    }
    ~JobLink() {
        delete h;
    }
    /* esp_restart(): a fresh bridge; only the record survives */
    void restartStick() {
        bl_job_record_t keep;
        memcpy(&keep, &h->be.rec, sizeof(keep));
        delete h;
        h = new Harness();
        memcpy(h->psk, key, BL_KEY_LEN);
        memcpy(&h->be.rec, &keep, sizeof(keep));
        h->begin();
    }
    void toBridge(const uint8_t* f, size_t n) {
        unsigned i = 0;
        int done = 0;
        while(!done) {
            uint8_t c[BL_BLE_CHUNK_MAX];
            size_t cl_len = 0;
            done = bl_frag_chunk(f, n, BL_BLE_CHUNK_DEFAULT, i++, c, sizeof(c), &cl_len);
            if(done < 0) return;
            h->bridge.onChunk(c, cl_len);
        }
    }
    void pump() {
        for(int guard = 0; guard < 8; guard++) {
            auto frames = h->drain();
            if(frames.empty()) return;
            for(auto& fr : frames) {
                uint8_t reply[BL_CLIENT_HS_FRAME_MAX];
                size_t rl = 0;
                bl_frame_t m;
                bool have = false;
                (void)bl_client_rx(&cl, fr.data(), fr.size(), reply, sizeof(reply), &rl, &m, &have);
                if(have) {
                    msgs.push_back(Msg{m.type, std::vector<uint8_t>(m.payload, m.payload + m.payload_len)});
                    /* what the app does with every response */
                    if(bl_jobc_on_response(&job, m.type, m.payload, m.payload_len, now, &text, &text_len) &&
                       text)
                        result.assign(text, text_len);
                }
                if(rl) toBridge(reply, rl);
            }
        }
    }
    void linkUp() {
        h->bridge.onConnect();
        uint8_t hello[BL_CLIENT_HS_FRAME_MAX];
        int n = bl_client_link_up(&cl, now, hello, sizeof(hello));
        if(n > 0) toBridge(hello, (size_t)n);
        pump();
    }
    void linkDown() {
        h->bridge.onDisconnect();
        bl_client_link_down(&cl);
        bl_jobc_link_down(&job);
        h->chunks.clear();
    }
    /* what the module's restartForJob() does: commit, then drop the link */
    void restartsForJob() {
        CHECK(h->bridge.commitJob());
        linkDown();
    }
    bool send(uint8_t type, const uint8_t* pl, size_t len) {
        uint8_t out[BL_MAX_MSG];
        int n = bl_client_request(&cl, type, pl, (uint16_t)len, now, out, sizeof(out));
        if(n <= 0) return false;
        toBridge(out, (size_t)n);
        pump();
        return true;
    }
    /* the app's tick: fetch as soon as a new session is up */
    void tick() {
        if(bl_jobc_want_fetch(&job, bl_client_ready(&cl))) {
            uint8_t p[8];
            int n = bl_jobc_fetch(&job, p, sizeof(p));
            if(n > 0) send(BL_MSG_JOB_RESULT_REQ, p, (size_t)n);
        }
        bl_jobc_tick(&job, now);
    }
};

static void t_job_e2e_across_restarts() {
    JobLink l;
    l.linkUp();
    CHECK(bl_client_ready(&l.cl));
    uint8_t p[16];
    int n = bl_jobc_start(&l.job, BL_CAP_WIFI_SCAN, 0, nullptr, 0, l.now, p, sizeof(p));
    CHECK(n > 0 && l.send(BL_MSG_JOB_START, p, (size_t)n));
    CHECK_EQ_INT(l.job.state, BL_JOBC_RUNNING);
    CHECK_EQ_INT(l.h->be.restarts, 1);
    const uint32_t id = l.job.job_id;
    CHECK(id != 0 && id == l.h->be.rec.job_id);
    l.tick(); /* same session: no fetch */
    CHECK_EQ_INT(l.job.state, BL_JOBC_RUNNING);

    /* boot 1: run the job with the link down */
    l.restartsForJob();
    l.restartStick();
    CHECK_EQ_INT(bl_job_on_boot(&l.h->be.rec), BL_JOB_BOOT_RUN);
    const char scan[] = "2 networks, 2210 ms\nHome -50dBm ch6\nCafe -71dBm ch11\n";
    bl_job_record_finish(&l.h->be.rec, BL_JOB_DONE, scan, sizeof(scan) - 1);
    l.now += 9000;
    l.tick();
    CHECK_EQ_INT(l.job.state, BL_JOBC_RUNNING); /* still waiting, no session */

    /* boot 2: BruceLink comes back by itself */
    l.restartStick();
    CHECK_EQ_INT(bl_job_on_boot(&l.h->be.rec), BL_JOB_BOOT_LINK);
    l.now += 9000;
    l.linkUp(); /* new session with the paired key */
    CHECK(bl_client_ready(&l.cl));
    l.tick(); /* fetches */
    CHECK_EQ_INT(l.job.state, BL_JOBC_DONE);
    CHECK_EQ_INT(l.job.outcome, BL_JOB_DONE);
    CHECK(l.result == scan);

    /* asking again (a lost reply) works; another id does not */
    uint8_t q[4];
    bl_job_encode_result_req(id, q, sizeof(q));
    l.msgs.clear();
    CHECK(l.send(BL_MSG_JOB_RESULT_REQ, q, sizeof(q)));
    CHECK(l.msgs.size() == 1 && l.msgs[0].type == BL_MSG_JOB_RESULT);
    bl_job_encode_result_req(id + 1, q, sizeof(q));
    l.msgs.clear();
    CHECK(l.send(BL_MSG_JOB_RESULT_REQ, q, sizeof(q)));
    CHECK(l.msgs.size() == 1 && l.msgs[0].type == BL_MSG_ERROR && l.msgs[0].err() == BL_ERR_NOT_FOUND);
    /* a later reboot is a normal boot; the result is still kept */
    l.linkDown();
    l.restartStick();
    CHECK_EQ_INT(bl_job_on_boot(&l.h->be.rec), BL_JOB_BOOT_NONE);
    CHECK(bl_job_record_valid(&l.h->be.rec));
}

static void t_job_e2e_crash_and_power_loss() {
    /* the tool crashes the StickS3: reported as interrupted, not re-run */
    JobLink a;
    a.linkUp();
    uint8_t p[16];
    int n = bl_jobc_start(&a.job, BL_CAP_WIFI_SCAN, 0, nullptr, 0, a.now, p, sizeof(p));
    a.send(BL_MSG_JOB_START, p, (size_t)n);
    a.restartsForJob();
    a.restartStick();
    CHECK_EQ_INT(bl_job_on_boot(&a.h->be.rec), BL_JOB_BOOT_RUN);
    a.restartStick(); /* panic */
    CHECK_EQ_INT(bl_job_on_boot(&a.h->be.rec), BL_JOB_BOOT_LINK);
    a.linkUp();
    a.tick();
    CHECK_EQ_INT(a.job.state, BL_JOBC_DONE);
    CHECK_EQ_INT(a.job.outcome, BL_JOB_INTERRUPTED);
    CHECK(contains(a.result, "not run again"));

    /* power lost: RTC memory is gone, the StickS3 has no such job */
    JobLink b;
    b.linkUp();
    n = bl_jobc_start(&b.job, BL_CAP_WIFI_SCAN, 0, nullptr, 0, b.now, p, sizeof(p));
    b.send(BL_MSG_JOB_START, p, (size_t)n);
    b.restartsForJob();
    b.restartStick();
    memset(&b.h->be.rec, 0xA5, sizeof(b.h->be.rec)); /* RTC after power-on */
    CHECK_EQ_INT(bl_job_on_boot(&b.h->be.rec), BL_JOB_BOOT_NONE);
    b.linkUp(); /* the user turned BruceLink on again */
    b.tick();
    CHECK(b.job.state == BL_JOBC_FAILED && b.job.fail == BL_JOBC_F_LOST);

    /* never comes back at all: the Flipper gives up at max_s + allowance */
    JobLink c;
    c.linkUp();
    n = bl_jobc_start(&c.job, BL_CAP_WIFI_SCAN, 0, nullptr, 0, c.now, p, sizeof(p));
    c.send(BL_MSG_JOB_START, p, (size_t)n);
    c.restartsForJob();
    c.now += 20000u + BL_JOB_RESTART_ALLOWANCE_MS - 1;
    c.tick();
    CHECK_EQ_INT(c.job.state, BL_JOBC_RUNNING);
    c.now += 1;
    c.tick();
    CHECK(c.job.state == BL_JOBC_FAILED && c.job.fail == BL_JOBC_F_TIMEOUT);
}

/* Review 2026-11-23: an e-stop or link loss after JOB_ACCEPTED but before
 * the restart left the record PENDING, so the job ran at the next boot. */
static void t_job_cancelled_before_the_restart() {
    /* e-stop from the Flipper while the stick flushes the reply */
    Harness h;
    h.begin();
    CHECK(h.connect());
    CHECK(acceptedId(h.jobStart(BL_CAP_WIFI_SCAN)) != 0);
    CHECK(h.bridge.jobPending());
    auto r = h.request(BL_MSG_ESTOP);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_ESTOP);
    CHECK_EQ_INT(h.be.cancels, 1);
    CHECK(!bl_job_record_valid(&h.be.rec));
    CHECK(!h.bridge.commitJob()); /* so the module does not restart */
    CHECK(!h.bridge.jobPending());
    CHECK_EQ_INT(bl_job_on_boot(&h.be.rec), BL_JOB_BOOT_NONE); /* and a reboot runs nothing */

    /* the link drops before the device commits */
    Harness g;
    g.begin();
    CHECK(g.connect());
    CHECK(acceptedId(g.jobStart(BL_CAP_WIFI_SCAN)) != 0);
    g.bridge.onDisconnect();
    CHECK_EQ_INT(g.be.cancels, 1);
    CHECK(!bl_job_record_valid(&g.be.rec));
    CHECK(!g.bridge.commitJob());

    /* committed: the drop is the restart itself and cancels nothing */
    Harness k;
    k.begin();
    CHECK(k.connect());
    const uint32_t id = acceptedId(k.jobStart(BL_CAP_WIFI_SCAN));
    CHECK(id != 0);
    CHECK(k.bridge.commitJob());
    CHECK(!k.bridge.commitJob()); /* once */
    k.bridge.onDisconnect();
    CHECK_EQ_INT(k.be.cancels, 0);
    CHECK(bl_job_record_valid(&k.be.rec) && k.be.rec.state == BL_JOB_S_PENDING && k.be.rec.job_id == id);

    /* an e-stop with no job pending touches no record */
    Harness m;
    m.begin();
    CHECK(m.connect());
    bl_job_record_create(&m.be.rec, 9, BL_CAP_WIFI_SCAN, 20, nullptr, 0);
    bl_job_record_finish(&m.be.rec, BL_JOB_DONE, "x", 1);
    m.request(BL_MSG_ESTOP);
    m.bridge.onDisconnect();
    CHECK_EQ_INT(m.be.cancels, 0);
    CHECK(bl_job_record_valid(&m.be.rec) && m.be.rec.state == BL_JOB_S_FINISHED);
    CHECK(!m.bridge.commitJob());
}

/* ----------------------------------------------- pairing mode (ADR 0005) */

/* The Flipper side of a pairing attempt against the bridge in pairing mode. */
struct PairLink {
    Harness h;
    bl_pair_t fl;
    uint32_t frng = 0xF1F1;

    explicit PairLink(bool provisioned = false) {
        if(provisioned) {
            h.begin();
        } else {
            h.bridge.begin(nullptr, &h.be, &Harness::tx, &h, h.mtu);
        }
        h.bridge.beginPairing("StickS3");
    }
    /* Link up: the bridge starts an attempt, the Flipper sends COMMIT. */
    void linkUp() {
        h.bridge.onConnect();
        bl_pair_init(&fl, BL_PAIR_INITIATOR, "Flipper", central_rng, &frng, h.be.now);
        uint8_t b[BL_MAX_MSG];
        int n = bl_pair_start(&fl, b, sizeof(b));
        h.toBridge(b, (size_t)n);
        pump();
    }
    /* Deliver the bridge's frames to the Flipper and its replies back. */
    void pump() {
        for(int guard = 0; guard < 6; guard++) {
            auto frames = h.drain();
            if(frames.empty()) return;
            for(auto& fr : frames) {
                uint8_t o[BL_MAX_MSG];
                size_t ol = 0;
                (void)bl_pair_rx(&fl, fr.data(), fr.size(), o, sizeof(o), &ol);
                if(ol) h.toBridge(o, ol);
            }
        }
    }
    void flipperAnswers(bool accept) {
        uint8_t o[BL_MAX_MSG];
        size_t ol = 0;
        CHECK_EQ_INT(bl_pair_accept(&fl, accept, o, sizeof(o), &ol), BL_OK);
        h.toBridge(o, ol);
        pump();
    }
    void stickAnswers(bool accept) {
        h.bridge.pairAnswer(accept);
        pump();
    }
};

static void t_pair_bridge_happy_path() {
    PairLink l; /* unprovisioned StickS3: first pairing */
    CHECK(l.h.bridge.pairing());
    l.linkUp();
    CHECK_EQ_INT(l.h.bridge.pairState(), BL_PS_COMPARE);
    uint32_t cs = 0, cf = 1;
    CHECK(l.h.bridge.pairCode(&cs));
    CHECK(bl_pair_code(&l.fl, &cf));
    CHECK_EQ_INT(cs, cf);
    CHECK(strcmp(l.h.bridge.pairPeerName(), "Flipper") == 0);
    CHECK(strcmp(bl_pair_peer_name(&l.fl), "StickS3") == 0);
    CHECK(!l.h.bridge.needsDisconnect()); /* the users are comparing */

    l.flipperAnswers(true);
    CHECK_EQ_INT(l.h.bridge.pairState(), BL_PS_COMPARE); /* StickS3 user still deciding */
    l.stickAnswers(true);
    CHECK_EQ_INT(l.h.bridge.pairState(), BL_PS_DONE);
    CHECK_EQ_INT(bl_pair_state(&l.fl), BL_PS_DONE);

    uint8_t k_st[BL_KEY_LEN], id_st[2], k_fl[BL_KEY_LEN], id_fl[2];
    CHECK(l.h.bridge.pairTakeResult(k_st, id_st));
    CHECK(!l.h.bridge.pairTakeResult(k_st, id_st)); /* only once */
    CHECK_EQ_INT(bl_pair_result(&l.fl, k_fl, id_fl), BL_OK);
    CHECK(memcmp(k_st, k_fl, BL_KEY_LEN) == 0);
    CHECK(memcmp(id_st, id_fl, 2) == 0);
    /* the attempt is over: the link goes, without a cooldown */
    CHECK(l.h.bridge.needsDisconnect());
    CHECK(!l.h.bridge.penalizeOnDrop());

    /* Leave pairing mode with the new key: the next link is a normal,
     * authenticated session with it. */
    l.h.bridge.endPairing();
    l.h.bridge.setKey(k_st);
    CHECK(l.h.bridge.needsDisconnect()); /* the pairing link still has to go */
    CHECK(!l.h.bridge.penalizeOnDrop());
    l.h.bridge.onDisconnect();
    memcpy(l.h.psk, k_fl, BL_KEY_LEN);
    CHECK(l.h.connect());
    auto r = l.h.request(BL_MSG_DEVICE_INFO_REQ);
    CHECK(r.size() == 1 && r[0].type == BL_MSG_DEVICE_INFO);
    /* and a paired key is not the demo key: active ops can be enabled */
    CHECK(contains(l.h.status(), "demo:0"));
}

static void t_pair_bridge_rejections() {
    /* the StickS3 user says no */
    PairLink a;
    a.linkUp();
    a.stickAnswers(false);
    CHECK_EQ_INT(a.h.bridge.pairState(), BL_PS_FAILED);
    CHECK_EQ_INT(a.h.bridge.pairFail(), BL_PF_REJECTED);
    CHECK_EQ_INT(bl_pair_state(&a.fl), BL_PS_FAILED);
    CHECK_EQ_INT(a.fl.fail, BL_PF_PEER_REJECTED);
    uint8_t k[BL_KEY_LEN], id[2];
    CHECK(!a.h.bridge.pairTakeResult(k, id));
    CHECK(a.h.bridge.needsDisconnect());
    CHECK(!a.h.bridge.penalizeOnDrop()); /* may try again straight away */

    /* the Flipper user says no */
    PairLink b;
    b.linkUp();
    b.flipperAnswers(false);
    CHECK_EQ_INT(b.h.bridge.pairState(), BL_PS_FAILED);
    CHECK_EQ_INT(b.h.bridge.pairFail(), BL_PF_PEER_REJECTED);
    CHECK_EQ_INT(b.h.bridge.pairPeerReason(), BL_PAIR_ABORT_REJECTED);

    /* answering when there is nothing to answer does nothing */
    PairLink c;
    c.h.bridge.pairAnswer(true); /* no link */
    CHECK(c.h.chunks.empty());
    c.linkUp();
    c.stickAnswers(true);
    c.h.bridge.pairAnswer(true); /* already answered */
    CHECK(c.h.chunks.empty());
}

/* A Flipper that is not pairing (it starts a session) is dropped, with a
 * cooldown, so the scan can find the one that is. */
static void t_pair_bridge_foreign_peer() {
    PairLink l(true);
    l.h.bridge.onConnect();
    bl_session_t c;
    uint32_t r = 5;
    bl_session_init(&c, BL_ROLE_CENTRAL, l.h.psk, central_rng, &r);
    uint8_t b[BL_MAX_MSG];
    int n = bl_session_start(&c, b, sizeof(b));
    l.h.toBridge(b, (size_t)n);
    CHECK(l.h.drain().empty()); /* no session in pairing mode */
    CHECK(!l.h.bridge.established());
    CHECK(l.h.bridge.needsDisconnect());
    CHECK(l.h.bridge.penalizeOnDrop());

    /* a peer that never says anything is dropped too, after the normal
     * authentication deadline */
    PairLink q;
    q.h.bridge.onConnect();
    CHECK(!q.h.bridge.needsDisconnect());
    q.h.be.now += brucelink::kAuthTimeoutMs;
    CHECK(q.h.bridge.needsDisconnect());
    CHECK(q.h.bridge.penalizeOnDrop());
}

/* Pairing mode never authenticates a session, even with the right key,
 * and normal mode never answers pairing. */
static void t_pair_bridge_modes_are_separate() {
    PairLink l(true);
    CHECK(!l.h.connect()); /* the provisioned key does not open a session here */
    l.h.bridge.endPairing();
    l.h.bridge.onDisconnect();
    CHECK(l.h.connect()); /* back in normal mode it does */

    Harness n;
    n.begin();
    n.bridge.onConnect();
    bl_pair_t fl;
    uint32_t r = 9;
    bl_pair_init(&fl, BL_PAIR_INITIATOR, "F", central_rng, &r, n.be.now);
    uint8_t b[BL_MAX_MSG];
    int len = bl_pair_start(&fl, b, sizeof(b));
    n.toBridge(b, (size_t)len);
    CHECK(n.drain().empty()); /* no PAIR_KEY from a bridge that is not pairing */
    CHECK_EQ_INT(n.bridge.pairState(), BL_PS_IDLE);

    /* an unprovisioned bridge outside pairing mode drops every link */
    Harness u;
    u.bridge.begin(nullptr, &u.be, &Harness::tx, &u, u.mtu);
    u.bridge.onConnect();
    CHECK(u.bridge.needsDisconnect());
}

/* Link loss and the time limit end an attempt without a key. */
static void t_pair_bridge_link_loss_and_timeout() {
    PairLink l;
    l.linkUp();
    CHECK_EQ_INT(l.h.bridge.pairState(), BL_PS_COMPARE);
    l.h.bridge.onDisconnect();
    CHECK_EQ_INT(l.h.bridge.pairState(), BL_PS_FAILED);
    CHECK_EQ_INT(l.h.bridge.pairFail(), BL_PF_CANCELLED);
    l.stickAnswers(true); /* too late: nothing is sent */
    CHECK(l.h.chunks.empty());
    /* the next link is a fresh attempt */
    l.linkUp();
    CHECK_EQ_INT(l.h.bridge.pairState(), BL_PS_COMPARE);

    /* nobody answers: the time limit ends it and tells the Flipper */
    l.h.be.now += BL_PAIR_TIMEOUT_MS;
    l.h.bridge.tick();
    CHECK_EQ_INT(l.h.bridge.pairState(), BL_PS_FAILED);
    CHECK_EQ_INT(l.h.bridge.pairFail(), BL_PF_TIMEOUT);
    l.pump();
    CHECK_EQ_INT(bl_pair_state(&l.fl), BL_PS_FAILED);
    CHECK_EQ_INT(l.fl.peer_reason, BL_PAIR_ABORT_TIMEOUT);
    CHECK(l.h.bridge.needsDisconnect());
    CHECK(!l.h.bridge.penalizeOnDrop());
}

int main() {
    printf("BruceLink bridge tests (host)\n[bridge]\n");
    RUN(t_pr1_reconnect_preserves_psk);
    RUN(t_pr1_estop_blocks_capture_start);
    RUN(t_pr1_poll_failure_stops_backend_via_tick);
    RUN(t_pr1_poll_failure_stops_backend_via_status);
    RUN(t_pr1_capture_init_without_backend);
    RUN(t_pr1_rate_limit_bounds_actual_frames);
    RUN(t_pr1_unused_frames_refunded);
    RUN(t_backend_overrun_engages_estop);
    RUN(t_active_rejects_broadcast_and_bad_channel);
    RUN(t_backend_text_length_clamped);
    RUN(t_capture_bad_mode_rejected);
    RUN(t_mtu_update_used_for_fragmentation);
    RUN(t_pr1_wifi_line_bounded_and_sanitized);
    printf("[supervision]\n");
    RUN(t_codex2_reenable_does_not_reset_budget);
    RUN(t_codex2_reconnect_does_not_reset_budget);
    RUN(t_codex2_bad_proof_requests_disconnect);
    RUN(t_codex2_silent_peer_times_out);
    RUN(t_codex2_stalled_handshake_times_out);
    RUN(t_codex2_established_never_dropped);
    RUN(t_codex2_unprovisioned_drops_immediately);
    RUN(t_codex2_auth_deadline_wraps);
    RUN(t_peer_cooldown);
    printf("[client-bridge]\n");
    RUN(t_e2e_client_handshake_and_requests);
    RUN(t_e2e_lost_handshake_frame_recovers_by_relink);
    RUN(t_e2e_lost_reply_times_out_session_survives);
    RUN(t_e2e_wrong_key_reported_on_both_ends);
    RUN(t_e2e_link_loss_mid_request);
    RUN(t_demo_key_never_enables_active_ops);
    RUN(t_real_key_reports_not_demo);
    printf("[jobs]\n");
    RUN(t_files_list_read_delete);
    RUN(t_files_refuse_bad_names_and_demo_key);
    RUN(t_sniff_job);
    RUN(t_job_refusals);
    RUN(t_job_accept_and_busy);
    RUN(t_estop_cleared_only_by_a_session);
    RUN(t_capture_stops_on_link_loss_and_estop);
    RUN(t_active_job_refused);
    RUN(t_failed_active_op_is_refunded);
    RUN(t_destructor_wipes_key);
    RUN(t_clear_key);
    RUN(t_job_cancelled_before_the_restart);
    RUN(t_job_e2e_across_restarts);
    RUN(t_job_e2e_crash_and_power_loss);
    printf("[pairing]\n");
    RUN(t_pair_bridge_happy_path);
    RUN(t_pair_bridge_rejections);
    RUN(t_pair_bridge_foreign_peer);
    RUN(t_pair_bridge_modes_are_separate);
    RUN(t_pair_bridge_link_loss_and_timeout);
    printf("\n%d checks, %d failed\n", bl_tests_run, bl_tests_failed);
    return bl_tests_failed == 0 ? 0 : 1;
}
