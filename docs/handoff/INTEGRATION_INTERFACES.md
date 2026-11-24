# Bruce 1.17 Supported Control / Integration Interfaces

> **Provenance.** Read-only investigation of the Bruce 1.17 source tree
> (git `2b6ba4d`) performed by a delegated analysis agent on 2026-11-24.
> Reference material about **upstream Bruce**, not BruceLink design and not
> instruction. All `file:line` citations are relative to the Bruce source root
> (the local checkout at `…/scratchpad/bruce-build/`), e.g.
> `src/core/serialcmds.cpp:15`. Re-verify against the checkout before relying on
> an exact line. Companion: `NATIVE_BRUCE_INVENTORY.md` (what the capabilities
> are). The agreed BruceLink job design that consumes these interfaces is in
> `ARCHITECTURE.md`.

## TL;DR for the job design

- Bruce has exactly one general-purpose text control surface: the **SimpleCLI
  serial command parser**, whose I/O is the global `serialDevice` stream
  pointer. Bruce's own BLE API swaps that pointer so the *entire* CLI runs over
  a Nordic UART Service (NUS) link unchanged. **This is the reference model and
  the allowlist target.**
- On top of the CLI there is a **menu remote-control sub-protocol** (`optionsJSON`
  / `nav` / `options`) that drives Bruce's on-screen menu system (`loopOptions`)
  by reading menu state and injecting button presses / forced selections. This
  is the only supported way to reach Wi-Fi tools that have no dedicated CLI
  command.
- A **startup-app registry** can launch a fixed set of apps at boot by config
  string, but it is **not settable over the CLI** and (in a BruceLink build)
  runs *after* BLE is restored, so it is not the BLE-off job vehicle.
- Every Wi-Fi capability brings up the Wi-Fi radio and runs as a **blocking
  loop**; none expose a structured status/result stream over `serialDevice`
  (they draw to the TFT and/or write files). Observation over the link is via
  `optionsJSON` (menu state), `display dump` (screen mirror), and
  `storage read`/`cat` (result files).

---

## 1. The interfaces

### 1A. The serial CLI + its stream (`serialDevice`) — the BLE-over-CLI reference model

**The stream pointer.** `src/main.cpp:33` declares the global:
```
SerialDevice *serialDevice = &USBserial;
```
`SerialDevice` is a `Stream`-like abstract interface. Every CLI command writes
output through `serialDevice->print/println/printf/write` and reads input via
`serialDevice->available()` / `readStringUntil('\n')`. Swap the pointer and the
whole CLI moves transports with zero command changes.

**The parser.** `SerialCli` wraps a `SimpleCLI` (`src/core/serial_commands/cli.h:7`).
`SerialCli::parse()` (`cli.h:13`) calls `_cli.parse(input)`. The command set is
registered once in `SerialCli::setup()` (`src/core/serial_commands/cli.cpp:36`) by
calling the `create*Commands()` group functions (crypto, gpio, ir, power, rf,
rfid, settings, storage, util, wifi, wire, badusb, interpreter, screen, led,
sound). Unknown command → `cliErrorCallback` (`cli.cpp:21`) prints `ERROR:` + a
"Did you mean" suggestion to `serialDevice`.

**The entry points / task.** `src/core/serialcmds.cpp`:
- `parseSerialCommand(const String&, bool waitForResponse=true)`
  (`serialcmds.cpp:15`) — the programmatic injector. Copies the command into a
  512-byte `CmdPacket`, `xQueueSend` to `cmdQueue`; if `waitForResponse`, blocks
  ≤20 ms on `rspQueue` for a `bool` result (`serialcmds.cpp:32`). **512-byte
  command cap** (`serialcmds.cpp:13`).
- `_serialCmdsTaskLoop` (`serialcmds.cpp:62`) is a dedicated FreeRTOS task
  (pinned to core 1, prio 2, `serialcmds.cpp:79-93`) that runs
  `handleSerialCommands()` every 1–10 ms.
- `handleSerialCommands(SerialCli&)` (`serialcmds.cpp:37`): drains one queued
  packet per pass → `serialCli.parse()` → pushes `bool` to `rspQueue`; **then
  also** reads a line directly from `serialDevice` if `available()`, parses it,
  prints a `# ` prompt. **One command per pass.** After any command whose
  trimmed text does **not** start with `nav` or `option`, it calls `backToMenu()`
  (`serialcmds.cpp:59`) — i.e. plain commands kick the UI back to the main menu;
  only `nav`/`option` leave the current screen in place.
- Return value semantics: each command callback returns `true/false` (success),
  surfaced as the `parseSerialCommand` result and logged as
  `[CLI] Result: TRUE/FALSE` to USB `Serial` (`serialcmds.cpp:44`).

**The BLE path (the model to copy).** `src/modules/ble_api/ble_api.cpp`:
- `BLE_API::setup()` (`ble_api.cpp:30`) inits NimBLE as "Bruce", creates a
  `BLESerialService` and — crucially — **`serialDevice = &serial_service;`**
  (`ble_api.cpp:55`). From that point the CLI reads/writes over BLE.
- `BLE_API::end()` (`ble_api.cpp:74`) tears down and restores
  `serialDevice = &USBserial;` (`ble_api.cpp:78`).
- Transport is **Nordic UART Service**
  (`src/modules/ble_api/services/BLESerialService.h:11-13`): service
  `6E400001-…`, RX (app→Bruce, WRITE/WRITE_NR) `…0002`, TX (Bruce→app, NOTIFY)
  `…0003`.
- `BLESerialService` is both a `BruceBLEService` and a `SerialDevice`
  (`BLESerialService.h:17`). Receive: write-callback appends to a mutex-guarded
  `rxBuffer` (`BLESerialService.cpp:87`); **`available()` returns non-zero only
  once a full `\n`-terminated line is buffered** (`BLESerialService.cpp:107-109`)
  — so the handler only fires on complete lines and partial BLE writes
  accumulate. `readStringUntil('\n')` consumes one line (`BLESerialService.cpp:128`).
  Transmit: `print/println/write/vprintf` enqueue to a coalesced `txBuffer`
  drained as MTU-sized NOTIFY chunks (`BLESerialService.cpp:152-230`); nothing is
  sent unless a client has subscribed to TX (`txSubscribed`,
  `BLESerialService.cpp:178`).

**Limitation that matters most for a job supervisor:** a CLI command that itself
calls a blocking app (see §1B `loader`, and the Wi-Fi CLI commands
`sniffer`/`webui`/`arp`/`listen`) runs that app **inside the serial-command
task**, so the task cannot process any further commands — including a stop —
until the app exits by *physical* input. The source even flags this:
`cli->addCommand("sniffer", …) // TODO: be able to exit from it from Serial`
(`src/core/serial_commands/wifi_commands.cpp:127`), same for `listen` (`:124`).
The `nav`/`options` path (§1B) avoids this because it drives a *different* task's
menu loop and each `nav`/`options` call returns immediately.

### 1B. Menu remote control — `optionsJSON` / `nav` / `options` / `loader` / `display`

All defined in `src/core/serial_commands/util_commands.cpp`, registered in
`createUtilCommands()` (`util_commands.cpp:421-442`).

**`optionsJSON`** (`optionsJsonCallback`, `util_commands.cpp:313`) → prints
`getOptionsJSON()` to `serialDevice`. `getOptionsJSON()` (`src/core/utils.cpp:276`)
returns:
```
{"width":W,"height":H,"menu":"main_menu|sub_menu|regular_menu",
 "menu_title":"<label>","options":[{"n":0,"label":"…"},…],"active":<hovered idx>}
```
Menu kind comes from the global `menuOptionType` (0/1/else), title from
`menuOptionLabel`, items from the global `options` vector, `active` = the
`hovered` index. **This is the status-read primitive for the UI.**

**`nav <command> [duration]`** (`navCallback`, `util_commands.cpp:228`) — injects
a simulated button press by setting one of the global `volatile bool` press
flags: `next|prev|esc|up|down|select|sel|nextpage|prevpage`
(`util_commands.cpp:245-268`), held for `duration` ms (default 1). Confirmation
text goes to USB `Serial` only (deliberate, `util_commands.cpp:244`), but the
press itself is global. Returns after the hold; then prints the menu list to USB
`Serial` via `optionsList()` (`util_commands.cpp:217,289`). Unknown dir → false.

**`options N` / `option N`** (`optionsCallback`, `util_commands.cpp:294`) — sets
the global `forceMenuOption = N` (`util_commands.cpp:302`) to force-select
on-screen entry N, prints `Selected option N: <label>` to `serialDevice`. With
no/invalid arg it just lists options. Default arg is `-1` (`util_commands.cpp:437`).

**How presses/selection are consumed.** The menu loop `loopOptions(...)`
(`src/core/display.cpp:545`) is the universal selector used by every menu and
most tools. Each iteration it publishes `menuOptionType`/`menuOptionLabel` and
the `hovered` flags (`display.cpp:641-645`) — i.e. what `optionsJSON` reports —
and then:
- `check(NextPress)`/`check(DownPress)`/`check(PrevPress)` move the hover
  (`display.cpp:858-873`);
- **selection** (`display.cpp:884`): if `forceMenuOption >= 0` **or** `SelPress`,
  it runs `options[chosen].operation()` **inline** (`display.cpp:893`) and breaks.
  `forceMenuOption` is consumed/reset to −1 (`display.cpp:888`).
`check(volatile bool&)` (`include/globals.h:245`) returns true and **resets** the
flag — so an injected press is single-shot. The press globals and
`forceMenuOption`/`menuOptionType`/`menuOptionLabel` are defined at
`src/main.cpp:61-74`.

**`loader list` / `loader open <appname>`** (`loaderCallback`,
`util_commands.cpp:363`): `list` prints every main-menu item name plus
`BadUSB`/`WebUI`/`LittleFS` (`util_commands.cpp:371-376`); `open` finds a
main-menu item by case-insensitive name and calls its `optionsMenu()`
(`util_commands.cpp:384`), with extra shortcuts for `badusb`/`webui`/`littlefs`.
**Limitation:** it can only open a *top-level* menu item's submenu, not a leaf
tool, and it runs that submenu's blocking `loopOptions` inside the serial task
(channel goes deaf until exit). `close`/`info` are TODO
(`util_commands.cpp:416-417`). Prefer `options`/`nav` over `loader open` for
supervised control.

**`display start|stop|status|dump|info`** (`displayCallback`,
`util_commands.cpp:319`): `start`→`tft.startAsyncSerial()`, `dump`→hex-dumps the
TFT binary log to `serialDevice` (`util_commands.cpp:333-344`). This is the
generic "see what's on the screen" mirror over the link — useful to observe
tools that only render to the TFT.

### 1C. Startup-app registry — `src/core/startup_app.cpp` / `.h`

`StartupApp` holds a `std::map<String, std::function<void()>>` (`startup_app.h:30`).
Registered apps (constructor, `startup_app.cpp:27-53`):

| App name (key) | Wi-Fi? | Notes |
|---|---|---|
| `Brucegotchi` | yes | `brucegotchi_start()` |
| `Sniffer` | yes | `sniffer_setup()` |
| `Wardriving` / `WardrivingNoRadio` / `WardrivingBTEOnly` / `WardrivingWifiOnly` | yes (except NoRadio) | `Wardriving(...)` variants |
| `WebUI` | yes | `startWebUi(!wifiConnecttoKnownNet())` |
| `GPS Tracker`, `PN532 BLE`, `PN532 UART`, `Clock`, `Custom SubGHz`, `Mass Storage`, `JS Interpreter` | no | non-Wi-Fi |

- `startApp(name)` (`startup_app.cpp:55`) looks up and runs the lambda
  **blocking**, returns false on unknown name. `getAppNames()`
  (`startup_app.cpp:70`) returns the keys (alphabetical, it's a `std::map`).
- **Invocation:** `src/main.cpp:579` — at the end of `setup()`:
  `if (bruceConfig.startupApp != "" && !startupApp.startApp(bruceConfig.startupApp)) bruceConfig.setStartupApp("");`.
  So the stored `startupApp` string runs every boot and is only cleared **on
  failure** → it is **persistent (re-runs every boot)** until explicitly changed,
  not one-shot.
- **Boot ordering (BruceLink build)** `src/main.cpp`: `bruceLinkBootEarly()`
  (`:552`, pending job runs here, BLE off) → `startSerialCommandsHandlerTask`
  (`:573`) → `bruceLinkBoot()` (`:577`, BLE back on) → Bruce `startupApp.startApp`
  (`:579`). **Consequence:** Bruce's native `startupApp` fires *after* BLE is
  already up, so it is **not** a BLE-off execution slot — BruceLink's own
  `bruceLinkBootEarly`/`BruceLinkJobs.cpp` is. (This also explains the ADR-0006
  "never re-run after reset" rule: Bruce's `startupApp` would otherwise re-run
  forever; BruceLink's job must self-clear.)
- **Setting it is NOT a supported CLI op.** `setStartupApp(String)`
  (`src/core/config.cpp:756`, persists via `saveFile()`) is called only from the
  on-device Config menu (`src/core/settings.cpp:1298`, reached via
  `ConfigMenu.cpp:174`) or directly in C++. The `settings` CLI command has no
  `startupApp` setter (see §1D). To set it remotely through supported Bruce
  interfaces you must either drive the Config→"Startup App" menu via
  `nav`/`options`, or call `bruceConfig.setStartupApp()` from your own glue.

### 1D. Config — `src/core/config.*` (`bruceConfig`)

- `BruceConfig` (`config.h:24`) is the global `bruceConfig` (`main.cpp:28`),
  persisted to `/bruce.conf` (`config.h:46`) via `saveFile()`; loaded by
  `fromFile()` at boot (`main.cpp:193`, `begin_storage`). `toJson()`/`fromFile`
  round-trip the whitelist.
- Wi-Fi-relevant persistent fields (`config.h`): `wifiAtStartup` (`:57`),
  `startupApp` (`:94`), `wifi` creds map (`:75`) + `wifiAp` (`:74`) + `webUI`
  creds (`:72`), `evilWifiNames` (`:76`), all `evilPortal*` (`:81-83`),
  `disabledMenus` (`:106`, hides menu entries → changes what `optionsJSON`
  shows), `devMode` (`:98`).
- **CLI access** — `settings` command (`settingsCallback`,
  `src/core/serial_commands/settings_commands.cpp:4`; registered as `set/tings`
  i.e. `set` or `settings`, `:104`):
  - No args → pretty-prints the whole config JSON to USB `Serial` (`:19`).
  - `settings <name>` → prints that field's value (read) (`:55-58`).
  - `settings <name> <value>` → writes, but **only for the explicit whitelist**
    at `:62-90`: `priColor, rot, dimmerSet, bright, tmz, soundEnabled,
    wifiAtStartup, webUI, wifiAp, wifi, wigleBasicToken, wdgwarsApiKey, devMode,
    disabledMenus` (+ pin fields at `:29-43`). A name valid in `toJson` but
    absent from this list (notably **`startupApp`**, in `toJson` at
    `config.cpp:65`) passes validation and returns true **but no setter runs** —
    effectively read-only over CLI. `addWifiCredential`/`addDisabledMenu` are the
    Wi-Fi-useful writers here.
  - `factory_reset` (`:95`) resets config.

---

## 2. Wi-Fi capability → cleanest supported control path

Legend for (a) launch: **CLI** = dedicated serial command; **MENU** = reach via
`options`/`nav` on `loopOptions`; **STARTUP** = `bruceConfig.startupApp`.
(c) observe: **JSON** = `optionsJSON` while it uses `loopOptions`; **SCREEN** =
TFT only (mirror via `display dump`); **FILE** = result written to storage
(`storage read`/`cat`); **Serial** = prints to the stream. (d) stop: **Esc** =
inject `nav esc` → the tool's `check(EscPress)`.

| Capability | (a) Launch | (b) Configure | (c) Observe | (d) Stop |
|---|---|---|---|---|
| **AP/network scan** (standalone) | **None dedicated.** `WiFi.scanNetworks` only runs *inside* connect/attacks (`wifi_atks.cpp:273`, `netcut`, etc.). No scan CLI cmd, no scan menu item, no startup app. | — | SCREEN/JSON of the host attack's result list | Esc |
| **Pcap sniffer / handshake capture** | **CLI `sniffer`** (`wifi_commands.cpp:127`→`snifferCallback:80`→`sniffer_setup`); or MENU WiFi→"Sniffer" (`WifiMenu.cpp:80`); or **STARTUP `Sniffer`** | Full vs HandshakesOnly auto-picked (`sniffer.cpp:1168`); SD vs LittleFS auto. No param API. | FILE: pcap on SD/LittleFS; counts on SCREEN; "Sniffer started!" to USB Serial | Esc **(CLI launch blocks the serial task → needs physical Esc; MENU/STARTUP launch → `nav esc` works)** |
| **Handshake-focused (Brucegotchi/pwnagotchi)** | MENU WiFi→"Brucegotchi" (`WifiMenu.cpp:105`→`brucegotchi_start`, `pwnagotchi.cpp:320`); or **STARTUP `Brucegotchi`** | On-screen SelPress menu: Find friends / Pwngrid spam / toggle all-channels (`pwnagotchi.cpp:381-389`) | FILE `/BrucePCAP/handshakes`; `num_HS` on SCREEN | `nav esc` (`check(EscPress) || pwnagotchi_exit`, `pwnagotchi.cpp:378`) |
| **Deauth — flood** | MENU WiFi→"Wifi Atks"→"Deauth Flood" (`wifi_atks.cpp:262,336`) | On-screen AP pick after `scanNetworks` | SCREEN | `nav esc` / returnToMenu |
| **Deauth — enhanced/targeted** (`deauther.cpp`) | MENU WiFi→"Wifi Atks"→"Enhanced Deauth" (`wifi_atks.cpp:263`→`enhancedDeauthMenu`); "Target Atks" for per-AP (`wifi_atks.cpp:257,647`) | On-screen target/scan submenus (`deauther.h:25-33`) | SCREEN | `nav esc` |
| **Beacon spam** | MENU WiFi→"Wifi Atks"→"Beacon SPAM" (`wifi_atks.cpp:261,825`) | On-screen (list/single) | SCREEN | `nav esc` |
| **Evil Portal** | MENU WiFi→"Evil Portal" (`WifiMenu.cpp:68`→`EvilPortal()` ctor, `evil_portal.cpp:17`). **No CLI, no startup app.** | Ctor params exist (SSID, channel, deauth, verifyPwd, autoMode, backgroundMode, templateFile) but menu uses defaults → config via on-screen + persistent `evilPortal*` cfg (`config.h:81-83`) | FILE `*_creds.csv`; cred count on SCREEN; some `[PORTAL]` lines to USB Serial | `nav esc` → on-screen "Exit Portal/View Creds/Resume" (`evil_portal.cpp:326-345`) |
| **Karma attack** | MENU WiFi→"Wifi Atks"→"Karma Attack" (`wifi_atks.cpp:259`→`karma_setup`). No CLI/startup. | On-screen submenus (templates, broadcast config) | SCREEN; handshakes to FILE | `nav esc` / returnToMenu |
| **NetCut (ARP poison)** | MENU WiFi→"NetCut" (`WifiMenu.cpp:72`→`netcutMenu`). No CLI/startup. | Requires STA (prompts connect); on-screen device list + Cut/Troll/Resume All (`netcut.cpp:857-890`, all `loopOptions`) | JSON (device-list submenu is `loopOptions`); SCREEN | `nav esc` → returnToMenu |
| **Channel analyzer** | MENU WiFi→"Channel Analyzer" (`WifiMenu.cpp:81`→`channel_analyzer_setup`). No CLI/startup. | On-screen Up/Down = dwell ms | SCREEN (spectrum view) — no serial output | `nav esc` |
| **Jam/deauth detector** | MENU WiFi→"Jam Detect" (`WifiMenu.cpp:85`→`jam_detect_setup`) | On-screen Up/Down = threshold | SCREEN only | `nav esc` |
| **AP info / diagnostics** | MENU WiFi→"AP info" (only when STA connected, `WifiMenu.cpp:64-66`→`displayAPInfo`) | — (reads current STA assoc) | SCREEN (ScrollableTextArea); waits for SelPress | SelPress/Esc |
| **Scan hosts (ARP)** | **CLI `arp`** (`wifi_commands.cpp:121`→`scanHostsCallback:68`); or MENU WiFi→"Scan Hosts" (`WifiMenu.cpp:89`) | Requires STA connected | Serial (to USB `Serial`) + SCREEN | blocks serial task (ARPScanner); Esc |
| **Wardriving** | MENU **GPS**→Wardriving; or **STARTUP** `Wardriving*` variants | variant chosen by startup-app key or menu | FILE (wigle logs); SCREEN | `nav esc` |
| **WiFi pass recovery / WiFite / SDR / responder / TCP listen / client / SOCKS4 / Telnet / SSH / Wireguard** | MENU WiFi (`WifiMenu.cpp:75-109`); `listen` also has **CLI `listen`** (blocks serial task) | On-screen | SCREEN/Serial/FILE per tool | `nav esc` (MENU) / physical Esc (CLI `listen`) |
| **Connect to known/AP, turn off Wi-Fi** | **CLI `wifi on\|off\|add SSID PWD`** (`wifi_commands.cpp:13-53,114-117`); or MENU WiFi (`WifiMenu.cpp:54-63`) | `wifi add` persists creds; `wifiAp` cfg | Serial + `info` shows IP | `wifi off` |
| **WebUI** | **CLI `webui [noAp]`** (`wifi_commands.cpp:55,111`); MENU; **STARTUP `WebUI`** | `noAp` flag; `webUI` creds cfg | Serial ("Starting Web UI…", "Press ESC to quit") | physical Esc (CLI blocks serial task) |

**Trade-offs between paths**
- **CLI command (where it exists):** one-line, returns a `bool`, output on the
  stream — ideal for allowlisting. But for commands that launch a *blocking* app
  (`sniffer`, `webui`, `arp`, `listen`) the serial task is occupied for the app's
  whole life, so you can't send a stop over the same link (acknowledged TODO,
  `wifi_commands.cpp:124,127`). Good for fire-and-forget or launch-as-job.
- **MENU (`options`/`nav`/`optionsJSON`):** the only route to Deauth/Evil
  Portal/Karma/NetCut/Channel Analyzer/Jam Detect (no CLI exists). Keeps the
  command channel live (each call returns at once; the tool runs on the
  display/loop task), so `nav esc` can stop it and `optionsJSON` can read menu
  state. Requires walking the menu tree (main → "WiFi"/"Wifi Atks" → leaf) and
  tracking `active`/labels, which are build- and `disabledMenus`-dependent.
  Order/labels are **not a stable API**.
- **STARTUP:** survives a reboot and runs the app headless-ish at boot; but it's
  persistent (re-runs every boot until cleared), not CLI-settable, and runs after
  BLE is restored — so it is a Bruce feature, not a substitute for BruceLink's
  BLE-off job hook.

---

## 3. Radio exclusivity (live over BLE, or BLE-off restart job?)

**Every capability above starts the Wi-Fi radio** — verified by
`esp_wifi_init/esp_wifi_start`/`WiFi.mode`/`WiFi.scanNetworks` usage in each
module (sniffer `esp_wifi_set_mode(WIFI_MODE_AP)+promiscuous` `sniffer.cpp:1200-1204`;
channel analyzer/jam detect `esp_wifi_init/start` `channel_analyzer.cpp:38-56`,
`jam_detect.cpp:46-66`; karma `WiFi.mode(WIFI_MODE_APSTA)`; evil portal softAP
`evil_portal.cpp:30`; deauth/beacon via `wifi_atk_setWifi` + raw frames;
netcut/arp/listen require an active STA). Even the *passive* diagnostics need the
radio in promiscuous/STA mode — **there is no Wi-Fi capability that avoids the
radio.**

**Therefore, under the StickS3 one-radio rule (ADR 0006: Wi-Fi + BLE can't share
the ~133 kB internal RAM once the ~72 kB link is up), none can be driven live
while the BLE link is active.** All Wi-Fi capabilities are **radio-exclusive ⇒
must run as a "restart with BLE off" job** (`BL_CAP_F_JOB`): scan, sniffer/
handshake, deauth (all forms), beacon spam, evil portal, karma, brucegotchi,
netcut, channel analyzer, jam detect, AP info, scan hosts, wardriving, webui, and
the TCP/SSH/etc. tools.

**What *can* be driven live over BLE (radio stays off):** menu navigation and
observation (`optionsJSON`, `nav`, `options`, `loader list`, `display *`),
`settings` read/write, all `storage`/`ls`/`cat`/`md5`/`crc32`/`rename`/`copy`
file ops (read back pcaps, handshakes, creds), `info`/`free`/`uptime`/`date`/`i2c`,
GPIO/LED/sound/screen, and config writes. These are the live supervision +
result-retrieval surface while the link is up; the radio tool runs only during
the BLE-off job window.

**Practical shape of a supervised job:** (1) live over BLE, stage config
(`settings …`, `wifi add …`, or drive the Config menu for `startupApp`/evil cfg);
(2) hand off to a BLE-off job that launches the native function (CLI command
where one exists, else the `options`/`nav` menu walk performed by the on-device
loop, or `startApp`); (3) bound it with ADR-0006's duration cap + on-stick stop
(there is **no in-firmware Wi-Fi watchdog** — tools only exit on
`EscPress`/`returnToMenu`); (4) back live over BLE, read results via
`storage read` / `display dump`.

---

## 4. Capabilities with NO clean supported control path (physical-button-only)

No CLI command and no startup-app entry, so the only programmatic route is the
fragile `options`/`nav` menu walk; with no link driving the menu they are
reachable only by on-device buttons:

- **Evil Portal** — menu-only (`WifiMenu.cpp:68`); run-time controls (toggle
  deauth, Exit/View Creds/Resume) are an on-screen `loopOptions`
  (`evil_portal.cpp:321-345`).
- **Karma attack** — menu-only (`wifi_atks.cpp:259`); deep on-screen submenus.
- **NetCut** — menu-only (`WifiMenu.cpp:72`); device-list actions on-screen.
- **Deauth (flood / enhanced / target) and Beacon spam** — only inside the "Wifi
  Atks" submenu (`wifi_atks.cpp:248-264`); no CLI, no startup app.
- **Channel Analyzer / Jam Detect** — menu-only (`WifiMenu.cpp:81,85`); the only
  config (dwell / threshold) is on-screen Up/Down with **no serial or JSON
  equivalent**, and status is **TFT-only** — observation requires `display dump`,
  not a structured read.
- **AP info** — entry appears **only when a STA connection is already up**; output
  TFT-only, blocks on SelPress.
- **Standalone AP/network scan** — does not exist as a user-facing function;
  scanning is always a side-effect of connect/attack flows.

Two "half-supported" cases: the CLI `sniffer`/`webui`/`arp`/`listen` commands
*launch* fine over the link but **cannot be stopped over the same link** (serial
task blocked; stop is physical-Esc-only — explicit TODOs at
`wifi_commands.cpp:124,127`); and **setting the startup app** has no CLI path
(`settings startupApp X` silently no-ops), reachable only via the on-device
Config menu or a direct `bruceConfig.setStartupApp()` call.

---

### Key Bruce source files (relative to the Bruce tree)
- `src/core/serialcmds.cpp` (+`.h`) — parser task / `parseSerialCommand`
- `src/core/serial_commands/cli.cpp` (+`.h`) — `SerialCli` / SimpleCLI registration
- `src/core/serial_commands/util_commands.cpp` — `nav`/`options`/`optionsJSON`/`loader`/`display`
- `src/core/serial_commands/wifi_commands.cpp`, `settings_commands.cpp`, `storage_commands.cpp`
- `src/modules/ble_api/ble_api.cpp` and `…/services/BLESerialService.cpp` — the BLE-over-CLI model
- `src/core/utils.cpp` — `getOptionsJSON` (`:276`), `backToMenu` (`:26`)
- `src/core/display.cpp` — `loopOptions` (`:545`, selection `:884-895`)
- `src/core/startup_app.cpp` (+`.h`) — app registry; invoked at `src/main.cpp:579`
- `src/core/config.cpp` (+`config.h`) — `bruceConfig`, `setStartupApp` (`config.cpp:756`)
- `src/core/menu_items/WifiMenu.cpp` — Wi-Fi menu tree; `include/globals.h:245` — `check()`

Note: an unrelated copy of similar sources exists at
`/home/claude/brucedevices/firmware/`; all findings above are from the
task-specified `bruce-build` tree only.
