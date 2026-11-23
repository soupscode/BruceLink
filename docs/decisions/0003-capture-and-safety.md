# ADR 0003 — Capture subsystem, command adapter, and remote-operation safety

Status: Accepted (design + host-verified core) — hardware-unverified
Date: 2026-11-23
Pinned source: Bruce `1.17` (`2b6ba4d`), board `m5stack-sticks3`.

## Context

The scope amendment adds remote packet capture, a Bruce capability registry, a
generalized command adapter, and (for authorized labs) frame-transmission tests.
The project owner's follow-up clarified the intent: **BruceLink is a remote
frontend to Bruce's existing functions, not a reimplementation**, and
authorization is at the connection level, not per function. We audited Bruce
`1.17` on the StickS3 before designing.

## Findings (verified in Bruce source)

- **Sniffer exists** (`src/modules/wifi/sniffer.cpp`): `esp_wifi_set_promiscuous`
  + `esp_wifi_set_promiscuous_rx_cb(sniffer)`, PSRAM ring buffer → FreeRTOS queue
  → writer task → pcap on SD/LittleFS. EAPOL 4-way detection + per-AP handshake
  pcap (`isItEAPOL`, `saveHandshake`). **No PMKID.**
- **PCAP format:** global header `magic a1b2c3d4`, snaplen 2500, **linktype 105
  (IEEE 802.11, no radiotap)** (`writeHeader`). BruceLink matches this byte for
  byte (`bl_pcap`).
- **StickS3 storage:** 3 MB LittleFS, **no built-in SD**. Full raw-pcap mode is
  SD-gated (`sniffer_full_mode_available` → `sdDetected`); on a bare StickS3 only
  **HandshakesOnly** and **DeauthOnly** modes run. → raw capture is marked
  `REQUIRES_EXT_HW`.
- **No headless session manager:** `sniffer_setup()`/`capture_handshake()` are
  blocking UI loops owning radio + file + TFT + buttons. A remote driver must
  start/stop the radio + file directly; `closeRawFile()` etc. are `static`.
- **TX is ungated:** `send_raw_frame` → `wifiRawTx` → `esp_wifi_80211_tx`;
  `deauthFloodAttack` sends 300 frames/AP with **no rate limit and no
  confirmation** — the only stop today is the device's Esc key/button.
- **No binary pcap-over-serial export** (only text `cat`; YMODEM is
  receive-only). WebUI `/file` download exists but **cannot run while sniffing**.
- **Coexistence:** no `esp_coex*` anywhere; only a DMA-memory guard
  (`radioHasMemForBle`). BLE + Wi-Fi-promiscuous concurrency is unmanaged.

## Decisions

### 1. Generalized command adapter (one bridge, not one transport per tool)

A single `BL_MSG_CMD_REQ`/`CMD_RESP` pair plus a **capability registry**
(`bl_capability`). `CMD_REQ` payload = `[u16 cap_id][args]`. The bridge maps
`cap_id` → a `BruceBackend` method; new Bruce features are added by registering
a capability + a backend method, no protocol change. The Flipper renders its
menus from `BL_MSG_CAPS`, so it never offers a function the connected build
lacks.

### 2. Capture session manager (`bl_capture`)

A hardware-independent state machine (`IDLE→RUNNING→STOPPING`) over a backend
vtable the Bruce glue supplies (`start`/`set_channel`/`poll`/`stop`/
`mode_available`). It validates channel (1–14), refuses a mode the hardware
can't run (raw without SD → `BL_E_DENIED`), tracks counts, and surfaces a
backend error (e.g. LittleFS full) as `ERROR`. The Bruce glue implements the
vtable by driving the existing sniffer backend (`ensureSnifferBackend`,
`sniffer_prepare_storage`, `sniffer_set_mode`, the `esp_wifi_*` calls) — i.e.
**reuses** Bruce's capture code behind a headless driver (the one piece Bruce
lacks).

### 3. PCAP export (`bl_pcap`) + integrity + resumable transfer

Standards-compliant pcap writing identical to Bruce's. File integrity via
CRC-32 (`bl_crc32`), carried in `CAP_FILE_INFO`. Resumable transfer via
`CAP_XFER_BEGIN/DATA/END` with an offset, so a dropped link resumes mid-file.
**Bulk transfer does not use the live BLE control link** during capture (BLE +
promiscuous coexistence is unmanaged upstream and throughput is low): the
operator stops capture, then pulls files over BLE, or — future option — a
dedicated Wi-Fi transfer channel. Marked for hardware evaluation.

### 4. Remote-operation safety (`bl_safety`) — connection-level, not per-function

Authorization is the authenticated session (ADR 0002). On top, the gate adds
only what **remote** operation makes necessary, because it removes Bruce's sole
physical control (the button):

- a single session-level **active-ops enable** (default **off**), set by an
  operator command over the authenticated link;
- an **emergency stop** that latches closed and is engaged automatically on
  **link loss** (`bl_safety_on_link_loss`) — the fail-safe Bruce lacks remotely;
- a **transmission-rate limiter that bounds actual frames**: the bridge reserves
  `min(16, frames left in the window)`, passes that number to Bruce as a hard
  cap, and refunds what was not sent; a backend that exceeds its cap trips the
  e-stop (revised after PR #1 review — the first version charged one frame per
  op while an op could send many). The window is the device's **transmit
  history**: only elapsed time expires it. Enabling or disabling active ops,
  resetting the e-stop and starting a new session never clear it (second
  review round: re-sending "enable" before each op, or reconnecting, refilled
  the budget — 320 frames against a 64-frame limit);
- an **optional** operator target scope (BSSID allowlist) to confine a test —
  a convenience, not a credential. Active ops need a **specific unicast**
  target: broadcast and multicast BSSIDs are refused (the first version only
  rejected a NULL pointer, which the wire format never produces).

E-stop latches for the session and blocks all radio use, including capture
start; a new authenticated session resets the gate with active ops disabled.
A capture whose backend fails is stopped and flushed at once (exactly-once
stop), never left with the radio in promiscuous mode or the file open.

No per-function token. Active capabilities carry `REQUIRES_AUTH` purely to route
them through this gate. The gate decides policy; it never emits RF — the actual
transmission stays in Bruce (`send_raw_frame`), unchanged.

## Consequences / what needs a Bruce patch

Reusable as-is: the sniffer backend, EAPOL logic, pcap writer, `wifiRawTx`, file
helpers, WebUI download. **Patch needed:** expose a headless
`sniffer_start/stop/status` (today static + UI-bound); add a status struct
getter; optionally a binary pcap-over-serial/BLE export; and the gate call sites
around `send_raw_frame`/`sendDeauthToAP`. These are additive and documented in
`docs/HARDWARE_TESTING.md` and the glue skeleton
(`esp32/src/modules/brucelink/`).

## Safety posture

BruceLink does not add any new offensive capability: the transmit primitives
already exist in Bruce and the operator already runs them from the device. The
remote frontend *adds* controls that the local UI lacks (authenticated link,
default-off active ops, link-loss auto-stop, rate limit, optional target scope).
Active modes are advertised `REQUIRES_AUTH` + `HW_UNVERIFIED` and are to be used
only in authorized, isolated lab environments.

## Amendment — demo key (2026-11-23)

Connection-level authorization only means something with a private key. Until
key provisioning exists (M5.3) both builds compile in a published demo key, so
a bridge that holds it treats the session as unauthenticated for anything that
transmits: "enable active ops" and every `BL_CAP_F_ACTIVE_RF` capability
return `not_authorized`, and `STATUS` reports `demo:1`. Passive requests are
still served so the link can be brought up. Host-tested
(`t_demo_key_never_enables_active_ops`).

Update (2026-11-23, ADR 0005): real devices now use a key from pairing, so the
demo-key lock no longer applies to them; it still applies to any bridge given
the demo key (the compile check, a misconfigured build). Being paired lifts only
that lock: the session-level enable, e-stop, rate limit and unicast-only target
rules apply in full, and in the current Bruce build the transmit hook still
reports "not available" (nothing is sent).
