# Feature Parity Matrix — native Bruce vs BruceLink

> Synthesis of `NATIVE_BRUCE_INVENTORY.md` (what Bruce has) and
> `INTEGRATION_INTERFACES.md` (how it can be driven), cross-referenced with
> BruceLink's current implementation state at `448b798`. Read those two for the
> source-level detail; this table is the at-a-glance map.
>
> **Guiding rule (owner intent, 2026-11-24):** native feature parity by
> *exposing* Bruce's tools through supported interfaces — not reimplementing
> them, and not inventing replacement tools to fill a gap. Where a native
> function cannot be exposed cleanly, that is recorded here, not worked around.

## Column meaning
- **Upstream**: present in Bruce 1.17 source (`2b6ba4d`).
- **On board**: compiled for env `m5stack-sticks3`. (`LITE_VERSION` is **not**
  defined for this board, so non-LITE features are in; exclusions are the
  C5-only and `BRUCE_NO_SDR_JAMMER` guards.)
- **RF**: PASSIVE (receive-only, though all Wi-Fi tools still key the radio) or
  ACTIVE (transmits frames / L2 injection).
- **Supported control path**: the cleanest *documented* way to drive it —
  `CLI`, `menu` (`options`/`nav`/`optionsJSON` walk), `startup` (startup-app
  registry), or `none` (physical-button only). All Wi-Fi tools are
  **radio-exclusive ⇒ BLE-off job** (ADR 0006); none run live over the link.
- **BruceLink status**: `shipped`, `planned`, `refused` (blocked by a safety
  boundary until its rules exist), or `not planned`.
- **Verified**: `hw` = owner-reported on hardware, `host` = host test/compile
  only, `none`.

## Passive / reconnaissance capabilities

| Capability | Upstream | On board | RF | Supported path | BruceLink status | Verified | Restrictions / notes |
|---|---|---|---|---|---|---|---|
| Wi-Fi scan | yes (`WiFi.scanNetworks`) | yes | PASSIVE (probe reqs) | menu side-effect / JS `wifiScan`; **no standalone scan command** | **shipped** — cap `wifi.scan`, BLE-off job; BruceLink runs its own scan in the job (Arduino API), not a Bruce menu | **hw** (owner: scan job worked, then "much faster") | Job limits {20 s default / 60 s max}. Candidate to re-base on a Bruce-native launch under ADR 0007. |
| Packet sniffer (full pcap) | yes (`sniffer_setup`) | yes | PASSIVE + hidden "BruceSniffer" AP beacons | CLI `sniffer` (can't stop over link), menu, startup `Sniffer` | **planned** (evaluate native Sniffer as a separate capability) | none | Full raw pcap is SD-gated on a bare stick. Bruce's sniffer beacons a hidden AP and hops channels only by hand. |
| Handshake capture (passive) | via Sniffer `HandshakesOnly` | yes | PASSIVE (listen) + hidden AP beacons | (BruceLink drives Bruce's sniffer in a job) | **shipped** — cap `wifi.sniff.handshake`, BLE-off job; uses Bruce's `sniffer_set_mode(HandshakesOnly)` + our hop loop | **host** (host-tested + built; **hardware pending**, not a recorded result); download/delete host only | Job limits {60 s default / 300 s max, arg = channel 0=hop/1..13}. NOT independently reviewed (a usage safeguard stopped that review). Distinct from Bruce's **active** Target-Atks capture (which deauths). |
| Capture files: list / download / delete | n/a (BruceLink feature over Bruce's `/BrucePCAP`) | yes | — | protocol msgs `pcap.list/transfer/delete` | **shipped** — `bl_files`, resumable, CRC-32 | **host** only (not hardware-confirmed end to end) | **Not encrypted** over BLE (M4.6). Demo-key bridge serves none. |
| AP info | yes (`displayAPInfo`) | yes | PASSIVE/local (STA req) | `none` (menu entry only when STA-connected; TFT-only output) | not planned | none | Exposes the saved PSK on screen. |
| Channel analyzer | yes | yes | PASSIVE | `none` (menu-only; config on-screen; TFT-only status) | not planned | none | Observation needs `display dump`, not a structured read. |
| Jam detect | yes | yes | PASSIVE | `none` (menu-only; TFT-only) | not planned | none | — |
| Scan hosts (ARP) | yes (`ARPScanner`) | yes | PASSIVE-ish (ARP over STA) | CLI `arp` (blocks serial task), menu | not planned | none | Requires an active STA connection. |
| Wardriving | yes (4 variants) | yes | PASSIVE (scan + GPS) | `startup` (registry) / GPS menu | not planned | none | Needs GPS peripheral. |
| WiFi pass recovery (offline WPA cracker) | yes (`wifi_recover`) | yes | none (CPU only) | `none` (menu-only) | not planned | none | No radio. Reads handshake + wordlist from SD. BruceLink RAM-trim already forces it out of IRAM. |
| Wi-Fi spectrum view | helper class only | yes | — | n/a (no entry point) | n/a | — | Shared view used by channel analyzer / jam detect. |

## Active / RF-transmitting capabilities — all **refused** pending ADR 0006 active-job rules

| Capability | Upstream | On board | RF | Supported path | BruceLink status | Verified | Restrictions / notes |
|---|---|---|---|---|---|---|---|
| Handshake capture (Target Atks) | yes (`capture_handshake`) | yes | **ACTIVE (deauth bursts)** | `none` (menu-only) | **refused** | none | Transmits deauth; distinct from the passive shipped capture above. |
| Deauth — flood / enhanced / targeted | yes (`deauthFloodAttack`, `deauther.cpp`) | yes | **ACTIVE (deauth frames)** | `none` (Wifi Atks submenu; no CLI/startup) | **refused** | none | Needs the safety gate's duration cap, on-stick stop, specific-target scope, no re-run. |
| Beacon spam | yes (`beaconAttack`) | yes | **ACTIVE (beacon flood)** | `none` (menu-only) | **refused** | none | — |
| Evil Portal (captive-portal clone) | yes (`EvilPortal`) | yes | **ACTIVE (fake AP + deauth)** | `none` (menu-only) | **refused** | none | Writes `*_creds.csv`. "Wi-Fi cloning" sense #1. |
| Karma attack (SSID impersonation / clone) | yes (`karma_setup`) | yes | **ACTIVE (beacon/probe-resp/deauth)** | `none` (menu-only) | **refused** | none | Has explicit `TIER_CLONE`. "Wi-Fi cloning" sense #2. |
| Brucegotchi (pwnagotchi) | yes (`brucegotchi_start`) | yes | **ACTIVE (deauth + pwngrid beacons)** | `startup` `Brucegotchi` / menu | **refused** | none | Deauths during INTERACT; beacons during ADVERTISE. |
| NetCut (ARP poison) | yes (`netcutMenu`) | yes | **ACTIVE (L2 ARP injection, STA req)** | `none` (menu-only) | **refused** | none | Not raw 802.11; still an active attack. |
| Responder (LLMNR/NBNS/SMB) | yes (`responder`) | yes | **ACTIVE (IP-layer poisoning)** | `none` (serial binding commented out) | **refused** | none | Writes `/BruceResponder/ntlm_hashes.txt`. |
| WiFite (WPS attack suite) | yes (`wifite_menu` + `lib/BruceWifite`) | yes (S3-only) | **ACTIVE (WPS/assoc)** | `none` (menu-only) | **refused** | none | Pixie Dust / WPS brute / PIN test. |

## Not available on this board

| Capability | Reason | Cite |
|---|---|---|
| SDR (raw I/Q jammer/spectrum) | `BRUCE_NO_SDR_JAMMER=1` in base env | `sdr.cpp:45`, `WifiMenu.cpp:86-88` |
| Dual-band analyzer | C5-only guard; board is S3 | `dual_band_analyzer.cpp:3`, `WifiMenu.cpp:82-84` |

## What this means for integration (summary)

1. **No standalone scan, and every offensive tool is menu-only.** Bruce's serial
   CLI exposes only `webui`/`wifi`/`arp`/`listen`/`sniffer`; JS only
   connect/scan/disconnect/MAC. So exposing deauth/evil-portal/karma/etc. through
   a *supported* interface means the `options`/`nav`/`optionsJSON` menu-walk, not
   a command — and the menu order/labels are not a stable API.
2. **Everything Wi-Fi is a BLE-off job.** There is no live-over-BLE Wi-Fi.
3. **Active RF stays refused** until ADR 0006's active-job rules (gate at
   acceptance, duration cap, on-stick stop, no re-run) are implemented and
   tested. That is a policy/safety boundary, not a technical gap — see
   `KNOWN_ISSUES.md` and `NEXT_STEPS.md`.
4. **Shipped today:** device info, capabilities, Wi-Fi scan (job), passive
   handshake capture (job), capture-file transfer. See `CURRENT_STATE.md` for the
   verification split.
