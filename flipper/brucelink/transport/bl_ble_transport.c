/*
 * BruceLink Flipper app — BLE transport (hardware-dependent stub).
 *
 * NOT YET IMPLEMENTED against hardware. The design, from the Momentum
 * mntm-012 source audit (docs/decisions/0001-ble-transport.md):
 *
 *   - The Flipper is a BLE peripheral; the StickS3 (NimBLE) is the
 *     central. The light copro stack is peripheral-only, so this is the
 *     only workable role split.
 *   - Define a FAP-owned FuriHalBleProfileTemplate with one custom
 *     128-bit service: an RX characteristic (WRITE / WRITE_NO_RSP with
 *     GATT_NOTIFY_ATTRIBUTE_WRITE) and a TX characteristic (NOTIFY),
 *     each <= 244 bytes, following the in-tree BadKB ext-profile
 *     pattern (applications/.../ble_hid_ext_profile.c).
 *   - Start: bt_disconnect, delay 200ms, bt_keys_storage_set_storage_path
 *     (APP_DATA ".brucelink.keys"), bt_profile_start, furi_hal_bt_
 *     start_advertising, bt_set_status_changed_callback. Exit: restore
 *     the default profile (see hid.c), unregister the GATT event handler.
 *   - GapConfig: own MAC + own name; GapPairingNone with ENCRY (not
 *     AUTHEN) permissions, the ESP32 starting Just Works pairing so the
 *     Connected event fires.
 *   - Frames are fragmented to the negotiated MTU with bl_reasm; the
 *     GATT write/notify boundary carries one chunk.
 *
 * Until a hardware session implements and verifies this, connect()
 * fails cleanly and the app falls back to the loopback transport.
 * SPDX-License-Identifier: MIT
 */
#include "bl_transport.h"

#include <furi.h>

static bool ble_connect(BlTransport* t) {
    UNUSED(t);
    FURI_LOG_W("BruceLink", "BLE transport not yet implemented (hardware session)");
    return false;
}
static void ble_disconnect(BlTransport* t) {
    UNUSED(t);
}
static int ble_send(BlTransport* t, const uint8_t* f, size_t n) {
    UNUSED(t);
    UNUSED(f);
    UNUSED(n);
    return BL_E_STATE;
}
static void ble_poll(BlTransport* t, BlTransportRxCb cb, void* ctx) {
    UNUSED(t);
    UNUSED(cb);
    UNUSED(ctx);
}
static bool ble_is_connected(BlTransport* t) {
    UNUSED(t);
    return false;
}

const BlTransportApi BL_BLE_TRANSPORT_API = {
    .name = "BLE (hardware)",
    .connect = ble_connect,
    .disconnect = ble_disconnect,
    .send = ble_send,
    .poll = ble_poll,
    .is_connected = ble_is_connected,
};
