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
