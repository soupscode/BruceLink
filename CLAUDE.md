# BruceLink

Native Momentum Flipper Zero remote for M5StickS3 running Bruce.

## Status
Initial scaffold only. BLE integration and hardware builds are unverified.

## Architecture
Flipper FAP UI -> authenticated versioned BLE transport -> StickS3 bridge -> Bruce interfaces. Verify BLE role compatibility and upstream APIs before implementation.

## Milestones
1. Pin Momentum and Bruce revisions; verify StickS3 board configuration.
2. Build Flipper UI and ESP32-S3 bridge separately.
3. Implement serial ping/status with tests.
4. Implement authenticated BLE connection and reconnection.
5. Add passive Wi-Fi/BLE scans and native result display.
6. Add USB hardware-in-the-loop tests and CI.

## Rules
Do not claim untested functionality works. Preserve Bruce upstream behavior using minimal patches. Bound command sizes, authenticate remote actions, and limit active radio tests to authorized environments. Document exact hardware and firmware versions.
