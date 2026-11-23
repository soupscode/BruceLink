/*
 * BruceLink shared core — demo pairing key (both devices).
 *
 * !!! DEMO KEY — PUBLIC, PROVIDES NO SECURITY !!!
 *
 * The session is authenticated with a 32-byte key shared by the Flipper and
 * the StickS3. Real per-device provisioning (task M5.3: the StickS3 generates
 * a key, the Flipper is given it out of band) is not implemented yet, so both
 * sides currently compile in this fixed key, which is published in the
 * repository. Until M5.3 lands, anyone can authenticate as either device.
 * The app says so on its Connection and About screens.
 *
 * Defined once here so the two sides cannot drift apart.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

static const uint8_t BRUCELINK_DEMO_PSK[BL_KEY_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a,
    0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};
