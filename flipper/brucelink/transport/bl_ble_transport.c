/*
 * BruceLink Flipper app — BLE transport (GATT peripheral for the StickS3).
 *
 * STATUS: compiles against the Momentum mntm-012 SDK; NOT yet run on
 * hardware. Everything radio-facing here needs the bring-up procedure in
 * docs/HARDWARE_TESTING.md step 5. The portable parts are host-tested:
 * event parsing (bl_gatt_evt), fragmentation (bl_reasm) and the session
 * client (bl_client).
 *
 * Design (ADR 0001, verified against Momentum mntm-012 source):
 *   - The Flipper is the peripheral; the StickS3 (NimBLE) is the central.
 *   - A FAP-owned FuriHalBleProfileTemplate with one custom 128-bit
 *     service: RX (central writes) and TX (we notify), <= 244 bytes each.
 *     The stock serial profile is not reusable: the bt service hijacks it
 *     for RPC.
 *   - No BLE-level pairing: GapPairingNone, no bonding, open attribute
 *     permissions. All authentication and integrity come from the
 *     BruceLink session (bl_session). NOTE: frames are authenticated but
 *     not encrypted; payloads are readable over the air (TASKS M4.6).
 *   - "Link up" is the central enabling TX notifications. It does not
 *     depend on the firmware's Connected status, which only fires after
 *     pairing. The app starts the handshake at that moment (the StickS3
 *     drops links that are not authenticated within 10 s).
 *
 * Threading:
 *   - ble_evt() runs on the BLE stack's event thread (about 1.3 KB of
 *     stack). It only parses, copies one chunk into a queue via a
 *     preallocated item, flips a few word-sized flags, and returns.
 *   - profile start/stop run on the bt service thread while the app
 *     thread is blocked inside bt_profile_start / _restore_default.
 *   - everything else runs on the app thread.
 * SPDX-License-Identifier: MIT
 */
#include "bl_transport.h"

#include "../../../shared/brucelink_core/src/bl_reasm.h"
#include "../../../shared/brucelink_core/src/bl_uuid.h"
#include "bl_gatt_evt.h"

#include <ble/core/ble_defs.h>
#include <bt/bt_service/bt.h>
#include <furi.h>
#include <furi_ble/event_dispatcher.h>
#include <furi_ble/gatt.h>
#include <furi_ble/profile_interface.h>
#include <furi_hal_bt.h>
#include <furi_hal_version.h>
#include <stdio.h>
#include <storage/storage.h>
#include <string.h>

#define TAG "BruceLinkBle"

#define BLE_KEYS_PATH APP_DATA_PATH(".brucelink.keys")
/* A full 540-byte frame is 30 chunks at the default MTU, and the queue is
 * only drained once per app tick. */
#define BLE_RX_QUEUE_DEPTH 32
_Static_assert(BL_GATT_RX_CHUNK_MAX == BL_BLE_CHUNK_MAX, "assembler and transport chunk size differ");
/* A central that connects but never enables notifications is dropped
 * after this long, so it cannot keep the StickS3 out. */
#define BLE_SUBSCRIBE_TIMEOUT_MS 20000u /* the StickS3 subscribes within a second; 20 s
                                         leaves room to do it by hand from a phone */
/* service + RX (decl, value) + TX (decl, value, CCCD) = 6; small margin */
#define BLE_MAX_ATTR_RECORDS 8
/* 0x09 = AD_TYPE_COMPLETE_LOCAL_NAME. A 31-byte advertisement holds flags
 * (3), the TX-power field the stack adds (3), the 128-bit service UUID (18)
 * and a name of at most 5 characters. If the stack rejects the data the
 * firmware only logs it and the radio stays silent, so the name is kept to
 * 2 characters for margin. The StickS3 matches on the UUID, not the name. */
#define BLE_ADV_NAME "\x09" "BL"

typedef struct {
    uint16_t len;
    uint8_t data[BL_BLE_CHUNK_MAX];
} BleChunk;

/* Passed as `source` to ble_gatt_characteristic_update for TX. */
typedef struct {
    const uint8_t* ptr;
    uint16_t len;
} BleTxRef;

typedef struct Ble Ble;

typedef struct {
    FuriHalBleProfileBase base; /* must be first */
    Ble* ble;
} BleProfile;

struct Ble {
    Bt* bt;
    FuriHalBleProfileBase* profile;
    bool started;

    /* GATT objects: written on the bt service thread during start/stop */
    uint16_t svc_handle;
    BleGattCharacteristicInstance rx_char;
    BleGattCharacteristicInstance tx_char;
    GapSvcEventHandler* handler;
    volatile bool svc_ready;

    /* BLE event thread -> app thread */
    FuriMessageQueue* rxq;
    BleChunk scratch; /* event thread only; keeps 246 bytes off its stack */
    bl_rx_asm_t rx_asm; /* event thread only: joins writes split across events */
    volatile bool connected; /* a central is connected (maybe not subscribed) */
    volatile bool subscribed;
    volatile uint32_t link_seq; /* bumped on every new subscription */
    volatile uint16_t mtu; /* negotiated ATT MTU; 0 = not yet known */
    /* troubleshooting counters since start(); shown on the Connection screen */
    volatile uint32_t rx_chunks; /* chunks queued for reassembly */
    volatile uint32_t rx_dropped; /* queue full or oversize: chunk lost */
    volatile uint32_t rx_frag; /* events that were a piece of a longer write */
    volatile uint32_t rx_odd; /* pieces that did not fit together: write lost */
    volatile uint16_t rx_odd_offset; /* raw offset field of the last one */
    volatile uint16_t rx_odd_len;
    uint32_t rx_frames; /* app thread: complete frames delivered */
    uint32_t rx_bad; /* app thread: reassembly rejected a chunk */
    uint32_t tx_fail; /* app thread: notify refused by the stack */

    /* app thread only */
    bl_reasm_t reasm;
    uint32_t seen_link;
    bl_sub_guard_t sub_guard;
};

/* ------------------------------------------------------------ GATT */

static const Service_UUID_t ble_svc_uuid = {.Service_UUID_128 = BL_UUID_SERVICE_LE};

/* TX value comes from the BleTxRef handed to ble_gatt_characteristic_update.
 * With data == NULL the firmware is asking for the maximum length. */
static bool ble_tx_data_cb(const void* context, const uint8_t** data, uint16_t* data_len) {
    if(data == NULL) {
        *data_len = BL_BLE_CHUNK_MAX;
        return false;
    }
    const BleTxRef* ref = context;
    *data = ref ? ref->ptr : NULL;
    *data_len = ref ? ref->len : 0;
    return false; /* nothing for the firmware to free */
}

static const BleGattCharacteristicParams ble_rx_params = {
    .name = "BruceLink RX",
    .data_prop_type = FlipperGattCharacteristicDataFixed,
    .data.fixed.length = BL_BLE_CHUNK_MAX,
    .uuid.Char_UUID_128 = BL_UUID_RX_LE,
    .uuid_type = UUID_TYPE_128,
    .char_properties = CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RESP,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask = GATT_NOTIFY_ATTRIBUTE_WRITE,
    .is_variable = CHAR_VALUE_LEN_VARIABLE,
};

static const BleGattCharacteristicParams ble_tx_params = {
    .name = "BruceLink TX",
    .data_prop_type = FlipperGattCharacteristicDataCallback,
    .data.callback.fn = ble_tx_data_cb,
    .data.callback.context = NULL,
    .uuid.Char_UUID_128 = BL_UUID_TX_LE,
    .uuid_type = UUID_TYPE_128,
    .char_properties = CHAR_PROP_NOTIFY,
    .security_permissions = ATTR_PERMISSION_NONE,
    .gatt_evt_mask = GATT_DONT_NOTIFY_EVENTS,
    .is_variable = CHAR_VALUE_LEN_VARIABLE,
};

/* BLE event thread. Must stay tiny: no logging, no blocking, no big locals. */
static BleEventAckStatus ble_evt(void* event, void* context) {
    Ble* ble = context;
    if(!ble->svc_ready) return BleEventNotAck;

    const uint8_t* pckt = event;
    bl_gatt_evt_t e;
    /* pckt[2] is the controller's own parameter length */
    bool consumed = bl_gatt_evt_parse(
        pckt, 3u + (size_t)pckt[2], ble->rx_char.handle, ble->tx_char.handle, &e);

    switch(e.kind) {
    case BL_GATT_EVT_RX_DATA: {
        const uint8_t* chunk = NULL;
        uint16_t clen = 0;
        if(e.offset != 0) ble->rx_frag++; /* reported in more than one event */
        switch(bl_rx_asm_push(&ble->rx_asm, e.offset, e.data, e.len, &chunk, &clen)) {
        case BL_RX_ASM_CHUNK:
            if(clen > 0 && clen <= BL_BLE_CHUNK_MAX) {
                ble->scratch.len = clen;
                memcpy(ble->scratch.data, chunk, clen);
                if(furi_message_queue_put(ble->rxq, &ble->scratch, 0) == FuriStatusOk) {
                    ble->rx_chunks++;
                } else {
                    ble->rx_dropped++; /* reassembly will fail; the request times out */
                }
            }
            break;
        case BL_RX_ASM_REJECT:
            ble->rx_odd++;
            ble->rx_odd_offset = e.offset;
            ble->rx_odd_len = e.len;
            break;
        default: /* BL_RX_ASM_PENDING: waiting for the rest of the write */
            break;
        }
        break;
    }
    case BL_GATT_EVT_SUBSCRIBED:
        bl_rx_asm_init(&ble->rx_asm);
        ble->link_seq++;
        ble->subscribed = true;
        break;
    case BL_GATT_EVT_UNSUBSCRIBED:
        ble->subscribed = false;
        break;
    case BL_GATT_EVT_MTU:
        ble->mtu = e.mtu;
        break;
    case BL_GATT_EVT_CONNECTED:
        ble->connected = true;
        break;
    case BL_GATT_EVT_DISCONNECTED:
        ble->connected = false;
        ble->subscribed = false;
        ble->mtu = 0;
        break;
    default:
        break;
    }
    return consumed ? BleEventAckFlowEnable : BleEventNotAck;
}

/* --------------------------------------------- profile (bt thread) */

static const FuriHalBleProfileTemplate ble_profile_template;

/* Remove everything ble_profile_start() added. bt service thread. */
static void ble_gatt_teardown(Ble* ble) {
    ble->svc_ready = false;
    ble->connected = false;
    ble->subscribed = false;
    /* The dispatcher asserts its handler list is empty on reset, so this
     * must be unregistered here. */
    if(ble->handler) {
        ble_event_dispatcher_unregister_svc_handler(ble->handler);
        ble->handler = NULL;
    }
    /* _delete also frees the descriptor copy _init made, so it is called
     * even for a characteristic the stack refused to add. */
    if(ble->rx_char.characteristic) {
        ble_gatt_characteristic_delete(ble->svc_handle, &ble->rx_char);
    }
    if(ble->tx_char.characteristic) {
        ble_gatt_characteristic_delete(ble->svc_handle, &ble->tx_char);
    }
    ble_gatt_service_delete(ble->svc_handle);
    memset(&ble->rx_char, 0, sizeof(ble->rx_char));
    memset(&ble->tx_char, 0, sizeof(ble->tx_char));
    ble->svc_handle = 0;
}

static FuriHalBleProfileBase* ble_profile_start(FuriHalBleProfileParams params) {
    Ble* ble = params;
    furi_check(ble);

    ble->svc_ready = false;
    ble->svc_handle = 0;
    memset(&ble->rx_char, 0, sizeof(ble->rx_char));
    memset(&ble->tx_char, 0, sizeof(ble->tx_char));

    if(!ble_gatt_service_add(
           UUID_TYPE_128, &ble_svc_uuid, PRIMARY_SERVICE, BLE_MAX_ATTR_RECORDS, &ble->svc_handle)) {
        FURI_LOG_E(TAG, "service add failed");
        return NULL;
    }
    ble->handler = ble_event_dispatcher_register_svc_handler(ble_evt, ble);
    ble_gatt_characteristic_init(ble->svc_handle, &ble_rx_params, &ble->rx_char);
    ble_gatt_characteristic_init(ble->svc_handle, &ble_tx_params, &ble->tx_char);
    /* _init only logs a failure. Attribute handles are never 0, and events
     * are matched on these handles, so refuse to run without both. */
    if(ble->rx_char.handle == 0 || ble->tx_char.handle == 0) {
        FURI_LOG_E(TAG, "characteristic add failed");
        ble_gatt_teardown(ble);
        return NULL;
    }

    BleProfile* profile = malloc(sizeof(BleProfile));
    profile->base.config = &ble_profile_template;
    profile->ble = ble;
    ble->svc_ready = true;
    return &profile->base;
}

static void ble_profile_stop(FuriHalBleProfileBase* base) {
    furi_check(base);
    furi_check(base->config == &ble_profile_template);
    BleProfile* profile = (BleProfile*)base;
    ble_gatt_teardown(profile->ble);
    free(profile);
}

static void ble_profile_get_gap_config(GapConfig* config, FuriHalBleProfileParams params) {
    UNUSED(params);
    furi_check(config);
    memset(config, 0, sizeof(GapConfig));

    static const uint8_t uuid[16] = BL_UUID_SERVICE_LE;
    config->adv_service.UUID_Type = UUID_TYPE_128;
    memcpy(config->adv_service.Service_UUID_128, uuid, sizeof(uuid));

    config->appearance_char = 0x0000;
    config->bonding_mode = false;
    config->pairing_method = GapPairingNone;

    /* Own address, distinct from the Flipper's default and HID profiles,
     * so a phone's cached GATT table for those is never applied to us. */
    memcpy(config->mac_address, furi_hal_version_get_ble_mac(), sizeof(config->mac_address));
    config->mac_address[2] = (uint8_t)(config->mac_address[2] + 2);

    strlcpy(config->adv_name, BLE_ADV_NAME, sizeof(config->adv_name));

    /* 7.5 ms .. 45 ms, as the stock serial profile requests */
    config->conn_param.conn_int_min = 0x06;
    config->conn_param.conn_int_max = 0x24;
    config->conn_param.slave_latency = 0;
    config->conn_param.supervisor_timeout = 0;
}

static const FuriHalBleProfileTemplate ble_profile_template = {
    .start = ble_profile_start,
    .stop = ble_profile_stop,
    .get_gap_config = ble_profile_get_gap_config,
};

/* ------------------------------------------- transport (app thread) */

static void ble_flush_rx(Ble* ble) {
    BleChunk c;
    while(furi_message_queue_get(ble->rxq, &c, 0) == FuriStatusOk) {
    }
    bl_reasm_init(&ble->reasm);
}

/* Give the radio back to the firmware's default profile. */
static void ble_restore_default(Ble* ble) {
    bt_disconnect(ble->bt);
    furi_delay_ms(200); /* let core 2 settle, as the stock HID app does */
    bt_keys_storage_set_default_path(ble->bt);
    if(!bt_profile_restore_default(ble->bt)) {
        FURI_LOG_E(TAG, "failed to restore the default BLE profile");
    }
    ble->profile = NULL;
}

static bool ble_start(BlTransport* t) {
    Ble* ble = t->impl;
    if(ble->started) return true;

    ble->connected = false;
    ble->subscribed = false;
    ble->mtu = 0;
    ble->seen_link = 0;
    bl_sub_guard_init(&ble->sub_guard);
    ble_flush_rx(ble);
    ble->link_seq = 0;
    ble->rx_chunks = ble->rx_dropped = ble->rx_frag = ble->rx_odd = 0;
    ble->rx_frames = ble->rx_bad = ble->tx_fail = 0;

    /* Same sequence as the stock HID app: drop the phone link, use our own
     * key file so the phone bond is untouched, then switch profiles. */
    bt_disconnect(ble->bt);
    furi_delay_ms(200);
    bt_keys_storage_set_storage_path(ble->bt, BLE_KEYS_PATH);
    ble->profile = bt_profile_start(ble->bt, &ble_profile_template, ble);
    if(ble->profile == NULL) {
        FURI_LOG_E(TAG, "profile start failed");
        ble_restore_default(ble);
        return false;
    }
    /* Deliberate, as in the stock HID app: starting this transport is an
     * explicit request to use the radio, so it advertises even when
     * Bluetooth is switched off in the Flipper's settings. The setting is
     * honoured again when the default profile is restored. */
    furi_hal_bt_start_advertising();
    ble->started = true;
    return true;
}

static void ble_stop(BlTransport* t) {
    Ble* ble = t->impl;
    if(!ble->started) return;
    ble->started = false;
    ble_restore_default(ble); /* runs ble_profile_stop on the bt thread */
    ble->connected = false;
    ble->subscribed = false;
    ble_flush_rx(ble);
}

/* The firmware can replace our profile behind our back (it reloads the
 * default one when the SD card is remounted, for example). The services
 * are gone then; only stopping and starting the transport brings it back. */
static bool ble_lost(const Ble* ble) {
    return ble->started && !ble->svc_ready;
}

/* The one place the link state is sampled. On every change, anything
 * queued or half-assembled belongs to the previous link and is discarded
 * (a peer sends nothing on a new link until it has received our HELLO, and
 * the caller sends HELLO only after seeing the new id from here). */
static uint32_t ble_link_id(BlTransport* t) {
    Ble* ble = t->impl;
    uint32_t link = 0;
    if(ble->started && ble->svc_ready && ble->subscribed) {
        link = ble->link_seq;
        if(link == 0) link = 1; /* 0 is reserved for "down" */
    }
    if(link != ble->seen_link) {
        ble->seen_link = link;
        ble_flush_rx(ble);
    }
    return link;
}

static void ble_drop_link(BlTransport* t) {
    Ble* ble = t->impl;
    if(!ble->started || ble_lost(ble)) return;
    ble->subscribed = false;
    ble->connected = false;
    /* bt_disconnect() ends the link through gap_stop_advertising(), which
     * also clears the firmware's restart-advertising flag, and returns as
     * soon as GAP is idle (before the disconnect has completed on air).
     * Ask for advertising again, or the StickS3 could never find us after
     * a failed handshake; that sets the flag, so GAP also re-advertises
     * when the disconnect completes. ble_poll() re-checks as well. */
    bt_disconnect(ble->bt);
    furi_hal_bt_start_advertising();
}

static int ble_send(BlTransport* t, const uint8_t* frame, size_t len) {
    Ble* ble = t->impl;
    if(!ble->started || !ble->svc_ready || !ble->subscribed) return BL_E_STATE;
    const size_t chunk_cap = bl_ble_chunk_for_mtu(ble->mtu);
    uint8_t chunk[BL_BLE_CHUNK_MAX];
    unsigned i = 0;
    int done = 0;
    while(!done) {
        size_t clen = 0;
        done = bl_frag_chunk(frame, len, chunk_cap, i++, chunk, sizeof(chunk), &clen);
        if(done < 0) return done;
        BleTxRef ref = {.ptr = chunk, .len = (uint16_t)clen};
        /* NB: this firmware call returns TRUE ON FAILURE. It already
         * retries internally while the stack's TX pool is full. */
        if(ble_gatt_characteristic_update(ble->svc_handle, &ble->tx_char, &ref)) {
            ble->tx_fail++;
            return BL_E_NO_RESOURCE;
        }
    }
    return BL_OK;
}

static void ble_poll(BlTransport* t, BlTransportRxCb cb, void* ctx) {
    Ble* ble = t->impl;
    if(!ble->started) return;
    if(ble_lost(ble)) {
        ble->connected = false;
        ble->subscribed = false;
        (void)ble_link_id(t); /* reports the link as down, flushes */
        return; /* not our radio any more: do not touch it */
    }

    (void)ble_link_id(t);

    /* A central that is connected but has not subscribed blocks everyone
     * else, since the radio does not advertise while connected. */
    if(bl_sub_guard_expired(
           &ble->sub_guard, ble->connected, ble->subscribed, furi_get_tick(),
           BLE_SUBSCRIBE_TIMEOUT_MS)) {
        ble_drop_link(t);
    }

    /* Self-heal: while this transport is running the radio must be either
     * advertising or connected. A peer-initiated disconnect re-advertises
     * by itself; this covers every other way of ending up idle. */
    if(!ble->subscribed && !furi_hal_bt_is_active()) {
        furi_hal_bt_start_advertising();
    }

    BleChunk c;
    while(furi_message_queue_get(ble->rxq, &c, 0) == FuriStatusOk) {
        const uint8_t* frame = NULL;
        size_t flen = 0;
        int rc = bl_reasm_push(&ble->reasm, c.data, c.len, &frame, &flen);
        if(rc < 0) {
            ble->rx_bad++;
        } else if(frame != NULL) {
            ble->rx_frames++;
            cb(frame, flen, ctx);
        }
    }
}

static const char* ble_status(BlTransport* t) {
    Ble* ble = t->impl;
    if(!ble->started) return "BLE off";
    if(ble_lost(ble)) return "BLE taken by system\nPress Connect again";
    if(ble->subscribed) return "BLE linked";
    return ble->connected ? "BLE connecting" : "BLE advertising";
}

/* Link counters. "frag" counts events that were a piece of a longer write;
 * "odd" counts pieces that could not be joined (then: raw Offset field in
 * hex / length of the last one). */
static void ble_diag(BlTransport* t, char* out, size_t cap) {
    Ble* ble = t->impl;
    if(cap == 0) return;
    out[0] = 0;
    if(!ble->started) return;
    int n = snprintf(
        out, cap, "link %lu mtu %u rx %lu/%lu\ndrop %lu bad %lu txf %lu",
        (unsigned long)ble->link_seq, (unsigned)ble->mtu, (unsigned long)ble->rx_chunks,
        (unsigned long)ble->rx_frames,
        (unsigned long)ble->rx_dropped, (unsigned long)ble->rx_bad, (unsigned long)ble->tx_fail);
    if((ble->rx_frag != 0 || ble->rx_odd != 0) && n > 0 && (size_t)n < cap) {
        snprintf(
            out + n, cap - (size_t)n, "\nfrag %lu odd %lu %04X/%u", (unsigned long)ble->rx_frag,
            (unsigned long)ble->rx_odd, (unsigned)ble->rx_odd_offset, (unsigned)ble->rx_odd_len);
    }
}

static void ble_free(BlTransport* t) {
    if(!t) return;
    Ble* ble = t->impl;
    ble_stop(t);
    furi_message_queue_free(ble->rxq);
    furi_record_close(RECORD_BT);
    free(ble);
    free(t);
}

static const BlTransportApi BLE_API = {
    .name = "BLE",
    .start = ble_start,
    .stop = ble_stop,
    .link_id = ble_link_id,
    .drop_link = ble_drop_link,
    .send = ble_send,
    .poll = ble_poll,
    .status = ble_status,
    .diag = ble_diag,
    .free = ble_free,
};

BlTransport* bl_transport_ble_alloc(void) {
    BlTransport* t = malloc(sizeof(BlTransport));
    Ble* ble = malloc(sizeof(Ble));
    memset(ble, 0, sizeof(*ble));
    ble->bt = furi_record_open(RECORD_BT);
    ble->rxq = furi_message_queue_alloc(BLE_RX_QUEUE_DEPTH, sizeof(BleChunk));
    bl_reasm_init(&ble->reasm);
    t->api = &BLE_API;
    t->impl = ble;
    return t;
}
