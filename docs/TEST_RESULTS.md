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

## Bruce 1.17 + BruceLink — full firmware build (m5stack-sticks3)

```
$ git clone Bruce, git checkout 2b6ba4d (1.17), git submodule update --init
$ python3 tools/install_into_bruce.py <bruce>        (BruceLink 59268b0)
$ pio run -e m5stack-sticks3
  |-- BruceLink @ 0.1.0
  Compiling lib/BruceLink/{BruceLinkBridge,BruceLinkFormat,BruceLinkPeerGuard,NimBleCentralTransport}.cpp, bl_*.c
  Compiling src/modules/brucelink/{BruceBackendImpl,BruceLinkModule,bruce_brucelink_hooks_unavailable}.cpp
RAM:   [====      ]  40.7% (used 133236 bytes from 327680 bytes)
Flash: [======    ]  56.8% (used 4761363 bytes from 8388608 bytes)
[merge_bin] Success -> Bruce-m5stack-sticks3.bin (4827440 bytes)
======================== [SUCCESS] ========================
sha256 eaf88e41b48b359c5cde3a4b1ea38852e448f4aa45a411a76ca12f7461119106
```
The app fits Bruce's 0x4E0000 factory partition (93 %). BruceLink's sources
produced no compiler diagnostics of their own. The menu string, the service
UUID and the bridge symbols are present in the image. **Build and link only:
this firmware has not been flashed or run.**

Not a byte-for-byte upstream build. The PlatformIO registry is unreachable from
the cloud sandbox, so every registry dependency was fetched from its GitHub
repository instead and given registry-style metadata locally. Bruce pins some
of these to a version and leaves others floating; floating ones were taken at
the default branch head on 2026-11-23:

| Library | Version | Commit | Pinned by Bruce? |
|---|---|---|---|
| NimBLE-Arduino | 2.5.1 | 5ae2c43 | yes |
| ArduinoJson | 7.4.2 | 733bc4e | no (floating) |
| AsyncTCP | 3.6.1 | 0f18137 | no |
| ESPAsyncWebServer | 3.13.0 | f4169cd | no |
| Time | 1.6.1 | a18e50d | yes |
| NTPClient | 3.2.1 | 78d4237 | yes |
| ESP32Time | 2.0.6 | 56c7cca | yes |
| ESP8266Audio | 2.4.1 | d6184e0 | `^2.4.1` |
| ESP8266SAM | 1.1.0 | bb3e255 | `^1.1.0` |
| TinyGPSPlus | 1.1.0 | 0a20575 | no |
| FFT | 0.0.1 | b3d44f3 | `^0.0.1` |
| RF24 | 1.4.11 | 6f3da43 | yes |
| Adafruit Si4713 Library | 1.2.3 | 43c9ecb | yes |
| JPEGDecoder | 2.0.0 | bba81c8 | no |
| AnimatedGIF | 2.2.3 | 17a6a2e | no |
| PNGdec | 1.1.7 | 7ef2a09 | `^1.1.2` |
| FastLED | 3.10.3 | 20667c3 | yes |
| RadioLib | 7.4.0 | b1ed821 | `^7.4.0` |
| LibSSH-ESP32 | 5.10.0 | 343fce1 | no |
| M5GFX | 0.3.0 | 28d4794 | no (via M5Unified) |
| M5Unified | 0.3.0 | 96576a1 | no |

The other workarounds were local to the scratch build and are not in this
repository: dependency declarations stripped from those copies, two alias
entries for slash-named Arduino `depends=` lines, and the framework's built-in
`FFat` library copied in. Bruce's own sources were unchanged apart from what
the installer does. A local build with the real registry is the reference;
this one shows the integration compiles, links and fits.

## Pairing (ADR 0005) — host tests and builds (2026-11-23)

```
$ make -C tests/c clean test cppcheck
[pair]      13 tests ok   (happy path + session with the new key, peer confirms
                           first, rejection either side, man in the middle,
                           tampering with name / nonce / key changes the code,
                           commitment enforced, forged confirm, out-of-order and
                           garbage ignored, malformed COMMITs, low-order key,
                           timeout across tick wrap + cancel, name sanitising,
                           wipe and argument checks)
2137 checks, 0 failed
[pairing]    5 tests ok   (bridge pairing mode: full pairing then a session with
                           the key, rejections, foreign / silent peers dropped
                           with cooldown, modes kept separate, link loss and
                           time limit)
266 checks, 0 failed
cppcheck clean
$ python3 -m unittest discover -s tests       20 tests OK
  incl. test_pairing_interop: Python recomputes commitment, transcript, code,
  key, both confirm tags and key id from the ADR and matches the C vector
```

Mutation check of `bl_pair.c`: six deliberately broken variants (no commitment
check, confirm never verified, names or responder nonce left out of the
transcript, secrets kept after failure, reveal sent before checking the peer's
key) each fail at least one test.

Bug found by the new bridge tests before shipping: taking the pairing result a
second time returned the wiped, all-zero buffer as a key.

Builds: Flipper FAP 45,616 bytes, APPCHK pass (enclave and FlipperFormat calls
are exported at API 87.1); ESP32 compile check OK; Bruce 1.17 + BruceLink for
`m5stack-sticks3` builds and links (RAM 40.8 %, Flash 56.9 %).

**Not run on hardware.** Not host-testable: the Flipper key file (secure
enclave), both pairing screens, the StickS3 NVS store.

## BLE-only Flipper app, BruceLink on Bruce's main menu (2026-11-23)

Owner's request: drop the loopback transport, and put BruceLink where it is
easy to reach on the StickS3 instead of under Config → Advanced.

```
$ make -C tests/c test cppcheck            2137 + 266 checks, 0 failed; cppcheck clean
$ python3 -m unittest discover -s tests    22 tests OK
  test_install_into_bruce: 8 tests (main_menu.h include + member and the
  main_menu.cpp entry, each inside a LITE_VERSION guard, BruceLink first
  before Wi-Fi; idempotent; uninstall restores all three files; CRLF kept;
  upgrade removes the old Config → Advanced entries; a missing anchor in one
  file writes nothing to any file; a tree without ConfigMenu.cpp installs)
```

- Flipper FAP v0.3: builds, APPCHK pass, 42,212 bytes (was 45,616). The demo
  key's bytes no longer occur in the `.fap`.
- ESP32 compile check: SUCCESS (RAM 10.2 %, Flash 17.2 %).
- Bruce 1.17 + BruceLink for `m5stack-sticks3`: installer run on the existing
  tree (old Config → Advanced install), which it upgraded; build SUCCESS
  (RAM 40.8 %, Flash 56.9 %), no BruceLink warnings; `BruceLinkMenu` is linked
  into `firmware.elf`; installed sources byte-identical to the repo.
- The menu icon was checked from a desktop rendering of the same geometry, not
  on the device.

**Not run on hardware.**

## Jobs: one radio at a time (ADR 0006) — host tests and builds (2026-11-23)

```
$ make -C tests/c test cppcheck
[job]        9 tests ok   (record bounds; every single-bit flip of a record
                           caught, CRC included; random RTC contents never
                           valid; normal job; crash mid-job -> interrupted,
                           never re-run, no loop; copy survives; payload
                           layouts and limits; Flipper tracker: happy path,
                           refusals, lost job, wrong id, retries, BUSY,
                           timeout across 2^32 wrap)
3073 checks, 0 failed
[jobs]       4 tests ok   (refusals incl. demo key and e-stop with nothing
                           written or restarted; accept + BUSY; end to end
                           with bl_client + tracker across two simulated
                           restarts; crash -> interrupted, power loss ->
                           not found, never back -> timeout)
308 checks, 0 failed
cppcheck clean
$ python3 -m unittest discover -s tests       23 tests OK (installer: 9,
  incl. the boot hook and an unknown main.cpp layout writing nothing)
```

Mutation check: 7 broken variants of `bl_job.c` (re-run after a crash,
RUNNING not sealed before the run, auto-start never cleared, fetch on the
starting session, no CRC check, result id not checked, deadline without the
restart allowance) and 7 of the bridge (demo key or e-stop not refusing a job,
a second job overwriting a pending one, a result for any id or before it is
finished, no limit check, a job capability answered inline) each fail a test.

Builds: Flipper FAP v0.4 APPCHK pass; ESP32 compile check SUCCESS (RAM 10.2 %,
Flash 17.2 %); Bruce 1.17 + BruceLink for `m5stack-sticks3` SUCCESS (RAM 40.8 %,
Flash 56.9 %), installed sources byte-identical to the repo. The job record
(536 bytes) is in `.rtc_noinit` (RTC slow memory, `0x50000204`), which the
startup code does not initialise.

**Not run on hardware.** Not host-testable: RTC retention across
`esp_restart`, the boot hook inside Bruce's `setup()`, the Wi-Fi scan job, the
restart timing, the Flipper's job screen.

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

### 2026-11-23 — First over-the-air link: Flipper ↔ StickS3 (owner's devices)

StickS3 flashed by the owner with the cloud-built image
`Bruce-1.17-BruceLink-59268b0-m5stack-sticks3.bin` (sha256 `eaf88e41…9106`,
see "Bruce 1.17 + BruceLink — full firmware build"); Flipper app on Transport:
BLE. Reported by the owner: **"looks like it's working well. flipper shows
connected, device info looks right."**

| Observation | What it shows |
|---|---|
| The firmware boots and BruceLink can be toggled on | The Bruce module starts on the device (task, NimBLE init) |
| Flipper shows `connected` | StickS3 central finds the advertisement, connects, subscribes; chunks flow both ways; the authenticated handshake completes over BLE with both devices running the shared core |
| Device Info looks right | An authenticated request/response round trip through the bridge and `BruceBackendImpl` |

Follow-up from the owner, same session:

| Check | Reported |
|---|---|
| Capabilities | list shows |
| Handshake (`authenticating...` → `connected`) | about 3 s (limit 5 s) |
| Toggle BruceLink off/on on the StickS3 | works (Flipper recovers) |
| **Wi-Fi Scan** | **"does not do anything"** — open defect, cause not yet known |

Wi-Fi Scan is the first request whose reply can span more than one BLE chunk
and the first that starts Wi-Fi next to a live BLE link; neither had run on
hardware. The first report did not say what the Flipper displayed, and nothing
on either device could distinguish a lost reply from a failed or slow scan.
Diagnostics added for the next run: link counters on the Flipper (Connection
screen and the timeout message), and on the StickS3 an explicit failure reason
plus the scan duration in the reply and on the serial log.

One candidate cause was fixed on suspicion, not on evidence: the Flipper
dropped every RX event whose Offset field was non-zero, and ST documents that
field as the mechanism by which the controller splits one write across events.
A reply longer than one chunk starts with a full 244-byte chunk, the largest
write the link carries and one byte more than the stock serial profile ever
uses (243). The transport now joins such pieces; the `frag` counter will show
whether it ever happens.

Second run, both devices on `72d9efc` (owner-reported):

| Observation | Reading |
|---|---|
| `requesting...` then `no reply (timed out)` | No complete reply within 10 s |
| Counters at the timeout: `link 1 mtu 247 rx 3/3 drop 0 bad 0 txf 0`, no `frag`/`odd` line | The link stayed up, the MTU exchange reaches the Flipper (247), and **no reply data arrived at all**. The split-write handling was not involved; that suspicion was wrong |
| No late text after a further 90 s | The StickS3 never answered, not even with the scan's own 60 s failure |
| Device Info afterwards also times out | The bridge task is stuck inside the Wi-Fi scan call |
| Toggling BruceLink off then "crashes the stick" | The stop path waits for a task that cannot finish; what "crash" is exactly (freeze, reboot) is not yet known |

StickS3 serial log of the same sequence (owner, firmware `72d9efc`):

```
[RAMLOG] stage=first-mainMenu | internal free= 135083 largest= 61428 dma= 61428 | psram free= 8384524
INFO: BruceLink ON > looking for Flipper (DEMO KEY, not secure)
E wifi:Expected to init 4 rx buffer, actual is 2
E wifi_init: Failed to deinit Wi-Fi driver (0x3001)
E wifi_init: Failed to deinit Wi-Fi (0x3001)
[E] wifiLowLevelInit(): esp_wifi_init 0x101: ESP_ERR_NO_MEM
E wifi:Expected to init 4 rx buffer, actual is 0
[E] wifiLowLevelInit(): esp_wifi_init 0x101: ESP_ERR_NO_MEM
[BruceLink] wifi scan: result -2 in 55 ms
```

**Root cause: not enough internal RAM for Wi-Fi next to BLE.** The StickS3 has
135 kB of internal heap free at the menu (PSRAM does not help: this build's
radio stacks allocate from internal RAM). With NimBLE up, `esp_wifi_init` runs
out part-way, cannot be torn down again, and leaves so little internal RAM
that the BLE stack can no longer send: the "scan failed" reply was produced
(55 ms) but never left the device, and nothing worked afterwards. The bridge
task was not blocked in the scan, as first read; it was starved. Bruce's own
15 kB block check (`radioHasMemForWifi`) passed, so it is not a sufficient
guard here.

Measured by the owner with BLE up and the Flipper linked (Device Info on
`72d9efc`): **internal heap 47,979 bytes free** (PSRAM 8,384,524), down from
135,083 at the menu. BLE plus BruceLink therefore cost about 87 kB of internal
RAM, and Wi-Fi start did not fit in the remaining 48 kB. What Wi-Fi start
costs on this build has not been measured yet.

Third run, StickS3 on `2b604e4` (guard, logging, BLE memory savings), serial
log from the owner:

```
[BruceLink] module: starting BLE                       | internal 133247 dma-blk 59380 | stack-left 11432
[BruceLink] module: BLE up, scanning for the Flipper   | internal  75707 dma-blk 31732 | stack-left 10952
[BruceLink] module: Flipper linked                     | internal  61735 dma-blk 31732 | stack-left  8396
[BruceLink] module: link down                          | internal  61675 dma-blk 31732
[BruceLink] module: Flipper linked                     | internal  61735 dma-blk 31732
[BruceLink] wifi scan: begin                           | internal  61675 dma-blk 31732 | stack-left  7948
[BruceLink] wifi scan: refused, Wi-Fi would not fit next to BLE
INFO: BruceLink OFF
(Bruce's own WiFi menu: scan)
[RAM] Low contiguous DMA memory for BLE, attempting to free WiFi...
[RAM] Still only 31732 bytes DMA block, minimum 49152 needed
ERR: BruceLink: not enough memory for BLE
```

| Finding | Evidence |
|---|---|
| The guard works: a scan is refused cleanly, the bridge survives and can be switched off | "refused" line, then `BruceLink OFF` without a crash |
| BLE start costs 57.5 kB of internal RAM; a linked Flipper another 14 kB | 133,247 → 75,707 → 61,735 |
| The memory savings recovered 13.7 kB with the Flipper linked | 47,979 (`72d9efc`) → 61,675 |
| The first link dropped after 2 s and re-established 7 s later | "link down" / second "linked"; cause not examined |
| After switching BruceLink off and using Bruce's Wi-Fi scan, BLE cannot start again until reboot | Bruce's guard: 31,732-byte block, 49,152 needed |
| Bridge task stack: at least 7.9 kB of 12 kB unused so far | `stack-left` |

Test B was meant to measure what Wi-Fi costs but was run without a reboot in
between, so it shows the re-enable limit instead. The cost of a Wi-Fi start is
still unmeasured.

Fourth run, StickS3 on `ee58380` (reduced, transient Wi-Fi start, ADR 0004),
Flipper on `main` (`dbcdac8`). Owner: **"Success from flipper"**: the network
list was shown on the Flipper. Serial log:

```
[BruceLink] module: start requested                  | internal 133431 dma-blk 59380 | stack-left 11628 |  52877 ms
[BruceLink] module: BLE up, scanning for the Flipper | internal  75779 dma-blk 31732 | stack-left 10952 |  52924 ms
[BruceLink] module: Flipper linked                   | internal  61807 dma-blk 31732 | stack-left  8396 |  56979 ms
[BruceLink] wifi scan: begin                         | internal  61639 dma-blk 31732 | stack-left  7948 |  63231 ms
[BruceLink] wifi scan: driver init                   | internal  30651 dma-blk 22516 |                  |  63236 ms
[BruceLink] wifi scan: radio started                 | internal  28911 dma-blk 20468 |                  |  63239 ms
[BruceLink] wifi scan: scan finished                 | internal  25911 dma-blk 16372 |                  |  68053 ms
[BruceLink] wifi scan: driver released               | internal  (garbled in the paste) dma-blk 31732 | 68059 ms
[BruceLink] module: link down                        | internal  55091 dma-blk 31732 | stack-left  7948 |  77082 ms
```

| Measurement | Value |
|---|---|
| Reduced driver init (incl. the one-time default event loop) | 30,988 bytes internal; DMA block 31,732 → 22,516 |
| Radio start | 1,740 bytes |
| Scan | 3,000 bytes |
| **Peak cost of a scan** | **35,728 bytes internal**; lowest free 25,911; lowest DMA block 16,372 |
| Scan duration with BLE active | 4.8 s (estimate was ~2 s: the radio is shared) |
| Request to reply | under 5 s, inside the Flipper's 10 s |
| Internal RAM after release | ~55 kB vs 61.6 kB before: about 6.5 kB not returned. Consistent with the default event loop (task + queue) created on first use and kept; to be confirmed by a second scan |
| Bridge task stack | 7,948 bytes of 12,288 never used |

The link going down 9 s after the scan was the owner leaving the Flipper app,
not a drop.

Fifth run, same session (owner): the Flipper app was opened again, the
StickS3 relinked by itself (`Flipper linked` at 284 s, BruceLink had kept
scanning), and Wi-Fi Scan was run three times in a row:

| Scan | Internal at begin | Lowest during scan | After release | Lowest DMA block | Duration |
|---|---|---|---|---|---|
| 2 | 55,059 | 25,879 | 55,059 | 16,372 | 4.8 s |
| 3 | 55,059 | 25,879 | 55,059 | 17,396 | 4.8 s |
| 4 | 55,059 | 25,319 | 55,059 | 16,372 | 4.8 s |

- **No leak**: every scan returns exactly what it took. The 6.5 kB kept after
  the first scan is a one-time allocation (the default event loop).
- **Cost of a repeated scan: 29.7 kB peak** (first scan 35.7 kB).
- Reconnecting after leaving and reopening the Flipper app works without
  touching the StickS3.
- **Guard headroom is thin**: in steady state 55,059 bytes are free against
  the 53,248-byte (52 kB) threshold, so about 1.8 kB more internal RAM use
  anywhere in Bruce would make scans refuse (cleanly). See ADR 0004.
- What the Flipper displayed for these three scans was not separately
  reported.

Earlier reading, kept for the record: on the StickS3, starting Wi-Fi from the
bridge task while the BLE link is up does not return. Bruce's own code never runs both radios at
once (`core/radio_mem.h`; BLE features call `wifiDisconnect()` first), so this
is the Wi-Fi/BLE coexistence risk listed since the capability audit, now seen
on hardware.

Response in the next build: Wi-Fi is started only if at least 80 kB of
internal RAM and a 16 kB DMA block are free (`wifiStartAllowed`, host-tested;
both numbers are estimates). Otherwise the request is refused with the figures
and the link survives. That makes the failure clean; it does not make Wi-Fi
fit. How much has to be saved is not known until the device reports its
figures with BLE up.

Not yet checked: behaviour when either device goes out of range, the rest of
Bruce while BruceLink is on. Both devices use the public demo key and the
link is unencrypted, so this demonstrates the link, not its security.

### 2026-11-23 — `b635b03` app-only image does not boot (owner's StickS3)

The owner flashed `Bruce-1.17-BruceLink-b635b03-m5stack-sticks3-app-only.bin`
(`firmware.bin`) and the StickS3 did not boot. The merged image of the same
build, flashed afterwards at `0x0`, **boots** (owner-reported), so the build is
fine and the failure lies in how or onto what the app-only file was written.
Not known: the offset and tool used, and what the StickS3 held before.

Checked in the cloud: the delivered file is byte-identical to the app inside
the merged image at `0x10000`; the bootloader and partition table (`factory`
app at `0x10000`, 0x4E0000 bytes; the app is 0x48DF40) are byte-identical in
every merged image delivered since the first. So written at `0x10000` over an
earlier BruceLink merged image, flash would match the merged image except for
NVS. Code that runs at boot and is new since the last build that ran
(`ee58380`): the main-menu item's constructor and icon (the first item drawn),
plus static constructors of the bridge; nothing found that could fail. An
emulator boot test was not run.

The merged image boots; see below.

Response: the docs no longer recommend `firmware.bin` alone. To keep NVS,
flash bootloader, partition table and app at `0x0` / `0x8000` / `0x10000`,
which needs nothing from what was on the device.

### 2026-11-23 — First pairing on hardware, `b635b03` merged image (owner's devices)

- **Pairing completed** on the Flipper (app v0.3) and the StickS3 (owner:
  "paired"). Whether both showed the same key id was not reported.
- **Connecting afterwards failed**, with a memory error on the StickS3 (owner:
  "can't connect - uses too much memory"; exact text and serial not reported).
- **After a reboot of the StickS3, Turn on and Connect work** with the paired
  key, and Device Info and the Wi-Fi scan work over that session. So the key
  survives a reboot in NVS on the StickS3 and in the Flipper's key file, both
  sides derived the same key (the session authenticates with it), and the
  paired bridge runs normally.

Reading: after storing the key, `b635b03` stopped BLE (pairing mode) and
started it again (normal mode). Starting BLE goes through Bruce's
`radioHasMemForBle()`, which wants one free DMA-capable block of 48 kB. That
held at boot (pairing mode started), so it is the restart that fell short:
memory allocated while BLE was up (the key store's NVS write happens then)
can sit inside the region BLE gives back and split it. Not confirmed; the
figures were not captured.

Response (next build): after pairing, the bridge task leaves pairing mode in
place (`endPairing` + `setKey`, host-tested in `test_bridge.cpp`), so BLE is
not restarted; a cancelled pairing returns to the old key the same way.
`moduleStop` waits 100 ms for the idle task to free the task's stack before
returning. A failed start now shows the largest free block and the 48 kB it
needs, and logs the figures.

### 2026-11-23 — `9f37f46` (jobs) on the StickS3 (owner's devices)

- Flashed with the three-part command (bootloader / partitions / app): **did
  not boot**. Command output and serial log not yet reported; second
  non-merged flash that failed (see `b635b03` app-only above).
- Flashed as the merged image at `0x0`: **boots**; paired again; Device Info
  works.
- Wi-Fi Scan from the Flipper: `StickS3 refused: unknown wire error`. That is
  the v0.3 app (no jobs) receiving the new `use_job` error (16), which it has
  no name for: the Flipper was still running v0.3. The job path itself has
  not run yet.

### 2026-11-23 — First job on hardware: Wi-Fi scan with BLE off (`9f37f46`, app v0.4)

- **The full cycle worked** (owner: "that worked"): the Flipper started the
  job, the StickS3 restarted, scanned, restarted, turned BruceLink back on, and
  the Flipper fetched and showed the result. So RTC memory kept the record
  across both restarts, the boot hook ran, and auto-start worked.
- **Too slow** for the owner; total time not reported. Known cost at the time:
  Bruce's boot animation runs for up to 7 s on every boot (twice per job), and
  the scan allowed up to 300 ms per channel.

Response (next build): the job runs before the animation, the boot after a
job skips it (and Wi-Fi at startup), scan 120 ms per channel, delays trimmed,
and the result reports scan time, stick boot time and the Flipper's total.

### 2026-11-23 — Faster jobs (`8f9285a`, app v0.5)

- Wi-Fi scan job: **"much faster"** (owner), cycle works. The `Took` / scan /
  boot figures were not reported, so how much each change saved is not
  known.

### 2026-11-23 — Independent review fixes (cloud, no hardware)

Four reviewers read the whole repository (protocol core, Flipper app, StickS3
glue, cross-cutting). Every fix that runs on the host has a regression test,
and each new test was run against the old code (or a mutant restoring it) and
failed there:

| Fix | Test | Old code |
|---|---|---|
| HELLO version range in the transcript | `t_session_hello_range_is_authenticated` | handshake completed after a range rewrite |
| Seal/open only when established | `t_session_strict_states_and_wipe` | AUTH frame opened by a waiting responder |
| Secrets wiped on failure | same | keys left after FAILED |
| Only AUTH_OK ends WAIT_AUTH_OK | `t_session_stray_frame_while_waiting_auth_ok` | repeated HELLO_ACK failed the handshake |
| Request tags (M4.7) | `t_client_late_response_dropped` | late ERROR surfaced as the next answer |
| Gate: no wrap | `t_safety_frames_cannot_wrap` | huge frame count passed |
| Gate: target always | `t_safety_active_needs_a_target` | target-less op passed |
| Gate: reset clears enable | `t_safety_estop_reset_leaves_active_off` | survived before |
| Fetch only after the drop | `t_jobc_no_fetch_before_the_link_drops` | fetched on the starting session |
| Cancel job before restart | `t_job_cancelled_before_the_restart` | job stayed PENDING after e-stop / drop |
| E-stop cleared only by a session | `t_estop_cleared_only_by_a_session` | a bare link cleared it |
| Demo key refuses capture | `t_demo_key_never_enables_active_ops` | capture started |
| Capture stops on drop / e-stop | `t_capture_stops_on_link_loss_and_estop` | mutants survived before |
| Active job refused | `t_active_job_refused` | branch never ran before |
| Refund after failed transmit | `t_failed_active_op_is_refunded` | mutant survived before |
| Destructor wipes the key | `t_destructor_wipes_key` | mutant survived before |
| Installer writes nothing on a failed check | `test_name_collision_writes_nothing` | Bruce files patched, then failed |

Also new: Python recomputation of the session handshake (transcript, both
confirm tags, both keys, MACs) and of the request tag; RFC 4231 cases 6–7, RFC
5869 A.2, all seven X25519 low-order points, header tamper and counter end; a
check that the demo key is in no Flipper source and not in the built `.fap`.
The interop suite now fails instead of skipping when the C tool does not build.

Not host-testable, built only: the StickS3 module changes (rescan while a peer
is on cooldown, in-place "Pair again", stop when NimBLE is released under it,
key wiped on task exit, checked NimBLE start, Forget checks, capture and active
ops refused on the live link) and the Flipper review fixes (job tracking on
link changes and timeouts, Back, pairing feedback, key file temp + rename).

| Check | Result |
|---|---|
| `make -C tests/c test cppcheck` | 3483 + 378 checks, 0 failed; cppcheck clean |
| `python3 -m unittest discover -s tests` | 28 tests OK |
| FAP (ufbt, mntm-012) | APPCHK pass, Target 7 / API 87.1 |
| `esp32/test_compile` | SUCCESS, RAM 10.2 % / Flash 17.3 % |
| Bruce 1.17 + BruceLink (`m5stack-sticks3`) | SUCCESS, static RAM 133,788 B |

**Wire change:** the transcript and header byte 5 changed. A Flipper and a
StickS3 on different sides of this change do not complete the handshake:
update both.

### 2026-11-23 — RAM review (cloud, no hardware)

Three reviewers looked for internal RAM the StickS3 build does not need.
Figures are computed or read from the map, not measured on the stick.

- Heap layout: of the ~133 kB "free" at the menu only one region (about
  170 kB at boot, before Bruce's own allocations) can hold BLE's 48 kB block;
  about 98 kB of the free figure is in smaller regions. The "largest DMA
  block" reading of 31,732 B is a separate 32 kB region, which hides the main
  region's own largest block.
- BruceLink-side, no feature loss: the bridge task stack to PSRAM (10 kB while
  on), NimBLE's host buffers to PSRAM (about 16 kB at BLE start, needs a
  forced-include build flag), passive scanning (fewer transient allocations),
  and offering "restart into BruceLink" when the memory guard refuses.
- Bruce-side, costs features or needs Bruce source edits: TinyUSB (about
  31 kB static; USB mass storage, USB BadUSB/HID, U2F), the Wi-Fi password
  cracker's IRAM (4 kB), NetCut's device table (4.3 kB), the RF stream buffer
  (4 kB), mDNS (2.4 kB), and smaller items.

### 2026-11-23 — RAM trims without feature loss (cloud, no hardware)

| Check | Result |
|---|---|
| Host tests, cppcheck, Python (29 incl. the installer's new ini patch and its failure case) | pass |
| `esp32/test_compile` with the forced include | SUCCESS; `nimble_platform_mem_malloc` / `_calloc` ask for caps `0x404` (PSRAM, 8-bit), not `0x804` (internal) |
| Bruce 1.17 + BruceLink, `m5stack-sticks3` | SUCCESS, static RAM 133,796 B (heap changes only); `pio project metadata` shows `-include bl_nimble_psram.h` in both the C and C++ flags and `lib/BruceLink` on the include path |

Expected on the stick (computed, not measured): about 16 kB more internal
RAM free after BLE starts (NimBLE's host buffers), 3 kB more while BruceLink is
on (stack), fewer transient allocations while scanning. To check: the
`internal` and `dma-blk` figures in the `[BruceLink] module: BLE up` and
`Flipper linked` serial lines, against the same lines from `222920f`.

### 2026-11-23 — RAM trims in Bruce (owner's choice; cloud, no hardware)

The owner chose to drop the features the RAM review listed (USB HID, USB mass
storage, mDNS, the cracker's IRAM) and move NetCut's and the RF decoder's
tables to PSRAM. All as lines guarded by `BRUCELINK_RAM_TRIM`, defined only in
the StickS3 build (`PATCH.md`).

| | `3fac5b5` | trimmed | change |
|---|---|---|---|
| Static internal RAM (PlatformIO "RAM") | 133,796 B | 91,260 B | −42,536 B |
| IRAM segments in the image | 111,012 B | 106,920 B | −4,092 B (goes to the heap) |
| Flash | 4,778,827 B | 4,656,491 B | −122 kB |

Checked in the image: no TinyUSB or mDNS object takes internal RAM (none in
the map's DRAM ranges; no `mdns_*` symbol left); `sha1_transform` is in flash;
NetCut's and the RF tables are 4-byte pointers in internal RAM. The mDNS cut
needed `lib_ignore = ESPmDNS` as well: PlatformIO links a library's objects
whenever its header is included, and ESPmDNS's global object kept the mDNS
component (2 kB) alive. Host tests, Python tests (incl. the installer's new
patches and its failure cases) and the compile check pass.

Expected on the stick (not measured): about 46 kB more internal heap at boot
than `3fac5b5`, on top of NimBLE's buffers in PSRAM. To check:
`HARDWARE_TESTING.md` step 4a.

### 2026-11-23 — RAM-trim build on the owner's devices (`cb11f31`)

- Owner: "all is working well". Which checks of step 4a were run was not
  itemized, and no memory figures were reported.

## Not yet tested (requires the local hardware session)

Updated 2026-11-23 after the independent review. Already run on hardware: see
the dated entries above.

- The review-fix build on either device (the handshake changed: both at once).
- Connecting straight after pairing (in-place switch); "Pair again" while on;
  Forget on both devices; refusal of a wrong key.
- Re-advertising after a dropped link and recovery after a failed handshake
  (the StickS3 now rescans every 5 s while a Flipper is on cooldown).
- Job timing figures (`Took` / scan / boot), the stick's stop button during a
  job, a crash during a job.
- Sniffer/EAPOL capture (to run as a job), pcap transfer.
- Active ops (refused; to run as jobs once ADR 0006's active-job rules are
  implemented and tested).
- Coexistence with Bruce's own BLE tools while BruceLink is on.
