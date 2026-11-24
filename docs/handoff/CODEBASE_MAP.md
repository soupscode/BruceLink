# BruceLink Codebase Map

Navigable map of the repository for a new developer or assistant. Every path is
repo-relative; every symbol was read from the source, not guessed. Companion
docs: `PROTOCOL.md` (the wire/session protocol in detail), `ARCHITECTURE.md`
(design intent), `CURRENT_STATE.md` (what works today), `FEATURE_MATRIX.md`
(capability-by-capability status), `NATIVE_BRUCE_INVENTORY.md` /
`INTEGRATION_INTERFACES.md` (upstream Bruce, reference only).

Pins (see `../../CLAUDE.md`, `CURRENT_STATE.md`): Momentum `mntm-012` (API 87.1),
Bruce `1.17` (`2b6ba4d`, env `m5stack-sticks3`), NimBLE-Arduino `2.5.1`,
pioarduino `55.03.39`. Baseline commit for this map: `448b798` on branch
`feat/radio-handoff`.

Status tags used below: **SW-VERIFIED** = host test or compile ran at `448b798`
(see `../TEST_RESULTS.md`); **HW-OWNER** = owner-reported
from hardware, not independently verified; **DESIGN** = intended, not a claim
about running code.

---

## 1. Top-level layout

| Path | What it is |
|---|---|
| `shared/brucelink_core/src/` | Portable C99 protocol core. Both devices + host tests compile the SAME sources (crypto, frame, session, client, pairing, jobs, files, safety, capability, reassembly, COBS, pcap, textbuf). No platform deps. |
| `shared/brucelink_protocol.py` | Python reference implementation of the protocol, for interop tests (`tests/test_protocol.py`). |
| `shared/protocol.md` | Human reference for wire protocol v1. Cross-check against `PROTOCOL.md`. |
| `flipper/brucelink/` | Flipper Zero FAP (BLE peripheral + session initiator). `transport/` = BLE profile; `core/` = generated wrappers over the shared core. |
| `esp32/lib/BruceLink/` | StickS3 bridge core + NimBLE central transport. Portable/NimBLE only; no Bruce headers. Installed into Bruce's `lib/`. |
| `esp32/src/modules/brucelink/` | Bruce glue (builds only inside a Bruce tree): menu, module task, backend impl, key store, jobs, boot hooks, patch notes. |
| `esp32/test_compile/` | Standalone ESP32-S3 compile/link check with a stub backend. Not the Bruce firmware. |
| `tools/` | `install_into_bruce.py` (installer), `gen_wrappers.py` (wrapper generator), `bl_vectors.c` (wire-vector emitter for Python interop). |
| `tests/c/` | Host C tests (ASan+UBSan) + bridge-core C++ tests + cppcheck. |
| `tests/*.py` | Python reference, C↔Python interop, installer test, demo-key absence test, wrapper-staleness test. |
| `scripts/hil/` | Hardware-in-the-loop helper scripts. |
| `docs/` | State docs, ADRs (`docs/decisions/`), handoff set (`docs/handoff/`). |
| `.github/workflows/tests.yml` | CI: host core tests + cppcheck + Python gate merges; FAP and ESP32 builds are best-effort. |

Root files: `CLAUDE.md` (agent instructions), `LOCAL_HANDOFF.md`, `README.md`,
`TASKS.md`, `CHANGELOG.md`, `LICENSE` (MIT).

---

## 2. `shared/brucelink_core/src/` — protocol core

Pure C99, allocation-free, no platform deps (`CLAUDE.md` architecture rule).
Change only with tests (`CLAUDE.md` testing rule). Full protocol detail is in
`PROTOCOL.md`; this table is the file/symbol index.

| File | Role | Key symbols |
|---|---|---|
| `bl_types.h` | Common limits + internal result codes. | `BL_WIRE_VERSION`, `BL_PROTO_MIN/MAX`, `BL_HDR_LEN`(12), `BL_MAC_LEN`(16), `BL_MAX_PAYLOAD`(512), `BL_MAX_MSG`, `BL_KEY_LEN`(32), `BL_NONCE_LEN`(16); `bl_result_t` (`BL_OK`, `BL_E_MAC`, `BL_E_REPLAY`, …) |
| `bl_proto.h` | Message types, header offsets, flags, wire error codes. | `bl_msg_type_t` (`BL_MSG_HELLO`…`BL_MSG_ERROR`), `BL_OFF_TAG`(5), `BL_OFF_SEQ`(6), `BL_OFF_LEN`(10), `BL_FLAG_RESPONSE/ERROR/EVENT`, `bl_wire_err_t` |
| `bl_frame.{h,c}` | Frame codec: 12 B header + payload + 16 B MAC. | `bl_frame_t` (incl. `.tag`), `bl_frame_encode`, `bl_frame_decode` (constant-time MAC verify), `bl_frame_size`, `BL_MAC_TAG_LEN` |
| `bl_session.{h,c}` | Authenticated session state machine (PSK + X25519 ECDH, HKDF keys, per-direction replay counters). | `bl_session_t`, `bl_role_t`, `bl_session_state_t`, `bl_session_init/start/handshake_rx/seal/seal_tag/open/close`; static labels `CONFIRM_P="BLcP"`, `CONFIRM_C="BLcC"`, `INFO_C2P="brucelink v1 c2p"`, `INFO_P2C="brucelink v1 p2c"`; `pick_proto` |
| `bl_client.{h,c}` | Flipper-side initiator: starts handshake on link-up, tracks one tagged outstanding request, turns silence into timeouts. | `bl_client_t`, `bl_client_state_t`, `bl_client_event_t`, `bl_client_init/link_up/link_down/rx/request/tick/ready`; `BL_CLIENT_HS_TIMEOUT_MS`(5000), `BL_CLIENT_REQ_TIMEOUT_MS`(10000) |
| `bl_pair.{h,c}` | Pairing (ADR 0005): key agreement with 6-digit numeric comparison. | `bl_pair_t`, `bl_pair_state_t`, `bl_pair_fail_t`, `bl_pair_abort_t`, `bl_pair_init/start/rx/code/accept/cancel/tick/result/key_id/wipe`; `BL_PAIR_CODE_MOD`(1e6), `BL_PAIR_TIMEOUT_MS`(90000) |
| `bl_job.{h,c}` | Jobs (ADR 0006): RTC-memory record + boot state machine + JOB_* payload codecs + Flipper tracker. | `bl_job_record_t`, `bl_job_state_t`, `bl_job_outcome_t`, `bl_job_boot_t`, `bl_job_on_boot`, `bl_job_record_create/valid/finish`, `bl_job_encode_*/parse_*`; tracker `bl_jobc_t`, `bl_jobc_init/start/on_response/want_fetch/fetch/tick`; `BL_JOB_MAX_S`(300) |
| `bl_files.{h,c}` | Capture-file transfer over the live link (CAP_* messages); stateless resumable reads + CRC. | `bl_file_name_ok`, `bl_file_entry_t`, `bl_files_encode_*/parse_*`; download tracker `bl_xferc_t`, `bl_xferc_init/start/next/on_response/retry/active`; `BL_FILE_CHUNK_MAX`(448), `BL_FILE_LIST_PAGE`(7), `BL_XFER_RETRY_MAX`(5) |
| `bl_safety.{h,c}` | Remote-operation safety gate (ADR 0003): active-ops enable, latching e-stop, link-loss auto-stop, rate limiter, optional target scope. | `bl_safety_t`, `bl_op_class_t` (`BL_OP_PASSIVE/ACTIVE`), `bl_safety_init/enable_active/set_allowlist/estop/reset/on_link_loss/authorize/budget/refund`, `bl_safety_bssid_is_specific` |
| `bl_capability.h` | Capability registry types + canonical ids + flags. | `bl_cap_id_t` (`BL_CAP_DEVICE_INFO`…`BL_CAP_DEAUTH`), `BL_CAP_F_ACTIVE_RF/REQUIRES_EXT_HW/HW_UNVERIFIED/REQUIRES_TARGET/JOB`, `bl_capability_desc_t`, `bl_caps_find/serialize/validate` (impl in `bl_capability.c`) |
| `bl_reasm.{h,c}` | Fragmentation/reassembly for message-framed transports (BLE). | `bl_reasm_t`, `bl_frag_chunk`, `bl_frag_count`, `bl_reasm_push`; `BL_FRAG_HDR`(2), `BL_FRAG_FIRST/LAST` |
| `bl_cobs.{h,c}` | COBS framing for byte-stream (serial) transports. Not used on BLE. | `bl_cobs_encode/decode`, `bl_cobs_stream_t`, `bl_cobs_stream_push` |
| `bl_crypto.{h,c}` | Self-contained SHA-256, HMAC-SHA256, HKDF, X25519, constant-time compare, secure zero. | `bl_sha256*`, `bl_hmac*`, `bl_hkdf_extract/expand`, `bl_x25519`, `bl_x25519_base`, `bl_ct_equal`, `bl_secure_zero` |
| `bl_pcap.{h,c}` | libpcap writer (LINKTYPE_IEEE802_11, snaplen 2500) + streamable CRC-32. | `bl_pcap_global_header`, `bl_pcap_record_header`, `bl_crc32_init/update/final/crc`; `BL_PCAP_MAGIC` |
| `bl_capture.{h,c}` | Hardware-independent capture session manager (backend vtable). Currently exercised only by host tests / jobs; see note. | `bl_capture_mgr_t`, `bl_capture_backend_t`, `bl_capture_mode_t`, `bl_capture_init/start/set_channel/poll/stop/is_running` |
| `bl_textbuf.{h,c}` | Bounded, all-or-nothing text builder with sanitizer for untrusted strings. | `bl_textbuf_t`, `bl_tb_init/str/sanitized/int/uint/hex16` |
| `bl_strings.c` | String tables for wire/result enums. | `bl_result_str`, `bl_wire_err_str`, `bl_msg_type_str` |
| `bl_uuid.h` | BLE GATT profile UUIDs, string + little-endian byte forms; chunk sizing. | `BL_UUID_SERVICE_STR`/`RX_STR`/`TX_STR`, `BL_UUID_*_LE`, `BL_BLE_CHUNK_MAX`(244), `bl_ble_chunk_for_mtu` |
| `bl_demo_key.h` | The **public** demo PSK. Used as a key only in host tests + the ESP32 compile check; devices carry it only to refuse it. | `BRUCELINK_DEMO_PSK` (never ship as a key; see Security rules in `CLAUDE.md`) |

Note — `bl_capture.c` / `bl_pcap.c` and the inline `CAP_START/STOP/STATUS`
path: the shipping StickS3 refuses inline capture (ADR 0006 — capture runs as a
job). The brief flags this machinery as "currently-unused capture/pcap
machinery flagged for review"; `bl_pcap`'s CRC-32 is still used by the file
transfer. **DESIGN/under-review** status — see `FEATURE_MATRIX.md`.

---

## 3. `flipper/brucelink/` — Flipper FAP (BLE only)

Entry point: `application.fam` → `appid="brucelink"`, `entry_point="brucelink_app"`,
`apptype=EXTERNAL`, `stack_size=8*1024`, `fap_category="Bluetooth"`,
`fap_version="0.7"`, `sources=["*.c"]` (globs app + `core/*.c` wrappers).
**SW-VERIFIED** FAP build at `448b798` (APPCHK pass, Target 7 / API 87.1);
link/handshake/requests/pairing/jobs **HW-OWNER** (`../TEST_RESULTS.md`).

| File | Role | Key symbols |
|---|---|---|
| `brucelink.c` | The whole app: a ViewDispatcher with a 50 ms tick (`BL_TICK_MS`) that drives all protocol logic on the app thread. Menu, connection, requests, jobs, capture setup, capture-file list/download/delete, pairing UI. | `brucelink_app` (entry), `bl_tick`, `bl_on_frame`, `bl_on_message`, `bl_connect`, `bl_request`, `bl_job_begin/tick`, `bl_xfer_begin/tick`, `bl_pairing_tick`, `bl_build_menu` (menu: Pair/Connect/Device Info/Wi-Fi Scan/Handshake Capture/Captures/Capabilities/Pair again/Forget/Settings/About) |
| `brucelink_i.h` | App state struct + view/event enums. | `BruceLinkApp`, `BlViewId`, `BlScreen`, `BlCustomEvent`, `BlPendingOp`; holds `bl_client_t client`, `bl_pair_t pair`, `bl_jobc_t job`, `bl_xferc_t xfer`, `BlTransport* transport` |
| `bl_keyfile.{h,c}` | Stored pairing key on SD, AES-encrypted under the Flipper's device-unique enclave key (like stock U2F); key-id checked on load. | `BlKeyfileResult` (`BlKeyfileOk/None/Foreign/Error`), `bl_keyfile_load/save/forget` |

### `transport/`

| File | Role | Key symbols |
|---|---|---|
| `bl_transport.h` | Transport abstraction the app talks through (frames in/out; link id semantics). | `BlTransport`, `BlTransportApi` (`start/stop/link_id/drop_link/send/poll/status/diag/free`), `bl_transport_ble_alloc` |
| `bl_ble_transport.c` | The only transport: Flipper as GATT **peripheral** (STM32WB stack) with one custom 128-bit service (RX write / TX notify, ≤244 B), `GapPairingNone`. "Link up" = central subscribes to TX notifications. | `bl_transport_ble_alloc`, `ble_evt` (BLE-thread handler), RX queue depth 32 |
| `bl_gatt_evt.{h,c}` | **Portable, host-tested** HCI event parser + RX write assembler + subscription guard. Keeps the on-device handler tiny. | `bl_gatt_evt_parse`, `bl_gatt_evt_t`/`bl_gatt_evt_kind_t`, `bl_rx_asm_t`/`bl_rx_asm_push`, `bl_sub_guard_t`/`bl_sub_guard_expired`; verified against STM32CubeWB v1.20.0 |

### `core/` — generated wrappers

One `.c` per shared-core source, each a two-line file:
`/* GENERATED … */` + `#include "../../../shared/brucelink_core/src/<name>.c"`.
See §9. Regenerate with `tools/gen_wrappers.py`; staleness caught by
`tests/test_wrappers.py`.

Built artifacts (checked in on this branch): `dist/brucelink.fap`,
`dist/debug/brucelink_d.elf`.

---

## 4. `esp32/lib/BruceLink/` — StickS3 bridge core + transport

Portable C++ depending ONLY on the shared protocol core (no Bruce, no NimBLE in
the bridge core). Included via `-I` (the brief notes it has **no**
`library.json`). Namespace `brucelink`.

| File | Role | Key symbols |
|---|---|---|
| `BruceLinkBridge.{h,cpp}` | The bridge: holds the session (peripheral/responder role), safety gate, capture manager, reassembly. Turns GATT chunks into frames, dispatches to a `BruceBackend`, emits response chunks. Owns the built-in capability registry + per-job limits. | `BruceLinkBridge` (`begin/onConnect/onDisconnect/onChunk/tick/needsDisconnect/established/commitJob/beginPairing/pairAnswer/pairTakeResult/setKey/clearKey/setRegistry`), abstract `BruceBackend`, `TxChunkFn`; `kCtrlEnableActive`(0xFF01), `kMaxFramesPerActiveOp`(16), `kAuthTimeoutMs`(10000); static `kCaps[]` registry + `kJobLimits[]` |
| `BruceLinkFormat.{h,cpp}` | Portable, host-tested response-text formatting (sanitizes untrusted SSIDs). | `brucelink::formatWifiApLine`, `kSsidMaxLen`(32) |
| `BruceLinkPeerGuard.{h,cpp}` | Portable, host-tested per-peer reconnect cooldown (so a failed-auth impostor does not starve the paired Flipper). | `brucelink::PeerCooldown` (`penalize/blocked/clear`), `kSlots`(8) |
| `NimBleCentralTransport.{h,cpp}` | StickS3 as BLE **central**: scan → connect → discover RX/TX → feed chunks to the bridge → write RX for tx. NimBLE callbacks never touch the bridge (queue + spinlock). **Not** host-tested (needs NimBLE); built by `esp32/test_compile`. | `brucelink::NimBleCentralTransport` (`begin/startScan/loop/end/stackGone/abandon/clearCooldowns/connected`), `kPeerCooldownMs`(30000), `kConnectTimeoutMs`(5000) |
| `bl_sticks3_ram.h` | Forced-include (`-include`) RAM header for the StickS3 build: moves NimBLE host buffers to PSRAM and (under `BRUCELINK_RAM_TRIM`) undoes the board's `-DUSB_as_HID`. See `MEMORY_OPTIMIZATION.md`. | `CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL`, `#undef USB_as_HID` |

Built-in registry (`kCaps[]` in `BruceLinkBridge.cpp`): `device.info`,
`status` (passive); `wifi.scan`, `wifi.sniff.handshake` (passive, **JOB** +
HW_UNVERIFIED); `wifi.sniff.raw` (REQUIRES_EXT_HW); `pcap.list`,
`pcap.transfer`, `pcap.delete` (passive, HW_UNVERIFIED); `wifi.deauth`
(ACTIVE_RF + REQUIRES_TARGET + HW_UNVERIFIED). Job limits: `wifi.scan`
default 20 s / max 60 s / 0 args; `wifi.sniff.handshake` default 60 s / max 300 s
/ 1 arg (channel).

---

## 5. `esp32/src/modules/brucelink/` — Bruce glue

Builds ONLY inside a Bruce 1.17 tree (needs Bruce headers); installed by
`tools/install_into_bruce.py` into `src/modules/brucelink/`. Compilation here is
**not** covered by `esp32/test_compile`; it is a **HW-OWNER** path. See
`PATCH.md` and `BUILD_AND_DEPLOY.md`.

| File | Role | Key symbols |
|---|---|---|
| `BruceLinkMenu.{h,cpp}` | The "BruceLink" item on Bruce's main menu (icon + submenu entry). Kept free of NimBLE/bridge headers. | `BruceLinkMenu : MenuItemInterface` (`optionsMenu/drawIcon`) |
| `BruceLinkModule.{h,cpp}` | The submenu (Turn on/off, Pair with Flipper, Forget) + the dedicated task that owns the bridge and NimBLE transport + the two boot hooks. | `brucelink::moduleStart/moduleStop/moduleRunning/moduleLinked`, `ModuleStartResult`, `bruceLinkMenu()`, **`bruceLinkBootEarly()`**, **`bruceLinkBoot()`** |
| `BruceBackendImpl.{h,cpp}` | The `BruceBackend`: every method calls Bruce's OWN implementation (BruceLink reimplements nothing). | `brucelink::BruceBackendImpl` (overrides `deviceInfo/jobRecord/jobRestart/jobCancel/captureFile*/captureBackend/activeOp/nowMs/randomBytes`), `kCaptureDir[]`, `logStep` |
| `BruceLinkKeyStore.{h,cpp}` | StickS3 copy of the pairing key in NVS (ESP32 `Preferences`, namespace `brucelink`). Never `bruce.conf`, never LittleFS. | `brucelink::keyStoreLoad/keyStoreSave/keyStoreForget/keyStoreInfo` |
| `BruceLinkJobs.{h,cpp}` | Jobs (ADR 0006): the RTC-memory record storage, restart request plumbing, and the job bodies run at boot (Wi-Fi scan job, handshake-capture job). | `brucelink::jobRecordStorage/jobRestartRequest/jobRestartRequested/jobRestartCancel/restartIntoBruceLink`; internal `wifiScanJob`, `handshakeJob` dispatched by cap id |
| `bruce_brucelink_hooks.h` | The minimal headless hooks BruceLink needs from Bruce (sniffer start/stop/status, deauth). The only new behaviours required from upstream. | `bruce_brucelink_sniffer_start/set_channel/status/stop`, `bruce_brucelink_raw_mode_available`, `bruce_brucelink_deauth`, `BruceSnifferStatus` |
| `bruce_brucelink_hooks_unavailable.cpp` | Placeholder hook bodies so capture/active ops cleanly report "not available" until the upstream sniffer edits (task M5.2) land. | (weak/placeholder implementations) |
| `PATCH.md` | Exactly what the installer changes in Bruce, the RAM-trim table, and the remaining upstream edits (M5.2). | — |

---

## 6. `esp32/test_compile/` — standalone compile/link check

`platformio.ini` env `s3-check`: board `esp32-s3-devkitc-1`, pioarduino
platform `55.03.39`, `lib_deps = NimBLE-Arduino#2.5.1`, `-DBOARD_HAS_PSRAM`,
`-include bl_sticks3_ram.h`, `-std=gnu++17`. `src/main.cpp` wires
`NimBleCentralTransport` + `BruceLinkBridge` + a `StubBackend` with the **public
demo key** (`BRUCELINK_DEMO_PSK`), proving the bridge + NimBLE usage build for
the target. `src/core/` + `src/bridge/` are generated wrappers (§9).
**SW-VERIFIED** SUCCESS at `448b798`. Not on-device behaviour.

---

## 7. `tools/`

| File | Role |
|---|---|
| `install_into_bruce.py` | Installs the bridge lib + glue + main-menu item + the two `setup()` boot hooks + StickS3 RAM trims into a Bruce checkout. Idempotent; `--uninstall` reverses; refuses a tree that is not Bruce `2b6ba4d` unless `--force`. Every added C++ line ends `// BruceLink`, every ini line `; BruceLink`, each guarded by `#if !defined(LITE_VERSION)`. Tested by `tests/test_install_into_bruce.py`. |
| `gen_wrappers.py` | Generates the one-line wrapper sources for the three device/compile build trees. `--check` exits 1 if stale. See §9. |
| `bl_vectors.c` | Emits deterministic wire vectors as JSON (frame/MAC/COBS/pcap/CRC32/handshake/pairing) for the Python reference to check byte-for-byte. Built by `tests/c`. |

---

## 8. Tests

### `tests/c/` — host C tests (ASan+UBSan by default)

`Makefile` compiles the shared core strict (`-Wall -Wextra -Werror -Wshadow
-Wconversion …`), builds `bl_tests` (core) and `bl_bridge_tests` (bridge core:
`BruceLinkBridge.cpp` + `BruceLinkFormat.cpp` + `BruceLinkPeerGuard.cpp`; NOT
`NimBleCentralTransport`, which needs NimBLE). Also host-tests the portable
Flipper piece `transport/bl_gatt_evt.c`.

Test units: `test_main.c` (runner, `bl_test.h`), `test_crypto.c`,
`test_protocol_core.c`, `test_client.c`, `test_capability.c`, `test_files.c`,
`test_gatt_evt.c`, `test_integration.c`, `test_job.c`, `test_pair.c`,
`test_textbuf.c`, `test_bridge.cpp`.
**SW-VERIFIED** at `448b798`: core **3681** checks + bridge **448** checks,
0 failed; cppcheck clean.

### `tests/` — Python

| File | Role |
|---|---|
| `test_protocol.py` | Python reference (`shared/brucelink_protocol.py`) + C↔Python interop: recomputes transcript, both confirm tags, both direction keys and MACs from the C vectors (`tools/bl_vectors.c`). |
| `test_demo_key.py` | Checks the demo key is absent from the Flipper sources and the built `.fap`. |
| `test_install_into_bruce.py` | Installer install/uninstall idempotence against a fixture tree. |
| `test_wrappers.py` | Runs `gen_wrappers.py --check` and asserts no tracked symlinks (Windows). |

**SW-VERIFIED** at `448b798`: `python3 -m unittest discover -s tests` = **31
tests OK**.

### `scripts/hil/`

`build_all.sh` runs every cloud-verifiable build+test (host C, cppcheck, Python,
FAP via `ufbt`, ESP32 via `pio run`). Others: `common.sh`, `detect_devices.sh`,
`firmware_versions.sh`, `backup_sticks3.sh`, `restore_sticks3.sh`,
`capture_logs.sh`. See `HARDWARE.md` / `docs/HARDWARE_TESTING.md`.

---

## 9. Generated-wrapper mechanism

Both devices must compile the **same** core sources, and
`esp32/test_compile` must compile the same bridge sources. Symlinks would do it
but break Windows checkouts (`tests/test_wrappers.py::test_no_symlinks_in_repo`).
Instead, each build tree holds tiny wrapper files that `#include` the real
source by relative path, one wrapper per source so every file stays its own
translation unit (the core has same-named `static` helpers in different files).

`tools/gen_wrappers.py` generates, for three TARGETS:
- `flipper/brucelink/core/` ← `shared/brucelink_core/src/*.c`
- `esp32/test_compile/src/core/` ← `shared/brucelink_core/src/*.c`
- `esp32/test_compile/src/bridge/` ← `esp32/lib/BruceLink/*.cpp`

Each generated file is exactly:
`/* GENERATED by tools/gen_wrappers.py - do not edit. */` then
`#include "<relative path to the real source>"`.

Run `gen_wrappers.py` after adding/removing/renaming a core or bridge source;
`gen_wrappers.py --check` (run by CI via `tests/test_wrappers.py`) fails if any
wrapper is stale, missing, or orphaned. The Flipper's `application.fam` globs
`*.c` recursively, so `core/*.c` is picked up automatically.

---

## 10. Dependencies per component

**No git submodules** in this repo (the brief; `tests/test_wrappers.py` also
forbids symlinks). Dependencies are pinned by each build system:

| Component | Depends on | How pulled |
|---|---|---|
| Flipper FAP | Momentum `mntm-012` SDK (API 87.1), STM32WB BLE stack | `ufbt` fetches the pinned SDK (`ufbt update --hw-target=f7 --url=…mntm-012…`). Shared core via `core/` wrappers (in-repo, relative include). |
| StickS3 bridge (compile check) | NimBLE-Arduino `2.5.1`, pioarduino `55.03.39` (arduino-esp32 3.3.9 / ESP-IDF 5.5.4) | PlatformIO `platform =` URL pin + `lib_deps = NimBLE-Arduino#2.5.1` in `esp32/test_compile/platformio.ini`. Shared + bridge sources via wrappers. |
| StickS3 bridge (real firmware) | Bruce `1.17` (`2b6ba4d`), its own NimBLE-Arduino 2.5.1 dep | `tools/install_into_bruce.py` copies `lib/` + glue into a Bruce checkout; build with `pio run -e m5stack-sticks3`. Bruce's own tree uses submodules — that is the upstream checkout, not this repo. |
| Host tests | system `cc`/`c++`, `cppcheck`, Python 3 | `make -C tests/c`, `python3 -m unittest`. No third-party libs (crypto is in-repo). |

ARM toolchain note (see `BUILD_AND_DEPLOY.md`): cloud uses xPack `arm-none-eabi-gcc`
12.3.1 as a substitute; locally use ufbt's real toolchain. `xtensa-esp-elf`
14.2.0, Python 3.13.

---

## 11. Entry points (quick reference)

| Entry | Where | Notes |
|---|---|---|
| FAP `brucelink_app(void*)` | `flipper/brucelink/brucelink.c:1453`; declared by `application.fam` `entry_point` | Allocates the app, runs the ViewDispatcher, frees. All logic on the 50 ms tick. |
| StickS3 bridge task | `esp32/src/modules/brucelink/BruceLinkModule.cpp` (`moduleStart` → task running `NimBleCentralTransport::loop()`) | Started from Bruce's "BruceLink → Turn on". Single owner of the bridge. |
| `bruceLinkBootEarly()` | installer adds the call to Bruce `src/main.cpp` after `_late_setup_gpio()` | Runs a pending job with BLE off, then restarts; no-op on a normal boot. |
| `bruceLinkBoot()` | installer adds the call to Bruce `src/main.cpp` before the startup app | After a job, restores settings and turns BruceLink on so the Flipper can fetch the result; no-op otherwise. |
| ESP32 compile check `setup()/loop()` | `esp32/test_compile/src/main.cpp` | Stub backend + demo key; build-only. |

---

## 12. Where to look to change X

| Task | Primary file(s) | Also |
|---|---|---|
| Add a Bruce capability | `shared/brucelink_core/src/bl_capability.h` (new `bl_cap_id_t` + flags) → `kCaps[]` in `esp32/lib/BruceLink/BruceLinkBridge.cpp` → a `BruceBackend` method in `BruceBackendImpl.{h,cpp}` | No new message type/transport per tool (`CLAUDE.md`). Jobs: add a `kJobLimits[]` row + `jobArgsOk` case + a body in `BruceLinkJobs.cpp`. |
| Change the wire format | `shared/brucelink_core/src/bl_proto.h` + `bl_frame.{h,c}` (+ `bl_types.h` limits) | Both devices must run the same build. Update `tests/c` + `shared/brucelink_protocol.py` + `tools/bl_vectors.c`; `shared/protocol.md` + `PROTOCOL.md`. |
| Touch the session/handshake | `bl_session.{h,c}` (labels, transcript, HKDF) | `tests/c/test_crypto.c`, `test_protocol_core.c`, `tests/test_protocol.py`; ADR 0002. |
| Touch the safety gate | `bl_safety.{h,c}` + gate call-sites in `BruceLinkBridge.cpp` (`handleActiveOp`, `handleCommand`) | ADR 0003/0006; never add per-function auth (`CLAUDE.md`). `tests/c/test_bridge.cpp`. |
| Change a RAM trim | `esp32/lib/BruceLink/bl_sticks3_ram.h` + the `BRUCELINK_RAM_TRIM` blocks the installer inserts (5 Bruce files) | `esp32/src/modules/brucelink/PATCH.md`; `tools/install_into_bruce.py`; `MEMORY_OPTIMIZATION.md`. |
| Change pairing | `bl_pair.{h,c}`; storage in `bl_keyfile.{h,c}` (Flipper) / `BruceLinkKeyStore.{h,cpp}` (StickS3) | ADR 0005; `tests/c/test_pair.c`. |
| Change jobs / boot flow | `bl_job.{h,c}` (core) + `BruceLinkJobs.{h,cpp}` + the `bruceLinkBoot*` hooks | ADR 0006; `tests/c/test_job.c`. |
| Change capture-file transfer | `bl_files.{h,c}` + the `handleFile*` methods in `BruceLinkBridge.cpp` + backend `captureFile*` | `tests/c/test_files.c`; Flipper UI in `brucelink.c` (`bl_xfer_*`). |
| Change the BLE transport | Flipper: `transport/bl_ble_transport.c` + `bl_gatt_evt.{h,c}`; StickS3: `NimBleCentralTransport.{h,cpp}` | UUIDs are shared in `bl_uuid.h` (one definition, tested to agree). ADR 0001. |
| Change the Bruce install/patch | `tools/install_into_bruce.py` + `esp32/src/modules/brucelink/PATCH.md` | `tests/test_install_into_bruce.py`. |

---

## 13. GAPS

- **Implementation bodies not line-read.** Roles/symbols above come from headers
  and targeted reads; the full `.cpp`/`.c` bodies of `BruceLinkBridge.cpp`,
  `BruceLinkModule.cpp`, `BruceLinkJobs.cpp`, `NimBleCentralTransport.cpp`,
  `bl_ble_transport.c`, `BruceBackendImpl.cpp` were not read end-to-end. Symbol
  names are verified; internal line references beyond those cited are not
  guaranteed exact — re-grep before relying on a line number.
- **`BruceLinkModule.cpp` menu/task internals** (task stack size, exact start
  refusal conditions beyond `ModuleStartResult`) not fully read.
- **`bl_capture.c` / inline-capture status** is DESIGN/under-review, not a claim
  about shipping behaviour — confirm against `FEATURE_MATRIX.md` /
  `CURRENT_STATE.md`.
- Hardware behaviour throughout is **HW-OWNER** (owner's devices, 2026-11-23),
  not independently verified in this session — see `../TEST_RESULTS.md`.
