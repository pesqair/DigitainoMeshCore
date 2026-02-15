# Digitaino MeshCore Firmware

Custom firmware for the **Wio Tracker L1 Pro** based on [MeshCore](https://github.com/ripplebiz/MeshCore), adding on-device UI features for standalone messaging without needing the companion phone app.

## About This Fork

This is a customized version of MeshCore firmware specifically tailored for personal use on the Wio Tracker L1 Pro. It extends the companion radio example with an enhanced on-device interface.

**Upstream Project:** [MeshCore by ripplebiz](https://github.com/ripplebiz/MeshCore)

## UI Pages

The home screen is a carousel of pages navigated with LEFT/RIGHT. Press ENTER to activate a page, CANCEL to return to the carousel.

### Home

Message count, date/time, and connection status. Shows BLE pairing PIN when waiting for pairing.

### Messages

Scrollable message log with channel filtering (LEFT/RIGHT to cycle filters). Messages show delivery status, repeat counts, and last-hop hash prefixes. Prefix stays fixed while message text scrolls horizontally on selection.

**Message Detail** (ENTER on a message):
- Full word-wrapped text, timestamp, sender/recipient, hop count
- Signal info (RSSI and SNR) for received messages
- Path chain with per-repeater selection (LEFT/RIGHT to highlight individual hops)
- Sent message tracking: repeat count, heard-by repeater list with per-repeater signal
- DM delivery receipts (Delivered / Pending)
- Reply options: Reply on channel, Reply DM, or Resend

### Quick Msg

Preset quick messages loaded from `/presets.txt` on the device filesystem (one per line). Includes options to compose custom messages, reply DM, send GPS coordinates, and send to channels. Presets can be added, edited, and deleted from the menu.

### Recent

List of recently heard nodes with names and time since last heard.

### Contacts

Contact list sorted by most recently heard, with favorites shown first (marked with `*`). Filter by type using LEFT/RIGHT: All, Contacts, or Repeaters.

Each contact shows hash prefix (`<XX>`), name, type suffix (`[D]`irect, `[N]` hops, `[R:D]` repeater direct, `[R:N]` repeater hops, `[Rm]` room, `[S]` sensor), and last-heard age.

**Contact Actions** (ENTER on a contact):
- **Chat nodes**: Send DM, Find Path, Telemetry, Send GPS, Request Location, Navigate
- **Repeaters/Sensors**: Find Path, Telemetry, Status, Navigate (if GPS available)
- Cached info displayed below actions: last heard, path/signal, battery, temperature, uptime

All async operations (path discovery, telemetry, status, location requests) show a waiting state with timeout.

### Nearby

Active discovery of nearby repeaters and sensors. Press ENTER to scan; results appear within 8 seconds. Shows discovered nodes with hash prefix, name (or `<XX> ???` if unknown), and SNR.

ENTER on a result opens the action menu (same as Contacts) if the node is in the contact database. Press ENTER again to rescan at any time.

### Radio

Displays current radio configuration: frequency, spreading factor, bandwidth, coding rate, TX power, and noise floor.

### Packets

Raw packet log showing packet type, first hop, RSSI, SNR, and age. Press ENTER for detail view with full type name, route type, signal info, path chain, and payload size. UP/DOWN to scroll through packets and detail items.

### Advert

Press ENTER to broadcast a mesh advertisement.

### GPS

GPS status display: on/off state, fix status, satellite count, position, and altitude. Press ENTER to toggle GPS on/off.

### Navigation

Full-screen compass rose with heading arrow and 16-point cardinal direction. Displays speed/max speed (mph), altitude (feet), satellite count, and odometer.

**Waypoint navigation**: When a waypoint is set (from a contact location request or the Navigate action), shows target name, distance, bearing arrow, and ETA based on velocity made good.

- UP: Toggle screen lock (keeps display on)
- DOWN: Clear waypoint / reset trip (max speed and odometer)

### Sensors

Live sensor data display (battery voltage, temperature, humidity, pressure, altitude, etc.) from connected sensors.

### Settings

Toggle options: battery voltage display, SNR/RSSI bar, speed HUD, Bluetooth, and GPS.

### Shutdown

Hibernate the device.

## Top Bar

Always-visible top bar shows node name and battery indicator. Optional overlays:
- **Battery voltage**: numeric voltage (`4.15V`) instead of battery icon
- **Speed HUD**: current speed and heading direction when GPS is on and moving
- **SNR display**: last received packet ID, RSSI, SNR, and age

## Companion App Sync

This firmware is compatible with the official MeshCore companion apps (iOS/Android). The device UI and companion app share a single radio:

- Channel messages sent from the app appear in the device message log (with repeat tracking)
- DMs sent from the app appear in the device message log (retries collapse with updated TX count)
- All received messages appear on both simultaneously
- Channel messages sent from the device are queued to the app's offline queue
- DMs sent from the device do **not** sync to the app (no outgoing flag in the protocol)

## Building

Requires [PlatformIO](https://platformio.org/):

```bash
pio run -e WioTrackerL1_companion_radio_ble
```

Flash output: `.pio/build/WioTrackerL1_companion_radio_ble/firmware.zip`

## Hardware

- **Device**: Seeed Studio Wio Tracker L1 Pro (nRF52840 + SX1262)
- **Display**: 128x64 OLED
- **Input**: 5-way joystick (UP/DOWN/LEFT/RIGHT/ENTER) + CANCEL button

## Project Structure

- `examples/companion_radio/ui-new/` -- Enhanced UI implementation
- `examples/companion_radio/MyMesh.cpp` -- Mesh networking + companion app sync
- `examples/companion_radio/AbstractUITask.h` -- UI callback interface

## Configuration

Create `/presets.txt` on the device's internal filesystem (one message per line) to customize quick message presets.

## License

Inherits MIT License from upstream MeshCore project.

## Credits

Built on [MeshCore](https://github.com/ripplebiz/MeshCore) by ripplebiz and contributors.

UI enhancements developed with assistance from Claude (Anthropic).
