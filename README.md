# BruceLink

Native Flipper Zero (Momentum) remote for a backpack-mounted M5StickS3 running Bruce firmware.

**Status: experimental scaffold.** BLE and Bruce integration are not yet implemented or hardware-tested.

## Project layout
- `flipper/brucelink/`: native Flipper external app UI placeholder
- `esp32/`: ESP32-S3 command-dispatch prototype, not yet integrated with Bruce
- `shared/`: experimental protocol and Python reference codec
- `tests/`: host-side protocol unit tests
- `docs/`: architecture, roadmap, status and hardware-in-the-loop plan
- `CLAUDE.md`: instructions for Claude Code

## First command

```sh
python3 -m unittest discover -s tests -v
```

## Development goals
Flipper-native controls and scan results, authenticated BLE commands, StickS3 performing passive Wi-Fi/BLE discovery, and USB-driven HIL tests. Start with a serial ping/status prototype; verify Momentum and Bruce APIs before integration.

Upstream: https://github.com/Next-Flip/Momentum-Firmware and https://github.com/BruceDevices/firmware. BruceLink is independent of these projects.
