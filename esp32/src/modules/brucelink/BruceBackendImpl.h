/*
 * BruceLink ↔ Bruce glue: BruceBackend implemented against Bruce 1.17.
 *
 * This file is meant to live in the Bruce tree at
 *   src/modules/brucelink/BruceBackendImpl.{h,cpp}
 * (Bruce's build_src_filter compiles everything under src/ automatically).
 * It is NOT built by esp32/test_compile — it needs Bruce's headers.
 *
 * Every method calls Bruce's OWN implementation (symbols verified in the
 * Bruce 1.17 audit). BruceLink reimplements none of Bruce's features; the
 * only new Bruce code is the thin headless hooks in bruce_brucelink_hooks.h
 * (see docs/decisions/0003-capture-and-safety.md, "needs a Bruce patch").
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <BruceLinkBridge.h> // lib/BruceLink (installed by tools/install_into_bruce.py)

namespace brucelink {

class BruceBackendImpl : public BruceBackend {
public:
    size_t deviceInfo(char* out, size_t cap) override;
    bl_job_record_t* jobRecord() override;
    void jobRestart() override;
    bool captureBackend(const bl_capture_backend_t** be, void** ctx) override;
    int activeOp(uint16_t cap_id, const uint8_t* target, uint8_t channel, uint32_t max_frames)
        override;
    uint32_t nowMs() override;
    void randomBytes(uint8_t* out, size_t len) override;
};

/* One USB-serial log line with the memory figures that decide whether a
 * radio can start. For finding where a remote operation stops. */
void logStep(const char* what);

} // namespace brucelink
