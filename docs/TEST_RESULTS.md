# BruceLink — test & build evidence

Actual results from runs in the cloud development session (2026-11-23).
**No result here involves physical hardware.** Hardware results are appended
by the local session (see `LOCAL_HANDOFF.md`).

## Environment

| Tool | Version |
|---|---|
| Host C compiler | gcc 13.3.0 (Ubuntu 24.04) |
| ARM cross compiler | xPack `arm-none-eabi-gcc` 12.3.1 (20230626) |
| Flipper SDK | Momentum `mntm-012`, `flipper-z-f7-sdk-mntm-012.zip` sha256 `c90530c3e0aa7bc4…` |
| ufbt | 0.2.6 |
| ESP32 platform | pioarduino `platform-espressif32` 55.03.39 (arduino-esp32 3.3.9, ESP-IDF 5.5.4) |
| Xtensa compiler | `xtensa-esp-elf` 14.2.0 (20260121) |
| NimBLE-Arduino | 2.5.1 |
| Python | 3.13.18 |

### Sandbox notes (affect reproducibility, not results)

- The Flipper toolchain host `update.flipperzero.one` and the PlatformIO registry
  API were unreachable from the sandbox. The **xPack ARM GCC 12.3.1** (same GCC
  major/minor as Flipper's toolchain 39) was used via `FBT_NOENV=1`; it emits
  harmless `DWARF error: line info` linker warnings against Flipper's prebuilt
  libs. NimBLE was fetched by its GitHub tag `2.5.1` instead of the registry.
  `tool-scons` was supplied from PyPI `scons` 4.11.1. Details in `LOCAL_HANDOFF.md`.

## M0 — original scaffold (audit baseline)

```
$ python3 -m unittest discover -s tests -v
Ran 5 tests ... OK
```
The original v0 Python codec tests passed. The C/C++ sources were stubs; the
FAP stub was later confirmed to compile.

## Shared protocol core — host unit + integration tests

Updated 2026-11-23 after PR #1 review fixes (was 1434 checks at `31629db`).

```
$ cd tests/c && make test          # -Wall -Wextra -Werror -Wconversion, ASan+UBSan
BruceLink core tests
[crypto]       8 tests  ok   (SHA-256 NIST, streaming, HMAC RFC4231,
                              HKDF RFC5869, X25519 RFC7748 + ECDH + low-order)
[protocol]    14 tests  ok   (frame, MAC tamper, malformed, handshake, wrong PSK,
                              replay, cross-key, fragmentation, COBS)
[capability+capture] 17 ok   (registry, pcap bytes, CRC32, safety gate incl.
                              unicast-only targets + frame budget/refund,
                              capture lifecycle/gating/failures, stop-exactly-once,
                              invalid mode, null backend)
[integration]  4 tests  ok   (simulated lossy transport: bulk, corruption,
                              dropped fragment, duplicate/replay)
[textbuf]      7 tests  ok   (bounded builder: truncation, exact fit, zero/NULL
                              capacity, long input, newline injection, LONG_MIN)

[client]      12 tests  ok   (handshake + request, HELLO fits 128 B, wrong key,
                              stray handshake frames ignored, handshake
                              timeout once / across tick wrap, request timeout
                              + busy, events keep a request pending, link-down
                              wipes keys, frames before a link ignored, UUID
                              string/byte forms agree, chunk size per MTU)
[gatt-evt]     9 tests  ok   (Flipper BLE event parser: RX data, subscribe /
                              unsubscribe, MTU + disconnect pass-through,
                              connection complete, foreign events, offset
                              writes, malformed, fuzz; subscription guard)

1775 checks, 0 failed           exit 0
```
(1517 after the first review round; 1527 after the second; `[client]` and
`[gatt-evt]` added with the Flipper BLE profile. `bl_gatt_evt.c` is Flipper
app source compiled for the host with the strict core flags.)

## ESP32 bridge core — host tests (new)

The portable bridge core (`esp32/lib/BruceLink/BruceLinkBridge.cpp`,
`BruceLinkFormat.cpp`) is now built with the host C++ compiler under ASan+UBSan
and driven by a real Flipper-side (central) session through fragmentation,
against a scripted mock `BruceBackend`.

```
$ ./bl_bridge_tests                 (built by `make test`)
[bridge]      13 tests  ok   (reconnect, ESTOP vs capture, poll-failure cleanup,
                              no-backend init, frame budget, refund, backend
                              overrun -> e-stop, broadcast/channel validation,
                              length clamping, capture args, MTU update,
                              Wi-Fi line formatting)
[supervision]  9 tests  ok   (rate window vs re-enable and reconnect, forged
                              proof, silent peer, stalled handshake, established
                              link kept, unprovisioned bridge, millis() wrap,
                              per-peer cooldown table)
[client-bridge] 7 tests ok   (the Flipper's bl_client against the real bridge
                              core, fragmented both ways: handshake + three
                              request types, lost HELLO_ACK -> timeout ->
                              re-link, lost reply -> timeout, session kept,
                              wrong key seen by both ends, link loss
                              mid-request, demo key never enables active ops,
                              real key unaffected)
188 checks, 0 failed            exit 0
```
The demo-key test was written first and failed against the previous bridge
(9 failed checks: "enable" accepted, 16 frames reached the mock backend).

### PR #1 second Codex review (`9f89c4d`) — reproduced, fixed, regression-tested

| Finding | Reproduction on `9f89c4d` | Regression test |
|---|---|---|
| Re-sending "enable active" reset the rate window (Codex P2) | 20 enable/deauth pairs at a constant clock sent **320 frames** vs the 64-frame limit; the new C test fails 6 assertions against the old `bl_safety.c` | `t_safety_window_survives_enable_and_reset`, `t_codex2_reenable_does_not_reset_budget` |
| A new session also reset it (found while fixing) | 64 frames, reconnect inside the window → next op accepted (80 frames) | `t_codex2_reconnect_does_not_reset_budget` |
| Unauthenticated peer could hold the only BLE connection (Codex P2) | By inspection: `connected_` set before auth, handshake failures ignored, rescan only when disconnected. The NimBLE transport is not host-runnable | Bridge supervision: `t_codex2_bad_proof_requests_disconnect`, `..._silent_peer_times_out`, `..._stalled_handshake_times_out`, `..._established_never_dropped`, `..._unprovisioned_drops_immediately`, `..._auth_deadline_wraps`; cooldown: `t_peer_cooldown` |
| Bridge mutated from two FreeRTOS tasks; HELLO could reach the bridge before `onConnect()` (found while fixing) | By inspection (notify callback on NimBLE host task vs `loop()`; subscribe happened before `onConnect()`) | Transport restructured (single owner); compile-checked only, see hardware step 5 |

### PR #1 review defects — reproduced against `31629db`, fixed, regression-tested

Each defect was first reproduced against the `31629db` sources (old-API harness,
not committed), then fixed, then covered by a test that runs in `make test`.

| Defect (reporter) | Reproduction on `31629db` | Regression test |
|---|---|---|
| PSK lost on reconnect (owner P1, Codex P1) | 2nd connection fails authentication | `t_pr1_reconnect_preserves_psk` |
| ESTOP did not block `CAP_START` (Codex P2) | capture backend started after ESTOP | `t_pr1_estop_blocks_capture_start` |
| Poll failure left backend open (Codex P2) | backend stop never called (0) | `t_pr1_poll_failure_stops_backend_via_{tick,status}`, `t_capture_poll_failure_transitions_error` |
| `capture_` not initialised (owner P2) | UBSan: invalid `bl_capture_state_t` value `0xA5A5A5A5` | `t_pr1_capture_init_without_backend`, `t_capture_null_backend` |
| Rate limit not bounding real frames (owner P2) | 64 ops accepted, **1024 frames** sent vs a 64-frame window | `t_pr1_rate_limit_bounds_actual_frames`, `t_pr1_unused_frames_refunded`, `t_safety_budget_and_refund` |
| Wi-Fi scan length bookkeeping (owner P2) | old loop returned `SIZE_MAX` for cap 0; cut lines mid-SSID; `\n` in an SSID injected a fake line | `t_pr1_wifi_line_bounded_and_sanitized`, `[textbuf]` suite |
| Broadcast/multicast BSSID accepted as active target (found during fixes) | `ff:ff:ff:ff:ff:ff` accepted | `t_safety_rejects_non_specific_targets`, `t_active_rejects_broadcast_and_bad_channel` |

```
$ make cppcheck      # core (C, style) + bridge core (C++)
(no findings)        exit 0
```

## C ↔ Python wire interop

```
$ python3 -m unittest discover -s tests -v
FrameTests     4 ok   CobsTests 1 ok   PcapTests 2 ok
InteropTests   4 ok   (frame+MAC, COBS, pcap headers, CRC32 — byte-identical
                       between the C core and the independent Python reference)
Ran 11 tests in 0.47s   OK
```

## Flipper FAP — cross-compile

```
$ ufbt          (mntm-012 SDK, xPack arm-none-eabi-gcc 12.3.1)
  CC  transport/bl_ble_transport.c
  CC  transport/bl_transport_loopback.c
  CC  brucelink.c   (+ shared core under brucelink_core/)
  LINK / FAP / FASTFAP
  APPCHK  brucelink.fap
          Target: 7, API: 87.1
dist/brucelink.fap   20900 bytes
```
Re-verified with a clean build after the PR #1 fixes (14 units incl.
`bl_textbuf.c`). The ARM build caught one portability bug the host build could
not: an enum-range check that is tautological with Flipper's unsigned short
enums (`-Werror=type-limits`); fixed with a single unsigned bound check.
Build success only at that point.

Rebuilt clean with the BLE profile and the `bl_client`-based app: 16 units,
APPCHK pass (every firmware symbol the profile uses is exported at API 87.1),
`dist/brucelink.fap` 30132 bytes. **This build has not run on a Flipper**; the
loopback hardware result below is for the build before it.

## ESP32-S3 bridge — cross-compile + link

```
$ cd esp32/test_compile && pio run     (board esp32-s3-devkitc-1, NimBLE 2.5.1)
RAM:   [=         ]  10.1% (used 33140 bytes from 327680 bytes)
Flash: [==        ]  17.1% (used 571936 bytes from 3342336 bytes)
========================= [SUCCESS] =========================
```
(After both PR #1 review rounds; includes `BruceLinkFormat.cpp`,
`BruceLinkPeerGuard.cpp`, the single-owner transport and `bl_textbuf.c`.)
Compiles the bridge core + NimBLE central transport + shared core with a
**stub** backend. Not the Bruce firmware, not flashed, not run.

## Independent review of the Flipper BLE profile (2026-11-23)

A separate reviewer checked the never-run firmware-facing code against the
pinned Momentum, ST (`stm32wb_copro`) and NimBLE-Arduino sources. No crash or
functional break was found from source; event layouts, handle offsets,
lifecycle assertions, threading, the TX callback and interop were confirmed.
Findings and outcome:

| Finding | Outcome |
|---|---|
| A central that connects but never subscribes holds the only link | Fixed: connection event parsed, dropped after a timeout (5 s, later 20 s) (`t_gatt_connection_complete`, `t_sub_guard`) |
| StickS3 chunk size not capped at 244 if a larger MTU is negotiated | Fixed: one shared rule (`t_ble_chunk_for_mtu`) |
| Any handshake receive error was terminal (≈40 s recovery from one stray frame) | Fixed; test failed on the old code (10 checks) (`t_client_stray_handshake_frame_ignored`) |
| 3 s handshake budget unmeasured against X25519 on the Flipper | Raised to 5 s; request timeout raised to 10 s; **to be measured on hardware** |
| Advertisement exactly 31 bytes, rejection is silent | Name shortened to `BL` (3 bytes of margin) |
| Firmware reloads the default profile on SD remount | Transport detects it, reports link down, stops touching the radio |
| Link state sampled in two places (possible flush after HELLO) | One sampling point |
| RX queue (8) too small for a full frame at the default MTU | 32 |
| Connect blocked the UI for seconds before the screen changed | Screen first, transport start on the next tick |
| Status screens did not say the link is unencrypted | Added |
| Session role comment inverted | Fixed |
| Not changed | `rx_dropped` not shown in the UI; StickS3 ignores a failed GATT write (the request then times out); a central may still start pairing (stock firmware behaviour) |

What only hardware can answer is listed in `docs/HARDWARE_TESTING.md` §5.

## Hardware results

### 2026-11-23 — Flipper app over the loopback transport (owner's Flipper)

First run of any BruceLink code on a device. Built and launched locally with
`ufbt launch` (real Flipper toolchain, Windows) from `fix/connect-stack-overflow`.

| Build | Result (reported by the owner) |
|---|---|
| `f100604` (PR #1 as merged) | **Connect crashed the Flipper: BusFault.** Stack overflow, fixed in `2410767` |
| `0d8d272` | App launches, Connect works, no crash; reported "works good" |

Scope of this result: the app's UI, tick-driven polling, the authenticated
handshake (X25519 on the Flipper's CPU) and request/response, all against the
**in-process mock**. No radio, no StickS3. The owner later confirmed all five
screens (Connect, Device Info, Capabilities, Wi-Fi Scan, clean exit) showed the
expected content. Not confirmed: the Flipper's exact Momentum version.

### 2026-11-23 — Flipper BLE profile, first run on a radio (owner's Flipper + phone scanner)

Branch `feat/flipper-ble-profile` (the owner's download; presumed `b648d78`),
Transport: BLE, phone running a BLE scanner. Reported by the owner:

| Observation | Reading |
|---|---|
| The scanner finds the service and connects | Profile start, advertising (128-bit UUID + name within 31 bytes) and the GATT service work |
| The connection lasts about 5 s | Consistent with the subscription guard dropping a central that has not enabled notifications; **not yet confirmed** that notifications were off |
| The Flipper kept showing `BLE advertising` throughout | Display defect: the Connection screen was redrawn only on link-id or session changes, so `BLE connecting` never appeared. Fixed in the next commit |

Follow-up: 5 s proved too short to enable notifications by hand in the scanner
app; the guard timeout was raised to 20 s.

Second attempt, same session, after being given the subscribe steps (connect,
expand the `a1b20001` service, enable notifications on `a1b20003`, expect
`BLE linked` / `authenticating...` and hex data on the phone): the owner
reported **"it all works"**. Taken as: the central's subscription is detected
and the Flipper sends `HELLO` as notifications. Which commit was on the device
is not recorded.

Not individually confirmed: re-advertising after the handshake timeout drop,
the radio being handed back to the phone app on exit, the `BLE connecting`
status fix, repeated Connect/Back cycles. Not tested at all: anything with the
StickS3 (no Bruce-side build exists yet).

## Not yet tested (requires the local hardware session)

- The Flipper BLE profile on a radio: advertising, subscription, MTU, notify,
  re-advertising after a dropped link, handing the radio back on exit.
- The current app build on a Flipper at all (loopback ran on the previous one).
- BruceBackendImpl compiled inside the Bruce tree; the Bruce patch.
- On-air Wi-Fi scan, sniffer/EAPOL capture to LittleFS, pcap transfer.
- Active-op forwarding and the safety gate under real transmission.
- BLE + Wi-Fi-promiscuous coexistence.
