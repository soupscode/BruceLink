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

1517 checks, 0 failed           exit 0
```

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
88 checks, 0 failed             exit 0
```

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
Build success only. The app has **not** been run on a Flipper.

## ESP32-S3 bridge — cross-compile + link

```
$ cd esp32/test_compile && pio run     (board esp32-s3-devkitc-1, NimBLE 2.5.1)
RAM:   [=         ]  10.1% (used 32972 bytes from 327680 bytes)
Flash: [==        ]  17.1% (used 570876 bytes from 3342336 bytes)
========================= [SUCCESS] =========================
```
(After PR #1 fixes; includes `BruceLinkFormat.cpp` and `bl_textbuf.c`.)
Compiles the bridge core + NimBLE central transport + shared core with a
**stub** backend. Not the Bruce firmware, not flashed, not run.

## Not yet tested (requires the local hardware session)

- BLE advertising, Just-Works pairing, MTU negotiation, notify throughput.
- FAP running on a Flipper (UI, loopback, then BLE).
- BruceBackendImpl compiled inside the Bruce tree; the Bruce patch.
- On-air Wi-Fi scan, sniffer/EAPOL capture to LittleFS, pcap transfer.
- Active-op forwarding and the safety gate under real transmission.
- BLE + Wi-Fi-promiscuous coexistence.
