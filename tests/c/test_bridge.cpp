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

extern "C" {
#include "bl_reasm.h"
#include "bl_textbuf.h"
}

#include "bl_test.h"

#include <new>
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
    int send_mode = 0; /* 0 = send max_frames, 1 = send fixed, 2 = overrun */
    int send_fixed = 0;
    /* text */
    bool lie_about_length = false;

    size_t deviceInfo(char* out, size_t cap) override {
        bl_textbuf_t tb;
        bl_tb_init(&tb, out, cap);
        bl_tb_str(&tb, "mock-dev");
        return lie_about_length ? cap + 100 : tb.len;
    }
    size_t wifiScan(char* out, size_t cap) override {
        bl_textbuf_t tb;
        bl_tb_init(&tb, out, cap);
        bl_tb_str(&tb, "1 networks\n");
        brucelink::formatWifiApLine(&tb, "Home", -50, 6);
        return tb.len;
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
    /* passive radio command also refused; read-only status still served */
    CHECK(isError(h.cmd(BL_CAP_WIFI_SCAN), BL_ERR_ESTOPPED));
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
    bl_frame_t f = {BL_WIRE_VERSION, BL_MSG_AUTH, 0, 0, sizeof(forged), forged};
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
    printf("\n%d checks, %d failed\n", bl_tests_run, bl_tests_failed);
    return bl_tests_failed == 0 ? 0 : 1;
}
