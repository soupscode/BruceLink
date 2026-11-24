/*
 * BruceLink: RAM overrides for the StickS3 build.
 *
 * Passed to every compile as a forced include (`-include`, added to the
 * m5stack-sticks3 build flags by tools/install_into_bruce.py, together with
 * -DBRUCELINK_RAM_TRIM). GCC applies all -D flags before forced includes, so
 * this header can also undo a -D from Bruce's board file. RAM review,
 * 2026-11-23; figures computed from the build, not measured on the stick.
 *
 * 1. NimBLE's host buffers in PSRAM. NimBLE-Arduino compiles the host from
 *    source but takes its settings from the framework's prebuilt sdkconfig.h,
 *    which puts the msys pools and the ACL and HCI event buffers (about 16 kB,
 *    allocated at BLE start) in internal RAM. sdkconfig.h is `#pragma once`,
 *    so redefining the mode here, before anything else, wins; a -D flag would
 *    not. The buffers are only used from tasks (no DMA, no ISR); IDF supports
 *    this mode when PSRAM is in the heap. Applies to Bruce's BLE tools too.
 *
 * 2. No USB HID (owner's choice): undoes the board's -DUSB_as_HID, removing
 *    BadUSB/keyboard and mouse over USB, the USB clicker and U2F. Together
 *    with no USB mass storage (massStorage.h, patched by the installer)
 *    nothing links TinyUSB, about 31 kB of static RAM. BLE keyboard and the
 *    USB serial console stay.
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "sdkconfig.h"

#if defined(BOARD_HAS_PSRAM) && defined(CONFIG_SPIRAM)
#undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_INTERNAL
#undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_IRAM_8BIT
#undef CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_DEFAULT
#define CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL 1
#endif

#if defined(BRUCELINK_RAM_TRIM)
#undef USB_as_HID
#endif
