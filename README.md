# Digitaino MeshCore Firmware

Custom firmware for the **Wio Tracker L1 Pro** based on [MeshCore](https://github.com/ripplebiz/MeshCore), adding on-device UI features for standalone messaging without needing the companion phone app.

## About This Fork

This is a customized version of MeshCore firmware specifically tailored for personal use on the Wio Tracker L1 Pro. It extends the companion radio example with an enhanced on-device interface.

**Upstream Project:** [MeshCore by ripplebiz](https://github.com/ripplebiz/MeshCore)

## UI Pages

The home screen is a carousel of pages navigated with LEFT/RIGHT. Press ENTER to activate a page, CANCEL to return to the carousel.

### Home

Info dashboard shown directly in the carousel (no click needed). Displays node name, full date/time, battery voltage and percentage, unread message count, GPS status, and BT connection status. Shows BLE pairing PIN when waiting for pairing.

### Messages

Scrollable message log with channel filtering (LEFT/RIGHT to cycle filters). Messages are displayed oldest-at-top / newest-at-bottom. In channel-specific views, the redundant channel name is omitted from each line to save space. Delivery status, repeat counts, and last-hop hash prefixes are shown. Prefix stays fixed while message text scrolls horizontally on selection. A dim `...` hint appears at the bottom when on the last message to indicate compose is below.

**Quick-Send Compose** (DOWN past last message, or UP dismisses back):
- In a filtered channel/DM tab: opens an overlay with `[Keyboard]` (full compose) and preset quick messages for one-tap sending
- In the "All" tab: opens a Channel/DM chooser, then proceeds to channel or contact selection and compose
- After sending, the message list scrolls to the latest message and returns to the matching filter tab

**Message Detail** (ENTER on a message):
- Full word-wrapped text, timestamp, sender/recipient, hop count
- Signal info (RSSI and SNR) for received messages
- Path chain with per-repeater selection (LEFT/RIGHT to highlight individual hops)
- Sent message tracking: repeat count, heard-by repeater list with per-repeater signal
- DM delivery receipts (Delivered / Pending)
- Reply options: Reply on channel, Reply DM, or Resend

### Quick Msg

Preset quick messages loaded from `/presets.txt` on the device filesystem (one per line). Includes options to compose custom messages, reply DM, send GPS coordinates, and send to channels. Presets can be added, edited, and deleted from the menu.

### Contacts

Contact list sorted by most recently heard, with favorites shown first (marked with `*`). Filter by type using LEFT/RIGHT: All, Contacts, or Repeaters.

Each contact shows hash prefix (`<XX>`), name, type suffix (`[D]`irect, `[N]` hops, `[R:D]` repeater direct, `[R:N]` repeater hops, `[Rm]` room, `[S]` sensor), and last-heard age.

**Contact Actions** (ENTER on a contact):
- **Chat nodes**: Send DM, Find Path, Telemetry, Send GPS, Request Location, Navigate
- **Repeaters**: Ping, Find Path, Telemetry, Status, Navigate (if GPS available)
- **Sensors**: Find Path, Telemetry, Navigate (if GPS available)
- Cached info displayed below actions: last heard, path/signal, battery, temperature, uptime

**Ping Repeater**: Sends a trace packet to a repeater and measures round-trip time (ms), SNR "there" (how well the repeater heard us), and SNR "back" (how well we heard the repeater). Available for repeater-type contacts in both Contacts and Nearby views.

All async operations (path discovery, telemetry, status, ping, location requests) show a waiting state with timeout.

### Nearby

Active discovery of nearby repeaters and sensors. Press ENTER to scan; results appear within 8 seconds. Shows discovered nodes with hash prefix, name (or `<XX> ???` if unknown), and SNR.

ENTER on a result opens the action menu (same as Contacts) if the node is in the contact database. Press RIGHT to rescan at any time.

### Recent

List of recently heard nodes with names and time since last heard.

### Packets

Raw packet log showing packet type, first hop, RSSI, SNR, and age. Press ENTER for detail view with full type name, route type, signal info, path chain, and payload size. UP/DOWN to scroll through packets and detail items.

### Radio

Displays current radio configuration: frequency, spreading factor, bandwidth, coding rate, TX power, and noise floor.

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

Toggle options: GMT offset (LEFT/RIGHT to adjust -12 to +14, applies to clock and message timestamps), battery voltage display (text instead of icon), signal bars (SNR-based with repeater ID), speed HUD, Bluetooth, and GPS.

### Hibernate

Power off the device.

## Status Bar

14px always-visible top bar with three zones:

- **Left zone**: HH:MM clock, GPS satellite dish icon with satellite count (when GPS is on), envelope icon with unread message count (clears when Messages page is viewed), speed/compass heading (auto-shown when moving)
- **Right zone**: vertical battery icon (or voltage text if enabled), mute icon (when buzzer is off), signal indicators (when enabled — fades after 5 minutes):
  - **RX** (`▼████XX`): how well we hear repeaters. Updates on any received packet; when retransmissions of a sent message are heard, cycles through all heard repeaters every 2 seconds (at least 2 full rotations before returning to live).
  - **TX** (`▲████XX`): how well repeaters hear us. Populated from ping `snr_there` data — after sending a message, heard repeaters (if in contacts) are auto-pinged to get accurate TX signal. Also updated by user-initiated pings.
  - Tiny arrow icons (▲ up = TX, ▼ down = RX) are drawn above the shortest bar to save horizontal space. TX section only appears when data exists.

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
