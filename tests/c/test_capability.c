/* Capability registry, PCAP writer, safety gate, capture lifecycle.
 * SPDX-License-Identifier: MIT */
#include "../../shared/brucelink_core/src/bl_capability.h"
#include "../../shared/brucelink_core/src/bl_capture.h"
#include "../../shared/brucelink_core/src/bl_pcap.h"
#include "../../shared/brucelink_core/src/bl_safety.h"
#include "bl_test.h"

/* --------------------------------------------------------- registry */

static const bl_capability_desc_t g_caps[] = {
    {BL_CAP_DEVICE_INFO, "device.info", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_WIFI_SCAN, "wifi.scan", BL_OP_PASSIVE, 0, 1},
    {BL_CAP_SNIFF_HANDSHAKE, "wifi.sniff.handshake", BL_OP_PASSIVE, BL_CAP_F_HW_UNVERIFIED, 1},
    {BL_CAP_SNIFF_RAW,
     "wifi.sniff.raw",
     BL_OP_PASSIVE,
     BL_CAP_F_REQUIRES_EXT_HW | BL_CAP_F_HW_UNVERIFIED,
     1},
    {BL_CAP_DEAUTH,
     "wifi.deauth",
     BL_OP_ACTIVE,
     BL_CAP_F_ACTIVE_RF | BL_CAP_F_REQUIRES_TARGET | BL_CAP_F_HW_UNVERIFIED,
     1},
};

static void t_registry_roundtrip(void) {
    bl_capability_registry_t reg = {g_caps, sizeof(g_caps) / sizeof(g_caps[0])};
    CHECK(bl_caps_find(&reg, BL_CAP_DEAUTH) != NULL);
    CHECK(bl_caps_find(&reg, 0x9999) == NULL);
    const bl_capability_desc_t* d = bl_caps_find(&reg, BL_CAP_DEAUTH);
    CHECK(d->op_class == BL_OP_ACTIVE);
    CHECK((d->flags & BL_CAP_F_ACTIVE_RF) != 0);

    uint8_t buf[BL_MAX_PAYLOAD];
    int n = bl_caps_serialize(&reg, buf, sizeof(buf));
    CHECK(n > 0);
    CHECK_EQ_INT(bl_caps_validate(buf, (size_t)n), (int)reg.count);
    /* truncation detected */
    CHECK(bl_caps_validate(buf, (size_t)n - 1) < 0);
    /* first byte is the count */
    CHECK_EQ_INT(buf[0], (int)reg.count);
}

/* ------------------------------------------------------------- pcap */

static void t_pcap_global_header_bytes(void) {
    uint8_t h[BL_PCAP_GLOBAL_LEN];
    int n = bl_pcap_global_header(
        BL_PCAP_DEFAULT_SNAPLEN, BL_PCAP_LINKTYPE_IEEE802_11, h, sizeof(h));
    CHECK_EQ_INT(n, 24);
    /* magic a1b2c3d4 little-endian */
    CHECK_EQ_INT(h[0], 0xd4);
    CHECK_EQ_INT(h[1], 0xc3);
    CHECK_EQ_INT(h[2], 0xb2);
    CHECK_EQ_INT(h[3], 0xa1);
    CHECK_EQ_INT(h[4], 2); /* v major LE */
    CHECK_EQ_INT(h[5], 0);
    CHECK_EQ_INT(h[6], 4); /* v minor */
    /* snaplen 2500 = 0x09C4 LE at offset 16 */
    CHECK_EQ_INT(h[16], 0xC4);
    CHECK_EQ_INT(h[17], 0x09);
    /* linktype 105 at offset 20 */
    CHECK_EQ_INT(h[20], 105);
    CHECK_EQ_INT(h[21], 0);
}

static void t_pcap_record_header_bytes(void) {
    uint8_t r[BL_PCAP_REC_LEN];
    int n = bl_pcap_record_header(0x11223344, 0x55667788, 100, 104, r, sizeof(r));
    CHECK_EQ_INT(n, 16);
    CHECK_EQ_INT(r[0], 0x44);
    CHECK_EQ_INT(r[3], 0x11);
    CHECK_EQ_INT(r[8], 100);
    CHECK_EQ_INT(r[12], 104);
}

static void t_crc32_known(void) {
    /* CRC-32/ISO-HDLC of "123456789" is 0xCBF43926 */
    CHECK_EQ_INT((long)bl_crc32("123456789", 9), (long)0xCBF43926u);
    /* streaming equals one-shot */
    uint32_t c = bl_crc32_init();
    c = bl_crc32_update(c, "12345", 5);
    c = bl_crc32_update(c, "6789", 4);
    CHECK_EQ_INT((long)bl_crc32_final(c), (long)0xCBF43926u);
}

/* ----------------------------------------------------------- safety */

static void t_safety_default_deny(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    uint8_t bssid[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
    /* passive always ok */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_PASSIVE, NULL, false, 0, 1000), BL_OK);
    /* active with nothing enabled -> denied (default-off) */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, bssid, true, 1, 1000), BL_E_DENIED);
}

static void t_safety_enable_and_scope(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    /* operator enables active ops over the (already authenticated) link */
    CHECK_EQ_INT(bl_safety_enable_active(&g, true), BL_OK);
    CHECK(bl_safety_active_ready(&g));

    /* Unicast BSSIDs (I/G bit clear). Earlier fixtures {1,2,3,..} and
     * {9,9,..} were group addresses, which the gate now correctly rejects
     * as non-specific targets (see t_safety_rejects_non_specific_targets). */
    uint8_t allowed[6] = {0x02, 0x11, 0x22, 0x33, 0x44, 0x55};
    uint8_t other[6] = {0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    /* with no scope, any specific target passes */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, other, true, 1, 1000), BL_OK);
    /* broadcast (no target) is never allowed for an active op */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, NULL, true, 1, 1000), BL_E_DENIED);

    /* operator confines the test to one target */
    uint8_t list[1][6];
    memcpy(list[0], allowed, 6);
    CHECK_EQ_INT(bl_safety_set_allowlist(&g, list, 1), BL_OK);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, allowed, true, 1, 1000), BL_OK);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, other, true, 1, 1000), BL_E_DENIED);

    /* disabling active ops denies again */
    bl_safety_enable_active(&g, false);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, allowed, true, 1, 1000), BL_E_DENIED);
}

static void t_safety_rate_limit(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    bl_safety_set_limits(&g, 1000, 10, 30000);
    bl_safety_enable_active(&g, true);
    uint8_t t[6] = {0x02, 1, 1, 1, 1, 1}; /* unicast */
    /* 10 frames in the window OK, 11th blocked */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 10, 5000), BL_OK);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 1, 5100), BL_E_LOCKED);
    /* next window resets budget */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 1, 6200), BL_OK);
}

static void t_safety_estop_and_link_loss(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    bl_safety_enable_active(&g, true);
    bl_safety_estop(&g);
    CHECK(bl_safety_is_estopped(&g));
    CHECK(!bl_safety_active_ready(&g));
    /* even passive ops are refused while estopped (fail safe) */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_PASSIVE, NULL, false, 0, 1000), BL_E_DENIED);
    /* re-enabling active is refused until reset */
    CHECK_EQ_INT(bl_safety_enable_active(&g, true), BL_E_DENIED);
    bl_safety_reset(&g);
    CHECK(!bl_safety_is_estopped(&g));
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_PASSIVE, NULL, false, 0, 1000), BL_OK);

    /* link loss engages the e-stop automatically */
    bl_safety_enable_active(&g, true);
    bl_safety_on_link_loss(&g);
    CHECK(bl_safety_is_estopped(&g));
    CHECK(!bl_safety_active_ready(&g));
}

/* Regression: "no broadcast active ops" previously held only for a NULL
 * target pointer, but the wire always carries 6 bytes, so ff:ff:ff:ff:ff:ff
 * or any multicast address passed. */
static void t_safety_rejects_non_specific_targets(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    bl_safety_enable_active(&g, true);
    uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    uint8_t mcast[6] = {0x01, 0x00, 0x5e, 0x00, 0x00, 0x01};
    uint8_t zero[6] = {0};
    uint8_t uni[6] = {0x3c, 0x22, 0xfb, 0x01, 0x02, 0x03};
    CHECK(!bl_safety_bssid_is_specific(bcast));
    CHECK(!bl_safety_bssid_is_specific(mcast));
    CHECK(!bl_safety_bssid_is_specific(zero));
    CHECK(!bl_safety_bssid_is_specific(NULL));
    CHECK(bl_safety_bssid_is_specific(uni));
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, bcast, true, 1, 1000), BL_E_DENIED);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, mcast, true, 1, 1000), BL_E_DENIED);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, zero, true, 1, 1000), BL_E_DENIED);
    /* even an operator scope that lists broadcast cannot authorize it */
    uint8_t list[1][6];
    memcpy(list[0], bcast, 6);
    bl_safety_set_allowlist(&g, list, 1);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, bcast, true, 1, 1000), BL_E_DENIED);
    /* a denied attempt consumes no budget */
    bl_safety_set_allowlist(&g, NULL, 0);
    CHECK_EQ_INT(bl_safety_budget(&g, 1000), g.max_frames_per_window);
}

/* Regression for "rate limit must bound actual transmissions": the
 * bridge reserves a budget, lets the backend send up to it, and refunds
 * the unused part. */
static void t_safety_budget_and_refund(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    bl_safety_set_limits(&g, 1000, 20, 30000);
    bl_safety_enable_active(&g, true);
    uint8_t t[6] = {0x02, 1, 2, 3, 4, 5};
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 20);
    /* reserve 16, only 4 actually sent -> refund 12 */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 16, 5000), BL_OK);
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 4);
    bl_safety_refund(&g, 12);
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 16);
    /* reserving more than remains is refused */
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 17, 5000), BL_E_LOCKED);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 16, 5000), BL_OK);
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 0);
    /* over-refund clamps at an empty window, never wraps */
    bl_safety_refund(&g, 1000);
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 20);
    /* window roll restores the full budget */
    bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 20, 5000);
    CHECK_EQ_INT(bl_safety_budget(&g, 5999), 0);
    CHECK_EQ_INT(bl_safety_budget(&g, 6000), 20);
}

/* Regression (PR #1, Codex 2nd review): enabling active ops cleared the
 * rate window, so "enable" before each op bypassed the limit; resetting
 * the gate for a new session cleared it as well. Only time may expire it. */
static void t_safety_window_survives_enable_and_reset(void) {
    bl_safety_t g;
    bl_safety_init(&g);
    bl_safety_set_limits(&g, 1000, 20, 30000);
    uint8_t t[6] = {0x02, 1, 2, 3, 4, 5};
    bl_safety_enable_active(&g, true);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 20, 5000), BL_OK);
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 0);
    /* re-enable while enabled, and disable/enable */
    CHECK_EQ_INT(bl_safety_enable_active(&g, true), BL_OK);
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 0);
    bl_safety_enable_active(&g, false);
    bl_safety_enable_active(&g, true);
    CHECK_EQ_INT(bl_safety_budget(&g, 5000), 0);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 1, 5000), BL_E_LOCKED);
    /* e-stop + reset (what a new session does) keeps the history too */
    bl_safety_estop(&g);
    bl_safety_reset(&g);
    bl_safety_enable_active(&g, true);
    CHECK_EQ_INT(bl_safety_budget(&g, 5500), 0);
    CHECK_EQ_INT(bl_safety_authorize(&g, BL_OP_ACTIVE, t, true, 1, 5500), BL_E_LOCKED);
    /* link loss + reset, same */
    bl_safety_on_link_loss(&g);
    bl_safety_reset(&g);
    bl_safety_enable_active(&g, true);
    CHECK_EQ_INT(bl_safety_budget(&g, 5999), 0);
    /* only elapsed time refills it */
    CHECK_EQ_INT(bl_safety_budget(&g, 6000), 20);
}

/* ---------------------------------------------------------- capture */

typedef struct {
    int start_rc, channel_rc, poll_rc, stop_rc;
    int started, stopped;
    int stop_calls; /* counts backend stop (flush+close) invocations */
    int raw_available;
    uint32_t packets;
} mock_be_t;

static int mb_start(void* c, bl_capture_mode_t m, uint8_t ch) {
    (void)m;
    (void)ch;
    mock_be_t* b = c;
    if(b->start_rc != BL_OK) return b->start_rc;
    b->started = 1;
    return BL_OK;
}
static int mb_set_channel(void* c, uint8_t ch) {
    (void)ch;
    return ((mock_be_t*)c)->channel_rc;
}
static int mb_poll(void* c, bl_capture_counts_t* o) {
    mock_be_t* b = c;
    if(b->poll_rc != BL_OK) return b->poll_rc;
    b->packets += 5;
    o->packets = b->packets;
    o->eapol = 1;
    return BL_OK;
}
static int mb_stop(void* c) {
    mock_be_t* b = c;
    b->stopped = 1;
    b->stop_calls++;
    return b->stop_rc;
}
static int mb_mode_avail(void* c, bl_capture_mode_t m) {
    mock_be_t* b = c;
    if(m == BL_CAP_MODE_RAW) return b->raw_available;
    return 1;
}
static const bl_capture_backend_t MOCK_BE = {
    mb_start, mb_set_channel, mb_poll, mb_stop, mb_mode_avail};

static void t_capture_lifecycle(void) {
    mock_be_t be = {0};
    be.raw_available = 1;
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    CHECK_EQ_INT(m.state, BL_CAPST_IDLE);

    /* invalid channel */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 99, 1, 100), BL_E_ARG);
    /* good start */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 6, 1, 100), BL_OK);
    CHECK(bl_capture_is_running(&m));
    CHECK(be.started);
    /* double start -> busy */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 6, 2, 100), BL_E_BUSY);
    /* poll accumulates counts */
    CHECK_EQ_INT(bl_capture_poll(&m), BL_OK);
    CHECK_EQ_INT(bl_capture_poll(&m), BL_OK);
    CHECK_EQ_INT(m.counts.packets, 10);
    /* channel change */
    CHECK_EQ_INT(bl_capture_set_channel(&m, 11), BL_OK);
    CHECK_EQ_INT(m.channel, 11);
    /* stop */
    CHECK_EQ_INT(bl_capture_stop(&m), BL_OK);
    CHECK(be.stopped);
    CHECK_EQ_INT(m.state, BL_CAPST_IDLE);
    /* stop again idempotent */
    CHECK_EQ_INT(bl_capture_stop(&m), BL_OK);
}

static void t_capture_raw_mode_gated(void) {
    mock_be_t be = {0};
    be.raw_available = 0; /* StickS3 with no SD */
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_RAW, 1, 1, 0), BL_E_DENIED);
    CHECK(!be.started);
    /* handshake mode still available */
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 1, 1, 0), BL_OK);
}

static void t_capture_backend_start_failure(void) {
    mock_be_t be = {0};
    be.raw_available = 1;
    be.start_rc = BL_E_NO_RESOURCE;
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 1, 1, 0), BL_E_NO_RESOURCE);
    CHECK_EQ_INT(m.state, BL_CAPST_ERROR);
    CHECK(!bl_capture_is_running(&m));
}

static void t_capture_poll_failure_transitions_error(void) {
    mock_be_t be = {0};
    be.raw_available = 1;
    be.poll_rc = BL_E_NO_RESOURCE; /* storage full mid-capture */
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 1, 1, 0);
    CHECK_EQ_INT(bl_capture_poll(&m), BL_E_NO_RESOURCE);
    CHECK_EQ_INT(m.state, BL_CAPST_ERROR);
    CHECK_EQ_INT(m.last_error, BL_E_NO_RESOURCE);
    /* Regression (PR #1, Codex): the failed poll itself must stop and flush
     * the backend — previously it only flagged ERROR and left the radio in
     * promiscuous mode with the pcap file open. */
    CHECK_EQ_INT(be.stop_calls, 1);
    CHECK(!bl_capture_is_running(&m));
    /* operator stop afterwards is clean and does NOT stop the backend twice */
    CHECK_EQ_INT(bl_capture_stop(&m), BL_OK);
    CHECK_EQ_INT(be.stop_calls, 1);
    CHECK_EQ_INT(m.state, BL_CAPST_IDLE);
    /* a new session can start after the error */
    be.poll_rc = BL_OK;
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 1, 2, 0), BL_OK);
}

static void t_capture_stop_exactly_once(void) {
    mock_be_t be = {0};
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 6, 1, 0);
    bl_capture_stop(&m);
    bl_capture_stop(&m);
    bl_capture_stop(&m);
    CHECK_EQ_INT(be.stop_calls, 1);
    /* a failed start never needs a stop */
    mock_be_t be2 = {0};
    be2.start_rc = BL_E_NO_RESOURCE;
    bl_capture_init(&m, &MOCK_BE, &be2);
    bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 6, 1, 0);
    bl_capture_stop(&m);
    CHECK_EQ_INT(be2.stop_calls, 0);
    CHECK_EQ_INT(m.state, BL_CAPST_IDLE);
}

static void t_capture_invalid_mode_rejected(void) {
    mock_be_t be = {0};
    bl_capture_mgr_t m;
    bl_capture_init(&m, &MOCK_BE, &be);
    CHECK_EQ_INT(bl_capture_start(&m, (bl_capture_mode_t)7, 6, 1, 0), BL_E_ARG);
    CHECK_EQ_INT(bl_capture_start(&m, (bl_capture_mode_t)-1, 6, 1, 0), BL_E_ARG);
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 0, 1, 0), BL_E_ARG);
    CHECK(!be.started);
}

/* Regression (PR #1 review: capture manager initialisation): a manager
 * with no backend must be a valid, permanently idle object. */
static void t_capture_null_backend(void) {
    bl_capture_mgr_t m;
    memset(&m, 0xA5, sizeof(m)); /* simulate uninitialised storage */
    bl_capture_init(&m, NULL, NULL);
    CHECK_EQ_INT(m.state, BL_CAPST_IDLE);
    CHECK(!bl_capture_is_running(&m));
    CHECK_EQ_INT(bl_capture_start(&m, BL_CAP_MODE_HANDSHAKE, 6, 1, 0), BL_E_ARG);
    CHECK_EQ_INT(bl_capture_poll(&m), BL_E_ARG);
    CHECK_EQ_INT(bl_capture_stop(&m), BL_OK);
}

void run_capability_tests(void) {
    RUN(t_registry_roundtrip);
    RUN(t_pcap_global_header_bytes);
    RUN(t_pcap_record_header_bytes);
    RUN(t_crc32_known);
    RUN(t_safety_default_deny);
    RUN(t_safety_enable_and_scope);
    RUN(t_safety_rate_limit);
    RUN(t_safety_estop_and_link_loss);
    RUN(t_safety_rejects_non_specific_targets);
    RUN(t_safety_budget_and_refund);
    RUN(t_safety_window_survives_enable_and_reset);
    RUN(t_capture_lifecycle);
    RUN(t_capture_raw_mode_gated);
    RUN(t_capture_backend_start_failure);
    RUN(t_capture_poll_failure_transitions_error);
    RUN(t_capture_stop_exactly_once);
    RUN(t_capture_invalid_mode_rejected);
    RUN(t_capture_null_backend);
}
