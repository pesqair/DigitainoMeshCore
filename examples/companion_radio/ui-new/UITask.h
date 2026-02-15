#pragma once

#include <MeshCore.h>
#include <helpers/ui/DisplayDriver.h>
#include <helpers/ui/UIScreen.h>
#include <helpers/SensorManager.h>
#include <helpers/BaseSerialInterface.h>
#include <Arduino.h>
#include <helpers/sensors/LPPDataHelpers.h>

#ifndef LED_STATE_ON
  #define LED_STATE_ON 1
#endif

#ifdef PIN_BUZZER
  #include <helpers/ui/buzzer.h>
#endif
#ifdef PIN_VIBRATION
  #include <helpers/ui/GenericVibration.h>
#endif

#include "../AbstractUITask.h"
#include "../NodePrefs.h"
#include <helpers/ContactInfo.h>

class UITask : public AbstractUITask {
  DisplayDriver* _display;
  SensorManager* _sensors;
#ifdef PIN_BUZZER
  genericBuzzer buzzer;
#endif
#ifdef PIN_VIBRATION
  GenericVibration vibration;
#endif
  unsigned long _next_refresh, _auto_off;
  NodePrefs* _node_prefs;
  char _alert[80];
  unsigned long _alert_expiry;
  int _msgcount;
  unsigned long ui_started_at, next_batt_chck;
  int next_backlight_btn_check = 0;
#ifdef PIN_STATUS_LED
  int led_state = 0;
  int next_led_change = 0;
  int last_led_increment = 0;
#endif

#ifdef PIN_USER_BTN_ANA
  unsigned long _analogue_pin_read_millis = millis();
#endif

  UIScreen* splash;
  UIScreen* home;
  UIScreen* msg_preview;
  UIScreen* compose;
  UIScreen* contact_select;
  UIScreen* channel_select;
  UIScreen* curr;

  void userLedHandler();

  // Button action handlers
  char checkDisplayOn(char c);
  char handleLongPress(char c);
  char handleDoubleClick(char c);
  char handleTripleClick(char c);

  void setCurrScreen(UIScreen* c);

public:

  UITask(mesh::MainBoard* board, BaseSerialInterface* serial) : AbstractUITask(board, serial), _display(NULL), _sensors(NULL) {
    next_batt_chck = _next_refresh = 0;
    ui_started_at = 0;
    curr = NULL;
    _msg_log_count = 0;
    _msg_log_next = 0;
    _preset_pending = false;
    _pending_preset[0] = '\0';
  }
  void begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs);

  struct MessageLogEntry {
    uint32_t timestamp;
    char origin[24];
    char text[80];
    bool is_sent;
    uint8_t path_len;     // hops: 0=direct/self, 0xFF=DM, else=flood hops
    int channel_idx;      // >=0 = channel msg (reply on this channel), -1 = DM
    char contact_name[24]; // for DM: sender name to reply to
    uint8_t path[MAX_PATH_SIZE]; // repeater hashes along the route
    int16_t rssi;             // RSSI at receive time (0 = not recorded)
    int8_t  snr_x4;           // SNR * 4 at receive time (0.25 dB resolution)
    // Sent message repeat tracking
    uint8_t packet_hash[MAX_HASH_SIZE]; // for matching RX packets against sent messages
    uint8_t heard_repeats;    // count of heard retransmissions
    int16_t repeat_rssi;      // RSSI of most recent repeat heard
    int8_t  repeat_snr_x4;    // SNR*4 of most recent repeat heard
    uint8_t repeat_path[MAX_PATH_SIZE]; // unique repeater hashes heard
    uint8_t repeat_path_len;  // count of unique repeaters heard
    uint8_t tx_count;         // number of times this message was transmitted
    uint32_t expected_ack;    // ACK hash for delivery tracking (DMs only)
    bool delivered;           // true when delivery ACK received
  };
  #define MSG_LOG_SIZE 16
  MessageLogEntry _msg_log[MSG_LOG_SIZE];
  int _msg_log_count;
  int _msg_log_next;

  // Packet log (circular buffer)
  struct PacketLogEntry {
    uint8_t payload_type;
    uint8_t first_hop;    // path[0] or 0
    int16_t rssi;
    int8_t  snr_x4;
    unsigned long timestamp;  // millis()
  };
  #define PACKET_LOG_SIZE 32
  PacketLogEntry _pkt_log[PACKET_LOG_SIZE];
  int _pkt_log_count = 0;
  int _pkt_log_next = 0;
  void logPacket(uint8_t payload_type, uint8_t path_len, const uint8_t* path, int16_t rssi, int8_t snr_x4) override;

  // Pending preset for Channel/DM target selection
  char _pending_preset[80];
  bool _preset_pending;

  void gotoHomeScreen() { setCurrScreen(home); }
  void gotoComposeScreen();
  void gotoContactSelect(bool gps_mode = false);
  void gotoChannelSelect();
  void startDMCompose(const ContactInfo& contact, const char* prefill_text = NULL);
  void startChannelCompose(int channel_idx, const char* channel_name, const char* prefill_text = NULL);
  void sendGPSDM(const ContactInfo& contact);
  void sendPresetToChannel(int channel_idx);
  void sendPresetDM(const ContactInfo& contact);
  void addToMsgLog(const char* origin, const char* text, bool is_sent, uint8_t path_len = 0, int channel_idx = -1, const char* contact_name = NULL, const uint8_t* path = NULL, const uint8_t* packet_hash = NULL, uint32_t expected_ack = 0);
  void updateMsgLogRetry(const char* text, const char* contact_name, const uint8_t* packet_hash, uint32_t expected_ack) override;
  void onAckReceived(uint32_t ack_hash) override;
  void matchRxPacket(const uint8_t* packet_hash, uint8_t path_len, const uint8_t* path, int16_t rssi, int8_t snr_x4) override;
  void onPathUpdated(const ContactInfo& contact, int16_t rssi, int8_t snr_x4) override;
  void onTelemetryResponse(const ContactInfo& contact, float voltage, float temperature) override;
  void onStatusResponse(const ContactInfo& contact, uint32_t uptime_secs, uint16_t batt_mv) override;
  void showAlert(const char* text, int duration_millis);
  int  getMsgCount() const { return _msgcount; }
  bool hasDisplay() const { return _display != NULL; }
  bool isButtonPressed() const;

  void extendAutoOff();
  void toggleBuzzer();
  bool getGPSState();
  void toggleGPS();


  // from AbstractUITask
  void msgRead(int msgcount) override;
  void newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount, int channel_idx = -1, const uint8_t* path = NULL) override;
  void notify(UIEventType t = UIEventType::none) override;
  void loop() override;

  void shutdown(bool restart = false);
};
