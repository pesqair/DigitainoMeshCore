# Digitaino MeshCore Firmware

Custom firmware for the **Wio Tracker L1 Pro** based on [MeshCore](https://github.com/ripplebiz/MeshCore), adding on-device UI features for standalone messaging without needing the companion phone app.

## About This Fork

This is a customized version of MeshCore firmware specifically tailored for personal use on the Wio Tracker L1 Pro. It extends the companion radio example with an enhanced on-device interface.

**Upstream Project:** [MeshCore by ripplebiz](https://github.com/ripplebiz/MeshCore)

## Features Added

- **QWERTY keyboard layout** for text composition
- **Message history** with scrollable list showing sent/received messages
- **Message detail view** showing hop count and reply options
- **Contact selector** for direct messaging (DMs)
- **Channel selector** for multi-channel messaging
- **GPS DM** feature to send location coordinates to specific contacts
- **Runtime preset configuration** via `/presets.txt` file
- **Two-line compose area** with visible prefix and message text
- **Keyboard improvements**: lowercase default, SHIFT toggle, ESC key
- **Non-interrupting notifications** - incoming messages don't disrupt compose/typing
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

- `examples/companion_radio/ui-new/` - Enhanced UI implementation
- `examples/companion_radio/MyMesh.cpp` - Mesh networking + companion app sync
- `examples/companion_radio/AbstractUITask.h` - UI interface definition

## Configuration

Create `/presets.txt` on the device's internal filesystem (one message per line) to customize quick message presets.

## Companion App

This firmware is compatible with the official MeshCore companion apps for channel messages. Direct messages (DMs) sent from the device currently don't sync to the companion app due to protocol limitations.

## License

Inherits MIT License from upstream MeshCore project.

## Credits

Built on [MeshCore](https://github.com/ripplebiz/MeshCore) by ripplebiz and contributors.

UI enhancements developed with assistance from Claude (Anthropic).
