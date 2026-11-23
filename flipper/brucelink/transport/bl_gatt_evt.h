/*
 * BruceLink Flipper app — parser for the BLE controller events the custom
 * GATT service cares about.
 *
 * The Flipper's service event handler receives raw HCI event packets from
 * the STM32WB BLE stack. This module turns one packet into a small typed
 * result. It is portable C with no SDK dependency so it is host-tested
 * (tests/c/test_gatt_evt.c), including malformed and truncated packets;
 * the on-device handler stays a few lines.
 *
 * Packet layout and codes verified against STM32CubeWB v1.20.0
 * (stm32wb_copro @ 133182d, the release Momentum mntm-012 pins):
 *   hci_uart_pckt   { u8 type; data[] }                       tl.h
 *   hci_event_pckt  { u8 evt; u8 plen; data[] }               tl.h
 *   evt_blecore_aci { u16 ecode; data[] }                     tl.h
 *   HCI_DISCONNECTION_COMPLETE_EVT_CODE        0x05           ble_std.h
 *   HCI_LE_META_EVT_CODE                       0x3E           ble_std.h
 *   HCI_LE_CONNECTION_COMPLETE_SUBEVT_CODE     0x01           ble_std.h
 *   evt_le_meta_event { u8 subevent; data[] }                 tl.h
 *   hci_le_connection_complete_event_rp0 { u8 status; u16 conn; ... }
 *   HCI_VENDOR_SPECIFIC_DEBUG_EVT_CODE         0xFF           ble_std.h
 *   ACI_GATT_ATTRIBUTE_MODIFIED_VSEVT_CODE     0x0C01         ble_vs_codes.h
 *   ACI_ATT_EXCHANGE_MTU_RESP_VSEVT_CODE       0x0C03         ble_vs_codes.h
 *   aci_gatt_attribute_modified_event_rp0
 *       { u16 conn; u16 attr_handle; u16 offset; u16 len; data[] }
 *   aci_att_exchange_mtu_resp_event_rp0 { u16 conn; u16 server_rx_mtu }
 * All multi-byte fields are little-endian.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    BL_GATT_EVT_NONE = 0, /* not ours / malformed: let other handlers see it */
    BL_GATT_EVT_RX_DATA, /* central wrote the RX characteristic value */
    BL_GATT_EVT_SUBSCRIBED, /* central enabled notifications on TX */
    BL_GATT_EVT_UNSUBSCRIBED,
    BL_GATT_EVT_MTU, /* ATT MTU negotiated */
    BL_GATT_EVT_DISCONNECTED,
    BL_GATT_EVT_CONNECTED, /* a central connected (not yet subscribed) */
} bl_gatt_evt_kind_t;

typedef struct {
    bl_gatt_evt_kind_t kind;
    const uint8_t* data; /* RX_DATA: points into the packet */
    uint16_t len; /* RX_DATA: byte count */
    uint16_t mtu; /* MTU: negotiated ATT MTU */
} bl_gatt_evt_t;

/*
 * Parse one HCI event packet (starting at the hci_uart_pckt type byte).
 * `avail` is the number of readable bytes at `pckt`; pass 3 + pckt[2] when
 * only the packet's own length field is known. rx_char_handle and
 * tx_char_handle are the characteristic declaration handles returned when
 * the characteristics were added (value = handle + 1, CCCD = handle + 2).
 * Returns true and fills *out for an event this service consumes
 * (RX_DATA / SUBSCRIBED / UNSUBSCRIBED). MTU, CONNECTED and DISCONNECTED
 * also fill *out but return false: they must stay visible to the
 * firmware's own GAP handler.
 */
bool bl_gatt_evt_parse(
    const uint8_t* pckt,
    size_t avail,
    uint16_t rx_char_handle,
    uint16_t tx_char_handle,
    bl_gatt_evt_t* out);

/*
 * Subscription guard. While a central is connected the Flipper does not
 * advertise, so a central that connects and never enables notifications
 * would keep the StickS3 out indefinitely. Call on every poll with the
 * current link flags; returns true (once per timeout period) when a
 * connected central has gone timeout_ms without subscribing and should be
 * dropped. Wrap-safe.
 */
typedef struct {
    bool waiting;
    uint32_t since_ms;
} bl_sub_guard_t;

void bl_sub_guard_init(bl_sub_guard_t* g);
bool bl_sub_guard_expired(
    bl_sub_guard_t* g,
    bool connected,
    bool subscribed,
    uint32_t now_ms,
    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
