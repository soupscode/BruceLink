/*
 * BruceLink shared core — BLE GATT profile identifiers.
 *
 * One definition for both devices. The StickS3 (NimBLE) wants UUID
 * strings; the Flipper (STM32WB stack) wants 16 raw bytes in
 * little-endian order, i.e. the string's bytes reversed.
 * tests/c/test_client.c checks the two forms agree, so the devices can
 * never be built looking for different services.
 *
 *   service  a1b20001-7a9c-4b1d-9c2e-5f6a7b8c9d00
 *   RX char  a1b20002-…   central -> peripheral   (write / write-no-response)
 *   TX char  a1b20003-…   peripheral -> central   (notify)
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define BL_UUID_SERVICE_STR "a1b20001-7a9c-4b1d-9c2e-5f6a7b8c9d00"
#define BL_UUID_RX_STR "a1b20002-7a9c-4b1d-9c2e-5f6a7b8c9d00"
#define BL_UUID_TX_STR "a1b20003-7a9c-4b1d-9c2e-5f6a7b8c9d00"

#define BL_UUID_LE_(id)                                                                 \
    {                                                                                   \
        0x00, 0x9d, 0x8c, 0x7b, 0x6a, 0x5f, 0x2e, 0x9c, 0x1d, 0x4b, 0x9c, 0x7a, (id), \
            0x00, 0xb2, 0xa1                                                            \
    }
#define BL_UUID_SERVICE_LE BL_UUID_LE_(0x01)
#define BL_UUID_RX_LE BL_UUID_LE_(0x02)
#define BL_UUID_TX_LE BL_UUID_LE_(0x03)

/* Largest GATT value either side sends in one write/notification: the
 * Flipper's stack caps characteristic updates below 250 bytes and inbound
 * attribute data at 245 per event (ADR 0001). */
#define BL_BLE_CHUNK_MAX 244u
/* Usable payload at the default ATT MTU (23 - 3), before negotiation. */
#define BL_BLE_CHUNK_DEFAULT 20u

/* Chunk size both ends use for a negotiated ATT MTU: MTU minus the 3-byte
 * ATT header, never above BL_BLE_CHUNK_MAX (the peer may agree to a larger
 * MTU than the Flipper's characteristics can carry), and the default until
 * an MTU is known. */
static inline size_t bl_ble_chunk_for_mtu(uint16_t att_mtu) {
    if(att_mtu < 23u) return BL_BLE_CHUNK_DEFAULT;
    size_t n = (size_t)att_mtu - 3u;
    return n > BL_BLE_CHUNK_MAX ? BL_BLE_CHUNK_MAX : n;
}
