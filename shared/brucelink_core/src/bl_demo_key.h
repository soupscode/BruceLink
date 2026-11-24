/*
 * BruceLink shared core — the public demo key.
 *
 * !!! DEMO KEY — PUBLIC, PROVIDES NO SECURITY !!!
 *
 * Real devices use the key from pairing (ADR 0005). This key is published
 * in the repository, so a session made with it authenticates nobody. It is
 * used as a key only by the host tests and the ESP32 compile check. The
 * StickS3 carries it only to refuse it: its key store rejects it, and a
 * bridge holding it refuses every active op, job and capture. The Flipper
 * app does not contain it at all (tests/test_demo_key.py checks the sources
 * and the built .fap).
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "bl_types.h"

static const uint8_t BRUCELINK_DEMO_PSK[BL_KEY_LEN] = {
    0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
    0xcc, 0xdd, 0xee, 0xff, 0x00, 0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a,
    0x69, 0x78, 0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0};
