# Project Overview

> Why BruceLink exists, how its goal evolved, and how the pieces relate. For the
> exact current state see `CURRENT_STATE.md`; for the decisions behind the shape
> see `DECISION_HISTORY.md`.

## What it is

BruceLink is a **native remote frontend and management bridge**: a Flipper Zero
(running Momentum firmware) drives a backpack **M5StickS3 running Bruce**
firmware over an **authenticated BLE link**. The Flipper is the control surface;
the StickS3 carries the radios and runs Bruce; Bruce provides the actual wireless
functionality. BruceLink is the glue that lets the Flipper discover, configure,
launch, supervise, and retrieve results from Bruce's tools — safely and over an
authenticated link.

## Original objective

Give the Flipper a first-class remote for Bruce's existing wireless
pentest/recon tooling **by reusing Bruce's functions**, not by reimplementing
them. The link had to be authenticated and safe (no unauthenticated control, no
unrestricted command execution), and the project had to avoid maintaining a full
Bruce fork.

## How the objective evolved

- **Transport simplified to BLE-only.** An early in-app loopback mock and a
  serial/USB path were dropped once the BLE profile worked; BLE is the only
  transport (the mock lives in git history).
- **Placement moved to the StickS3 home.** The on-device entry moved from
  Config → Advanced to a first-class "BruceLink" item on Bruce's main menu.
- **The radio reality forced "one radio at a time".** On the StickS3, Wi-Fi and
  BLE cannot share the ~133 kB of internal RAM once the ~72 kB BLE link is up
  (seen on hardware as `esp_wifi_init` OOM). Rather than fight coexistence,
  BruceLink runs Wi-Fi tools as **jobs**: the stick restarts with BLE off, runs
  the tool, restarts, and brings BruceLink back; the Flipper fetches the result
  (ADR 0006).
- **Capture and file retrieval were added.** Passive handshake capture as a job,
  plus resumable, CRC-checked capture-file transfer to the Flipper.
- **The scope was sharpened to "bridge, not framework" (2026-11-24).** The owner
  clarified that BruceLink is a communication/management bridge for Bruce's
  *existing* capabilities — not an independent wireless-attack framework and not
  a reimplementation. Native feature parity should come from *exposing* Bruce's
  tools through supported interfaces; where a tool cannot be exposed cleanly,
  that is documented rather than rebuilt. This is being written up as ADR 0007
  and converges the per-tool job code toward one generic supervised
  native-function interface (see `ARCHITECTURE.md`).

## How the pieces relate

| Piece | Role |
|---|---|
| **Flipper Zero (Momentum)** | The remote/UI. Runs the BruceLink FAP, initiates the authenticated session, and is where the operator drives everything. BLE peripheral. |
| **M5StickS3** | The field device. Runs Bruce firmware + the BruceLink bridge. BLE central + session responder. Carries the single 2.4 GHz radio and the storage that holds captures. |
| **Bruce firmware** | The capability provider. Owns all wireless tools (scan, sniffer, handshake capture, deauth, evil portal, karma, brucegotchi, diagnostics, …). BruceLink calls into it; it is patched only by the installer, never forked. |
| **BruceLink** | The integration layer: authenticated BLE link, pairing, discovery, job lifecycle, file transfer, safety gate, e-stop. |

## Non-goals (explicit)

- Not an independent attack framework; BruceLink builds no RF frames.
- Not a Bruce fork; integration is via `tools/install_into_bruce.py` patches.
- Not a reimplementation of Bruce's tools; no inventing replacement tools to fill
  a capability gap.
