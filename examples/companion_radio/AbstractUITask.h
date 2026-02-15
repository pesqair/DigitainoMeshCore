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
  virtual void onRxPacket(uint8_t first_path_byte) { _last_rx_id = first_path_byte; _last_rx_time = millis(); }
  virtual void onPathUpdated(const ContactInfo& contact, int16_t rssi, int8_t snr_x4) { }
  virtual void logPacket(uint8_t payload_type, uint8_t path_len, const uint8_t* path, int16_t rssi, int8_t snr_x4, uint8_t route_type = 0, uint8_t payload_len = 0) { }
  uint8_t _last_rx_id = 0;
  unsigned long _last_rx_time = 0;
  virtual void onTelemetryResponse(const ContactInfo& contact, float voltage, float temperature, float gps_lat = 0, float gps_lon = 0) { }
  virtual void onStatusResponse(const ContactInfo& contact, uint32_t uptime_secs, uint16_t batt_mv) { }
  virtual void addToMsgLog(const char* origin, const char* text, bool is_sent, uint8_t path_len = 0, int channel_idx = -1, const char* contact_name = NULL, const uint8_t* path = NULL, const uint8_t* packet_hash = NULL, uint32_t expected_ack = 0) { }
  virtual void onAckReceived(uint32_t ack_hash) { }
  virtual void updateMsgLogRetry(const char* text, const char* contact_name, const uint8_t* packet_hash, uint32_t expected_ack) { }
};
