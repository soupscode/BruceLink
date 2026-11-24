# BruceLink Capability Matrix

Pinned: Bruce `1.17` (`2b6ba4d`) · board `m5stack-sticks3` (ESP32-S3, 8 MB flash,
3 MB LittleFS, **no built-in SD**, 2.4 GHz only) · Momentum `mntm-012`.
Last updated: 2026-11-23.

**Nothing here is "verified on physical hardware" yet.** Every hardware column is
`—` until a local session runs it (see `LOCAL_HANDOFF.md`). Do not read
"Supported on StickS3" as "tested": it means the upstream code path exists and
nothing in the board config rules it out.

## Legend

| Column | Meaning |
|---|---|
| **Upstream** | The function exists in Bruce `1.17` source (verified, file cited). |
| **StickS3** | Likely runnable on a bare StickS3: `Yes` / `Likely` / `No` / `?`. |
| **BruceLink** | Exposed through the BruceLink bridge: `Impl` (code exists), `Design` (specified, not coded), `—`. |
| **Sim** | Covered by host/simulated tests (`tests/`). |
| **HW** | Verified on physical hardware. **All `—` today.** |
| **Ext HW** | Needs hardware the StickS3 lacks. |
| **Class** | `Passive` or `Active-RF` (transmits; routed through the remote safety gate). |

## Matrix

| Capability (id) | Upstream (Bruce source) | StickS3 | BruceLink | Sim | HW | Ext HW | Class |
|---|---|---|---|---|---|---|---|
| Device info `0x0001` | `infoCallback`, `BRUCE_VERSION`, `getBattery()` (`util_commands.cpp`, `interface.cpp:114`) | Yes | Impl | Yes¹ | — | No | Passive |
| Status `0x0002` | globals `wifiConnected`, `sdcardMounted` (`globals.h`) | Yes | Impl | Yes | — | No | Passive |
| Wi-Fi scan `0x0010` (job) | `WiFi.mode(WIFI_MODE_STA)` + `WiFi.scanNetworks` as Bruce's own scan (`wifi_common.cpp`) | Likely | Impl² | Yes¹ | — | No | Passive |
| BLE scan `0x0011` | `getScan()->getResults` (model: `native_bleScan`, `ble_js.cpp:5`) | Likely | Design | — | — | No | Passive |
| Sniff: EAPOL handshake `0x0020` | `isItEAPOL`/`saveHandshake` (`sniffer.cpp:221,300`) → LittleFS | Likely | Impl³ | Yes | — | No | Passive |
| Sniff: full raw pcap `0x0021` | `sniffer_full_mode_available` = `sdDetected` (`sniffer.cpp:869`) | **No** (SD-gated) | Impl³ (gated) | Yes | — | **SD module** | Passive |
| PCAP list `0x0022` | `ls`/`stat` (`storage_commands`), `/listfiles` WebUI | Yes | Design | — | — | No | Passive |
| PCAP transfer `0x0023` | WebUI `/file` download only; **no binary serial export** | Partial | Design⁴ | Yes⁵ | — | No | Passive |
| PCAP delete `0x0024` | `rm`/`deleteFromSd` | Yes | Design | — | — | No | Passive |
| PMKID capture | **Not in upstream** | No | — | — | — | — | — |
| Deauth `0x0040` | `send_raw_frame`→`esp_wifi_80211_tx` (`wifi_atks.cpp:140`); **ungated** | Likely | Impl⁶ | Yes⁷ | — | No | **Active-RF** |
| Beacon spam `0x0041` | `beaconAttack` (`wifi_atks.cpp:825`) | Likely | Design⁶ | — | — | No | **Active-RF** |
| Probe flood `0x0042` | probe TX path (`wifi_atks.cpp`) | Likely | Design⁶ | — | — | No | **Active-RF** |
| Sub-GHz (CC1101) | `rf_commands` | **No** | — | — | — | **CC1101** | Active-RF |
| NRF24 | NRF24 module | **No** | — | — | — | **NRF24** | Active-RF |
| IR TX/RX | `ir` commands, IR TX 46 / RX 42 | Likely | — | — | — | No | Active (IR) |
| BLE↔Wi-Fi coexistence mgmt | **Not in upstream** (no `esp_coex*`) | ? | Design⁸ | — | — | No | — |

## Notes

1. Exercised end-to-end in host tests through the shared protocol core and the
   bridge core (Flipper client ↔ bridge, `tests/c/test_bridge.cpp`); until v0.2
   also on a Flipper through the app's loopback mock (since removed). The radio
   call itself is a stub until hardware.
2. A job (ADR 0006): the bridge accepts `JOB_START` and the StickS3 restarts;
   the scan runs at boot with BLE off (`BruceLinkJobs.cpp`), the result is
   fetched with `JOB_RESULT_REQ`. Host-tested end to end; not yet run on
   hardware. (Before ADR 0006 an inline scan worked on hardware.)
3. `bl_capture` session manager + mock backend fully simulated (lifecycle, gated
   raw mode, start/poll failure). The Bruce-side capture backend is a documented
   skeleton; Bruce needs a small headless `sniffer_start/stop/status` patch
   (ADR 0003).
4. Resumable `CAP_XFER_*` designed; bulk transfer deliberately **off** the live
   BLE control link (ADR 0003). Transport choice (BLE vs USB vs Wi-Fi) pending
   hardware throughput/coexistence measurement.
5. PCAP header bytes and CRC32 verified against the C core and an independent
   Python implementation (`tests/test_protocol.py`).
6. BruceLink adds **no** transmit code. Active ops forward to Bruce's existing
   primitives through the remote safety gate (connection-level auth + default-off
   enable + link-loss e-stop + rate limit + optional target scope, ADR 0003).
   For authorized, isolated lab use only.
7. Gate policy (default-deny, enable, scope, rate limit, e-stop, link-loss) is
   fully simulated in `tests/c/test_capability.c`. The RF transmission is not.
8. Design intent only: pause bulk BLE traffic while promiscuous capture runs;
   measure on hardware before committing.

## How to update this matrix

When a capability is verified on a physical StickS3, change its **HW** cell to
`Yes (YYYY-MM-DD, Bruce <commit>)` and link the evidence in
`docs/TEST_RESULTS.md`. Never mark HW `Yes` from simulation alone.
