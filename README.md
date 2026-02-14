# Digitaino MeshCore Firmware

Custom firmware for the **Wio Tracker L1 Pro** based on [MeshCore](https://github.com/ripplebiz/MeshCore), adding on-device UI features for standalone messaging without needing the companion phone app.

## About This Fork

This is a customized version of MeshCore firmware specifically tailored for personal use on the Wio Tracker L1 Pro. It extends the companion radio example with an enhanced on-device interface.

**Upstream Project:** [MeshCore by ripplebiz](https://github.com/ripplebiz/MeshCore)

## Features Added

### Messaging
- **QWERTY keyboard** with lowercase default, SHIFT toggle, and ESC key
- **Two-line compose area** with visible prefix and message text
- **Message history** with scrollable list showing sent/received messages
- **Message list prefixes**: sent messages show `(N)` repeat count, incoming multi-hop messages show `<AB>` last-repeater hash
- **Prefix stays fixed** while message text scrolls horizontally
- **Message detail view** with timestamp, hop count, signal info, and path display
- **Reply options**: reply on channel (with @mention), reply via DM, or reply to DM sender
- **Message resend**: sent messages with no heard repeats show "Resend" option; resending appends TX count and re-tracks repeats
- **Non-interrupting notifications** — incoming messages don't disrupt compose/typing

### Contacts
- **Contacts page** with list of all known contacts sorted by most recently heard
- **Last heard time** displayed for each contact
- **Contact detail view** with action menu: Send DM, Request Telemetry, Request Status
- **Cached contact info**: RSSI, SNR, and repeater hop hashes shown in contact list
- **Telemetry display**: voltage and temperature from remote nodes
- **Status display**: uptime and battery millivolts from remote nodes

### Radio & Signal
- **Sent message repeat tracking**: counts how many times your packet was heard retransmitted
- **Unique repeater accumulation**: "Heard by" shows all unique repeater hashes across all bounces (not just the last path)
- **Signal info**: RSSI and SNR displayed for received messages and heard repeats
- **Path display**: full repeater chain shown for incoming multi-hop messages

### Navigation
- **Navigation page** with compass heading, GPS speed, odometer, and current coordinates
- **GPS speed** (mph) displayed on home screen
- **Screen lock** to prevent accidental input during navigation

### Other
- **Channel selector** for multi-channel messaging
- **Contact selector** for direct messaging (DMs)
- **GPS DM** feature to send location coordinates to specific contacts
- **Runtime preset configuration** via `/presets.txt` file
- **Battery voltage toggle** on home screen
- **Companion app sync** — bidirectional sync with the companion app (see details below)
- Custom splash screen branding

## Building

Requires [PlatformIO](https://platformio.org/):

```bash
pio run -e WioTrackerL1_companion_radio_ble
```

Flash output located at: `.pio/build/WioTrackerL1_companion_radio_ble/firmware.zip`

## Hardware

- **Target Device**: Seeed Studio Wio Tracker L1 Pro (nRF52840 + SX1262)
- **Display**: 64x128 OLED
- **Input**: Joystick navigation

## Project Structure

- `examples/companion_radio/ui-new/` — Enhanced UI implementation
- `examples/companion_radio/MyMesh.cpp` — Mesh networking + companion app sync
- `examples/companion_radio/AbstractUITask.h` — UI interface definition

## Configuration

Create `/presets.txt` on the device's internal filesystem (one message per line) to customize quick message presets.

## Companion App Sync

This firmware is compatible with the official MeshCore companion apps (iOS/Android). The device UI and companion app share a single radio, so messages flow between them:

**App → Device UI:**
- Channel messages sent from the app appear in the device's message log (with repeat tracking)
- DMs sent from the app appear in the device's message log
- All received messages (channel and DM) appear on both simultaneously

**Device UI → App:**
- Channel messages sent from the device are queued to the app's offline queue, so the app sees them too
- DMs sent from the device do **not** sync to the app — the protocol has no "outgoing" flag, so sent DMs would incorrectly appear as incoming in the companion app

## License

Inherits MIT License from upstream MeshCore project.

## Credits

Built on [MeshCore](https://github.com/ripplebiz/MeshCore) by ripplebiz and contributors.

UI enhancements developed with assistance from Claude (Anthropic).
