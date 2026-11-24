/*
 * BruceLink: NimBLE host buffers in PSRAM (StickS3 build).
 *
 * Passed to every compile as a forced include (`-include`, added to the
 * m5stack-sticks3 build flags by tools/install_into_bruce.py). NimBLE-Arduino
 * compiles the NimBLE host from source but takes its settings from the
 * framework's prebuilt sdkconfig.h, which puts the host's buffers in internal
 * RAM: the msys pools, the ACL and HCI event buffers, about 16 kB allocated at
 * BLE start (computed from the effective config; RAM review 2026-11-23).
 * Internal RAM is what the StickS3 runs out of; it has 8 MB of PSRAM.
 *
 * sdkconfig.h is `#pragma once`, so redefining the allocation mode here,
 * before anything else is included, wins for the whole build. A `-D` flag
 * does not: sdkconfig.h would define the internal mode again.
 *
 * These buffers are only used from tasks (the NimBLE host task and the
 * callers of its API), never by DMA or an ISR, and IDF supports this mode
 * when PSRAM is part of the heap (CONFIG_SPIRAM_USE_MALLOC). It applies to
 * Bruce's own BLE tools as well. Not yet run on hardware.
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
