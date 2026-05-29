#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/BaseSerialInterface.h>
#include <Arduino.h>

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif

#include "NodePrefs.h"
#include <helpers/ContactInfo.h>

enum class UIEventType {
    none,
    contactMessage,
    channelMessage,
    roomMessage,
    newContactMessage,
    ack
};

class AbstractUITask {
protected:
  mesh::MainBoard* _board;
  BaseSerialInterface* _serial;
  bool _connected;

  AbstractUITask(mesh::MainBoard* board, BaseSerialInterface* serial) : _board(board), _serial(serial) {
    _connected = false;
  }

public:
  void setHasConnection(bool connected) { _connected = connected; }
  bool hasConnection() const { return _connected; }
  uint16_t getBattMilliVolts() const { return _board->getBattMilliVolts(); }
  bool isSerialEnabled() const { return _serial->isEnabled(); }
  void enableSerial() { _serial->enable(); }
  void disableSerial() { _serial->disable(); }
  virtual void msgRead(int msgcount) = 0;
  virtual void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount, int channel_idx = -1, const uint8_t* path = NULL) = 0;
  virtual void notify(UIEventType t = UIEventType::none) = 0;
  virtual void loop() = 0;
  virtual void matchRxPacket(const uint8_t* packet_hash, uint8_t path_len, const uint8_t* path, int16_t rssi, int8_t snr_x4) { }

  uint8_t _last_rx_id = 0;
  unsigned long _last_rx_time = 0;
  int16_t _last_rx_rssi = 0;
  int8_t _last_rx_snr_x4 = 0;
  unsigned long _last_tx_time = 0;

  // Unified per-repeater signal display (status bar)
  #define SIGNAL_MAX 8
  struct SignalEntry {
    uint8_t id;         // 1-byte repeater hash
    int8_t rx_snr_x4;   // RX: how well we hear them (from retransmission)
    int8_t tx_snr_x4;   // TX: how well they hear us (from ping snr_there)
    bool has_rx;         // RX data available
    bool has_tx;         // TX data available (ping succeeded)
    bool tx_failed;      // ping was attempted but failed
    unsigned long last_heard;  // millis() when last RX'd from this repeater
    uint16_t rx_count;   // number of packets received from this repeater
    uint16_t tx_count;   // number of ack'd pings to this repeater
    uint32_t last_rtt_ms; // last ping round-trip time in ms (0 = unknown)
    uint8_t fail_count;           // consecutive ping failures (for backoff)
    unsigned long last_fail_time; // millis() when last failure occurred
    unsigned long last_ping_time; // millis() when last ping was sent to this repeater
  };
  SignalEntry _signals[SIGNAL_MAX];
  uint8_t _signal_count = 0;
  uint8_t _signal_cycle = 0;
  unsigned long _signal_time = 0;       // when data was last updated
  unsigned long _signal_cycle_time = 0; // when to advance cycle

  // Auto-ping queue (ping heard repeaters to get accurate snr_there)
  #define AUTO_PING_QUEUE_MAX 4
  uint8_t _auto_ping_queue[AUTO_PING_QUEUE_MAX];  // 1-byte repeater hashes
  uint8_t _auto_ping_queue_count = 0;
  uint8_t _auto_ping_next = 0;         // next index to ping
  bool _auto_ping_pending = false;      // waiting for response
  unsigned long _auto_ping_timeout = 0; // response timeout
  uint8_t _auto_ping_current_id = 0;    // hash of repeater being pinged
  unsigned long _auto_ping_next_time = 0; // when to send next ping
  unsigned long _retry_ping_time = 0;  // when to next sweep for failed pings to retry

  // Adaptive TX check state
  bool _auto_tx_enabled = true;        // toggle from settings (persisted via ui_flags bit4)
  uint8_t _motion_mode = 0;            // 0=off, 1=auto, 2=bike, 3=drive (ui_flags bits 5-6)
  uint8_t _best_ping_id = 0;           // currently tracked best repeater hash
  uint8_t _best_ping_count = 0;        // number of checks since last reset

  uint8_t _td = 1;              // current motion timing divisor (1/2/4), updated each loop
  uint8_t _manual_ping_id = 0;  // non-zero = waiting for manual ping result from signal detail
  unsigned long _discovery_sweep_time = 0;  // next periodic discovery while in motion

  // Signal probe state
  bool _probe_active = false;        // discovery scan phase in progress
  bool _probe_done = false;          // prevents re-triggering auto-probe until new RX
  unsigned long _probe_timeout = 0;  // discovery scan timeout
  uint32_t _probe_scan_tag = 0;      // tag for this probe's discovery scan

  // ----- Phone motion hint (from companion app via SYNC_ID_MOTION_HINT) -----
  uint8_t _phone_motion_level = 0;      // 0=stationary, 1=slow(walk/bike), 2=fast(run/drive)
  unsigned long _phone_motion_time = 0; // millis() when hint last received
  unsigned long _last_signal_poll = 0;  // millis() when the app last read the signal table (getSync)

  // True if a companion app read the signal table recently => it's actively mirroring it.
  bool isAppSyncingSignals() const {
    return _last_signal_poll != 0 && (millis() - _last_signal_poll) < 15000UL;
  }

  void setMotionHint(uint8_t level) {
    _phone_motion_level = (level > 2) ? 2 : level;
    _phone_motion_time = millis();
  }
  // Timing divisor implied by a fresh phone motion hint (1/2/4); 1 if stale/none.
  // 60s staleness window so GPS still wins whenever it's present and reporting.
  uint8_t phoneMotionDivisor() const {
    if (_phone_motion_level == 0) return 1;
    if (millis() - _phone_motion_time > 60000UL) return 1;  // stale
    return (_phone_motion_level == 1) ? 2 : 4;
  }

  // ----- Unified signal-bar scoring (shared by OLED + companion app) -----
  // Score in snr_x4 units. Bidirectional links: 0.6*TX + 0.4*RX, but if either
  // leg is <= -10 dB (-40 in x4) the link is ranked by its dead leg (weak-leg guard).
  int16_t signalScore(const SignalEntry& e) const {
    if (e.has_rx && e.has_tx) {
      int16_t weak = (e.rx_snr_x4 < e.tx_snr_x4) ? e.rx_snr_x4 : e.tx_snr_x4;
      if (weak <= -40) return weak;
      int s = (6 * (int)e.tx_snr_x4 + 4 * (int)e.rx_snr_x4) / 10;
      return (int16_t)s;
    }
    if (e.has_rx) return e.rx_snr_x4;
    if (e.has_tx) return e.tx_snr_x4;
    return -128;
  }
  // Index of the best repeater (bidirectional preferred, then signalScore). -1 if none.
  int pickBestSignal() const {
    if (_signal_count == 0) return -1;
    int best = 0;
    for (int i = 1; i < _signal_count; i++) {
      const SignalEntry& cur = _signals[i];
      const SignalEntry& bst = _signals[best];
      bool cur_bidi = cur.has_rx && cur.has_tx;
      bool bst_bidi = bst.has_rx && bst.has_tx;
      if (cur_bidi && !bst_bidi) {
        best = i;  // bidirectional beats unidirectional
      } else if (cur_bidi == bst_bidi) {
        if (signalScore(cur) > signalScore(bst)) best = i;
      }
    }
    return best;
  }

  // Serialize the live signal table for SYNC_ID_SIGNAL_BARS (radio -> app).
  // Layout: [ver=1][count] then count x { id, rx_x4, tx_x4, flags, age_s(LE), rtt_ms(LE) }
  // flags: bit0 has_rx, bit1 has_tx, bit2 tx_failed, bit3 is_best.
  uint16_t serializeSignalBars(uint8_t* out, uint16_t max) {
    _last_signal_poll = millis();   // record that a companion app is reading the table
    if (max < 2) return 0;
    int best = pickBestSignal();
    uint8_t count = _signal_count;
    if ((uint16_t)(2 + count * 8) > max) count = (uint8_t)((max - 2) / 8);
    uint16_t i = 0;
    out[i++] = 1;       // version
    out[i++] = count;
    unsigned long now = millis();
    for (uint8_t e = 0; e < count; e++) {
      const SignalEntry& s = _signals[e];
      out[i++] = s.id;
      out[i++] = (uint8_t)s.rx_snr_x4;
      out[i++] = (uint8_t)s.tx_snr_x4;
      uint8_t flags = 0;
      if (s.has_rx)       flags |= 0x01;
      if (s.has_tx)       flags |= 0x02;
      if (s.tx_failed)    flags |= 0x04;
      if ((int)e == best) flags |= 0x08;
      out[i++] = flags;
      unsigned long age = (now - s.last_heard) / 1000UL;
      uint16_t age16 = (age > 65535UL) ? 65535 : (uint16_t)age;
      out[i++] = (uint8_t)(age16 & 0xFF);
      out[i++] = (uint8_t)(age16 >> 8);
      uint16_t rtt = (s.last_rtt_ms > 65535UL) ? 65535 : (uint16_t)s.last_rtt_ms;
      out[i++] = (uint8_t)(rtt & 0xFF);
      out[i++] = (uint8_t)(rtt >> 8);
    }
    return i;
  }

  // Companion-app or device-UI trigger: re-ping repeater(s) now via the auto-ping queue.
  // target_id != 0 pings just that repeater; 0 re-pings all tracked repeaters.
  void requestSignalRefresh(uint8_t target_id = 0) {
    if (_auto_ping_pending || _probe_active) return;  // busy — caller can retry
    _auto_ping_queue_count = 0;
    _auto_ping_next = 0;
    if (target_id != 0) {
      for (uint8_t i = 0; i < _signal_count; i++) {
        if (_signals[i].id == target_id) {
          _signals[i].tx_failed = false;
          _signals[i].fail_count = 0;
          _signals[i].has_tx = false;
          break;
        }
      }
      _auto_ping_queue[0] = target_id;
      _auto_ping_queue_count = 1;
    } else {
      for (uint8_t i = 0; i < _signal_count && _auto_ping_queue_count < AUTO_PING_QUEUE_MAX; i++) {
        _signals[i].tx_failed = false;
        _signals[i].fail_count = 0;
        _auto_ping_queue[_auto_ping_queue_count++] = _signals[i].id;
      }
    }
    _auto_ping_next_time = millis() + 200;
  }

  virtual void onRxPacket(uint8_t first_path_byte, int16_t rssi, int8_t snr_x4) {
    _probe_done = false;
    _last_rx_id = first_path_byte; _last_rx_time = millis(); _last_rx_rssi = rssi; _last_rx_snr_x4 = snr_x4;
    if (first_path_byte == 0) return;  // direct packet, no repeater

    // Find or add repeater in _signals[]
    int idx = -1;
    for (int i = 0; i < _signal_count; i++) {
      if (_signals[i].id == first_path_byte) { idx = i; break; }
    }
    if (idx < 0 && _auto_ping_pending) {
      // Don't create new entries from packets received during an active ping
      // (reply may route through a different repeater)
      return;
    } else if (idx < 0 && _signal_count < SIGNAL_MAX) {
      idx = _signal_count++;
      _signals[idx].id = first_path_byte;
      _signals[idx].has_tx = false;
      _signals[idx].tx_failed = false;
      _signals[idx].tx_snr_x4 = 0;
      _signals[idx].rx_snr_x4 = snr_x4;
      _signals[idx].has_rx = true;
      _signals[idx].last_heard = millis();
      _signals[idx].rx_count = 1;
      _signals[idx].tx_count = 0;
      _signals[idx].last_rtt_ms = 0;
      _signals[idx].fail_count = 0;
      _signals[idx].last_fail_time = 0;
      _signals[idx].last_ping_time = 0;
    } else if (idx < 0) {
      // Array full — evict oldest entry
      int oldest = 0;
      for (int i = 1; i < _signal_count; i++) {
        if (_signals[i].last_heard < _signals[oldest].last_heard) oldest = i;
      }
      idx = oldest;
      _signals[idx].id = first_path_byte;
      _signals[idx].has_tx = false;
      _signals[idx].tx_failed = false;
      _signals[idx].tx_snr_x4 = 0;
      _signals[idx].rx_snr_x4 = snr_x4;
      _signals[idx].has_rx = true;
      _signals[idx].last_heard = millis();
      _signals[idx].rx_count = 1;
      _signals[idx].tx_count = 0;
      _signals[idx].last_rtt_ms = 0;
      _signals[idx].fail_count = 0;
      _signals[idx].last_fail_time = 0;
      _signals[idx].last_ping_time = 0;
    } else {
      // Rolling average: 75% old + 25% new
      _signals[idx].rx_snr_x4 = (int8_t)((_signals[idx].rx_snr_x4 * 3 + snr_x4) / 4);
      _signals[idx].last_heard = millis();
      _signals[idx].rx_count++;
    }
    _signal_time = millis();

    // Queue auto-ping if idle and this repeater needs TX data or TX is stale
    bool ping_busy = (_auto_ping_queue_count > 0 &&
                      (_auto_ping_next < _auto_ping_queue_count || _auto_ping_pending));
    if (_auto_tx_enabled && !ping_busy && idx >= 0) {
      bool needs_tx = !_signals[idx].has_tx && !_signals[idx].tx_failed;
      // Best repeater: ping back if TX data is stale (>60s/td since last ping)
      bool tx_stale = _signals[idx].has_tx && first_path_byte == _best_ping_id &&
                      _signals[idx].last_ping_time > 0 &&
                      (millis() - _signals[idx].last_ping_time > 60000UL / _td);
      if (needs_tx || tx_stale) {
        _auto_ping_queue[0] = first_path_byte;
        _auto_ping_queue_count = 1;
        _auto_ping_next = 0;
        _auto_ping_next_time = millis() + 2000;  // 2s delay: let packet burst settle
      }
    }
  }
  virtual void onPathUpdated(const ContactInfo& contact, int16_t rssi, int8_t snr_x4) { }
  virtual void logPacket(uint8_t payload_type, uint8_t path_len, const uint8_t* path, int16_t rssi, int8_t snr_x4, uint8_t route_type = 0, uint8_t payload_len = 0) { }

  virtual void onTelemetryResponse(const ContactInfo& contact, float voltage, float temperature, float gps_lat = 0, float gps_lon = 0) { }
  virtual void onStatusResponse(const ContactInfo& contact, uint32_t uptime_secs, uint16_t batt_mv) { }
  virtual void onPingResponse(uint32_t latency_ms, float snr_there, float snr_back) { }
  virtual void onDiscoverResponse(uint8_t node_type, int8_t snr_x4, int16_t rssi, uint8_t path_len, const uint8_t* pub_key, uint8_t pub_key_len) { }
  virtual void addToMsgLog(const char* origin, const char* text, bool is_sent, uint8_t path_len = 0, int channel_idx = -1, const char* contact_name = NULL, const uint8_t* path = NULL, const uint8_t* packet_hash = NULL, uint32_t expected_ack = 0) { }
  virtual void onAckReceived(uint32_t ack_hash, int16_t rssi = 0, int8_t snr_x4 = 0) { }
  virtual void updateMsgLogRetry(const char* text, const char* contact_name, const uint8_t* packet_hash, uint32_t expected_ack) { }
};
