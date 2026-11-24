# BruceLink architecture

BruceLink is a native Flipper Zero (Momentum) remote frontend for an
M5StickS3 (ESP32-S3) running Bruce firmware, over an authenticated BLE link.
It reuses Bruce's existing functions; it does not reimplement them.

```
┌─────────────────────────┐        BLE (custom 128-bit GATT)       ┌──────────────────────────┐
│ Flipper Zero (Momentum) │                                        │ M5StickS3 (ESP32-S3)      │
│                         │  RX char: central→peripheral (write)   │  Bruce 1.17 + BruceLink    │
│  BruceLink .fap         │ ───────────────────────────────────▶  │  bridge                    │
│   ┌───────────────────┐ │  TX char: peripheral→central (notify)  │ ┌────────────────────────┐ │
│   │ ViewDispatcher UI │ │ ◀───────────────────────────────────  │ │ NimBleCentralTransport │ │
│   │ menu/info/scan/.. │ │                                        │ └──────────┬─────────────┘ │
│   ├───────────────────┤ │                                        │ ┌──────────▼─────────────┐ │
│   │ BlTransport       │ │   Flipper = BLE peripheral             │ │ BruceLinkBridge (core) │ │
│   │  BLE              │ │   StickS3 = BLE central                │ │  session + safety +    │ │
│   ├───────────────────┤ │                                        │ │  capture + dispatch    │ │
│   │ shared core (C99) │ │   Flipper = session initiator          │ ├──────────┬─────────────┤ │
│   │ session/frame/..  │ │   StickS3 = session responder          │ │ shared core (C99)      │ │
│   └───────────────────┘ │                                        │ ├──────────▼─────────────┤ │
└─────────────────────────┘                                        │ │ BruceBackend (glue)    │ │
                                                                    │ │  → Bruce sniffer/scan/ │ │
                                                                    │ │    TX via existing     │ │
                                                                    │ │    Bruce functions     │ │
                                                                    │ └────────────────────────┘ │
                                                                    └──────────────────────────┘
```

## The shared protocol core (`shared/brucelink_core`, C99)

The heart of the system, identical on both devices and fully host-tested:

- `bl_crypto` — SHA-256 / HMAC / HKDF / X25519 (RFC vectors verified).
- `bl_frame` — frame codec + truncated-HMAC MAC.
- `bl_session` — mutual-auth handshake (PSK + ephemeral ECDH), per-direction
  keys, strictly-increasing replay counter.
- `bl_reasm` / `bl_cobs` — fragmentation for BLE / COBS for serial.
- `bl_capability` — the capability registry + CAPS serialization.
- `bl_capture` — hardware-independent capture session state machine.
- `bl_safety` — the remote-operation gate (connection-level).
- `bl_pcap` — standards-compliant pcap headers + CRC-32.

It is pure, allocation-free C99 with no platform dependencies, so the Flipper
FAP (ARM) and the ESP32 bridge (Xtensa) run byte-identical protocol code, and
the host builds it for tests.

## Flipper application (`flipper/brucelink`)

A native ViewDispatcher app: a main menu, a scrollable text view (device info /
capabilities / results), and a settings list. Protocol traffic runs through a
`BlTransport` abstraction; the one transport is **BLE** (a FAP-owned GATT
peripheral profile, ADR 0001; run on hardware). Until v0.2 there was also a
loopback transport, an in-process mock StickS3 for running the app without
hardware; it was removed once the real link worked (git history: `5fdc650`).
Connect needs a pairing (ADR 0005). The 50 ms ViewDispatcher
tick polls the transport on the app thread and feeds frames to `bl_client`, the
shared-core state machine that owns the handshake, the one outstanding request
and their timeouts. A transport reports a link id that changes with every new
link; the app starts a fresh handshake on each change. The UI shows live
connection status and recovers from timeouts and link loss.

## StickS3 bridge (`esp32/lib/BruceLink` + `esp32/src/modules/brucelink`)

- `BruceLinkBridge` (portable C++, depends only on the shared core) holds the
  responder session, the safety gate, the capture manager and reassembly. It
  turns GATT chunks into frames, dispatches to a `BruceBackend`, and emits
  response chunks. Compiled and linked for the ESP32-S3.
- `NimBleCentralTransport` (NimBLE-Arduino 2.5.1) scans, connects, discovers the
  RX/TX characteristics and bridges notify↔write.
- `BruceBackendImpl` + `bruce_brucelink_hooks` (Bruce tree) call Bruce's own
  functions: device info, Wi-Fi scan, the sniffer backend (via a thin headless
  patch), and the existing transmit primitive for active ops.

## Boundaries

- **UI ↔ transport:** the app never calls BLE directly; it speaks frames.
- **Bridge ↔ Bruce:** everything Bruce-specific is behind `BruceBackend`; the
  bridge core has no Bruce or NimBLE dependency and is unit-tested off-device.
- **Protocol ↔ transport:** `bl_session` emits/consumes frame buffers;
  fragmentation and the physical link are separate concerns.

## Security model (summary; ADR 0002/0003)

Connection-level mutual authentication (PSK + ephemeral ECDH) with forward
secrecy and replay protection. No per-function tokens. Active (RF-transmitting)
capabilities are default-off and, once enabled over the authenticated link, pass
a remote-operation gate: one specific target (never broadcast), emergency
stop, automatic stop on link loss, and a transmission-rate limiter (the core
also has an optional target scope that nothing on the wire sets yet). On the
StickS3 they are refused until they run as jobs (ADR 0006). BruceLink adds no
transmit code of its own.

## What is and isn't verified

Host-verified: the entire protocol core (1434 assertions, ASan+UBSan), C↔Python
wire interop, and that the FAP (ARM) and the bridge + NimBLE (Xtensa) compile and
link. **Unverified:** anything requiring the radios or real Bruce — BLE
pairing/MTU/throughput, Bruce patch integration, on-air capture and TX. See
`docs/CAPABILITY_MATRIX.md` and `LOCAL_HANDOFF.md`.
