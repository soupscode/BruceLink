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

```
$ cd tests/c && make test          # -Wall -Wextra -Werror -Wconversion, ASan+UBSan
BruceLink core tests
[crypto]       8 tests  ok   (SHA-256 NIST, streaming, HMAC RFC4231,
                              HKDF RFC5869, X25519 RFC7748 + ECDH + low-order)
[protocol]    14 tests  ok   (frame, MAC tamper, malformed, handshake, wrong PSK,
                              replay, cross-key, fragmentation, COBS)
[capability+capture] 12 ok   (registry, pcap bytes, CRC32, safety gate,
                              capture lifecycle/gating/failures)
[integration]  4 tests  ok   (simulated lossy transport: bulk, corruption,
                              dropped fragment, duplicate/replay)

1434 checks, 0 failed           exit 0
```

```
$ make cppcheck      # --enable=warning,style,performance,portability
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
Build success only. The app has **not** been run on a Flipper.

## ESP32-S3 bridge — cross-compile + link

```
$ cd esp32/test_compile && pio run     (board esp32-s3-devkitc-1, NimBLE 2.5.1)
RAM:   [=         ]  10.1% (used 32932 bytes from 327680 bytes)
Flash: [==        ]  17.0% (used 569248 bytes from 3342336 bytes)
Successfully created ESP32-S3 image.
========================= [SUCCESS] =========================
```
Compiles the bridge core + NimBLE central transport + shared core with a
**stub** backend. Not the Bruce firmware, not flashed, not run.

## Not yet tested (requires the local hardware session)

- BLE advertising, Just-Works pairing, MTU negotiation, notify throughput.
- FAP running on a Flipper (UI, loopback, then BLE).
- BruceBackendImpl compiled inside the Bruce tree; the Bruce patch.
- On-air Wi-Fi scan, sniffer/EAPOL capture to LittleFS, pcap transfer.
- Active-op forwarding and the safety gate under real transmission.
- BLE + Wi-Fi-promiscuous coexistence.
