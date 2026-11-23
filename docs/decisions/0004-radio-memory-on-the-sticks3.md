# ADR 0004 — Wi-Fi next to the BLE link on the StickS3

Status: Accepted (2026-11-23) — implemented; the scan path **returned networks
over the air** on the owner's devices (one run). Numbers marked *measured* come
from the owner's device.

## Context

BruceLink drives Wi-Fi functions over a BLE link, so on the StickS3 both radio
stacks must be up at once. Both allocate from **internal RAM**; the board's
8 MB of PSRAM does not help (this Bruce build's framework allocates NimBLE and
Wi-Fi internally, and Bruce ships the framework libraries prebuilt, so that is
not ours to reconfigure).

Measured on the StickS3, Bruce 1.17 + BruceLink:

| Point | Internal RAM free | Largest DMA block |
|---|---|---|
| Main menu, before BLE | 133,247 | 59,380 |
| BLE up, scanning for the Flipper | 75,707 | 31,732 |
| Flipper linked (after memory savings) | 61,675 | 31,732 |
| Flipper linked (before the savings) | 47,979 | — |

First attempt (a full Arduino `WiFi.mode(STA)` + scan with 48 kB free):
`esp_wifi_init` failed with `ESP_ERR_NO_MEM` while allocating its RX buffers,
could not be deinitialised, and left the BLE stack unable to send. The bridge
was dead until reboot. Bruce's own guard (`radioHasMemForWifi`, a 15 kB block)
passed, so it does not protect this case. Bruce itself never runs both radios
together: its BLE features call `wifiDisconnect()` first.

## Decision

1. **Never start Wi-Fi unless it should fit** (`wifiStartAllowed`, host-tested):
   at least 52 kB internal free and a 16 kB DMA block, unless Wi-Fi is already
   up. Otherwise the request is refused with the figures and the link stays up.
   52 kB sits above the measured failure point and below what is available; it
   is an estimate until the cost of the start below is measured.
2. **Use Wi-Fi transiently and small** (`BruceLinkWifi.cpp`). When the driver is
   not initialised, BruceLink initialises it directly with a reduced buffer
   configuration (2 static RX, 2 static TX, 8 management buffers, no A-MPDU, no
   NVS), runs the operation, then stops and deinitialises it. A scan needs no
   association and no data path. Arduino's Wi-Fi layer and Bruce's Wi-Fi state
   are left exactly as found.
3. **If Bruce already has Wi-Fi up**, scan through the Arduino API as Bruce's
   own scan does and start nothing. **If another Bruce tool holds the driver**,
   refuse.
4. **Spend less internal RAM on BLE.** The scan for the Flipper no longer
   stores every advertiser seen (NimBLE keeps them all, unbounded, by default)
   and the chunk queue lives in PSRAM. Together about 14 kB (*measured*).
5. Every step logs internal RAM, the largest DMA block and the bridge task's
   stack headroom on USB serial, so the next change is made from data.

## Measured result (ee58380)

| Step | Internal free | Largest DMA block |
|---|---|---|
| Before the scan (Flipper linked) | 61,639 | 31,732 |
| Reduced driver initialised | 30,651 | 22,516 |
| Radio started | 28,911 | 20,468 |
| Scan finished (lowest point) | 25,911 | 16,372 |
| Driver released | ~55,000 | 31,732 |

Peak cost 35,728 bytes, scan 4.8 s with BLE active. The link stayed up through
the scan (it went down 9 s later when the owner left the Flipper app). The 52 kB threshold leaves
about 16 kB above that cost, and the host test now pins the threshold to the
measured cost plus at least 12 kB. About 6.5 kB was not returned after release,
which matches the default event loop created on first use; a second scan will
show whether anything else leaks.

## Consequences

- This is a departure from "call exactly what Bruce calls" for the scan, taken
  because Bruce's call cannot work in this situation. It builds no frames and
  adds no capability; it is the same scan with a smaller driver.
- Each scan pays a driver start and stop: 8 ms measured; the scan itself takes
  4.8 s because the radio is shared with BLE.
- Capture and active operations (M5.2) need the same treatment: they must
  bring the driver up through this path, not through Bruce's own full start.
- **Open:** after Bruce's own Wi-Fi has been used, the largest block stays
  around 32 kB and Bruce's BLE guard (48 kB) refuses to start BLE again, so
  BruceLink cannot be re-enabled until reboot (*measured*). Whether BLE
  deinitialisation itself leaves the heap fragmented is not yet known; the
  module now logs memory when it stops.
- **Open:** the bridge task's 12 kB stack has shown about 8 kB of headroom so
  far; it can probably shrink once the scan path has run.
- Hardware verification: `docs/HARDWARE_TESTING.md` step 5.
