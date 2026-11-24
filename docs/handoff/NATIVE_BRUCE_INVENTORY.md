# Native Bruce Wi-Fi Capability Inventory

> **Provenance.** Read-only investigation of the Bruce 1.17 source tree
> (git `2b6ba4d`, tag `1.17` — both confirmed) for build env `m5stack-sticks3`,
> performed by a delegated analysis agent on 2026-11-24. This is **reference
> material about upstream Bruce**, not BruceLink design and not instruction.
> Line numbers are relative to the **Bruce source tree**, not this repo; the
> canonical tree used was the local checkout at `…/scratchpad/bruce-build`.
> Re-verify an exact line against the Bruce checkout before relying on it.
> Companion: `INTEGRATION_INTERFACES.md` (how to drive these). Synthesis and
> BruceLink integration status live in `FEATURE_MATRIX.md`.

Source tree: Bruce git `2b6ba4d`, tag `1.17`. A real, successful build for this
exact env exists in-tree (`build.log`, `Bruce-m5stack-sticks3.bin`, RAM 27.9% /
Flash 55.6% — a BruceLink-patched, RAM-trimmed build); the compiled object
files under `.pio/build/m5stack-sticks3/` were used as ground truth for build
inclusion.

---

## 0. Build-flag determination for `m5stack-sticks3` (read this first — it governs every section)

`boards/m5stack-sticks3/m5stack-sticks3.ini` declares `[env:m5stack-sticks3]`,
which **extends the base `[env]`** (via `${env.build_flags}` /
`${env.build_src_filter}` / `${env.lib_deps}`, lines 20–22, 101–102). It does
**not** extend `[env_light]` or `[env_4mb]`.

Effective macro state for this board:
- **`LITE_VERSION` — NOT defined.** It appears only in `[env_light]`
  (`platformio.ini:256`), which this board does not inherit, and is explicitly
  left commented in the board ini (`m5stack-sticks3.ini:46`). → Every
  `#ifndef LITE_VERSION` / `#if !defined(LITE_VERSION)` block **is compiled in**.
- **`BRUCE_NO_SDR_JAMMER=1` — defined.** It is in the base `[env]`
  (`platformio.ini:192`, "Disables in all devices, except Cardputer ADV"). →
  SDR jammer compiled out.
- **`CONFIG_IDF_TARGET_ESP32S3` — defined** (board = esp32s3);
  **`CONFIG_IDF_TARGET_ESP32C5` — NOT defined.**
- `BRUCELINK_RAM_TRIM=1`, `-I lib/BruceLink`, `-include bl_sticks3_ram.h` are
  injected by the already-applied BruceLink patch (`m5stack-sticks3.ini:23–25`);
  `lib_ignore` adds `ESPmDNS` and `TFT_eSPI` (lines 105–107). PSRAM enabled;
  2-button board (`HAS_2_BUTTONS`).
- `build_src_filter = ${env.build_src_filter} +<../boards/m5stack-sticks3>`
  (`m5stack-sticks3.ini:20`); base filter is `+<*>` (`platformio.ini:150-154`),
  so **all of `src/modules/wifi/` is handed to the compiler** — build inclusion
  is therefore decided by internal `#ifdef` guards, not by the source filter.

**Hardware note that applies to every Wi-Fi tool:** the StickS3 (ESP32-S3) has
a single 2.4 GHz radio and **no 5 GHz** — all channel lists are 1–11/1–12
(`sniffer.h:36-38`, non-C5 branch). Every capability below needs that one
radio, so per the BruceLink project's own one-radio rule (ADR 0006) none can
run while the BLE link is up; they are the `BL_CAP_F_JOB` case.

**Excluded on this board (cited):**
- **SDR** (`sdr.cpp`): whole body gated
  `#if !defined(LITE_VERSION) && !defined(BRUCE_NO_SDR_JAMMER)` (`sdr.cpp:45`).
  `BRUCE_NO_SDR_JAMMER` is set → compiled out (object `sdr.cpp.o` is a
  3,816-byte empty shell). Menu entry also gated out (`WifiMenu.cpp:86-88`).
- **Dual-Band Analyzer** (`dual_band_analyzer.cpp`): whole body gated
  `#if !defined(LITE_VERSION) && defined(CONFIG_IDF_TARGET_ESP32C5)`
  (`dual_band_analyzer.cpp:3`, closes line 254). Board is S3, not C5 →
  compiled out. Menu entry gated out (`WifiMenu.cpp:82-84`).

---

## Invocation hub

All on-device Wi-Fi tools hang off **Main menu → "WiFi"** →
`WifiMenu::optionsMenu()` (`src/core/menu_items/WifiMenu.cpp:49-119`; `WifiMenu`
is a member of `MainMenu`, `src/core/main_menu.h:41`).

**Serial CLI** (`src/core/serial_commands/wifi_commands.cpp`,
`createWifiCommands()` line 110): only `webui`, `wifi on|off|add <ssid> <pwd>`,
and (non-LITE, so present) `arp`, `listen`, `sniffer`. **No deauth / portal /
karma / wifite over serial.** `responder` is commented out (lines 97-108).

**JS interpreter** (`src/modules/bjs_interpreter/wifi_js.cpp`, registered in
`mqjs_stdlib.h:5823-5841`): only `wifiConnected`, `wifiConnectDialog`,
`wifiConnect`, `wifiScan`, `wifiDisconnect`, `wifiMACAddress` (plus HTTP fetch).
**No attack primitives exposed to JS.**

Net: offensive capabilities are reachable **only through the on-device WiFi
menu**, never via serial or JS.

---

## 1. Wi-Fi scanning / recon

- **Name:** standard network scan (`WiFi.scanNetworks`).
- **Source:** used as a building block everywhere — e.g. `wifi_atks.cpp:273, 344`;
  JS `native_wifiScan` (`wifi_js.cpp:76`). Menu "Connect to Wifi" / "Start WiFi
  AP" (`WifiMenu.cpp:55-61`).
- **What it does:** active managed scan; lists SSID/BSSID/RSSI/channel/encryption.
  `showHiddenNetworks` global (`WifiMenu.cpp:47`, toggled in Config at 149-158)
  adds hidden SSIDs.
- **Deps:** Arduino `WiFi` / esp-wifi. **HW:** radio, display. **RF class:**
  effectively PASSIVE (the stack emits normal probe requests during an active
  scan). **Build:** included. **Invocation:** menu + JS `wifiScan`.

## 2. Packet sniffing

- **Name:** **Sniffer**.
- **Source:** `src/modules/wifi/sniffer.cpp`; `sniffer_setup()` (promiscuous RX
  callback `sniffer()`); modes `SnifferMode::{Full,HandshakesOnly,DeauthOnly}`
  (`sniffer.h:...`, set at `sniffer.cpp:1168-1169`).
- **What it does:** ESP32 promiscuous capture; writes `.pcap`; tracks
  EAPOL/handshakes and beacons.
- **Deps:** `esp_wifi_set_promiscuous*`, SD/LittleFS, `core/display`. Shares
  handshake state with Target-Atks/Brucegotchi.
- **HW:** radio exclusive; writes storage. **Build:** included
  (`#if !defined(LITE_VERSION)`, true here).
- **Invocation:** WiFi menu "Sniffer" (`WifiMenu.cpp:80`); startup app "Sniffer"
  (`startup_app.cpp:30`); serial `sniffer` (`wifi_commands.cpp:126`).
- **RF class:** nominally PASSIVE, **but not RF-silent** — ⚠️ it brings up a
  **hidden WPA2 AP SSID `"BruceSniffer"` / PSK `"brucenet"`, channel 1**
  (`sniffer.cpp:1190-1202`) *before* enabling promiscuous mode, so the radio
  beacons (hidden, 100 ms interval) while sniffing. Starts on channel 1; channel
  hop is **manual** via the Next button (`sniffer.cpp:1245-1249`), not timed.
  Writes pcaps to storage; stops promiscuous mode if LittleFS fills.

## 3. Handshake capture

- **Name:** **Target Atks → capture handshake** (plus Sniffer's HandshakesOnly
  mode and Brucegotchi).
- **Source:** `wifi_atks.cpp:400 capture_handshake()` (reached via
  `target_atk_menu` from the "Target Atks" scan list, `wifi_atks.cpp:315-323`).
- **What it does:** sets `WIFI_MODE_APSTA`, enables promiscuous capture of EAPOL
  for a chosen BSSID, and **periodically transmits deauth bursts** (5 frames
  each) to force clients to reauth and expose the 4-way handshake
  (`wifi_atks.cpp:505-534`). Saves to `/BrucePCAP/handshakes/` on SD, else
  LittleFS (`wifi_atks.cpp:464-479`).
- **Deps:** sniffer callback + `send_raw_frame`/`wsl_bypasser_send_raw_frame`
  (`wifi_atks.cpp:140, 147`), SD/LittleFS.
- **HW:** radio exclusive; storage writes. **Build:** included.
- **RF class:** **ACTIVE** — transmits 802.11 deauthentication frames (forged
  source = target AP, dest = broadcast). **Invocation:** WiFi menu → Wifi Atks →
  Target Atks.

> **BruceLink note (not part of the upstream inventory):** BruceLink's existing
> passive capture job uses the **Sniffer HandshakesOnly** path (listen + hop),
> which does **not** transmit deauth, unlike this Target-Atks capture. See
> `FEATURE_MATRIX.md` and the Sniffer-vs-passive-capture evaluation in
> `NEXT_STEPS.md`.

## 4. Pwnagotchi — named **"Brucegotchi"**

- **Source:** `src/modules/pwnagotchi/` — `pwnagotchi.cpp` (entry
  `brucegotchi_start()` at line 320), `pwngrid.cpp` (pwngrid beacon protocol),
  `spam.cpp` (standalone pwngrid spam), `mood.cpp`, `ui.cpp`.
- **What it does:** pwnagotchi clone with a phase loop (`pwnagotchi.cpp`):
  **RECON** (scan/collect beacons) → **INTERACT** (per-channel **deauth**
  `wsl_bypasser_send_raw_frame` + `send_raw_frame`, lines 250-251, then wait for
  handshakes) → **ADVERTISE** (pwngrid beacon spam, `advertise()` line 297).
  Saves handshakes to `/BrucePCAP/handshakes` (lines 331-334). Its menu also
  offers **"Pwngrid spam"** → `send_pwnagotchi_beacon_main` in `spam.cpp`
  (`pwnagotchi.cpp:385`). `pwngrid.cpp` transmits raw pwngrid beacon frames via
  `esp_wifi_80211_tx` (lines 79-124).
- **Deps:** sniffer handshake cache, `esp_wifi` promiscuous + raw TX, SD/LittleFS,
  ArduinoJson (pwngrid payload). Ported from 7h30th3r0n3 / palnagotchi projects.
- **HW:** radio exclusive; display; storage. **Build:** included (registered
  under `#ifndef LITE_VERSION`, `startup_app.cpp:28-29`).
- **RF class:** **ACTIVE** — transmits deauth frames **and** pwngrid
  advertisement beacons. **Invocation:** WiFi menu "Brucegotchi"
  (`WifiMenu.cpp:105`); startup app "Brucegotchi" (`startup_app.cpp:29`).

## 5. Deauthentication

- **Name:** **Deauth Flood** and **Enhanced Deauth** (under "Wifi Atks").
- **Source:** raw TX primitives `send_raw_frame()` (`wifi_atks.cpp:140`) and
  `wsl_bypasser_send_raw_frame()` (`wifi_atks.cpp:147`); `deauthFloodAttack()`
  (`wifi_atks.cpp:336`); enhanced/target logic in `src/modules/wifi/deauther.cpp`
  — `stationDeauth()` (line 359), `runDeauthAll()` (line 703), `deauthAllMenu()`
  (line 882), plus `deauthAll`/`deauthTargetList`/per-client selection
  (`deauther.h`). `enhancedDeauthMenu()` is called from the Wifi Atks menu
  (`wifi_atks.cpp:263`).
- **What it does:** scans, then continuously transmits 802.11 deauth frames —
  broadcast flood across all scanned APs/channels (Deauth Flood), or targeted at
  selected AP/clients (Enhanced Deauth). Default frame `deauth_frame_default[]`
  in `wifi_atks.h`.
- **Deps:** `esp_wifi_80211_tx` (via `send_raw_frame`), scan results, display.
- **HW:** radio exclusive. **Build:** included.
- **RF class:** **ACTIVE** — transmits deauthentication/disassociation management
  frames (reason 0x0002). **Invocation:** WiFi menu → **Wifi Atks**
  (`WifiMenu.cpp:67`) → "Deauth Flood" / "Enhanced Deauth" (`wifi_atks.cpp:262-263`).

## 6. "Wi-Fi cloning" — precise identification

Bruce has **no single feature literally called "clone".** The cloning-type
behaviors are three distinct tools; name them precisely:

**6a. Evil Portal** (captive-portal / evil-twin credential clone) — the closest match.
- Source: `src/modules/wifi/evil_portal.cpp`, class `EvilPortal` (ctor line 17;
  `setup()` 62; `beginAP()` 125; `setupRoutes()` 149; `loop()` 290).
- What it does: stands up a **fake open softAP** (default "Free Wifi" or a
  user/Evil-Wifi-list name; `WIFI_MODE_AP`, or `APSTA` when "verify password" is
  on to validate creds against the real AP — lines 130-138), a **DNS server on
  :53 catching all names** (line 145), and an async web server that answers every
  OS captive-portal probe (`/generate_204`, `/hotspot-detect.html`, `/ncsi.txt`,
  `/connecttest.txt`, Firefox/MS endpoints, etc., lines 150-219) to force the
  login popup and serve a **cloned HTML portal**. It also **sends a deauth frame**
  at setup (`wsl_bypasser_send_raw_frame`, line 94) to knock clients off the real
  AP. Captured credentials are written to **`<ssid>_creds.csv` /
  `default_creds.csv`** (lines 502, 525, 595, 656).
- RF class: **ACTIVE** — transmits beacons for the fake AP + a deauth frame.
  Build: included. Invocation: WiFi menu "Evil Portal" (`WifiMenu.cpp:68`).
  Config submenu manages Evil-Wifi SSID list and endpoint/creds settings
  (`WifiMenu.cpp:124-145`).

**6b. Karma Attack** (SSID impersonation / "clone network" mode).
- Source: `src/modules/wifi/karma_attack.cpp` (`karma_setup()`); header
  `karma_attack.h` (entirely `#ifndef LITE_VERSION`).
- What it does: listens for client **probe requests** and answers as whatever
  SSID the client is looking for — probe responses (`sendProbeResponse()` line
  1314), beacon advertisement (`sendBeaconFrameHelper()` line 1274,
  `sendBeaconFrames()` 1400), optional **deauth** (`sendDeauth()` line 1360), and
  optional auto-launch of a background Evil Portal per lured SSID. Modes
  `MODE_PASSIVE/BROADCAST/FULL`; attack tiers include an explicit **`TIER_CLONE`
  / `enableCloneMode` / `maxCloneNetworks`** ("Clone network attacks",
  `karma_attack.h`). Can mass-broadcast SSIDs from a file (`ActiveBroadcastAttack`,
  `SSIDDatabase`).
- RF class: **ACTIVE** — beacons, probe responses, deauth (all via
  `esp_wifi_80211_tx`). Build: included. Invocation: WiFi menu → **Wifi Atks →
  "Karma Attack"** (`wifi_atks.cpp:259`).

**6c. Beacon SPAM** (SSID flood — not true cloning, but often conflated).
- Source: `beaconAttack()` (`wifi_atks.cpp:825`). Modes: Funny SSID, Rick Roll,
  Random SSID, **Single SSID** (base + suffixes), **Custom SSIDs** (from file).
- RF class: **ACTIVE** — floods beacon frames for many fake SSIDs. Build:
  included. Invocation: WiFi menu → Wifi Atks → "Beacon SPAM" (`wifi_atks.cpp:261`).

## 7. Network / AP diagnostics

**AP info** — `ap_info.cpp:displayAPInfo()` (line 119). Shows the
currently-connected AP's SSID, **saved PSK** (`bruceConfig.getWifiPassword`),
signal, gateway, channel, BSSID, vendor, auth mode, ciphers, PHY modes.
**RF: PASSIVE / local read** (`esp_wifi_sta_get_ap_info`). STA-connection
required; menu entry only appears when connected (`WifiMenu.cpp:64-66`).
Build: included.

**Channel Analyzer** — `channel_analyzer.cpp:channel_analyzer_setup()`.
Promiscuous RX only, empty filter, hops ch 1–11, estimates per-channel airtime
load; renders via shared `WifiSpectrumView`. **RF: PASSIVE**
(`esp_wifi_set_promiscuous`, no TX — `channel_analyzer.cpp:50-60`). Build:
included (`#if !defined(LITE_VERSION)`). Menu `WifiMenu.cpp:81`.

**Jam Detect** — `jam_detect.cpp:jam_detect_setup()`. Promiscuous RX of
management frames, hops ch 1–11 (~100 ms dwell, full sweep ~1.1 s,
`jam_detect.cpp:16-30`), raises a visual alert on high deauth/disassoc rate; uses
`WifiSpectrumView`. **RF: PASSIVE** (detector, no TX — `jam_detect.cpp:59-70`).
Build: included. Menu `WifiMenu.cpp:85`.

**Wi-Fi Spectrum** — **not a standalone menu tool.** `wifi_spectrum.{h,cpp}` is a
shared display class `WifiSpectrumView` (2.4 GHz channel lobes/waterfall)
consumed by Channel Analyzer and Jam Detect only. Compiled in
(`#if !defined(LITE_VERSION)`) but has no user entry point of its own.

**Dual Band Analyzer** — **EXCLUDED on this board** (C5-only; see §0).

**NetCut** — `netcut.cpp:netcutMenu()`. ARP-spoofing "network cut" ported from
NetCut-ESP32: discovers LAN hosts, learns gateway MAC, then **poisons ARP tables
via LwIP `netif->linkoutput()`** (raw ARP injection at L2) to cut victims off;
has AUTO-CUT and AUTO-TROLL modes. **Requires STA connection**
(`WiFi.isConnected()`, `netcut.cpp:256`). **RF: ACTIVE at the ARP/L2 layer** over
a normal STA association — it does **not** inject raw 802.11 frames. Build:
included. Menu `WifiMenu.cpp:72`. (This file already carries BruceLink RAM-trim
patches moving its device table to PSRAM, lines 28-44.)

**Scan Hosts** — `ARPScanner` in `src/modules/ethernet/ARPScanner.{h,cpp}`
(invoked from `WifiMenu.cpp:89-102` and serial `arp`, `wifi_commands.cpp:68-78`).
ARP-based LAN host discovery on the connected network. **RF: PASSIVE-ish**
(normal ARP over STA). Requires STA connection. Build: included.

**WiFi Pass Recovery** — `wifi_recover.cpp:wifi_recover_menu()`. ⚠️ **Not a
saved-password reader — it is an OFFLINE WPA handshake dictionary cracker**
("WiFi Password Cracker", header lines 3-15): loads a captured handshake
(ANonce/SNonce/EAPOL/MIC) and brute-forces a wordlist from SD using dual-core
software PBKDF2-HMAC-SHA1 (~13-14 pw/s on S3). **RF class: PASSIVE / none** —
pure CPU crypto on a captured handshake, no radio TX. Reads handshake + wordlist
from SD. Build: included (`#ifndef LITE_VERSION`). Menu `WifiMenu.cpp:109`.
(BruceLink patch forces it to run from flash, not IRAM — `wifi_recover.cpp:62-65`.)

## 8. Other wireless utilities present

**Responder** — `responder.cpp:responder()`. LLMNR/NBNS poisoner + SMB NTLM-hash
harvester (ports NBNS 137, LLMNR 5355, SMB 445; `responder.cpp:34-42`), ported
from Evil-M5Project. Answers name queries claiming to be the requested host, runs
a fake SMB server, captures NTLMv1/v2 challenge-response and **appends them to
`/BruceResponder/ntlm_hashes.txt`** (`responder.cpp:295`). **RF: ACTIVE at IP
layer** (STA/AP network traffic), not raw 802.11. Build: included
(`#ifndef LITE_VERSION`). Menu `WifiMenu.cpp:104`. (Serial binding exists but is
commented out.)

**WiFite** — `wifite.cpp:wifite_menu()`, backed by the **vendored
`lib/BruceWifite/` library** (`bruce_wifite.h`, `vendor/src/wps/`,
`pixie_prng.h`, `eap_peer/`). It is a **WPS attack suite**, not the full Linux
Wifite: per-target menu = **Auto test, Pixie Dust, WPS brute force, Test one PIN**
(`wifite.cpp:114, 121`); tracks WPS advertised/lock status. Top menu: Scan
networks, Router table, Last terminal log, Save log to SD, About/limits
(`wifite.cpp:237`). Self-describes as "Experimental WPS transport; hardware
compatibility varies." **RF: ACTIVE** (association/EAPOL-WPS frames). **Build:**
included — guard is
`#if defined(CONFIG_IDF_TARGET_ESP32S3) && !defined(LITE_VERSION)`
(`wifite.cpp:6`), both true here (object `wifite.cpp.o` ≈ 769 KB). **S3-only**
(other boards excluded). Menu entry also S3-gated (`WifiMenu.cpp:106-108`).

**SOCKS4 Proxy / Listen TCP / Client TCP / TelNET / SSH** — benign TCP network
tooling over an STA (or AP) association, no raw RF: `socks4_proxy.cpp:socks4Proxy(1080)`
(WiFiServer/WiFiClient relay); `tcp_utils.cpp:listenTcpPort()/clientTCP()`;
`clients.cpp:telnet_setup()/ssh_setup()`. **RF: PASSIVE** (normal data frames).
All under `#ifndef LITE_VERSION` → included. Menu `WifiMenu.cpp:75-79`; serial
`listen` (`wifi_commands.cpp:86`).

**Wireguard** — `wg_setup()` VPN client (not an attack). Menu `WifiMenu.cpp:103`.

**Wardriving** — `src/modules/gps/wardriving.cpp` (`Wardriving(scanWiFi, scanBLE)`,
line 39). WiFi + optional BLE scan logged with GPS to CSV (Wigle-style). **RF:
PASSIVE** (scan + GPS serial read). Registered as 4 startup-app variants
(`startup_app.cpp:40-43`): Wardriving, WardrivingNoRadio, WardrivingBTEOnly,
WardrivingWifiOnly. Build: included.

**SDR** — `sdr.cpp` (raw I/Q takeover of the S3 Wi-Fi radio — jammer/spectrum).
**EXCLUDED on this board** by `BRUCE_NO_SDR_JAMMER` (see §0). Would be ACTIVE RF
if present.

---

## Summary table (env `m5stack-sticks3`)

| Capability | Entry fn / file | RF class | Built in? |
|---|---|---|---|
| Wi-Fi scan/recon | `WiFi.scanNetworks` (many); JS `wifiScan` | PASSIVE (probe reqs) | yes |
| Sniffer | `sniffer_setup` `sniffer.cpp` | PASSIVE + hidden "BruceSniffer" AP beacons | yes |
| Handshake capture (Target Atks) | `capture_handshake` `wifi_atks.cpp:400` | **ACTIVE (deauth)** | yes |
| Brucegotchi (pwnagotchi) | `brucegotchi_start` `pwnagotchi.cpp:320` | **ACTIVE (deauth + pwngrid beacons)** | yes |
| Deauth Flood / Enhanced Deauth | `deauthFloodAttack` `wifi_atks.cpp:336`; `deauther.cpp` | **ACTIVE (deauth)** | yes |
| Evil Portal (captive-portal clone) | `EvilPortal` `evil_portal.cpp` | **ACTIVE (fake AP + deauth)** | yes |
| Karma Attack (SSID impersonation/clone) | `karma_setup` `karma_attack.cpp` | **ACTIVE (beacon/probe-resp/deauth)** | yes |
| Beacon SPAM | `beaconAttack` `wifi_atks.cpp:825` | **ACTIVE (beacon flood)** | yes |
| AP info | `displayAPInfo` `ap_info.cpp:119` | PASSIVE/local (STA req) | yes |
| Channel Analyzer | `channel_analyzer_setup` | PASSIVE | yes |
| Jam Detect | `jam_detect_setup` | PASSIVE | yes |
| Wi-Fi Spectrum (`WifiSpectrumView`) | shared view class | PASSIVE (no own entry point) | yes (helper only) |
| NetCut | `netcutMenu` `netcut.cpp` | **ACTIVE (ARP poison, L2, STA req)** | yes |
| Scan Hosts (ARPScanner) | `modules/ethernet/ARPScanner` | PASSIVE-ish (ARP, STA req) | yes |
| WiFi Pass Recovery | `wifi_recover_menu` `wifi_recover.cpp` | **PASSIVE / none (offline cracker)** | yes |
| Responder | `responder` `responder.cpp` | **ACTIVE (IP-layer LLMNR/NBNS/SMB)** | yes |
| WiFite (WPS suite) | `wifite_menu` `wifite.cpp` + `lib/BruceWifite` | **ACTIVE (WPS/assoc)** | yes (S3-only) |
| SOCKS4 / TCP / TelNET / SSH | `socks4_proxy`/`tcp_utils`/`clients` | PASSIVE (data over STA) | yes |
| Wireguard | `wg_setup` | PASSIVE (VPN) | yes |
| Wardriving (4 variants) | `Wardriving` `gps/wardriving.cpp` | PASSIVE (scan+GPS) | yes |
| **Dual Band Analyzer** | `dual_band_analyzer.cpp` | — | **NO — C5-only guard** |
| **SDR** | `sdr.cpp` | (ACTIVE if present) | **NO — `BRUCE_NO_SDR_JAMMER`** |

**Storage-writing tools** (caveat for safety/data handling): Sniffer,
Target-Atks handshake capture, Brucegotchi → `/BrucePCAP/...`; Evil Portal →
`*_creds.csv`; Responder → `/BruceResponder/ntlm_hashes.txt`; WiFi Pass Recovery
reads handshake+wordlist from SD; Wardriving → GPS CSV; WiFite can save logs to SD.

**Access-surface caveat:** every offensive capability is on-device-menu-only.
Serial CLI and the JS interpreter expose connect/scan/disconnect/MAC/webui/
sniffer/arp/listen **only** — no deauth, portal, karma, beacon, responder, or
wifite binding exists to drive remotely today.
