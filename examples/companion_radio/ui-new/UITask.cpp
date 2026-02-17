#include "UITask.h"
#include <math.h>
#include <helpers/TxtDataHelpers.h>
#include <helpers/AdvertDataHelpers.h>
#include "../MyMesh.h"
#include "target.h"
#ifdef WIFI_SSID
  #include <WiFi.h>
#endif

#ifndef AUTO_OFF_MILLIS
  #define AUTO_OFF_MILLIS     15000   // 15 seconds
#endif
#define BOOT_SCREEN_MILLIS   3000   // 3 seconds

#ifdef PIN_STATUS_LED
#define LED_ON_MILLIS     20
#define LED_ON_MSG_MILLIS 200
#define LED_CYCLE_MILLIS  4000
#endif

#define LONG_PRESS_MILLIS   1200
#define TOP_BAR_H           14   // height of always-visible top bar

#ifndef UI_RECENT_LIST_SIZE
  #define UI_RECENT_LIST_SIZE 4
#endif

#if UI_HAS_JOYSTICK
  #define PRESS_LABEL "press Enter"
#else
  #define PRESS_LABEL "long press"
#endif

#ifndef PRESET_MSG_COUNT
  #define PRESET_MSG_COUNT 8
#endif
#ifndef PRESET_MSG_1
  #define PRESET_MSG_1 "OK"
#endif
#ifndef PRESET_MSG_2
  #define PRESET_MSG_2 "Help!"
#endif
#ifndef PRESET_MSG_3
  #define PRESET_MSG_3 "On my way"
#endif
#ifndef PRESET_MSG_4
  #define PRESET_MSG_4 "At destination"
#endif
#ifndef PRESET_MSG_5
  #define PRESET_MSG_5 "Yes"
#endif
#ifndef PRESET_MSG_6
  #define PRESET_MSG_6 "No"
#endif
#ifndef PRESET_MSG_7
  #define PRESET_MSG_7 "SOS Emergency"
#endif
#ifndef PRESET_MSG_8
  #define PRESET_MSG_8 ""
#endif
#define PRESET_GPS_INDEX 7

// Runtime preset storage - loaded from /presets.txt or compile-time defaults
#define PRESET_MAX_LEN 48
static char preset_buf[PRESET_MSG_COUNT][PRESET_MAX_LEN];
static const char* preset_defaults[PRESET_MSG_COUNT] = {
  PRESET_MSG_1, PRESET_MSG_2, PRESET_MSG_3, PRESET_MSG_4,
  PRESET_MSG_5, PRESET_MSG_6, PRESET_MSG_7, PRESET_MSG_8
};
static const char* preset_messages[PRESET_MSG_COUNT];

static void loadPresetsFromFile() {
  // Initialize with compile-time defaults
  for (int i = 0; i < PRESET_MSG_COUNT; i++) {
    strncpy(preset_buf[i], preset_defaults[i], PRESET_MAX_LEN - 1);
    preset_buf[i][PRESET_MAX_LEN - 1] = '\0';
    preset_messages[i] = preset_buf[i];
  }

  // Try to load from /presets.txt (one message per line)
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File f = InternalFS.open("/presets.txt", FILE_O_READ);
#elif defined(RP2040_PLATFORM)
  File f = LittleFS.open("/presets.txt", "r");
#elif defined(ESP32)
  File f = SPIFFS.open("/presets.txt", "r", false);
#else
  File f;
#endif
  if (f) {
    int idx = 0;
    while (idx < PRESET_MSG_COUNT && f.available()) {
      char line[PRESET_MAX_LEN];
      int len = 0;
      while (len < PRESET_MAX_LEN - 1 && f.available()) {
        char ch = f.read();
        if (ch == '\n') break;
        if (ch == '\r') continue;
        line[len++] = ch;
      }
      line[len] = '\0';
      if (len > 0) {
        strncpy(preset_buf[idx], line, PRESET_MAX_LEN - 1);
        preset_buf[idx][PRESET_MAX_LEN - 1] = '\0';
        idx++;
      }
    }
    f.close();
    MESH_DEBUG_PRINTLN("Loaded presets from /presets.txt");
  }
}

static void savePresetsToFile() {
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  File f = InternalFS.open("/presets.txt", FILE_O_WRITE);
  if (f) { f.truncate(0); f.seek(0); }
#elif defined(RP2040_PLATFORM)
  File f = LittleFS.open("/presets.txt", "w");
#elif defined(ESP32)
  File f = SPIFFS.open("/presets.txt", "w", true);
#else
  File f;
#endif
  if (f) {
    for (int i = 0; i < PRESET_MSG_COUNT; i++) {
      f.print(preset_buf[i]);
      f.print('\n');
    }
    f.close();
    MESH_DEBUG_PRINTLN("Saved presets to /presets.txt");
  }
}

#include "icons.h"

class SplashScreen : public UIScreen {
  UITask* _task;
  unsigned long dismiss_after;
  char _version_info[12];

public:
  SplashScreen(UITask* task) : _task(task) {
    // strip off dash and commit hash by changing dash to null terminator
    // e.g: v1.2.3-abcdef -> v1.2.3
    const char *ver = FIRMWARE_VERSION;
    const char *dash = strchr(ver, '-');

    int len = dash ? dash - ver : strlen(ver);
    if (len >= sizeof(_version_info)) len = sizeof(_version_info) - 1;
    memcpy(_version_info, ver, len);
    _version_info[len] = 0;

    dismiss_after = millis() + BOOT_SCREEN_MILLIS;
  }

  int render(DisplayDriver& display) override {
    // meshcore logo
    display.setColor(DisplayDriver::BLUE);
    int logoWidth = 128;
    display.drawXbm((display.width() - logoWidth) / 2, 3, meshcore_logo, logoWidth, 13);

    // version info
    display.setColor(DisplayDriver::LIGHT);
    display.setTextSize(2);
    display.drawTextCentered(display.width()/2, 22, _version_info);

    display.setTextSize(1);
    display.drawTextCentered(display.width()/2, 42, FIRMWARE_BUILD_DATE);

    display.setColor(DisplayDriver::YELLOW);
    display.drawTextCentered(display.width()/2, 54, "custom digitaino fw");

    return 1000;
  }

  void poll() override {
    if (millis() >= dismiss_after) {
      _task->gotoHomeScreen();
    }
  }
};

class HomeScreen : public UIScreen {
  friend class UITask;
  enum HomePage {
    FIRST,
    MESSAGES,
    PRESETS,
    TRACE,
    RECENT,
    SIGNALS,
    PACKETS,
    RADIO,
    NEARBY,
#if ENV_INCLUDE_GPS == 1
    GPS,
    NAV,
#endif
#if UI_SENSORS_PAGE == 1
    SENSORS,
#endif
    SETTINGS,
    ADVERT,
    SHUTDOWN,
    Count    // keep as last
  };

  UITask* _task;
  mesh::RTCClock* _rtc;
  SensorManager* _sensors;
  NodePrefs* _node_prefs;
  uint8_t _page;
  uint8_t _preset_sel;
  bool _preset_target_choosing; // true = showing Channel/DM sub-menu
  uint8_t _preset_target_sel;   // 0=Channel, 1=DM
  bool _preset_edit_mode;       // true = showing edit/delete sub-menu
  uint8_t _preset_edit_sel;     // selected preset in edit mode
  uint8_t _msg_sel;
  uint8_t _msg_sel_prev;    // to detect selection change and reset scroll
  int _msg_scroll_px;       // horizontal pixel offset for selected message
  bool _msg_detail;         // true = showing message detail/reply view
  uint8_t _msg_detail_scroll; // scroll offset within detail items
  bool _msg_reply_menu;     // true = showing reply type selection (channel msgs only)
  uint8_t _msg_reply_sel;   // 0 = Reply Ch, 1 = Reply DM
  bool _shutdown_init;
  bool _show_voltage;
  bool _show_speed;
  bool _show_snr;
  bool _beep_on_ble;
  bool _auto_tx_check;
  uint8_t _motion_mode_setting;
  int8_t _gmt_offset;
  uint8_t _pkt_sel;  // selected packet in packet log page
  bool _pkt_detail;           // whether detail view is showing
  uint8_t _pkt_detail_scroll; // scroll offset in detail view
  int8_t _path_sel;   // -1 = no repeater selected, 0..N = index into path
  bool _needs_fast_refresh; // true when any visible age < 60s
  float _max_speed;
  float _odometer;          // miles traveled
  unsigned long _odo_last;  // last odometer sample time
  long _odo_last_lat;       // last position latitude (microdegrees)
  long _odo_last_lon;       // last position longitude (microdegrees)
  bool _nav_screen_lock;
  // Waypoint navigation state
  bool _nav_has_waypoint;
  float _nav_wp_lat, _nav_wp_lon;  // decimal degrees
  char _nav_wp_name[12];           // truncated contact name
  bool _page_active;
  uint8_t _settings_sel;
  uint8_t _ct_filter;  // 0=All, 1=Contacts, 2=Repeaters
  // Message channel filter
  int _msg_vscroll;             // first visible display line in multi-line mode
  uint8_t _msg_filter;          // 0=All, 1+=specific channel filter index
  int _msg_filter_channels[8];  // channel_idx values for each filter option
  int _msg_filter_count;        // total filter options (including "All")
  char _msg_filter_dm_names[4][24]; // DM contact names for filter tabs (negative sentinel entries)
  bool _msg_compose_menu;           // true when showing quick-send overlay
  uint8_t _msg_compose_sel;         // 0=Keyboard, 1..N=preset messages
  bool _msg_target_menu;            // true when showing Channel/DM chooser (All tab)
  uint8_t _msg_target_sel;          // 0=Channel, 1=DM
  AdvertPath recent[UI_RECENT_LIST_SIZE];

  // Cached contact info (persists across page visits)
  struct ContactCache {
    uint8_t pub_key_prefix[4];  // first 4 bytes of pub key for matching
    bool valid;
    // Path info
    bool has_path_info;
    uint8_t path_hops;
    uint8_t path[MAX_PATH_SIZE];
    int16_t rssi;
    int8_t  snr_x4;
    // Telemetry info
    bool has_telem;
    float voltage;
    float temperature;  // -274 = not available
    // Status info (repeaters)
    bool has_status;
    uint32_t uptime_secs;
    uint16_t batt_mv;
    // GPS from telemetry response
    bool has_gps;
    float gps_lat, gps_lon;  // decimal degrees
  };
  #define CT_CACHE_SIZE 16
  ContactCache _ct_cache[CT_CACHE_SIZE];

  ContactCache* findOrCreateCache(const uint8_t* pub_key) {
    // Find existing
    for (int i = 0; i < CT_CACHE_SIZE; i++) {
      if (_ct_cache[i].valid && memcmp(_ct_cache[i].pub_key_prefix, pub_key, 4) == 0)
        return &_ct_cache[i];
    }
    // Find empty slot
    for (int i = 0; i < CT_CACHE_SIZE; i++) {
      if (!_ct_cache[i].valid) {
        memset(&_ct_cache[i], 0, sizeof(ContactCache));
        memcpy(_ct_cache[i].pub_key_prefix, pub_key, 4);
        _ct_cache[i].valid = true;
        return &_ct_cache[i];
      }
    }
    // Overwrite oldest (slot 0)
    memset(&_ct_cache[0], 0, sizeof(ContactCache));
    memcpy(_ct_cache[0].pub_key_prefix, pub_key, 4);
    _ct_cache[0].valid = true;
    return &_ct_cache[0];
  }

  ContactCache* findCache(const uint8_t* pub_key) {
    for (int i = 0; i < CT_CACHE_SIZE; i++) {
      if (_ct_cache[i].valid && memcmp(_ct_cache[i].pub_key_prefix, pub_key, 4) == 0)
        return &_ct_cache[i];
    }
    return NULL;
  }

  // Contacts page state
  uint8_t _ct_sel;           // selected contact in list
  uint16_t _ct_sorted[64];   // sorted contact indices (favorites first)
  int _ct_count;             // number of contacts in sorted list
  bool _ct_action;           // true = showing action menu for selected contact
  uint8_t _ct_action_sel;    // selected action in menu
  uint8_t _ct_action_count;  // number of available actions
  uint8_t _ct_detail_scroll; // scroll offset in detail/action view
  uint8_t _ct_action_key[PUB_KEY_SIZE]; // pub_key of contact in action menu (stable across re-sorts)
  // Path discovery sub-state
  bool _ct_path_pending;
  unsigned long _ct_path_timeout;
  uint8_t _ct_path_key[PUB_KEY_SIZE];
  bool    _ct_path_found;
  char    _ct_target_name[32];
  // Telemetry sub-state
  bool _ct_telem_pending;
  unsigned long _ct_telem_timeout;
  bool _ct_telem_done;
  // Status sub-state
  bool _ct_status_pending;
  unsigned long _ct_status_timeout;
  bool _ct_status_done;
  // Ping sub-state
  bool _ct_ping_pending;
  unsigned long _ct_ping_timeout;
  bool _ct_ping_done;
  uint32_t _ct_ping_latency;
  float _ct_ping_snr_there;
  float _ct_ping_snr_back;
  // GPS request sub-state
  bool _ct_gps_pending;
  unsigned long _ct_gps_timeout;
  bool _ct_gps_done;     // location received
  bool _ct_gps_no_fix;   // responded but no GPS

  // Nearby scan state
  struct ScanResult {
    bool valid;
    uint8_t pub_key[PUB_KEY_SIZE];
    uint8_t node_type;
    int8_t snr_x4;
    int16_t rssi;
    uint8_t path_len;
    char name[32];
    bool in_contacts;
  };
  #define SCAN_RESULT_SIZE 16
  ScanResult _scan_results[SCAN_RESULT_SIZE];
  int _scan_count;
  bool _scan_active;
  unsigned long _scan_timeout;
  uint32_t _scan_tag;
  uint8_t _scan_sel;
  bool _scan_action;
  uint8_t _scan_action_sel;
  uint8_t _scan_action_count;
  uint8_t _scan_detail_scroll;

  // Signals page state
  uint8_t _sig_sel;
  bool _sig_action;
  uint8_t _sig_action_sel;
  uint8_t _sig_detail_scroll;

  // Interactive status bar
  enum StatusBarItem : uint8_t {
    SB_CLOCK, SB_GPS, SB_ENVELOPE, SB_SPEED,
    SB_SIGNAL, SB_MUTE, SB_BATTERY
  };
  struct SBSlot { StatusBarItem type; int x; int w; };
  #define SB_MAX_SLOTS 7
  SBSlot _sb_slots[SB_MAX_SLOTS];
  uint8_t _sb_count = 0;
  bool _sb_active = false;    // true = user is focused on status bar
  int8_t _sb_sel = 0;         // which slot is selected (0..count-1)
  uint8_t _sb_prev_page = 0;  // page before entering status bar
  bool _sb_prev_active = false; // page_active before entering status bar
  bool _sb_return = false;    // true = current page was launched from status bar

  // Build message filter options by scanning message log for unique channel_idx values and DM contacts
  void rebuildMsgFilters() {
    _msg_filter_count = 1;  // index 0 = "All"
    _msg_filter_channels[0] = -99;  // sentinel for "All"
    int dm_count = 0;
    for (int i = 0; i < _task->_msg_log_count && _msg_filter_count < 8; i++) {
      int buf_idx = (_task->_msg_log_next - 1 - i + MSG_LOG_SIZE) % MSG_LOG_SIZE;
      auto& entry = _task->_msg_log[buf_idx];
      int ch = entry.channel_idx;
      if (ch >= 0) {
        // Channel message
        bool found = false;
        for (int j = 1; j < _msg_filter_count; j++) {
          if (_msg_filter_channels[j] == ch) { found = true; break; }
        }
        if (!found) {
          _msg_filter_channels[_msg_filter_count++] = ch;
        }
      } else if (ch < 0 && entry.contact_name[0] != '\0' && dm_count < 4) {
        // DM message — check if this contact is already in filters
        bool found = false;
        for (int j = 1; j < _msg_filter_count; j++) {
          if (_msg_filter_channels[j] <= -2) {
            int dm_idx = -_msg_filter_channels[j] - 2;
            if (strcmp(_msg_filter_dm_names[dm_idx], entry.contact_name) == 0) {
              found = true; break;
            }
          }
        }
        if (!found && _msg_filter_count < 8) {
          strncpy(_msg_filter_dm_names[dm_count], entry.contact_name, 23);
          _msg_filter_dm_names[dm_count][23] = '\0';
          _msg_filter_channels[_msg_filter_count++] = -(dm_count + 2);  // -2, -3, -4, -5
          dm_count++;
        }
      }
    }
    if (_msg_filter >= _msg_filter_count) _msg_filter = 0;
  }

  // Check if message passes current filter
  bool msgPassesFilter(int log_index) {
    if (_msg_filter == 0) return true;  // "All" shows everything
    int buf_idx = (_task->_msg_log_next - 1 - log_index + MSG_LOG_SIZE) % MSG_LOG_SIZE;
    auto& entry = _task->_msg_log[buf_idx];
    int fval = _msg_filter_channels[_msg_filter];
    if (fval >= 0) {
      return entry.channel_idx == fval;  // channel filter
    }
    // DM filter: match by contact_name
    int dm_idx = -fval - 2;
    if (dm_idx >= 0 && dm_idx < 4) {
      return entry.channel_idx < 0 && strcmp(entry.contact_name, _msg_filter_dm_names[dm_idx]) == 0;
    }
    return false;
  }

  // Count messages that pass the current filter
  int countFilteredMsgs() {
    int count = 0;
    for (int i = 0; i < _task->_msg_log_count; i++) {
      if (msgPassesFilter(i)) count++;
    }
    return count;
  }

  // Get the log_index of the nth filtered message (0-based, oldest first)
  int getFilteredMsgIndex(int nth) {
    int count = 0;
    for (int i = _task->_msg_log_count - 1; i >= 0; i--) {
      if (msgPassesFilter(i)) {
        if (count == nth) return i;
        count++;
      }
    }
    return -1;
  }

  void rebuildContactsSorted() {
    _ct_count = 0;
    int n = the_mesh.getNumContacts();
    int fav_start = 0, fav_count = 0;
    // First pass: favorites
    for (int i = 0; i < n && _ct_count < 64; i++) {
      ContactInfo ci;
      if (the_mesh.getContactByIdx(i, ci) && (ci.flags & 0x01)) {
        if (_ct_filter == 1 && ci.type != ADV_TYPE_CHAT) continue;
        if (_ct_filter == 2 && ci.type != ADV_TYPE_REPEATER) continue;
        _ct_sorted[_ct_count++] = i;
        fav_count++;
      }
    }
    // Second pass: non-favorites
    int nonfav_start = _ct_count;
    for (int i = 0; i < n && _ct_count < 64; i++) {
      ContactInfo ci;
      if (the_mesh.getContactByIdx(i, ci) && !(ci.flags & 0x01)) {
        if (_ct_filter == 1 && ci.type != ADV_TYPE_CHAT) continue;
        if (_ct_filter == 2 && ci.type != ADV_TYPE_REPEATER) continue;
        _ct_sorted[_ct_count++] = i;
      }
    }
    int nonfav_count = _ct_count - nonfav_start;
    // Sort each group by lastmod descending (most recently heard first)
    // Simple insertion sort (small N, no stdlib qsort context pointer needed)
    auto sortGroup = [](uint16_t* arr, int count) {
      for (int i = 1; i < count; i++) {
        uint16_t key = arr[i];
        ContactInfo ci_key;
        the_mesh.getContactByIdx(key, ci_key);
        int j = i - 1;
        while (j >= 0) {
          ContactInfo ci_j;
          the_mesh.getContactByIdx(arr[j], ci_j);
          if (ci_j.lastmod >= ci_key.lastmod) break;
          arr[j + 1] = arr[j];
          j--;
        }
        arr[j + 1] = key;
      }
    };
    sortGroup(&_ct_sorted[fav_start], fav_count);
    sortGroup(&_ct_sorted[nonfav_start], nonfav_count);
    if (_ct_sel >= _ct_count && _ct_count > 0) _ct_sel = _ct_count - 1;
  }

  void reselectContact(const char* name) {
    rebuildContactsSorted();
    ContactInfo ci;
    for (int i = 0; i < _ct_count; i++) {
      if (the_mesh.getContactByIdx(_ct_sorted[i], ci) && strcmp(ci.name, name) == 0) {
        _ct_sel = i;
        return;
      }
    }
  }

  // Find contact by pub_key (stable across re-sorts)
  bool getContactByKey(const uint8_t* pub_key, ContactInfo& out_ci) {
    int n = the_mesh.getNumContacts();
    for (int i = 0; i < n; i++) {
      if (the_mesh.getContactByIdx(i, out_ci) && memcmp(out_ci.id.pub_key, pub_key, PUB_KEY_SIZE) == 0) {
        return true;
      }
    }
    return false;
  }

  // Renders vertical battery icon (5px wide x 12px tall) at right_edge.
  // Returns new right_x (left edge of battery area) for further right-zone packing.
  int renderVerticalBattery(DisplayDriver& display, int right_edge, uint16_t batteryMilliVolts) {
    if (_show_voltage) {
      // Show voltage as text instead of icon
      char vbuf[8];
      float volts = (float)batteryMilliVolts / 1000.0f;
      snprintf(vbuf, sizeof(vbuf), "%.2fV", volts);
      display.setColor(DisplayDriver::GREEN);
      display.setTextSize(1);
      int tw = (int)strlen(vbuf) * 6;
      int textX = right_edge - tw;
      display.setCursor(textX, 3);
      display.print(vbuf);
      return textX - 2;
    }

#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
    int pct = ((batteryMilliVolts - BATT_MIN_MILLIVOLTS) * 100) / (BATT_MAX_MILLIVOLTS - BATT_MIN_MILLIVOLTS);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    // Vertical battery: 5px wide x 12px tall (1px cap + 11px body)
    int bw = 5, bh = 11;
    int bx = right_edge - bw;
    int by = 2;  // top of body

    display.setColor(DisplayDriver::GREEN);
    // Cap (centered on top, 3px wide x 1px)
    display.fillRect(bx + 1, by - 1, 3, 1);
    // Body outline
    display.drawRect(bx, by, bw, bh);
    // Fill segments (4 segments, 3x2 each, bottom-up)
    int segs = 0;
    if (pct >= 88) segs = 4;
    else if (pct >= 63) segs = 3;
    else if (pct >= 38) segs = 2;
    else if (pct >= 13) segs = 1;
    for (int s = 0; s < segs; s++) {
      int sy = by + bh - 2 - s * 2 - 1;  // bottom-up positioning
      display.fillRect(bx + 1, sy, 3, 2);
    }

    return bx - 2;
  }

  void renderStatusBar(DisplayDriver& display) {
    _sb_count = 0;
    SBSlot right_tmp[3]; uint8_t rc = 0;

    display.setColor(DisplayDriver::GREEN);
    display.setTextSize(1);

    // === RIGHT ZONE (packed from right edge) ===
    int right_x = display.width();

    // 1. Vertical battery
    int batt_x_before = right_x;
    right_x = renderVerticalBattery(display, right_x, _task->getBattMilliVolts());
    right_tmp[rc++] = {SB_BATTERY, right_x, batt_x_before - right_x};

    // 2. Mute icon (8x8, shown when buzzer is quiet)
#ifdef PIN_BUZZER
    if (_task->isBuzzerQuiet()) {
      right_x -= 8;
      display.setColor(DisplayDriver::RED);
      display.drawXbm(right_x, 3, muted_icon, 8, 8);
      right_tmp[rc++] = {SB_MUTE, right_x, 8};
      right_x -= 2;
    }
#endif

    // 3. Signal bars (when _show_snr is ON)
    // Unified layout: AA ▲[BARS] ▼[BARS] [mute][bat]
    // Draw order (right-to-left): RX bars, TX bars, hex ID
    if (_show_snr) {
      int sig_start = right_x;
      int bars_w = 11;
      int bars_y = 3;

      // Blink arrows on recent activity (300ms on/off)
      bool rx_recent = _task->_last_rx_time > 0 && (millis() - _task->_last_rx_time) < 600;
      bool tx_recent = _task->_last_tx_time > 0 && (millis() - _task->_last_tx_time) < 600;
      bool blink_phase = (millis() / 150) & 1;  // toggles every 150ms
      bool rx_blink_on = rx_recent ? blink_phase : true;
      bool tx_blink_on = tx_recent ? blink_phase : true;
      if (rx_recent || tx_recent) _needs_fast_refresh = true;

      // Motion-aware prune threshold
      unsigned long prune_ms = 300000UL;
#if ENV_INCLUDE_GPS == 1
      {
        unsigned int ptd = 1;
        if (_task->_motion_mode == 2) { ptd = 2; }
        else if (_task->_motion_mode == 3) { ptd = 4; }
        else if (_task->_motion_mode == 1) {
          LocationProvider* nmea = sensors.getLocationProvider();
          if (nmea != NULL && nmea->isValid()) {
            float speed_mph = nmea->getSpeed() / 1000.0f * 1.15078f;
            if (speed_mph >= 25.0f) ptd = 4;
            else if (speed_mph >= 5.0f) ptd = 2;
          }
        }
        prune_ms /= ptd;
      }
#endif

      // Prune stale entries (>prune_ms since last heard)
      for (int i = 0; i < _task->_signal_count; ) {
        if (millis() - _task->_signals[i].last_heard > prune_ms) {
          // Shift remaining entries down
          for (int j = i; j < _task->_signal_count - 1; j++) {
            _task->_signals[j] = _task->_signals[j + 1];
          }
          _task->_signal_count--;
          if (_task->_signal_cycle >= _task->_signal_count && _task->_signal_count > 0) {
            _task->_signal_cycle = 0;
          }
        } else {
          i++;
        }
      }

      bool use_cycling = _task->_signal_count > 0 &&
                         (millis() - _task->_signal_time) / 1000 < (prune_ms / 1000);
      bool use_live = !use_cycling &&
                      _task->_last_rx_time > 0 &&
                      (millis() - _task->_last_rx_time) / 1000 < (prune_ms / 1000);

      if (use_cycling) {
        // Pick best repeater: prioritize bidirectional (has_tx + has_rx), then strongest signal
        int best = 0;
        for (int i = 1; i < _task->_signal_count; i++) {
          auto& cur = _task->_signals[i];
          auto& bst = _task->_signals[best];
          bool cur_bidi = cur.has_rx && cur.has_tx;
          bool bst_bidi = bst.has_rx && bst.has_tx;
          if (cur_bidi && !bst_bidi) {
            best = i;  // bidirectional beats unidirectional
          } else if (cur_bidi == bst_bidi) {
            // Same tier — compare signal strength
            // For bidi: use weaker of the two (link limited by weakest direction)
            // For non-bidi: use RX SNR
            int8_t cur_snr = cur_bidi ? min(cur.rx_snr_x4, cur.tx_snr_x4) : cur.rx_snr_x4;
            int8_t bst_snr = bst_bidi ? min(bst.rx_snr_x4, bst.tx_snr_x4) : bst.rx_snr_x4;
            if (cur_snr > bst_snr) best = i;
          }
        }
        auto& entry = _task->_signals[best];

        // Compute RX bars from entry
        int rx_bars = 0;
        float rx_snr = (float)entry.rx_snr_x4 / 4.0f;
        if (rx_snr > 10) rx_bars = 4;
        else if (rx_snr > 5) rx_bars = 3;
        else if (rx_snr > 0) rx_bars = 2;
        else if (rx_snr > -10) rx_bars = 1;

        // Draw RX bars with down-arrow (▼) on bar 0
        right_x -= bars_w;
        int bars_x = right_x;
        if (rx_blink_on) {
          display.setColor(rx_recent ? DisplayDriver::GREEN : DisplayDriver::LIGHT);
          display.fillRect(bars_x, bars_y, 5, 1);
          display.fillRect(bars_x + 1, bars_y + 1, 3, 1);
          display.fillRect(bars_x + 2, bars_y + 2, 1, 1);
        }
        for (int b = 0; b < 4; b++) {
          int bh = 3 + b * 2;
          int bx = bars_x + b * 3;
          int by = bars_y + (10 - bh);
          if (b < rx_bars) {
            display.setColor(DisplayDriver::GREEN);
            display.fillRect(bx, by, 2, bh);
          } else {
            display.setColor(DisplayDriver::GREEN);
            display.fillRect(bx, bars_y + 9, 2, 1);
          }
        }

        right_x -= 1;  // gap between RX and TX

        // Draw TX bars with up-arrow (▲) on bar 0
        right_x -= bars_w;
        bars_x = right_x;
        if (tx_blink_on) {
          display.setColor(tx_recent ? DisplayDriver::GREEN : DisplayDriver::LIGHT);
          display.fillRect(bars_x + 2, bars_y, 1, 1);
          display.fillRect(bars_x + 1, bars_y + 1, 3, 1);
          display.fillRect(bars_x, bars_y + 2, 5, 1);
        }
        if (entry.has_tx) {
          int tx_bars = 0;
          float tx_snr = (float)entry.tx_snr_x4 / 4.0f;
          if (tx_snr > 10) tx_bars = 4;
          else if (tx_snr > 5) tx_bars = 3;
          else if (tx_snr > 0) tx_bars = 2;
          else if (tx_snr > -10) tx_bars = 1;
          for (int b = 0; b < 4; b++) {
            int bh = 3 + b * 2;
            int bx = bars_x + b * 3;
            int by = bars_y + (10 - bh);
            if (b < tx_bars) {
              display.setColor(DisplayDriver::GREEN);
              display.fillRect(bx, by, 2, bh);
            } else {
              display.setColor(DisplayDriver::GREEN);
              display.fillRect(bx, bars_y + 9, 2, 1);
            }
          }
        } else if (entry.tx_failed) {
          // TX ping failed — draw "X"
          display.setColor(DisplayDriver::RED);
          display.setCursor(bars_x + 3, bars_y + 3);
          display.print("X");
        } else {
          // TX ping pending — draw "?"
          display.setColor(DisplayDriver::LIGHT);
          display.setCursor(bars_x + 3, bars_y + 3);
          display.print("?");
        }

        // Gap + hex ID
        right_x -= 2;
        char hex_id[4];
        snprintf(hex_id, sizeof(hex_id), "%02X", entry.id);
        right_x -= 12;
        display.setColor(DisplayDriver::LIGHT);
        display.setCursor(right_x, 3);
        display.print(hex_id);

      } else if (use_live) {
        _needs_fast_refresh = true;

        // Compute RX bars from live data
        int rx_bars = 0;
        float snr = (float)_task->_last_rx_snr_x4 / 4.0f;
        if (snr > 10) rx_bars = 4;
        else if (snr > 5) rx_bars = 3;
        else if (snr > 0) rx_bars = 2;
        else if (snr > -10) rx_bars = 1;

        // Draw RX bars with down-arrow (▼)
        right_x -= bars_w;
        int bars_x = right_x;
        if (rx_blink_on) {
          display.setColor(rx_recent ? DisplayDriver::GREEN : DisplayDriver::LIGHT);
          display.fillRect(bars_x, bars_y, 5, 1);
          display.fillRect(bars_x + 1, bars_y + 1, 3, 1);
          display.fillRect(bars_x + 2, bars_y + 2, 1, 1);
        }
        for (int b = 0; b < 4; b++) {
          int bh = 3 + b * 2;
          int bx = bars_x + b * 3;
          int by = bars_y + (10 - bh);
          if (b < rx_bars) {
            display.setColor(DisplayDriver::GREEN);
            display.fillRect(bx, by, 2, bh);
          } else {
            display.setColor(DisplayDriver::GREEN);
            display.fillRect(bx, bars_y + 9, 2, 1);
          }
        }

        // Gap + hex ID (no TX section for live-only)
        right_x -= 2;
        char hex_id[4];
        snprintf(hex_id, sizeof(hex_id), "%02X", _task->_last_rx_id);
        right_x -= 12;
        display.setColor(DisplayDriver::LIGHT);
        display.setCursor(right_x, 3);
        display.print(hex_id);
      } else {
        // No signal data — draw empty bar stubs with horizontal dash
        right_x -= 11;
        int bars_x_start = right_x;
        display.setColor(DisplayDriver::LIGHT);
        // Bar stubs at base (2px wide each, spaced 3px apart)
        for (int b = 0; b < 4; b++) {
          display.fillRect(bars_x_start + b * 3, bars_y + 9, 2, 1);
        }
        // Horizontal dash across the middle
        display.fillRect(bars_x_start, bars_y + 6, 11, 1);
      }

      if (rc < 3) right_tmp[rc++] = {SB_SIGNAL, right_x, sig_start - right_x};
      right_x -= 1;
    }

    // Append right-zone slots in left-to-right order
    for (int i = rc - 1; i >= 0; i--) {
      if (_sb_count < SB_MAX_SLOTS) _sb_slots[_sb_count++] = right_tmp[i];
    }

    // === LEFT ZONE (packed from left edge) ===
    int left_x = 0;
    int left_max = right_x - 2;

    // 1. HH:MM clock
    {
      uint32_t now = _rtc->getCurrentTime();
      if (now > 1577836800) {
        int clk_x = left_x;
        uint32_t t = now + _gmt_offset * 3600;
        int mins = (t / 60) % 60;
        int hours = (t / 3600) % 24;
        char clk[6];
        snprintf(clk, sizeof(clk), "%02d:%02d", hours, mins);
        display.setColor(DisplayDriver::GREEN);
        display.setCursor(left_x, 3);
        display.print(clk);
        left_x += 5 * 6;  // 5 chars * 6px
        if (_sb_count < SB_MAX_SLOTS) _sb_slots[_sb_count++] = {SB_CLOCK, clk_x, left_x - clk_x};
      }
    }

    // 2. GPS indicator (satellite icon + sat count, or off icon when disabled)
#if ENV_INCLUDE_GPS == 1
    {
      bool gps_on = _task->getGPSState();
      int gps_x = left_x;
      if (gps_on) {
        display.setColor(DisplayDriver::LIGHT);
        display.drawXbm(left_x, 3, sat_icon, 8, 8);
        left_x += 9;
        LocationProvider* nmea = sensors.getLocationProvider();
        char gps_num[4];
        if (nmea == NULL || !nmea->isValid()) {
          strcpy(gps_num, "?");
        } else {
          snprintf(gps_num, sizeof(gps_num), "%ld", nmea->satellitesCount());
        }
        display.setCursor(left_x, 3);
        display.print(gps_num);
        left_x += (int)strlen(gps_num) * 6;
      } else {
        display.setColor(DisplayDriver::LIGHT);
        display.drawXbm(left_x, 3, gps_off_icon, 8, 8);
        left_x += 9;
      }
      if (_sb_count < SB_MAX_SLOTS) _sb_slots[_sb_count++] = {SB_GPS, gps_x, left_x - gps_x};
    }
#endif

    // 3. Envelope (always shown — unread: filled + count, read: open outline)
    {
      int msg_count = _task->getMsgCount();
      if (left_x > 0) left_x += 2;
      if (left_x + 7 <= left_max) {
        int env_x = left_x;
        if (msg_count > 0) {
          display.setColor(DisplayDriver::YELLOW);
          display.drawXbm(left_x, 5, envelope_icon, 7, 5);
          left_x += 8;
          char cnt[6];
          snprintf(cnt, sizeof(cnt), "%d", msg_count);
          if (left_x + (int)strlen(cnt) * 6 <= left_max) {
            display.setCursor(left_x, 3);
            display.print(cnt);
            left_x += (int)strlen(cnt) * 6;
          }
        } else {
          display.setColor(DisplayDriver::LIGHT);
          display.drawXbm(left_x, 5, envelope_read_icon, 7, 5);
          left_x += 8;
        }
        if (_sb_count < SB_MAX_SLOTS) _sb_slots[_sb_count++] = {SB_ENVELOPE, env_x, left_x - env_x};
      }
    }

    // 4. Speed/compass (auto when GPS speed > 2mph)
#if ENV_INCLUDE_GPS == 1
    if (_show_speed) {
      LocationProvider* nmea = sensors.getLocationProvider();
      if (nmea != NULL && nmea->isValid()) {
        float speed_mph = nmea->getSpeed() / 1000.0f * 1.15078f;
        if (speed_mph > 2.0f) {
          static const char* hud_dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
          float course_deg = nmea->getCourse() / 1000.0f;
          int di = ((int)(course_deg + 22.5f) % 360) / 45;
          if (di < 0) di = 0;
          if (di > 7) di = 7;
          char spd[14];
          snprintf(spd, sizeof(spd), "%.0f%s", speed_mph, hud_dirs[di]);
          int sw = (int)strlen(spd) * 6;
          if (left_x > 0) left_x += 2;
          if (left_x + sw <= left_max) {
            int spd_x = left_x;
            display.setColor(DisplayDriver::GREEN);
            display.setCursor(left_x, 3);
            display.print(spd);
            left_x += sw;
            if (_sb_count < SB_MAX_SLOTS) _sb_slots[_sb_count++] = {SB_SPEED, spd_x, left_x - spd_x};
          }
        }
      }
    }
#endif

    // Draw focus indicator when status bar is active
    if (_sb_active && _sb_count > 0) {
      if (_sb_sel >= _sb_count) _sb_sel = _sb_count - 1;
      if (_sb_sel >= 0 && _sb_sel < _sb_count) {
        display.setColor(DisplayDriver::LIGHT);
        display.fillRect(_sb_slots[_sb_sel].x, 13, _sb_slots[_sb_sel].w, 1);
        _needs_fast_refresh = true;
      }
    }
  }

  CayenneLPP sensors_lpp;
  int sensors_nb = 0;
  bool sensors_scroll = false;
  int sensors_scroll_offset = 0;
  int next_sensors_refresh = 0;
  
  void refresh_sensors() {
    if (millis() > next_sensors_refresh) {
      sensors_lpp.reset();
      sensors_nb = 0;
      sensors_lpp.addVoltage(TELEM_CHANNEL_SELF, (float)board.getBattMilliVolts() / 1000.0f);
      sensors.querySensors(0xFF, sensors_lpp);
      LPPReader reader (sensors_lpp.getBuffer(), sensors_lpp.getSize());
      uint8_t channel, type;
      while(reader.readHeader(channel, type)) {
        reader.skipData(type);
        sensors_nb ++;
      }
      sensors_scroll = sensors_nb > UI_RECENT_LIST_SIZE;
#if AUTO_OFF_MILLIS > 0
      next_sensors_refresh = millis() + 5000; // refresh sensor values every 5 sec
#else
      next_sensors_refresh = millis() + 60000; // refresh sensor values every 1 min
#endif
    }
  }

  uint8_t buildUiFlags() const {
    return (_show_voltage ? 0x01 : 0) | (_show_snr ? 0x02 : 0) |
           (_show_speed ? 0x04 : 0) | (_beep_on_ble ? 0x08 : 0) |
           (_auto_tx_check ? 0 : 0x10) | ((_motion_mode_setting & 0x03) << 5);
  }

public:
  HomeScreen(UITask* task, mesh::RTCClock* rtc, SensorManager* sensors, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _sensors(sensors), _node_prefs(node_prefs), _page(0),
       _preset_sel(0), _msg_sel(0xFF), _msg_sel_prev(0xFF), _msg_scroll_px(0),
       _msg_detail(false), _msg_detail_scroll(0), _msg_reply_menu(false), _msg_reply_sel(0), _shutdown_init(false), _show_voltage(node_prefs->ui_flags & 0x01), _show_speed(node_prefs->ui_flags & 0x04),
       _show_snr(node_prefs->ui_flags & 0x02), _beep_on_ble(node_prefs->ui_flags & 0x08), _auto_tx_check(!(node_prefs->ui_flags & 0x10)), _motion_mode_setting((node_prefs->ui_flags >> 5) & 0x03), _gmt_offset(node_prefs->gmt_offset), _pkt_sel(0), _pkt_detail(false), _pkt_detail_scroll(0), _path_sel(-1), _max_speed(0), _odometer(0), _odo_last(0), _odo_last_lat(0), _odo_last_lon(0), _nav_screen_lock(false), _nav_has_waypoint(false),
       _page_active(false), _settings_sel(0), _ct_filter(0), _msg_vscroll(0), _msg_filter(0), _msg_filter_count(0), _msg_compose_menu(false), _msg_compose_sel(0), _msg_target_menu(false), _msg_target_sel(0),
       _preset_target_choosing(false), _preset_target_sel(0), _preset_edit_mode(false), _preset_edit_sel(0),
       _ct_sel(0), _ct_count(0), _ct_action(false), _ct_action_sel(0), _ct_action_count(0), _ct_detail_scroll(0),
       _ct_path_pending(false), _ct_path_found(false),
       _ct_telem_pending(false), _ct_telem_done(false),
       _ct_status_pending(false), _ct_status_done(false),
       _ct_ping_pending(false), _ct_ping_done(false),
       _ct_gps_pending(false), _ct_gps_done(false), _ct_gps_no_fix(false),
       _scan_count(0), _scan_active(false), _scan_timeout(0), _scan_tag(0),
       _scan_sel(0), _scan_action(false), _scan_action_sel(0), _scan_action_count(0), _scan_detail_scroll(0),
       _sig_sel(0), _sig_action(false), _sig_action_sel(0), _sig_detail_scroll(0),
       _sb_count(0), _sb_active(false), _sb_sel(0), _sb_prev_page(0), _sb_prev_active(false), _sb_return(false),
       sensors_lpp(200) { memset(_ct_cache, 0, sizeof(_ct_cache)); memset(_scan_results, 0, sizeof(_scan_results)); }

  bool isUserBusy() const {
    return _page_active || _msg_detail || _msg_compose_menu || _msg_target_menu || _pkt_detail || _ct_action || _ct_path_pending || _ct_telem_pending ||
           _ct_telem_done || _ct_status_pending || _ct_status_done ||
           _ct_ping_pending || _ct_ping_done ||
           _ct_gps_pending || _ct_gps_done || _ct_gps_no_fix ||
           _preset_target_choosing || _preset_edit_mode || _scan_action || _sig_action;
  }

  bool isInSubState() const {
    return _msg_detail || _msg_compose_menu || _msg_target_menu || _msg_reply_menu ||
           _pkt_detail || _ct_action || _ct_path_pending || _ct_telem_pending ||
           _ct_telem_done || _ct_status_pending || _ct_status_done ||
           _ct_ping_pending || _ct_ping_done ||
           _ct_gps_pending || _ct_gps_done || _ct_gps_no_fix ||
           _preset_target_choosing || _preset_edit_mode || _scan_action || _sig_action;
  }

  void enterStatusBar() {
    if (isInSubState()) return;
    _sb_prev_page = _page;
    _sb_prev_active = _page_active;
    _sb_return = false;  // clear: new SB session starts from current location
    _sb_active = true;
    _sb_sel = 0;
  }

  void resetSelToFirst() {
    switch ((HomePage)_page) {
      case MESSAGES: _msg_sel = 0; break;
      case SIGNALS: _sig_sel = 0; break;
      case SETTINGS: _settings_sel = 0; break;
      case PRESETS: _preset_sel = 0; break;
      case TRACE: _ct_sel = 0; break;
      case PACKETS: _pkt_sel = 0; break;
      case NEARBY: _scan_sel = 0; break;
      default: break;
    }
  }

  void resetSelToLast() {
    switch ((HomePage)_page) {
      case MESSAGES: { int ft = countFilteredMsgs(); _msg_sel = ft > 0 ? ft - 1 : 0; } break;
      case SIGNALS: _sig_sel = _task->_signal_count > 0 ? _task->_signal_count - 1 : 0; break;
      case SETTINGS: _settings_sel = countSettings() - 1; break;
      case PRESETS: _preset_sel = PRESET_MSG_COUNT + 5; break;
      case TRACE: _ct_sel = _ct_count > 0 ? _ct_count - 1 : 0; break;
      case PACKETS: _pkt_sel = _task->_pkt_log_count > 0 ? _task->_pkt_log_count - 1 : 0; break;
      case NEARBY: _scan_sel = _scan_count > 0 ? _scan_count - 1 : 0; break;
      default: break;
    }
  }

  int countSettings() const {
    int sc = 1 + 1 + 1;  // GMT + Voltage + SNR
#if ENV_INCLUDE_GPS == 1
    sc++;  // Speed
#endif
    sc += 2;  // Beep + Auto TX
#if ENV_INCLUDE_GPS == 1
    sc++;  // Motion
#endif
    sc++;  // BLE
#if ENV_INCLUDE_GPS == 1
    sc++;  // GPS
#endif
    return sc;
  }

  bool handleStatusBarKey(char c) {
    if (c == KEY_LEFT) {
      _sb_sel = _sb_sel > 0 ? _sb_sel - 1 : _sb_count - 1;
      return true;
    }
    if (c == KEY_RIGHT) {
      _sb_sel = _sb_sel < _sb_count - 1 ? _sb_sel + 1 : 0;
      return true;
    }
    if (c == KEY_DOWN) {
      _sb_active = false;
      if (_page_active) resetSelToFirst();
      return true;
    }
    if (c == KEY_UP) {
      _sb_active = false;
      if (_page_active) resetSelToLast();
      return true;
    }
    if (c == KEY_CANCEL) {
      _sb_active = false;
      return true;
    }
    if (c == KEY_ENTER) {
      activateStatusBarItem();
      return true;
    }
    return true;  // consume all keys while in status bar
  }

  void activateStatusBarItem() {
    if (_sb_sel < 0 || _sb_sel >= _sb_count) return;
    StatusBarItem type = _sb_slots[_sb_sel].type;
    _sb_active = false;

    switch (type) {
      case SB_CLOCK:    _page = HomePage::SETTINGS; _page_active = true; _settings_sel = 0; _sb_return = true; break;
      case SB_GPS:
        _task->toggleGPS();
        _sb_active = true;  // stay in status bar so user sees change
        break;
      case SB_ENVELOPE: _page = HomePage::MESSAGES; _page_active = true; _msg_sel = 0xFF; _sb_return = true; break;
      case SB_SPEED:    _page = HomePage::NAV; _page_active = true; _sb_return = true; break;
      case SB_SIGNAL:   _page = HomePage::SIGNALS; _page_active = true; _sig_sel = 0; _sb_return = true; break;
      case SB_MUTE:
        _task->toggleBuzzer();
        _sb_active = true;  // stay in status bar so user sees change
        break;
      case SB_BATTERY:
        _show_voltage = !_show_voltage;
        _node_prefs->ui_flags = buildUiFlags();
        the_mesh.savePrefs();
        _sb_active = true;  // stay in status bar
        break;
    }
  }

  void poll() override {
    if (_shutdown_init && !_task->isButtonPressed()) {  // must wait for USR button to be released
      _task->shutdown();
    }
  }

  // Simulate word wrapping to count lines (matches drawStringMaxWidth behavior)
  int getTextLines(DisplayDriver& display, const char* str, int max_width) {
    if (max_width <= 0 || !str || !str[0]) return 1;
    int lines = 1;
    int lineWidth = 0;
    const char* p = str;
    while (*p) {
      // Find next word: advance past non-break chars, then include the break char
      const char* wEnd = p;
      while (*wEnd && *wEnd != ' ' && *wEnd != '-' && *wEnd != '/') wEnd++;
      if (*wEnd) wEnd++; // include the space/dash/slash
      // Measure this word segment
      char word[80];
      int wLen = wEnd - p;
      if (wLen >= (int)sizeof(word)) wLen = sizeof(word) - 1;
      memcpy(word, p, wLen);
      word[wLen] = 0;
      int wWidth = display.getTextWidth(word);
      if (lineWidth + wWidth >= max_width && lineWidth > 0) {
        lines++;
        lineWidth = wWidth;
      } else {
        lineWidth += wWidth;
      }
      p = wEnd;
    }
    return lines;
  }

  int render(DisplayDriver& display) override {
    char tmp[80];
    _needs_fast_refresh = false;
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);

    // === TOP BAR (always visible) ===
    renderStatusBar(display);

    if (!_page_active && _page == HomePage::FIRST) {
      // === HOME DASHBOARD (shown directly in carousel, no click needed) ===
      display.setTextSize(1);
      int cx = display.width() / 2;

      // Row 1: Node name (the only place the name is shown)
      display.setColor(DisplayDriver::YELLOW);
      display.drawTextCentered(cx, TOP_BAR_H + 0, _node_prefs->node_name);

      // Row 2: Full date and time
      {
        uint32_t now = _rtc->getCurrentTime();
        if (now > 1577836800) { // after 2020-01-01
          uint32_t t = now + _gmt_offset * 3600;
          int mins = (t / 60) % 60;
          int hours = (t / 3600) % 24;
          uint32_t d = t / 86400;
          int year = 1970;
          while (true) {
            int ydays = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
            if ((int)d < ydays) break;
            d -= ydays; year++;
          }
          static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
          static const char* mnames[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
          bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
          int month = 0;
          for (month = 0; month < 12; month++) {
            int md = mdays[month] + ((month == 1 && leap) ? 1 : 0);
            if ((int)d < md) break;
            d -= md;
          }
          int day = (int)d + 1;
          snprintf(tmp, sizeof(tmp), "%02d-%s-%d %02d:%02d", day, mnames[month], year, hours, mins);
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(cx, TOP_BAR_H + 10, tmp);
        } else {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(cx, TOP_BAR_H + 10, "No time set");
        }
      }

      // Row 3: Battery voltage and percentage
      {
        uint16_t mv = _task->getBattMilliVolts();
        float volts = (float)mv / 1000.0f;
#ifndef BATT_MIN_MILLIVOLTS
  #define BATT_MIN_MILLIVOLTS 3000
#endif
#ifndef BATT_MAX_MILLIVOLTS
  #define BATT_MAX_MILLIVOLTS 4200
#endif
        int pct = ((int)(mv - BATT_MIN_MILLIVOLTS) * 100) / (BATT_MAX_MILLIVOLTS - BATT_MIN_MILLIVOLTS);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        snprintf(tmp, sizeof(tmp), "Batt: %.2fV %d%%", volts, pct);
        display.setColor(DisplayDriver::GREEN);
        display.drawTextCentered(cx, TOP_BAR_H + 20, tmp);
      }

      // Row 4: Messages + GPS status
      {
        char line[32];
        int mc = _task->getMsgCount();
#if ENV_INCLUDE_GPS == 1
        bool gps_on = _task->getGPSState();
        const char* gps_str;
        char gps_buf[12];
        if (!gps_on) {
          gps_str = "GPS:off";
        } else {
          LocationProvider* nmea = sensors.getLocationProvider();
          if (nmea == NULL || !nmea->isValid()) {
            gps_str = "GPS:no fix";
          } else {
            snprintf(gps_buf, sizeof(gps_buf), "GPS:%ldsat", nmea->satellitesCount());
            gps_str = gps_buf;
          }
        }
        snprintf(line, sizeof(line), "Msg:%d  %s", mc, gps_str);
#else
        snprintf(line, sizeof(line), "Msg:%d", mc);
#endif
        display.setColor(DisplayDriver::LIGHT);
        display.drawTextCentered(cx, TOP_BAR_H + 30, line);
      }

      // Row 5: Connection status
#ifdef WIFI_SSID
      {
        IPAddress ip = WiFi.localIP();
        snprintf(tmp, sizeof(tmp), "IP:%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        display.setColor(DisplayDriver::LIGHT);
        display.drawTextCentered(cx, TOP_BAR_H + 40, tmp);
      }
#else
      if (_task->hasConnection()) {
        display.setColor(DisplayDriver::GREEN);
        display.drawTextCentered(cx, TOP_BAR_H + 40, "BT: Connected");
      } else if (the_mesh.getBLEPin() != 0) {
        display.setColor(DisplayDriver::RED);
        snprintf(tmp, sizeof(tmp), "Pin: %d", the_mesh.getBLEPin());
        display.drawTextCentered(cx, TOP_BAR_H + 40, tmp);
      } else {
        display.setColor(DisplayDriver::LIGHT);
        display.drawTextCentered(cx, TOP_BAR_H + 40, "BT: Waiting...");
      }
#endif

      // Page number at bottom-right corner
      snprintf(tmp, sizeof(tmp), "%d/%d", _page + 1, (int)HomePage::Count);
      display.setColor(DisplayDriver::LIGHT);
      display.setCursor(display.width() - (int)strlen(tmp) * 6, 56);
      display.print(tmp);

      return 5000;
    }

    if (!_page_active) {
      // === LEVEL 1: CAROUSEL MODE (all pages except Home) ===
      const char* page_title = "";
      char summary_buf[32];
      const char* page_summary = "";

      switch ((HomePage)_page) {
        case MESSAGES: page_title = "Messages";
          snprintf(summary_buf, sizeof(summary_buf), "%d messages", _task->_msg_log_count);
          page_summary = summary_buf; break;
        case PRESETS: page_title = "Quick Msg"; page_summary = "Quick messages"; break;
        case RECENT: page_title = "Recent"; page_summary = "Recently heard"; break;
        case TRACE: page_title = "Contacts";
          snprintf(summary_buf, sizeof(summary_buf), "%d contacts", the_mesh.getNumContacts());
          page_summary = summary_buf; break;
        case NEARBY: page_title = "Nearby";
          if (_scan_count > 0) { snprintf(summary_buf, sizeof(summary_buf), "%d found", _scan_count); page_summary = summary_buf; }
          else page_summary = "Scan nearby";
          break;
        case SIGNALS: page_title = "Signals";
          if (_task->_signal_count > 0) {
            snprintf(summary_buf, sizeof(summary_buf), "%d repeaters", _task->_signal_count);
            page_summary = summary_buf;
          } else page_summary = "No signals";
          break;
        case RADIO: page_title = "Radio";
          snprintf(summary_buf, sizeof(summary_buf), "%.3f SF%d", _node_prefs->freq, _node_prefs->sf);
          page_summary = summary_buf; break;
        case PACKETS: page_title = "Packets";
          snprintf(summary_buf, sizeof(summary_buf), "%d packets", _task->_pkt_log_count);
          page_summary = summary_buf; break;
        case ADVERT: page_title = "Advert"; page_summary = "Send advert"; break;
#if ENV_INCLUDE_GPS == 1
        case GPS: page_title = "GPS"; {
          bool gps_on = _task->getGPSState();
          LocationProvider* nmea = sensors.getLocationProvider();
          page_summary = gps_on ? (nmea && nmea->isValid() ? "GPS: fix" : "GPS: no fix") : "GPS: off";
        } break;
        case NAV: page_title = "Navigation"; page_summary = "Nav & compass"; break;
#endif
#if UI_SENSORS_PAGE == 1
        case SENSORS: page_title = "Sensors"; page_summary = "Sensor data"; break;
#endif
        case SETTINGS: page_title = "Settings"; page_summary = "Device settings"; break;
        case SHUTDOWN: page_title = "Hibernate"; page_summary = "Power off"; break;
        default: page_title = "?"; page_summary = ""; break;
      }

      // Page title (centered, with arrows)
      display.setColor(DisplayDriver::YELLOW);
      display.setTextSize(2);
      display.drawTextCentered(display.width() / 2, 24, page_title);

      // Arrow indicators
      display.setTextSize(1);
      display.setColor(DisplayDriver::GREEN);
      display.setCursor(4, 28);
      display.print("<");
      display.setCursor(display.width() - 10, 28);
      display.print(">");

      // Summary line
      display.setColor(DisplayDriver::LIGHT);
      display.drawTextCentered(display.width() / 2, 46, page_summary);

      // Page number
      snprintf(tmp, sizeof(tmp), "%d/%d", _page + 1, (int)HomePage::Count);
      display.setColor(DisplayDriver::LIGHT);
      display.drawTextCentered(display.width() / 2, 56, tmp);

      return 5000;
    }

    // === LEVEL 2: INSIDE PAGE (full screen) ===

    if (_page == HomePage::MESSAGES) {
      display.setTextSize(1);

      if (_msg_detail && _task->_msg_log_count > 0) {
        // Scrollable detail view for selected message
        int log_idx = getFilteredMsgIndex(_msg_sel);
        if (log_idx < 0) { _msg_detail = false; log_idx = 0; }
        int buf_idx = (_task->_msg_log_next - 1 - log_idx + MSG_LOG_SIZE) % MSG_LOG_SIZE;
        auto& entry = _task->_msg_log[buf_idx];

        // Build detail items: Message text lines, separator, then metadata
        char detail_items[28][48];
        uint8_t detail_count = 0;

        // Full message text, word-wrapped
        {
          int chars_per_line = display.width() / 6;  // ~21 for 128px display
          if (chars_per_line > 46) chars_per_line = 46;
          if (chars_per_line < 10) chars_per_line = 10;
          // Filter the text for display (replace multi-byte UTF-8 with blocks)
          char filtered_text[168];
          display.translateUTF8ToBlocks(filtered_text, entry.text, sizeof(filtered_text));
          const char* src = filtered_text;
          int src_len = strlen(src);
          int pos = 0;
          while (pos < src_len && detail_count < 16) {  // max 16 lines for message
            // Find how much fits on this line
            int line_end = pos + chars_per_line;
            if (line_end >= src_len) {
              line_end = src_len;
            } else {
              // Try to break at a space
              int brk = line_end;
              while (brk > pos && src[brk] != ' ') brk--;
              if (brk > pos) line_end = brk;
            }
            int len = line_end - pos;
            memcpy(detail_items[detail_count], &src[pos], len);
            detail_items[detail_count][len] = '\0';
            detail_count++;
            pos = line_end;
            // Skip the space at the break point
            if (pos < src_len && src[pos] == ' ') pos++;
          }
        }

        // Separator
        uint8_t separator_idx = detail_count;
        strcpy(detail_items[detail_count], "---");
        detail_count++;

        // Timestamp
        if (entry.timestamp > 1577836800) { // after 2020-01-01
          uint32_t t = entry.timestamp + _gmt_offset * 3600;
          int mins = (t / 60) % 60;
          int hours = (t / 3600) % 24;
          int days = (int)(t / 86400);
          int year = 1970;
          while (true) {
            int ydays = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0)) ? 366 : 365;
            if (days < ydays) break;
            days -= ydays;
            year++;
          }
          static const int mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
          static const char* mnames[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
          bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
          int month = 0;
          for (month = 0; month < 12; month++) {
            int md = mdays[month] + ((month == 1 && leap) ? 1 : 0);
            if (days < md) break;
            days -= md;
          }
          snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                   "%02d-%s %02d:%02d", days + 1, mnames[month], hours, mins);
        } else {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Time: unknown");
        }
        detail_count++;

        // Item 1: From
        if (entry.is_sent) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "From: You");
        } else if (entry.channel_idx >= 0) {
          // Channel messages: sender is anonymous, show as "someone"
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "From: someone");
        } else {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "From: %s", entry.origin);
        }
        detail_count++;

        // Item 1: To (channel or DM target)
        if (entry.channel_idx >= 0) {
          ChannelDetails cd;
          if (the_mesh.getChannel(entry.channel_idx, cd)) {
            snprintf(detail_items[detail_count], sizeof(detail_items[0]), "To: %s%s",
                     cd.name[0] == '#' ? "" : "#", cd.name);
          } else {
            snprintf(detail_items[detail_count], sizeof(detail_items[0]), "To: Ch %d", entry.channel_idx);
          }
        } else if (entry.is_sent) {
          if (entry.contact_name[0] != '\0') {
            snprintf(detail_items[detail_count], sizeof(detail_items[0]), "To: %s", entry.contact_name);
          } else {
            snprintf(detail_items[detail_count], sizeof(detail_items[0]), "To: broadcast");
          }
        } else {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "To: You");
        }
        detail_count++;

        // Item 2: Hops
        if (entry.is_sent) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Hops: local");
        } else if (entry.path_len == 0xFF) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Hops: DM (direct)");
        } else if (entry.path_len == 0) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Hops: 0 (direct)");
        } else {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Hops: %d", entry.path_len);
        }
        detail_count++;

        // Signal + Path lines (tracked for repeater nav)
        int signal_detail_idx = -1;
        int path_detail_idx = -1;
        int path_count_for_nav = 0;
        int heard_detail_idx = -1;
        int heard_count_for_nav = 0;
        int heard_rpt_map[MAX_PATH_SIZE]; // maps display index -> repeat_path[] index

        // Sent messages: Repeats, repeat Signal, repeat Path (Heard by)
        if (entry.is_sent && entry.heard_repeats > 0) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                   "Repeats: %d", entry.heard_repeats);
          detail_count++;

          if (entry.repeat_rssi != 0) {
            signal_detail_idx = detail_count;
            float rsnr = (float)entry.repeat_snr_x4 / 4.0f;
            snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                     "Signal: %d/%.1f", entry.repeat_rssi, rsnr);
            detail_count++;
          }

          if (entry.repeat_path_len > 0) {
            // Build heard-by list, only including repeaters with signal data
            int heard_shown = 0;
            char* p = detail_items[detail_count];
            int pos = snprintf(p, sizeof(detail_items[0]), "Heard by:");
            for (int i = 0; i < entry.repeat_path_len && pos < (int)sizeof(detail_items[0]) - 4; i++) {
              if (entry.repeat_path_rssi[i] == 0) continue; // skip no-signal (intermediate hops)
              if (heard_shown > 0) {
                pos += snprintf(p + pos, sizeof(detail_items[0]) - pos, ",");
              }
              heard_rpt_map[heard_shown] = i; // map display index -> repeat_path index
              pos += snprintf(p + pos, sizeof(detail_items[0]) - pos, "%02X", entry.repeat_path[i]);
              heard_shown++;
            }
            if (heard_shown > 0) {
              heard_detail_idx = detail_count;
              heard_count_for_nav = heard_shown;
              detail_count++;
            }
          }
        }

        // Sent DM: Delivery status
        if (entry.is_sent && entry.channel_idx < 0 && entry.contact_name[0] != '\0') {
          if (entry.delivered) {
            snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Delivered");
          } else if (entry.expected_ack != 0) {
            snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Pending...");
          }
          if (entry.delivered || entry.expected_ack != 0) detail_count++;
        }

        // Signal (RSSI/SNR) for received messages and delivered sent DMs
        if (entry.rssi != 0 && (!entry.is_sent || entry.delivered)) {
          signal_detail_idx = detail_count;
          float snr = (float)entry.snr_x4 / 4.0f;
          snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                   "%s %d/%.1f", entry.is_sent ? "ACK Signal:" : "Signal:", entry.rssi, snr);
          detail_count++;
        }

        // Received messages: Path (only if there are repeater hops)
        if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF) {
          path_detail_idx = detail_count;
          path_count_for_nav = entry.path_len;
          char* p = detail_items[detail_count];
          int pos = snprintf(p, 6, "Path:");
          for (int i = 0; i < entry.path_len && pos < 46; i++) {
            if (i > 0) {
              pos += snprintf(p + pos, sizeof(detail_items[0]) - pos, ">");
            }
            pos += snprintf(p + pos, sizeof(detail_items[0]) - pos, "%02X", entry.path[i]);
          }
          detail_count++;
        }

        // Reply options
        uint8_t reply_start_idx = detail_count;
        if (entry.channel_idx >= 0 && !entry.is_sent && _msg_reply_menu) {
          // Showing reply type selection for channel message
          snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                   "%sReply @Ch", _msg_reply_sel == 0 ? "> " : "  ");
          detail_count++;
          snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                   "%sReply DM", _msg_reply_sel == 1 ? "> " : "  ");
          detail_count++;
        } else if (entry.channel_idx >= 0 && !entry.is_sent) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Enter=Reply");
          detail_count++;
        } else if (entry.contact_name[0] != '\0' && !entry.is_sent) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Enter=Reply DM");
          detail_count++;
        } else if (entry.is_sent && entry.heard_repeats == 0 && !entry.delivered) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Enter=Resend");
          detail_count++;
        } else {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]), "Cancel=Back");
          detail_count++;
        }

        // Clamp scroll (ensure last items visible at most)
        int detail_visible = 4;
        int max_scroll = detail_count > detail_visible ? detail_count - detail_visible : 0;
        if (_msg_detail_scroll > max_scroll) _msg_detail_scroll = max_scroll;

        // Header
        display.setColor(DisplayDriver::YELLOW);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, "-- Msg Detail --");

        // Clamp path selection to valid range
        int nav_path_len = path_count_for_nav > 0 ? path_count_for_nav : heard_count_for_nav;
        if (_path_sel >= nav_path_len && nav_path_len > 0) _path_sel = nav_path_len - 1;
        if (nav_path_len == 0) _path_sel = -1;

        // Update Signal line for sent messages when a repeater is selected
        if (entry.is_sent && _path_sel >= 0 && signal_detail_idx >= 0 && heard_detail_idx >= 0) {
          // Map display index to actual repeat_path index
          int rp_idx = (_path_sel < heard_count_for_nav) ? heard_rpt_map[_path_sel] : _path_sel;
          uint8_t sel_hash = entry.repeat_path[rp_idx];
          int16_t sel_rssi = entry.repeat_path_rssi[rp_idx];
          float sel_snr = (float)entry.repeat_path_snr_x4[rp_idx] / 4.0f;
          if (sel_rssi != 0) {
            snprintf(detail_items[signal_detail_idx], sizeof(detail_items[0]),
                     "[%02X] %d/%.1f", sel_hash, sel_rssi, sel_snr);
          } else {
            snprintf(detail_items[signal_detail_idx], sizeof(detail_items[0]),
                     "[%02X] no signal", sel_hash);
          }
        }

        // Render visible items from scroll offset (11px spacing to fit 4 rows in 64px display)
        int y = TOP_BAR_H + 10;
        for (int i = _msg_detail_scroll; i < _msg_detail_scroll + detail_visible && i < detail_count; i++, y += 10) {
          bool is_path_line = ((i == path_detail_idx) || (i == heard_detail_idx));
          if (is_path_line) {
            // Render path/heard-by with per-repeater highlighting
            const char* text = detail_items[i];
            // Find the colon to print prefix
            const char* colon = strchr(text, ':');
            int prefix_len = colon ? (int)(colon - text + 1) : 0;
            display.setColor(DisplayDriver::LIGHT);
            display.setCursor(0, y);
            char prefix[12];
            if (prefix_len > 0 && prefix_len < (int)sizeof(prefix)) {
              memcpy(prefix, text, prefix_len);
              prefix[prefix_len] = '\0';
              display.print(prefix);
            }
            // Render each repeater hash with horizontal scroll to keep selected visible
            const char* p = text + prefix_len;
            int prefix_w = prefix_len > 0 ? display.getTextWidth(prefix) : 0;
            int max_x = display.width();
            int avail_w = max_x - prefix_w;

            // Pre-compute x positions of each repeater to determine scroll offset
            int rpt_positions[MAX_PATH_SIZE];
            int rpt_widths[MAX_PATH_SIZE];
            int sep_widths[MAX_PATH_SIZE]; // separator before each repeater (comma)
            int n_rpts = 0;
            int total_w = 0;
            {
              const char* q = p;
              while (*q && n_rpts < MAX_PATH_SIZE) {
                sep_widths[n_rpts] = 0;
                if ((*q >= '0' && *q <= '9') || (*q >= 'A' && *q <= 'F') || (*q >= 'a' && *q <= 'f')) {
                  char hex[3] = { q[0], q[1] ? q[1] : '\0', '\0' };
                  rpt_positions[n_rpts] = total_w;
                  rpt_widths[n_rpts] = display.getTextWidth(hex);
                  total_w += rpt_widths[n_rpts];
                  q += (q[1] && q[1] != '>' && q[1] != ',') ? 2 : 1;
                  n_rpts++;
                } else {
                  char sep[2] = { *q, '\0' };
                  int sw = display.getTextWidth(sep);
                  total_w += sw;
                  // Attribute separator width to next repeater
                  if (n_rpts < MAX_PATH_SIZE) sep_widths[n_rpts] = sw;
                  // Shift positions of subsequent repeaters
                  q++;
                }
              }
            }

            // Compute scroll offset to keep selected repeater visible
            int scroll_x = 0;
            if (_path_sel >= 0 && _path_sel < n_rpts && total_w > avail_w) {
              int sel_left = rpt_positions[_path_sel];
              int sel_right = sel_left + rpt_widths[_path_sel];
              if (sel_right - scroll_x > avail_w) {
                scroll_x = sel_right - avail_w;
              }
              if (sel_left < scroll_x) {
                scroll_x = sel_left;
              }
            }

            // Render with scroll offset
            int rpt_idx = 0;
            int cur_x = prefix_w;
            int content_x = 0; // position in content space (before scroll)
            while (*p) {
              if ((*p >= '0' && *p <= '9') || (*p >= 'A' && *p <= 'F') || (*p >= 'a' && *p <= 'f')) {
                char hex[3] = { p[0], p[1] ? p[1] : '\0', '\0' };
                int hw = display.getTextWidth(hex);
                int draw_x = prefix_w + content_x - scroll_x;
                if (draw_x >= prefix_w && draw_x + hw <= max_x) {
                  if (rpt_idx == _path_sel) {
                    display.setColor(DisplayDriver::YELLOW);
                    display.fillRect(draw_x, y, hw + 1, 10);
                    display.setColor(DisplayDriver::DARK);
                    display.setCursor(draw_x, y);
                    display.print(hex);
                  } else {
                    display.setColor(DisplayDriver::LIGHT);
                    display.setCursor(draw_x, y);
                    display.print(hex);
                  }
                }
                content_x += hw;
                p += (p[1] && p[1] != '>' && p[1] != ',') ? 2 : 1;
                rpt_idx++;
              } else {
                char sep[2] = { *p, '\0' };
                int sw = display.getTextWidth(sep);
                int draw_x = prefix_w + content_x - scroll_x;
                if (draw_x >= prefix_w && draw_x + sw <= max_x) {
                  display.setColor(DisplayDriver::LIGHT);
                  display.setCursor(draw_x, y);
                  display.print(sep);
                }
                content_x += sw;
                p++;
              }
            }
          } else {
            if (i < separator_idx) {
              display.setColor(entry.is_sent ? DisplayDriver::YELLOW : DisplayDriver::GREEN);
            } else if (i >= reply_start_idx) {
              display.setColor(DisplayDriver::GREEN);
            } else {
              display.setColor(DisplayDriver::LIGHT);
            }
            display.drawTextEllipsized(0, y, display.width(), detail_items[i]);
          }
        }
      } else {
        // Rebuild filter list and compute filtered counts
        rebuildMsgFilters();
        int filtered_total = countFilteredMsgs();
        if (_msg_sel >= filtered_total && filtered_total > 0) _msg_sel = filtered_total - 1;

        // Clear UI unread counter when viewing messages
        if (filtered_total > 0) _task->clearUnread();

        display.setColor(DisplayDriver::YELLOW);
        char hdr[40];
        // Show filter name in header
        if (_msg_filter == 0) {
          snprintf(hdr, sizeof(hdr), "- All (%d/%d) -",
                   filtered_total > 0 ? _msg_sel + 1 : 0, filtered_total);
        } else {
          int fval = _msg_filter_channels[_msg_filter];
          if (fval >= 0) {
            // Channel filter
            ChannelDetails cd;
            const char* ch_name = "Ch?";
            if (the_mesh.getChannel(fval, cd)) {
              ch_name = cd.name;
            }
            snprintf(hdr, sizeof(hdr), "- %s%s (%d/%d) -",
                     ch_name[0] == '#' ? "" : "#", ch_name,
                     filtered_total > 0 ? _msg_sel + 1 : 0, filtered_total);
          } else {
            // DM filter
            int dm_idx = -fval - 2;
            const char* dm_name = (dm_idx >= 0 && dm_idx < 4) ? _msg_filter_dm_names[dm_idx] : "?";
            snprintf(hdr, sizeof(hdr), "- DM:%s (%d/%d) -",
                     dm_name, filtered_total > 0 ? _msg_sel + 1 : 0, filtered_total);
          }
        }
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, hdr);

        if (_msg_compose_menu) {
          // === Quick-send compose menu overlay ===
          // Build list of non-empty presets
          int preset_count = 0;
          int preset_indices[PRESET_MSG_COUNT];
          for (int i = 0; i < PRESET_MSG_COUNT; i++) {
            if (i == PRESET_GPS_INDEX) continue;  // skip GPS slot
            if (preset_messages[i] && preset_messages[i][0] != '\0') {
              preset_indices[preset_count++] = i;
            }
          }
          int total_items = 1 + preset_count;  // Keyboard + presets
          int visible = 4;
          int scroll_top = 0;
          if (_msg_compose_sel >= visible) scroll_top = _msg_compose_sel - visible + 1;
          if (scroll_top > total_items - visible) scroll_top = total_items - visible;
          if (scroll_top < 0) scroll_top = 0;
          int y = TOP_BAR_H + 10;
          for (int v = scroll_top; v < scroll_top + visible && v < total_items; v++, y += 10) {
            bool is_sel = (v == (int)_msg_compose_sel);
            display.setColor(is_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            if (is_sel) {
              display.setCursor(0, y);
              display.print(">");
            }
            const char* label;
            if (v == 0) {
              label = "[Keyboard]";
            } else {
              label = preset_messages[preset_indices[v - 1]];
            }
            display.drawTextEllipsized(8, y, display.width() - 8, label);
          }
        } else if (filtered_total == 0) {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H + 24, "No messages yet");
          // Hint: compose is below
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 57, "...");
        } else if (_msg_filter > 0) {
          // === Multi-line rendering for filtered views ===
          bool is_channel_filter = (_msg_filter_channels[_msg_filter] >= 0);
          // Reset vscroll when selection changes
          if (_msg_sel != _msg_sel_prev) {
            _msg_sel_prev = _msg_sel;
            // Ensure selected message is visible by adjusting _msg_vscroll below
          }
          int char_w = display.getTextWidth("A");
          int avail_w = display.width() - 12; // text area width (after indent)
          int chars_per_line = (char_w > 0) ? avail_w / char_w : 20;
          int indent_px = 12;  // continuation line indent

          // Build display lines: walk messages from top, computing line counts
          // First compute which display line the selected message starts at
          int display_lines = 4;  // visible lines on screen
          int sel_start_line = 0;
          for (int v = 0; v < (int)_msg_sel && v < filtered_total; v++) {
            int log_idx = getFilteredMsgIndex(v);
            if (log_idx < 0) break;
            int buf_idx = (_task->_msg_log_next - 1 - log_idx + MSG_LOG_SIZE) % MSG_LOG_SIZE;
            auto& entry = _task->_msg_log[buf_idx];
            char prefix[8] = "";
            if (entry.is_sent && entry.channel_idx < 0 && entry.delivered) {
              snprintf(prefix, sizeof(prefix), "(D) ");
            } else if (entry.is_sent && entry.heard_repeats > 0) {
              snprintf(prefix, sizeof(prefix), "(%d) ", entry.heard_repeats);
            } else if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF) {
              snprintf(prefix, sizeof(prefix), "<%02X> ", entry.path[entry.path_len - 1]);
            }
            char line[104];
            if (is_channel_filter)
              snprintf(line, sizeof(line), "%s%s", prefix, entry.text);
            else
              snprintf(line, sizeof(line), "%s%s: %s", prefix, entry.origin, entry.text);
            int line_len = strlen(line);
            // First line has full width, continuation lines have reduced width
            int first_chars = (avail_w - 0) / char_w;  // no indent on first line
            int cont_chars = (avail_w - indent_px) / char_w;
            if (first_chars < 1) first_chars = 1;
            if (cont_chars < 1) cont_chars = 1;
            int msg_lines = 1;
            if (line_len > first_chars) {
              int remaining = line_len - first_chars;
              msg_lines += (remaining + cont_chars - 1) / cont_chars;
            }
            sel_start_line += msg_lines;
          }
          // Adjust vscroll to keep selected message visible
          // First try to show the end of the message
          {
            int log_idx = getFilteredMsgIndex(_msg_sel);
            if (log_idx >= 0) {
              int buf_idx = (_task->_msg_log_next - 1 - log_idx + MSG_LOG_SIZE) % MSG_LOG_SIZE;
              auto& entry = _task->_msg_log[buf_idx];
              char prefix[8] = "";
              if (entry.is_sent && entry.channel_idx < 0 && entry.delivered) {
                snprintf(prefix, sizeof(prefix), "(D) ");
              } else if (entry.is_sent && entry.heard_repeats > 0) {
                snprintf(prefix, sizeof(prefix), "(%d) ", entry.heard_repeats);
              } else if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF) {
                snprintf(prefix, sizeof(prefix), "<%02X> ", entry.path[entry.path_len - 1]);
              }
              char line[104];
              if (is_channel_filter)
                snprintf(line, sizeof(line), "%s%s", prefix, entry.text);
              else
                snprintf(line, sizeof(line), "%s%s: %s", prefix, entry.origin, entry.text);
              int line_len = strlen(line);
              int first_chars = avail_w / char_w;
              int cont_chars = (avail_w - indent_px) / char_w;
              if (first_chars < 1) first_chars = 1;
              if (cont_chars < 1) cont_chars = 1;
              int msg_lines = 1;
              if (line_len > first_chars) {
                int remaining = line_len - first_chars;
                msg_lines += (remaining + cont_chars - 1) / cont_chars;
              }
              int sel_end_line = sel_start_line + msg_lines;
              if (sel_end_line > _msg_vscroll + display_lines)
                _msg_vscroll = sel_end_line - display_lines;
            }
          }
          // Then ensure the START is always visible (takes precedence over end)
          if (sel_start_line < _msg_vscroll) _msg_vscroll = sel_start_line;
          if (_msg_vscroll < 0) _msg_vscroll = 0;

          // Now render: walk messages, skip display lines before _msg_vscroll
          int cur_dline = 0;
          int y = TOP_BAR_H + 10;
          int lines_drawn = 0;
          for (int v = 0; v < filtered_total && lines_drawn < display_lines; v++) {
            int log_idx = getFilteredMsgIndex(v);
            if (log_idx < 0) break;
            int buf_idx = (_task->_msg_log_next - 1 - log_idx + MSG_LOG_SIZE) % MSG_LOG_SIZE;
            auto& entry = _task->_msg_log[buf_idx];
            char prefix[8] = "";
            if (entry.is_sent && entry.channel_idx < 0 && entry.delivered) {
              snprintf(prefix, sizeof(prefix), "(D) ");
            } else if (entry.is_sent && entry.heard_repeats > 0) {
              snprintf(prefix, sizeof(prefix), "(%d) ", entry.heard_repeats);
            } else if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF) {
              snprintf(prefix, sizeof(prefix), "<%02X> ", entry.path[entry.path_len - 1]);
            }
            char line[104];
            if (is_channel_filter)
              snprintf(line, sizeof(line), "%s%s", prefix, entry.text);
            else
              snprintf(line, sizeof(line), "%s%s: %s", prefix, entry.origin, entry.text);
            int line_len = strlen(line);
            int first_chars = avail_w / char_w;
            int cont_chars = (avail_w - indent_px) / char_w;
            if (first_chars < 1) first_chars = 1;
            if (cont_chars < 1) cont_chars = 1;
            int msg_lines = 1;
            if (line_len > first_chars) {
              int remaining = line_len - first_chars;
              msg_lines += (remaining + cont_chars - 1) / cont_chars;
            }
            bool is_sel = (v == (int)_msg_sel);
            display.setColor(is_sel ? DisplayDriver::YELLOW :
                             (entry.is_sent ? DisplayDriver::YELLOW : DisplayDriver::GREEN));
            // Render each sub-line of this message
            int char_pos = 0;
            for (int sl = 0; sl < msg_lines && lines_drawn < display_lines; sl++) {
              if (cur_dline >= _msg_vscroll) {
                int x_off = (sl == 0) ? 0 : indent_px;
                int max_ch = (sl == 0) ? first_chars : cont_chars;
                // Draw selection marker on first visible line of selected msg
                if (is_sel && sl == 0) {
                  display.setCursor(0, y);
                  display.print(">");
                }
                // Extract substring
                char sub[104];
                int copy_len = line_len - char_pos;
                if (copy_len > max_ch) copy_len = max_ch;
                memcpy(sub, &line[char_pos], copy_len);
                sub[copy_len] = '\0';
                display.setCursor(x_off + (is_sel && sl == 0 ? 8 : (sl == 0 ? 8 : 0)), y);
                display.drawTextEllipsized(x_off + (sl == 0 ? 8 : 0), y,
                                           (sl == 0 ? avail_w : avail_w - indent_px + 12), sub);
                y += 10;
                lines_drawn++;
              }
              int max_ch = (sl == 0) ? first_chars : cont_chars;
              char_pos += max_ch;
              cur_dline++;
            }
            // Skip remaining lines if message extends past visible area
            if (cur_dline < _msg_vscroll) {
              // This message was entirely above the visible area, skip it
            }
          }
          // Hint: compose is below when on last message
          if (_msg_sel >= filtered_total - 1) {
            display.setColor(DisplayDriver::LIGHT);
            display.drawTextCentered(display.width() / 2, 57, "...");
          }
        } else if (_msg_target_menu) {
          // === Channel/DM target chooser overlay (All tab) ===
          int y = TOP_BAR_H + 16;
          const char* opts[2] = { "[Channel...]", "[DM...]" };
          for (int i = 0; i < 2; i++, y += 14) {
            display.setColor(i == _msg_target_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            if (i == _msg_target_sel) {
              display.setCursor(0, y);
              display.print(">");
            }
            display.setCursor(8, y);
            display.print(opts[i]);
          }
        } else {
          // === Original single-line rendering for "All" view ===
          // Reset scroll when selection changes
          if (_msg_sel != _msg_sel_prev) {
            _msg_scroll_px = 0;
            _msg_sel_prev = _msg_sel;
          }

          int visible = 4;
          int total = filtered_total;
          int scroll_top = 0;
          if (_msg_sel >= visible) scroll_top = _msg_sel - visible + 1;
          if (scroll_top > total - visible) scroll_top = total - visible;
          if (scroll_top < 0) scroll_top = 0;

          int avail_w = display.width() - 8; // pixels available for text after ">" marker
          int y = TOP_BAR_H + 10;
          for (int v = scroll_top; v < scroll_top + visible && v < total; v++, y += 10) {
            int log_idx = getFilteredMsgIndex(v);
            if (log_idx < 0) break;
            int buf_idx = (_task->_msg_log_next - 1 - log_idx + MSG_LOG_SIZE) % MSG_LOG_SIZE;
            auto& entry = _task->_msg_log[buf_idx];
            display.setColor(entry.is_sent ? DisplayDriver::YELLOW : DisplayDriver::GREEN);
            if (v == _msg_sel) {
              display.setCursor(0, y);
              display.print(">");
            }
            // Build prefix (fixed) and body (scrollable) separately
            char prefix[8] = "";
            char line[80];
            if (entry.is_sent && entry.channel_idx < 0 && entry.delivered) {
              snprintf(prefix, sizeof(prefix), "(D) ");
            } else if (entry.is_sent && entry.heard_repeats > 0) {
              snprintf(prefix, sizeof(prefix), "(%d) ", entry.heard_repeats);
            } else if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF) {
              snprintf(prefix, sizeof(prefix), "<%02X> ", entry.path[entry.path_len - 1]);
            }
            snprintf(line, sizeof(line), "%s: %s", entry.origin, entry.text);

            int prefix_w = display.getTextWidth(prefix);
            int body_x = 8 + prefix_w;
            int body_avail = avail_w - prefix_w;

            if (v == _msg_sel) {
              // Draw fixed prefix
              if (prefix[0]) {
                display.setCursor(8, y);
                display.print(prefix);
              }
              // Selected item: horizontal scroll if text exceeds width
              int text_w = display.getTextWidth(line);
              if (text_w > body_avail) {
                int char_w = display.getTextWidth("A");
                int char_off = (char_w > 0) ? _msg_scroll_px / char_w : 0;
                int line_len = strlen(line);
                if (char_off >= line_len) {
                  _msg_scroll_px = 0;
                  char_off = 0;
                }
                display.drawTextEllipsized(body_x, y, body_avail, &line[char_off]);
                _msg_scroll_px += char_w;
              } else {
                display.drawTextEllipsized(body_x, y, body_avail, line);
              }
            } else {
              // Non-selected: draw prefix + body together, ellipsized
              char full[88];
              snprintf(full, sizeof(full), "%s%s", prefix, line);
              display.drawTextEllipsized(8, y, avail_w, full);
            }
          }
          // Hint: compose is below when on last message
          if (_msg_sel >= filtered_total - 1) {
            display.setColor(DisplayDriver::LIGHT);
            display.drawTextCentered(display.width() / 2, 57, "...");
          }
        }
      }
    } else if (_page == HomePage::PRESETS) {
      display.setColor(DisplayDriver::YELLOW);
      display.setTextSize(1);

      if (_preset_target_choosing) {
        // Sub-menu: choose Channel or DM target
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, "-- Send To --");

        int y = TOP_BAR_H + 16;
        const char* opts[2] = { "[Channel...]", "[DM...]" };
        for (int i = 0; i < 2; i++, y += 14) {
          if (i == _preset_target_sel) {
            display.setColor(DisplayDriver::YELLOW);
            display.setCursor(0, y);
            display.print(">");
          }
          display.setColor(i == _preset_target_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
          display.setCursor(8, y);
          display.print(opts[i]);
        }
      } else if (_preset_edit_mode) {
        // Edit/Delete sub-menu: list all presets with delete option
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, "-- Edit Presets --");

        // Count non-empty presets
        int preset_count = 0;
        for (int i = 0; i < PRESET_MSG_COUNT; i++) {
          if (preset_buf[i][0] != '\0') preset_count++;
        }
        if (_preset_edit_sel >= preset_count) _preset_edit_sel = preset_count > 0 ? preset_count - 1 : 0;

        if (preset_count == 0) {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H + 24, "No presets");
        } else {
          int visible = 4;
          int scroll_top = 0;
          if (_preset_edit_sel >= visible) scroll_top = _preset_edit_sel - visible + 1;
          if (scroll_top > preset_count - visible) scroll_top = preset_count - visible;
          if (scroll_top < 0) scroll_top = 0;

          int y = TOP_BAR_H + 10;
          int vi = 0;
          for (int i = 0; i < PRESET_MSG_COUNT && vi < scroll_top + visible; i++) {
            if (preset_buf[i][0] == '\0') continue;
            if (vi < scroll_top) { vi++; continue; }
            if (vi == _preset_edit_sel) {
              display.setColor(DisplayDriver::YELLOW);
              display.setCursor(0, y);
              display.print(">");
            }
            display.setColor(vi == _preset_edit_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            display.drawTextEllipsized(8, y, display.width() - 8, preset_buf[i]);
            y += 10;
            vi++;
          }
        }
      } else {
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, "-- Quick Msg --");

        // items: presets + [Compose] + [Reply DM] + [Send GPS DM] + [Channel Msg] + [Add Msg] + [Edit/Delete]
        int total_items = PRESET_MSG_COUNT + 6;
        int visible = 4;
        int scroll_top = 0;
        if (_preset_sel >= visible) scroll_top = _preset_sel - visible + 1;
        if (scroll_top > total_items - visible) scroll_top = total_items - visible;
        if (scroll_top < 0) scroll_top = 0;

        int y = TOP_BAR_H + 10;
        for (int i = scroll_top; i < scroll_top + visible && i < total_items; i++, y += 10) {
          if (i == _preset_sel && !_sb_active) {
            display.setColor(DisplayDriver::YELLOW);
            display.setCursor(0, y);
            display.print(">");
          }
          display.setColor(i == _preset_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
          const char* label;
          if (i < PRESET_MSG_COUNT) {
            label = (i == PRESET_GPS_INDEX) ? "Send Location" : preset_messages[i];
          } else if (i == PRESET_MSG_COUNT) {
            label = "[Compose...]";
          } else if (i == PRESET_MSG_COUNT + 1) {
            label = "[Reply DM...]";
          } else if (i == PRESET_MSG_COUNT + 2) {
            label = "[Send GPS DM...]";
          } else if (i == PRESET_MSG_COUNT + 3) {
            label = "[Channel Msg...]";
          } else if (i == PRESET_MSG_COUNT + 4) {
            label = "[+ Add Message]";
          } else {
            label = "[Edit/Delete]";
          }
          display.drawTextEllipsized(8, y, display.width() - 8, label);
        }
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      display.setColor(DisplayDriver::GREEN);
      int y = TOP_BAR_H + 2;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 10) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        int secs = _rtc->getCurrentTime() - a->recv_timestamp;
        if (secs < 60) {
          sprintf(tmp, "%ds", secs);
          _needs_fast_refresh = true;
        } else if (secs < 60*60) {
          sprintf(tmp, "%dm", secs / 60);
        } else {
          sprintf(tmp, "%dh", secs / (60*60));
        }
        
        int timestamp_width = display.getTextWidth(tmp);
        int max_name_width = display.width() - timestamp_width - 1;
        
        char filtered_recent_name[sizeof(a->name)];
        display.translateUTF8ToBlocks(filtered_recent_name, a->name, sizeof(filtered_recent_name));
        display.drawTextEllipsized(0, y, max_name_width, filtered_recent_name);
        display.setCursor(display.width() - timestamp_width - 1, y);
        display.print(tmp);
      }
    } else if (_page == HomePage::TRACE) {
      display.setTextSize(1);
      if (_ct_gps_pending || _ct_gps_done || _ct_gps_no_fix) {
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Location: %s", _ct_target_name);
        display.setCursor(0, TOP_BAR_H + 6);
        display.printWordWrap(tmp, display.width());
        int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
        if (_ct_gps_done) {
          ContactCache* cc = findCache(_ct_path_key);
          display.setColor(DisplayDriver::LIGHT);
          if (cc && cc->has_gps) {
            snprintf(tmp, sizeof(tmp), "%.5f, %.5f", cc->gps_lat, cc->gps_lon);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextEllipsized(0, cy, display.width(), tmp);
            if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height()) {
              display.setColor(DisplayDriver::GREEN);
              display.drawTextCentered(display.width() / 2, cy + 10, "ENTER to navigate");
            }
          } else {
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextEllipsized(0, cy, display.width(), "No GPS data");
          }
        } else if (_ct_gps_no_fix) {
          display.setColor(DisplayDriver::LIGHT);
          if (cy >= TOP_BAR_H && cy < display.height())
            display.drawTextCentered(display.width() / 2, cy, "No GPS fix");
        } else {
          display.setColor(DisplayDriver::LIGHT);
          if (cy >= TOP_BAR_H && cy < display.height())
            display.drawTextCentered(display.width() / 2, cy, "Requesting...");
          if (millis() > _ct_gps_timeout) {
            _ct_gps_pending = false;
            reselectContact(_ct_target_name);
            _ct_action = true;
            _task->showAlert("Location timeout", 1200);
          }
        }
      } else if (_ct_ping_pending || _ct_ping_done) {
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Ping: %s", _ct_target_name);
        display.setCursor(0, TOP_BAR_H + 6);
        display.printWordWrap(tmp, display.width());
        int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
        if (_ct_ping_done) {
          display.setColor(DisplayDriver::GREEN);
          snprintf(tmp, sizeof(tmp), "RTT: %lums", (unsigned long)_ct_ping_latency);
          if (cy >= TOP_BAR_H && cy < display.height())
            display.drawTextEllipsized(0, cy, display.width(), tmp);
          snprintf(tmp, sizeof(tmp), "SNR there: %.1fdB", _ct_ping_snr_there);
          if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
            display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
          snprintf(tmp, sizeof(tmp), "SNR back: %.1fdB", _ct_ping_snr_back);
          if (cy + 20 >= TOP_BAR_H && cy + 20 < display.height())
            display.drawTextEllipsized(0, cy + 20, display.width(), tmp);
        } else {
          display.setColor(DisplayDriver::LIGHT);
          if (cy >= TOP_BAR_H && cy < display.height())
            display.drawTextCentered(display.width() / 2, cy, "Pinging...");
          if (millis() > _ct_ping_timeout) {
            _ct_ping_pending = false;
            reselectContact(_ct_target_name);
            _ct_action = true;
            _task->showAlert("Ping timeout", 1200);
          }
        }
      } else if (_ct_status_pending || _ct_status_done) {
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Status: %s", _ct_target_name);
        display.setCursor(0, TOP_BAR_H + 6);
        display.printWordWrap(tmp, display.width());
        int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
        if (_ct_status_done) {
          ContactCache* cc = findCache(_ct_path_key);
          display.setColor(DisplayDriver::LIGHT);
          if (cc && cc->has_status) {
            uint32_t up = cc->uptime_secs;
            uint32_t d = up / 86400; uint32_t h = (up % 86400) / 3600; uint32_t m = (up % 3600) / 60;
            if (d > 0)
              snprintf(tmp, sizeof(tmp), "Up: %lud %luh %lum", d, h, m);
            else
              snprintf(tmp, sizeof(tmp), "Up: %luh %lum", h, m);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextEllipsized(0, cy, display.width(), tmp);
            snprintf(tmp, sizeof(tmp), "Power: %umV", cc->batt_mv);
            if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
              display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
          } else {
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextEllipsized(0, cy, display.width(), "No data");
          }
        } else {
          display.setColor(DisplayDriver::LIGHT);
          if (cy >= TOP_BAR_H && cy < display.height())
            display.drawTextCentered(display.width() / 2, cy, "Waiting...");
          if (millis() > _ct_status_timeout) {
            _ct_status_pending = false;
            reselectContact(_ct_target_name);
            _ct_action = true;
            _task->showAlert("Status timeout", 1200);
          }
        }
      } else if (_ct_telem_pending || _ct_telem_done) {
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Telemetry: %s", _ct_target_name);
        display.setCursor(0, TOP_BAR_H + 6);
        display.printWordWrap(tmp, display.width());
        int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
        if (_ct_telem_done) {
          ContactCache* cc = findCache(_ct_path_key);
          display.setColor(DisplayDriver::LIGHT);
          if (cc && cc->has_telem) {
            snprintf(tmp, sizeof(tmp), "Batt: %.2fV", cc->voltage);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextEllipsized(0, cy, display.width(), tmp);
            if (cc->temperature > -274) {
              snprintf(tmp, sizeof(tmp), "Temp: %.1fC", cc->temperature);
              if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
                display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
            }
          } else {
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextEllipsized(0, cy, display.width(), "No data");
          }
        } else {
          display.setColor(DisplayDriver::LIGHT);
          if (cy >= TOP_BAR_H && cy < display.height())
            display.drawTextCentered(display.width() / 2, cy, "Waiting...");
          if (millis() > _ct_telem_timeout) {
            _ct_telem_pending = false;
            reselectContact(_ct_target_name);
            _ct_action = true;
            _task->showAlert("Telem timeout", 1200);
          }
        }
      } else if (_ct_path_pending) {
        // Path discovery in progress
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Finding: %s", _ct_target_name);
        display.setCursor(0, TOP_BAR_H + 6);
        display.printWordWrap(tmp, display.width());
        int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
        if (_ct_path_found) {
          ContactCache* cc = findCache(_ct_path_key);
          if (cc && cc->has_path_info) {
            if (cc->path_hops > 0) {
              snprintf(tmp, sizeof(tmp), "Found! %d hops", cc->path_hops);
              display.setColor(DisplayDriver::YELLOW);
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), tmp);
              // Hop hex chain
              char hops[48] = "";
              int pos = 0;
              for (int h = 0; h < cc->path_hops && pos < 44; h++) {
                pos += snprintf(hops + pos, sizeof(hops) - pos, "%s%02X", h > 0 ? " " : "", cc->path[h]);
              }
              display.setColor(DisplayDriver::LIGHT);
              if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
                display.drawTextEllipsized(0, cy + 10, display.width(), hops);
              float snr_f = (float)cc->snr_x4 / 4.0f;
              snprintf(tmp, sizeof(tmp), "RSSI:%d SNR:%.1f", cc->rssi, snr_f);
              if (cy + 20 >= TOP_BAR_H && cy + 20 < display.height())
                display.drawTextEllipsized(0, cy + 20, display.width(), tmp);
            } else {
              display.setColor(DisplayDriver::YELLOW);
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), "Found! Direct");
              float snr_f = (float)cc->snr_x4 / 4.0f;
              snprintf(tmp, sizeof(tmp), "RSSI:%d SNR:%.1f", cc->rssi, snr_f);
              display.setColor(DisplayDriver::LIGHT);
              if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
                display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
            }
          } else {
            display.setColor(DisplayDriver::YELLOW);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextCentered(display.width() / 2, cy, "Path updated!");
          }
        } else {
          display.setColor(DisplayDriver::LIGHT);
          if (cy >= TOP_BAR_H && cy < display.height())
            display.drawTextCentered(display.width() / 2, cy, "Searching...");
          if (millis() > _ct_path_timeout) {
            _ct_path_pending = false;
            reselectContact(_ct_target_name);
            _ct_action = true;
            _task->showAlert("No path found", 1200);
          }
        }
      } else if (_ct_action) {
        // Contact detail + action menu
        ContactInfo ci;
        if (getContactByKey(_ct_action_key, ci)) {
          display.setColor(DisplayDriver::YELLOW);
          char ct_hdr[48];
          snprintf(ct_hdr, sizeof(ct_hdr), "<%02X> %s", ci.id.pub_key[0], ci.name);
          display.drawTextEllipsized(0, TOP_BAR_H, display.width(), ct_hdr);

          // Build combined list: actions first, then cached info lines
          const char* items[14];
          bool item_is_action[14];
          int item_count = 0;

          // Actions
          if (ci.type == ADV_TYPE_CHAT) {
            items[item_count] = "Send DM"; item_is_action[item_count++] = true;
            items[item_count] = "Find Path"; item_is_action[item_count++] = true;
            items[item_count] = "Telemetry"; item_is_action[item_count++] = true;
#if ENV_INCLUDE_GPS == 1
            items[item_count] = "Send GPS"; item_is_action[item_count++] = true;
            items[item_count] = "Req Location"; item_is_action[item_count++] = true;
#endif
          } else if (ci.type == ADV_TYPE_REPEATER) {
            items[item_count] = "Ping"; item_is_action[item_count++] = true;
            items[item_count] = "Find Path"; item_is_action[item_count++] = true;
            items[item_count] = "Telemetry"; item_is_action[item_count++] = true;
            items[item_count] = "Status"; item_is_action[item_count++] = true;
          } else {
            items[item_count] = "Find Path"; item_is_action[item_count++] = true;
            items[item_count] = "Telemetry"; item_is_action[item_count++] = true;
          }
#if ENV_INCLUDE_GPS == 1
          {
            // Show Navigate if contact has GPS (from advert or telemetry cache)
            ContactCache* ncc = findCache(ci.id.pub_key);
            bool has_advert_gps = (ci.gps_lat != 0 || ci.gps_lon != 0);
            bool has_cached_gps = (ncc && ncc->has_gps);
            if (has_advert_gps || has_cached_gps) {
              items[item_count] = "Navigate"; item_is_action[item_count++] = true;
            }
          }
#endif
          _ct_action_count = item_count;  // number of selectable actions

          // Cached info lines (not selectable)
          static char info_lines[6][40];

          // Last heard
          {
            uint32_t now = the_mesh.getRTCClock()->getCurrentTime();
            if (ci.lastmod > 0 && now >= ci.lastmod) {
              uint32_t ago = now - ci.lastmod;
              if (ago < 60) snprintf(info_lines[5], sizeof(info_lines[5]), "Heard: %lus ago", (unsigned long)ago);
              else if (ago < 3600) snprintf(info_lines[5], sizeof(info_lines[5]), "Heard: %lum ago", (unsigned long)(ago / 60));
              else if (ago < 86400) snprintf(info_lines[5], sizeof(info_lines[5]), "Heard: %luh ago", (unsigned long)(ago / 3600));
              else snprintf(info_lines[5], sizeof(info_lines[5]), "Heard: %lud ago", (unsigned long)(ago / 86400));
            } else {
              strcpy(info_lines[5], "Heard: never");
            }
            items[item_count] = info_lines[5]; item_is_action[item_count++] = false;
          }
          ContactCache* cc = findCache(ci.id.pub_key);
          if (cc && cc->has_path_info) {
            if (cc->path_hops > 0) {
              char hops[24] = "";
              int pos = 0;
              for (int h = 0; h < cc->path_hops && pos < 20; h++) {
                pos += snprintf(hops + pos, sizeof(hops) - pos, "%s%02X", h > 0 ? " " : "", cc->path[h]);
              }
              snprintf(info_lines[0], sizeof(info_lines[0]), "Path: %s", hops);
            } else {
              strcpy(info_lines[0], "Path: Direct");
            }
            items[item_count] = info_lines[0]; item_is_action[item_count++] = false;
            float snr_f = (float)cc->snr_x4 / 4.0f;
            snprintf(info_lines[1], sizeof(info_lines[1]), "RSSI:%d SNR:%.1f", cc->rssi, snr_f);
            items[item_count] = info_lines[1]; item_is_action[item_count++] = false;
          }
          if (cc && cc->has_telem) {
            if (cc->temperature > -274) {
              snprintf(info_lines[2], sizeof(info_lines[2]), "%.2fV  %.1fC", cc->voltage, cc->temperature);
            } else {
              snprintf(info_lines[2], sizeof(info_lines[2]), "Batt: %.2fV", cc->voltage);
            }
            items[item_count] = info_lines[2]; item_is_action[item_count++] = false;
          }
          if (cc && cc->has_status) {
            uint32_t up = cc->uptime_secs;
            uint32_t d = up / 86400; uint32_t h = (up % 86400) / 3600; uint32_t m = (up % 3600) / 60;
            if (d > 0)
              snprintf(info_lines[3], sizeof(info_lines[3]), "Up: %lud %luh %lum", d, h, m);
            else
              snprintf(info_lines[3], sizeof(info_lines[3]), "Up: %luh %lum", h, m);
            items[item_count] = info_lines[3]; item_is_action[item_count++] = false;
            snprintf(info_lines[4], sizeof(info_lines[4]), "RPwr: %umV", cc->batt_mv);
            items[item_count] = info_lines[4]; item_is_action[item_count++] = false;
          }

          if (_ct_action_sel >= _ct_action_count && _ct_action_count > 0)
            _ct_action_sel = _ct_action_count - 1;
          if (_ct_detail_scroll > item_count - 1 && item_count > 0)
            _ct_detail_scroll = item_count - 1;

          int visible = 4;
          int y = TOP_BAR_H + 10;
          for (int i = _ct_detail_scroll; i < _ct_detail_scroll + visible && i < item_count; i++, y += 10) {
            if (item_is_action[i] && i == _ct_action_sel) {
              display.setColor(DisplayDriver::YELLOW);
              display.setCursor(0, y);
              display.print(">");
            }
            display.setColor(item_is_action[i] && i == _ct_action_sel ? DisplayDriver::YELLOW :
                           item_is_action[i] ? DisplayDriver::LIGHT : DisplayDriver::LIGHT);
            display.drawTextEllipsized(8, y, display.width() - 8, items[i]);
          }
        }
      } else {
        // Contact list
        rebuildContactsSorted();
        display.setColor(DisplayDriver::YELLOW);
        const char* filter_name = _ct_filter == 0 ? "All" : _ct_filter == 1 ? "Contacts" : "Repeaters";
        snprintf(tmp, sizeof(tmp), "-- %s (%d) --", filter_name, _ct_count);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, tmp);

        if (_ct_count == 0) {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H + 22, "No contacts");
        } else {
          int visible = 4;
          int scroll_top = 0;
          if (_ct_sel >= visible) scroll_top = _ct_sel - visible + 1;
          if (scroll_top > _ct_count - visible) scroll_top = _ct_count - visible;
          if (scroll_top < 0) scroll_top = 0;

          int y = TOP_BAR_H + 10;
          for (int v = scroll_top; v < scroll_top + visible && v < _ct_count; v++, y += 10) {
            ContactInfo ci;
            if (the_mesh.getContactByIdx(_ct_sorted[v], ci)) {
              if (v == _ct_sel && !_sb_active) {
                display.setColor(DisplayDriver::YELLOW);
                display.setCursor(0, y);
                display.print(">");
              }
              display.setColor(v == _ct_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
              // Hash prefix
              uint8_t hash = ci.id.pub_key[0];
              char prefix[8];
              snprintf(prefix, sizeof(prefix), "<%02X>", hash);
              display.setCursor(8, y);
              display.print(prefix);
              int prefix_w = display.getTextWidth(prefix);
              // Name with suffix
              char line[48];
              bool is_fav = (ci.flags & 0x01) != 0;
              char suffix[12] = "";
              if (ci.type == ADV_TYPE_REPEATER) {
                if (ci.out_path_len > 0) snprintf(suffix, sizeof(suffix), "R:%d", ci.out_path_len);
                else if (ci.out_path_len == 0) strcpy(suffix, "R:D");
                else strcpy(suffix, "R:?");
              } else if (ci.type == ADV_TYPE_ROOM) {
                strcpy(suffix, "Rm");
              } else if (ci.type == ADV_TYPE_SENSOR) {
                strcpy(suffix, "S");
              } else {
                if (ci.out_path_len > 0) snprintf(suffix, sizeof(suffix), "%d", ci.out_path_len);
                else if (ci.out_path_len == 0) strcpy(suffix, "D");
                else strcpy(suffix, "?");
              }
              snprintf(line, sizeof(line), "%s%s [%s]", is_fav ? "*" : "", ci.name, suffix);
              // Compute contact staleness from lastmod
              char age_buf[8] = "";
              if (ci.lastmod > 0) {
                int secs = _rtc->getCurrentTime() - ci.lastmod;
                if (secs < 0) secs = 0;
                if (secs < 60) { snprintf(age_buf, sizeof(age_buf), "%ds", secs); _needs_fast_refresh = true; }
                else if (secs < 3600) snprintf(age_buf, sizeof(age_buf), "%dm", secs / 60);
                else if (secs < 86400) snprintf(age_buf, sizeof(age_buf), "%dh", secs / 3600);
                else snprintf(age_buf, sizeof(age_buf), "%dd", secs / 86400);
              }
              int age_w = age_buf[0] ? display.getTextWidth(age_buf) + 4 : 0;
              int name_x = 8 + prefix_w + 2;
              int name_avail = display.width() - name_x - age_w;
              // Truncate name if needed (no ellipsis)
              display.setCursor(name_x, y);
              if (display.getTextWidth(line) <= name_avail) {
                display.print(line);
              } else {
                // Print chars that fit
                char trunc[48];
                strncpy(trunc, line, sizeof(trunc));
                trunc[sizeof(trunc) - 1] = '\0';
                int len = strlen(trunc);
                while (len > 0 && display.getTextWidth(trunc) > name_avail) {
                  trunc[--len] = '\0';
                }
                display.print(trunc);
              }
              if (age_buf[0]) {
                display.setCursor(display.width() - display.getTextWidth(age_buf) - 1, y);
                display.print(age_buf);
              }
            }
          }
        }
      }
    } else if (_page == HomePage::NEARBY) {
      display.setTextSize(1);
      // Reuse the same pending sub-state rendering as contacts page
      if (_ct_path_pending || _ct_path_found ||
          _ct_telem_pending || _ct_telem_done ||
          _ct_status_pending || _ct_status_done ||
          _ct_ping_pending || _ct_ping_done ||
          _ct_gps_pending || _ct_gps_done || _ct_gps_no_fix) {
        // Render pending sub-states (same as contacts page)
        if (_ct_gps_pending || _ct_gps_done || _ct_gps_no_fix) {
          display.setColor(DisplayDriver::YELLOW);
          snprintf(tmp, sizeof(tmp), "Location: %s", _ct_target_name);
          display.setCursor(0, TOP_BAR_H + 6);
          display.printWordWrap(tmp, display.width());
          int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
          if (_ct_gps_done) {
            ContactCache* cc = findCache(_ct_path_key);
            display.setColor(DisplayDriver::LIGHT);
            if (cc && cc->has_gps) {
              snprintf(tmp, sizeof(tmp), "%.5f, %.5f", cc->gps_lat, cc->gps_lon);
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), tmp);
              if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height()) {
                display.setColor(DisplayDriver::GREEN);
                display.drawTextCentered(display.width() / 2, cy + 10, "ENTER to navigate");
              }
            } else {
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), "No GPS data");
            }
          } else if (_ct_gps_no_fix) {
            display.setColor(DisplayDriver::LIGHT);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextCentered(display.width() / 2, cy, "No GPS fix");
          } else {
            display.setColor(DisplayDriver::LIGHT);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextCentered(display.width() / 2, cy, "Requesting...");
            if (millis() > _ct_gps_timeout) { _ct_gps_pending = false; _scan_action = true; _task->showAlert("Location timeout", 1200); }
          }
        } else if (_ct_ping_pending || _ct_ping_done) {
          display.setColor(DisplayDriver::YELLOW);
          snprintf(tmp, sizeof(tmp), "Ping: %s", _ct_target_name);
          display.setCursor(0, TOP_BAR_H + 6);
          display.printWordWrap(tmp, display.width());
          int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
          if (_ct_ping_done) {
            display.setColor(DisplayDriver::GREEN);
            snprintf(tmp, sizeof(tmp), "RTT: %lums", (unsigned long)_ct_ping_latency);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextEllipsized(0, cy, display.width(), tmp);
            snprintf(tmp, sizeof(tmp), "SNR there: %.1fdB", _ct_ping_snr_there);
            if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
              display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
            snprintf(tmp, sizeof(tmp), "SNR back: %.1fdB", _ct_ping_snr_back);
            if (cy + 20 >= TOP_BAR_H && cy + 20 < display.height())
              display.drawTextEllipsized(0, cy + 20, display.width(), tmp);
          } else {
            display.setColor(DisplayDriver::LIGHT);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextCentered(display.width() / 2, cy, "Pinging...");
            if (millis() > _ct_ping_timeout) { _ct_ping_pending = false; _scan_action = true; _task->showAlert("Ping timeout", 1200); }
          }
        } else if (_ct_status_pending || _ct_status_done) {
          display.setColor(DisplayDriver::YELLOW);
          snprintf(tmp, sizeof(tmp), "Status: %s", _ct_target_name);
          display.setCursor(0, TOP_BAR_H + 6);
          display.printWordWrap(tmp, display.width());
          int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
          if (_ct_status_done) {
            ContactCache* cc = findCache(_ct_path_key);
            display.setColor(DisplayDriver::LIGHT);
            if (cc && cc->has_status) {
              uint32_t up = cc->uptime_secs;
              uint32_t d = up / 86400; uint32_t h = (up % 86400) / 3600; uint32_t m = (up % 3600) / 60;
              if (d > 0) snprintf(tmp, sizeof(tmp), "Up: %lud %luh %lum", d, h, m);
              else snprintf(tmp, sizeof(tmp), "Up: %luh %lum", h, m);
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), tmp);
              snprintf(tmp, sizeof(tmp), "Power: %umV", cc->batt_mv);
              if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
                display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
            } else {
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), "No data");
            }
          } else {
            display.setColor(DisplayDriver::LIGHT);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextCentered(display.width() / 2, cy, "Waiting...");
            if (millis() > _ct_status_timeout) { _ct_status_pending = false; _scan_action = true; _task->showAlert("Status timeout", 1200); }
          }
        } else if (_ct_telem_pending || _ct_telem_done) {
          display.setColor(DisplayDriver::YELLOW);
          snprintf(tmp, sizeof(tmp), "Telemetry: %s", _ct_target_name);
          display.setCursor(0, TOP_BAR_H + 6);
          display.printWordWrap(tmp, display.width());
          int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
          if (_ct_telem_done) {
            ContactCache* cc = findCache(_ct_path_key);
            display.setColor(DisplayDriver::LIGHT);
            if (cc && cc->has_telem) {
              snprintf(tmp, sizeof(tmp), "Batt: %.2fV", cc->voltage);
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), tmp);
              if (cc->temperature > -274) {
                snprintf(tmp, sizeof(tmp), "Temp: %.1fC", cc->temperature);
                if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
                  display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
              }
            } else {
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextEllipsized(0, cy, display.width(), "No data");
            }
          } else {
            display.setColor(DisplayDriver::LIGHT);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextCentered(display.width() / 2, cy, "Waiting...");
            if (millis() > _ct_telem_timeout) { _ct_telem_pending = false; _scan_action = true; _task->showAlert("Telem timeout", 1200); }
          }
        } else if (_ct_path_pending) {
          display.setColor(DisplayDriver::YELLOW);
          snprintf(tmp, sizeof(tmp), "Finding: %s", _ct_target_name);
          display.setCursor(0, TOP_BAR_H + 6);
          display.printWordWrap(tmp, display.width());
          int cy = TOP_BAR_H + 6 + getTextLines(display, tmp, display.width()) * 10 - _ct_detail_scroll * 10;
          if (_ct_path_found) {
            ContactCache* cc = findCache(_ct_path_key);
            if (cc && cc->has_path_info) {
              if (cc->path_hops > 0) {
                snprintf(tmp, sizeof(tmp), "Found! %d hops", cc->path_hops);
                display.setColor(DisplayDriver::YELLOW);
                if (cy >= TOP_BAR_H && cy < display.height())
                  display.drawTextEllipsized(0, cy, display.width(), tmp);
              } else {
                display.setColor(DisplayDriver::YELLOW);
                if (cy >= TOP_BAR_H && cy < display.height())
                  display.drawTextEllipsized(0, cy, display.width(), "Found! Direct");
              }
              float snr_f = (float)cc->snr_x4 / 4.0f;
              snprintf(tmp, sizeof(tmp), "RSSI:%d SNR:%.1f", cc->rssi, snr_f);
              display.setColor(DisplayDriver::LIGHT);
              if (cy + 10 >= TOP_BAR_H && cy + 10 < display.height())
                display.drawTextEllipsized(0, cy + 10, display.width(), tmp);
            } else {
              display.setColor(DisplayDriver::YELLOW);
              if (cy >= TOP_BAR_H && cy < display.height())
                display.drawTextCentered(display.width() / 2, cy, "Path updated!");
            }
          } else {
            display.setColor(DisplayDriver::LIGHT);
            if (cy >= TOP_BAR_H && cy < display.height())
              display.drawTextCentered(display.width() / 2, cy, "Searching...");
            if (millis() > _ct_path_timeout) { _ct_path_pending = false; _scan_action = true; _task->showAlert("No path found", 1200); }
          }
        }
      } else if (_scan_action) {
        // Action menu for selected scan result
        ScanResult& sr = _scan_results[_scan_sel];
        display.setColor(DisplayDriver::YELLOW);
        char sr_hdr[48];
        snprintf(sr_hdr, sizeof(sr_hdr), "<%02X> %s", sr.pub_key[0], sr.name);
        display.drawTextEllipsized(0, TOP_BAR_H, display.width(), sr_hdr);

        const char* items[12];
        bool item_is_action[12];
        int item_count = 0;

        if (sr.in_contacts) {
          if (sr.node_type == ADV_TYPE_REPEATER) {
            items[item_count] = "Ping"; item_is_action[item_count++] = true;
          }
          items[item_count] = "Find Path"; item_is_action[item_count++] = true;
          items[item_count] = "Telemetry"; item_is_action[item_count++] = true;
          if (sr.node_type == ADV_TYPE_REPEATER) {
            items[item_count] = "Status"; item_is_action[item_count++] = true;
          }
#if ENV_INCLUDE_GPS == 1
          {
            ContactInfo ci;
            if (getContactByKey(sr.pub_key, ci)) {
              ContactCache* ncc = findCache(ci.id.pub_key);
              bool has_gps = (ci.gps_lat != 0 || ci.gps_lon != 0) || (ncc && ncc->has_gps);
              if (has_gps) {
                items[item_count] = "Navigate"; item_is_action[item_count++] = true;
              }
            }
          }
#endif
        }
        _scan_action_count = item_count;

        // Info lines (always shown)
        static char scan_info[4][40];
        float snr_f = (float)sr.snr_x4 / 4.0f;
        snprintf(scan_info[0], sizeof(scan_info[0]), "SNR: %.1f dB", snr_f);
        items[item_count] = scan_info[0]; item_is_action[item_count++] = false;
        snprintf(scan_info[1], sizeof(scan_info[1]), "RSSI: %d dBm", sr.rssi);
        items[item_count] = scan_info[1]; item_is_action[item_count++] = false;
        snprintf(scan_info[2], sizeof(scan_info[2]), "Hops: %d", sr.path_len);
        items[item_count] = scan_info[2]; item_is_action[item_count++] = false;
        snprintf(scan_info[3], sizeof(scan_info[3]), "Type: %s", sr.node_type == ADV_TYPE_REPEATER ? "Repeater" : "Sensor");
        items[item_count] = scan_info[3]; item_is_action[item_count++] = false;

        // Cached info from contacts
        if (sr.in_contacts) {
          ContactCache* cc = findCache(sr.pub_key);
          static char scan_cache_info[4][40];
          if (cc && cc->has_path_info) {
            snprintf(scan_cache_info[0], sizeof(scan_cache_info[0]), "Path RSSI:%d SNR:%.1f", cc->rssi, (float)cc->snr_x4 / 4.0f);
            items[item_count] = scan_cache_info[0]; item_is_action[item_count++] = false;
          }
          if (cc && cc->has_telem) {
            snprintf(scan_cache_info[1], sizeof(scan_cache_info[1]), "Batt: %.2fV", cc->voltage);
            items[item_count] = scan_cache_info[1]; item_is_action[item_count++] = false;
          }
          if (cc && cc->has_status) {
            uint32_t up = cc->uptime_secs;
            uint32_t d = up / 86400; uint32_t h = (up % 86400) / 3600; uint32_t m = (up % 3600) / 60;
            if (d > 0)
              snprintf(scan_cache_info[2], sizeof(scan_cache_info[2]), "Up: %lud %luh %lum", d, h, m);
            else
              snprintf(scan_cache_info[2], sizeof(scan_cache_info[2]), "Up: %luh %lum", h, m);
            items[item_count] = scan_cache_info[2]; item_is_action[item_count++] = false;
          }
        }

        if (_scan_action_sel >= _scan_action_count && _scan_action_count > 0)
          _scan_action_sel = _scan_action_count - 1;
        if (_scan_detail_scroll > item_count - 1 && item_count > 0)
          _scan_detail_scroll = item_count - 1;

        int visible = 4;
        int y = TOP_BAR_H + 10;
        for (int i = _scan_detail_scroll; i < _scan_detail_scroll + visible && i < item_count; i++, y += 10) {
          if (item_is_action[i] && i == _scan_action_sel) {
            display.setColor(DisplayDriver::YELLOW);
            display.setCursor(0, y);
            display.print(">");
          }
          display.setColor(item_is_action[i] && i == _scan_action_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
          display.drawTextEllipsized(8, y, display.width() - 8, items[i]);
        }
      } else {
        // Scan result list
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "-- Nearby (%d) --", _scan_count);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, tmp);

        if (_scan_count == 0) {
          display.setColor(DisplayDriver::LIGHT);
          if (_scan_active) {
            display.drawTextCentered(display.width() / 2, TOP_BAR_H + 22, "Scanning...");
            if (millis() > _scan_timeout) {
              _scan_active = false;
              _scan_tag = 0;
            }
          } else if (_scan_tag != 0) {
            display.drawTextCentered(display.width() / 2, TOP_BAR_H + 22, "No results");
            display.drawTextCentered(display.width() / 2, TOP_BAR_H + 34, "RIGHT to rescan");
          } else {
            display.drawTextCentered(display.width() / 2, TOP_BAR_H + 22, "ENTER to scan");
          }
        } else {
          // Check scan timeout
          if (_scan_active && millis() > _scan_timeout) {
            _scan_active = false;
          }

          int visible = 4;
          int scroll_top = 0;
          if (_scan_sel >= visible) scroll_top = _scan_sel - visible + 1;
          if (scroll_top > _scan_count - visible) scroll_top = _scan_count - visible;
          if (scroll_top < 0) scroll_top = 0;

          int y = TOP_BAR_H + 10;
          for (int v = scroll_top; v < scroll_top + visible && v < _scan_count; v++, y += 10) {
            ScanResult& sr = _scan_results[v];
            if (v == _scan_sel && !_sb_active) {
              display.setColor(DisplayDriver::YELLOW);
              display.setCursor(0, y);
              display.print(">");
            }
            display.setColor(v == _scan_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            char line[48];
            float snr_f = (float)sr.snr_x4 / 4.0f;
            snprintf(line, sizeof(line), "<%02X> %s %.1f", sr.pub_key[0], sr.name, snr_f);
            display.drawTextEllipsized(8, y, display.width() - 8, line);
          }

          if (_scan_active) {
            display.setColor(DisplayDriver::LIGHT);
            display.setCursor(display.width() - 18, TOP_BAR_H);
            display.print("...");
          }
        }
      }
    } else if (_page == HomePage::SIGNALS) {
      display.setTextSize(1);
      if (_sig_action && _task->_signal_count > 0 && _sig_sel < _task->_signal_count) {
        // Action menu for selected signal
        AbstractUITask::SignalEntry& se = _task->_signals[_sig_sel];
        ContactInfo* ci = the_mesh.lookupContactByPubKey(&se.id, 1);
        if (ci && ci->name[0]) {
          snprintf(tmp, sizeof(tmp), "<%02X> %s", se.id, ci->name);
        } else {
          snprintf(tmp, sizeof(tmp), "<%02X> Signal", se.id);
        }
        display.setColor(DisplayDriver::YELLOW);
        display.drawTextEllipsized(0, TOP_BAR_H, display.width(), tmp);

        const char* items[8];
        bool item_is_action[8];
        int item_count = 0;
        items[item_count] = "Ping"; item_is_action[item_count++] = true;
        items[item_count] = "Delete"; item_is_action[item_count++] = true;

        static char sig_info[5][40];
        float rx_f = (float)se.rx_snr_x4 / 4.0f;
        if (se.has_rx) snprintf(sig_info[0], sizeof(sig_info[0]), "RX: %.1f dB", rx_f);
        else snprintf(sig_info[0], sizeof(sig_info[0]), "RX: ?");
        items[item_count] = sig_info[0]; item_is_action[item_count++] = false;

        if (se.has_tx) {
          float tx_f = (float)se.tx_snr_x4 / 4.0f;
          snprintf(sig_info[1], sizeof(sig_info[1]), "TX: %.1f dB", tx_f);
        } else if (se.tx_failed) {
          snprintf(sig_info[1], sizeof(sig_info[1]), "TX: FAILED");
        } else {
          snprintf(sig_info[1], sizeof(sig_info[1]), "TX: ?");
        }
        items[item_count] = sig_info[1]; item_is_action[item_count++] = false;

        snprintf(sig_info[2], sizeof(sig_info[2]), "Pkts: %u rx / %u tx", se.rx_count, se.tx_count);
        items[item_count] = sig_info[2]; item_is_action[item_count++] = false;

        if (se.last_rtt_ms > 0)
          snprintf(sig_info[3], sizeof(sig_info[3]), "RTT: %lums", (unsigned long)se.last_rtt_ms);
        else
          snprintf(sig_info[3], sizeof(sig_info[3]), "RTT: ?");
        items[item_count] = sig_info[3]; item_is_action[item_count++] = false;

        unsigned long age_s = (millis() - se.last_heard) / 1000;
        if (age_s < 60) snprintf(sig_info[4], sizeof(sig_info[4]), "Age: %lus", age_s);
        else snprintf(sig_info[4], sizeof(sig_info[4]), "Age: %lum", age_s / 60);
        items[item_count] = sig_info[4]; item_is_action[item_count++] = false;

        if (_sig_action_sel >= 2) _sig_action_sel = 1;
        int max_scroll = item_count > 4 ? item_count - 4 : 0;
        if (_sig_detail_scroll > max_scroll) _sig_detail_scroll = max_scroll;

        int y = TOP_BAR_H + 10;
        for (int i = _sig_detail_scroll; i < item_count && y < 64; i++, y += 10) {
          if (item_is_action[i] && i == _sig_action_sel) {
            display.setColor(DisplayDriver::YELLOW);
            display.setCursor(0, y);
            display.print(">");
          }
          display.setColor(item_is_action[i] && i == _sig_action_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
          display.drawTextEllipsized(8, y, display.width() - 8, items[i]);
        }
      } else {
        // Signal list
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "-- Signals (%d) --", _task->_signal_count);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, tmp);

        if (_task->_signal_count == 0) {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H + 22, "No signals yet");
        } else {
          // Sort by signal quality (best first)
          for (int i = 1; i < _task->_signal_count; i++) {
            AbstractUITask::SignalEntry tmp_se = _task->_signals[i];
            int j = i - 1;
            while (j >= 0) {
              AbstractUITask::SignalEntry& sj = _task->_signals[j];
              bool cur_bidi = tmp_se.has_rx && tmp_se.has_tx;
              bool cmp_bidi = sj.has_rx && sj.has_tx;
              bool swap = false;
              if (cur_bidi && !cmp_bidi) {
                swap = true;
              } else if (cur_bidi == cmp_bidi) {
                int8_t cur_snr = cur_bidi ? min(tmp_se.rx_snr_x4, tmp_se.tx_snr_x4) : tmp_se.rx_snr_x4;
                int8_t cmp_snr = cmp_bidi ? min(sj.rx_snr_x4, sj.tx_snr_x4) : sj.rx_snr_x4;
                if (cur_snr > cmp_snr) swap = true;
              }
              if (!swap) break;
              _task->_signals[j + 1] = _task->_signals[j];
              j--;
            }
            _task->_signals[j + 1] = tmp_se;
          }

          // Column positions (fixed so everything aligns)
          const int col_id   = 8;   // hex ID
          const int col_tx   = 22;  // TX arrow + bars (matches status bar order)
          const int col_rx   = 38;  // RX arrow + bars
          const int col_cnt  = 55;  // packet counts (rx/tx)
          const int col_age  = 90;  // age

          int visible = 4;
          int scroll_top = 0;
          if (_sig_sel >= visible) scroll_top = _sig_sel - visible + 1;
          if (scroll_top > _task->_signal_count - visible) scroll_top = _task->_signal_count - visible;
          if (scroll_top < 0) scroll_top = 0;

          int y = TOP_BAR_H + 10;
          for (int v = scroll_top; v < scroll_top + visible && v < _task->_signal_count; v++, y += 10) {
            AbstractUITask::SignalEntry& se = _task->_signals[v];
            bool sel = (v == _sig_sel);

            // Selector (hidden when status bar is focused)
            if (sel && !_sb_active) {
              display.setColor(DisplayDriver::YELLOW);
              display.setCursor(0, y);
              display.print(">");
            }

            // Hex ID
            display.setColor(sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            snprintf(tmp, sizeof(tmp), "%02X", se.id);
            display.setCursor(col_id, y);
            display.print(tmp);

            // TX bars: up-arrow + 4 bars / X / ? (drawn first, matching status bar order)
            int bars_y = y + 1;
            int bx = col_tx;
            display.setColor(DisplayDriver::GREEN);
            // Up arrow ▲
            display.fillRect(bx + 1, bars_y, 1, 1);
            display.fillRect(bx, bars_y + 1, 3, 1);
            bx += 4;
            if (se.has_tx) {
              int tx_bars = 0;
              float tx_snr = (float)se.tx_snr_x4 / 4.0f;
              if (tx_snr > 10) tx_bars = 4;
              else if (tx_snr > 5) tx_bars = 3;
              else if (tx_snr > 0) tx_bars = 2;
              else if (tx_snr > -10) tx_bars = 1;
              for (int b = 0; b < 4; b++) {
                int bh = 2 + b * 2;
                int bx2 = bx + b * 3;
                int by2 = bars_y + (8 - bh);
                if (b < tx_bars) {
                  display.setColor(DisplayDriver::GREEN);
                  display.fillRect(bx2, by2, 2, bh);
                } else {
                  display.setColor(DisplayDriver::GREEN);
                  display.fillRect(bx2, bars_y + 7, 2, 1);
                }
              }
            } else if (se.tx_failed) {
              display.setColor(DisplayDriver::RED);
              display.setCursor(bx + 2, y);
              display.print("X");
            } else {
              bool pinging = _task->_auto_ping_pending && _task->_auto_ping_current_id == se.id;
              if (!pinging || (millis() / 300) % 2) {
                display.setColor(pinging ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
                display.setCursor(bx + 2, y);
                display.print("?");
              }
            }

            // RX bars: down-arrow + 4 bars
            bx = col_rx;
            display.setColor(DisplayDriver::GREEN);
            // Down arrow ▼
            display.fillRect(bx, bars_y, 3, 1);
            display.fillRect(bx + 1, bars_y + 1, 1, 1);
            bx += 4;
            if (se.has_rx) {
              int rx_bars = 0;
              float rx_snr = (float)se.rx_snr_x4 / 4.0f;
              if (rx_snr > 10) rx_bars = 4;
              else if (rx_snr > 5) rx_bars = 3;
              else if (rx_snr > 0) rx_bars = 2;
              else if (rx_snr > -10) rx_bars = 1;
              for (int b = 0; b < 4; b++) {
                int bh = 2 + b * 2;
                int bx2 = bx + b * 3;
                int by2 = bars_y + (8 - bh);
                if (b < rx_bars) {
                  display.setColor(DisplayDriver::GREEN);
                  display.fillRect(bx2, by2, 2, bh);
                } else {
                  display.setColor(DisplayDriver::GREEN);
                  display.fillRect(bx2, bars_y + 7, 2, 1);
                }
              }
            } else {
              display.setColor(DisplayDriver::LIGHT);
              display.setCursor(bx + 2, y);
              display.print("?");
            }

            // Packet counts (tx/rx — matches bar order)
            display.setColor(sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            snprintf(tmp, sizeof(tmp), "%u/%u", se.tx_count, se.rx_count);
            display.setCursor(col_cnt, y);
            display.print(tmp);

            // Age
            {
              unsigned long age_s = (millis() - se.last_heard) / 1000;
              if (age_s < 60) snprintf(tmp, sizeof(tmp), "%lus", age_s);
              else if (age_s < 3600) snprintf(tmp, sizeof(tmp), "%lum", age_s / 60);
              else snprintf(tmp, sizeof(tmp), "%luh", age_s / 3600);
            }
            display.setColor(sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            display.setCursor(col_age, y);
            display.print(tmp);
          }
        }
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(DisplayDriver::YELLOW);
      display.setTextSize(1);
      // freq / sf
      display.setCursor(0, TOP_BAR_H + 2);
      sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
      display.print(tmp);

      display.setCursor(0, TOP_BAR_H + 14);
      sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
      display.print(tmp);

      // tx power,  noise floor
      display.setCursor(0, TOP_BAR_H + 26);
      sprintf(tmp, "TX: %ddBm", _node_prefs->tx_power_dbm);
      display.print(tmp);
      display.setCursor(0, TOP_BAR_H + 38);
      sprintf(tmp, "Noise floor: %d", radio_driver.getNoiseFloor());
      display.print(tmp);
    } else if (_page == HomePage::PACKETS) {
      display.setTextSize(1);
      int total = _task->_pkt_log_count;

      if (_pkt_detail && total > 0) {
        // Detail view for selected packet
        if (_pkt_sel >= total) _pkt_sel = total - 1;
        int idx = (_task->_pkt_log_next - 1 - _pkt_sel + PACKET_LOG_SIZE) % PACKET_LOG_SIZE;
        auto& pkt = _task->_pkt_log[idx];

        display.setColor(DisplayDriver::YELLOW);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, "-- Pkt Detail --");

        // Build detail items
        char detail_items[12][24];
        uint8_t detail_count = 0;

        // Type (full name)
        {
          const char* type_full;
          switch (pkt.payload_type) {
            case 0x00: type_full = "Request"; break;
            case 0x01: type_full = "Response"; break;
            case 0x02: type_full = "Text Msg"; break;
            case 0x03: type_full = "ACK"; break;
            case 0x04: type_full = "Advert"; break;
            case 0x05: case 0x06: type_full = "Group"; break;
            case 0x07: type_full = "Anon"; break;
            case 0x08: type_full = "Path"; break;
            case 0x09: type_full = "Trace"; break;
            case 0x0A: type_full = "Multipart"; break;
            case 0x0B: type_full = "Control"; break;
            case 0x0F: type_full = "Raw"; break;
            default:   type_full = "Unknown"; break;
          }
          snprintf(detail_items[detail_count++], 24, "Type: %s", type_full);
        }

        // Route
        {
          const char* route_str;
          switch (pkt.route_type) {
            case 0x00: route_str = "Xport Flood"; break;
            case 0x01: route_str = "Flood"; break;
            case 0x02: route_str = "Direct"; break;
            default:   route_str = "Unknown"; break;
          }
          snprintf(detail_items[detail_count++], 24, "Route: %s", route_str);
        }

        // RSSI
        snprintf(detail_items[detail_count++], 24, "RSSI: %d", pkt.rssi);

        // SNR
        {
          float snr_f = (float)pkt.snr_x4 / 4.0f;
          snprintf(detail_items[detail_count++], 24, "SNR: %.1f", snr_f);
        }

        // First Hop
        snprintf(detail_items[detail_count++], 24, "Heard: %02X", pkt.first_hop);

        // Path (if path_len > 0)
        if (pkt.path_len > 0) {
          char path_buf[20];
          int pos = 0;
          for (int p = 0; p < pkt.path_len && pos < 16; p++) {
            if (p > 0) path_buf[pos++] = ' ';
            pos += snprintf(&path_buf[pos], sizeof(path_buf) - pos, "%02X", pkt.path[p]);
          }
          path_buf[pos] = '\0';
          snprintf(detail_items[detail_count++], 24, "Path: %s", path_buf);
        } else {
          snprintf(detail_items[detail_count++], 24, "Path: (direct)");
        }

        // Payload size
        snprintf(detail_items[detail_count++], 24, "Payload: %d B", pkt.payload_len);

        // Age
        {
          unsigned long age_s = (millis() - pkt.timestamp) / 1000;
          if (age_s < 60) {
            snprintf(detail_items[detail_count++], 24, "Age: %lus", age_s);
            _needs_fast_refresh = true;
          } else if (age_s < 3600) {
            snprintf(detail_items[detail_count++], 24, "Age: %lum", age_s / 60);
          } else {
            snprintf(detail_items[detail_count++], 24, "Age: %luh", age_s / 3600);
          }
        }

        // Clamp scroll
        int detail_visible = 4;
        int max_scroll = (detail_count > detail_visible) ? detail_count - detail_visible : 0;
        if (_pkt_detail_scroll > max_scroll) _pkt_detail_scroll = max_scroll;

        // Render detail rows
        int y = TOP_BAR_H + 10;
        for (int i = _pkt_detail_scroll; i < _pkt_detail_scroll + detail_visible && i < detail_count; i++, y += 10) {
          display.setColor(DisplayDriver::LIGHT);
          display.setCursor(0, y);
          display.print(detail_items[i]);
        }

      } else {
        // List view
        display.setColor(DisplayDriver::YELLOW);
        if (total > 0) {
          snprintf(tmp, sizeof(tmp), "-- Packets %d/%d --", _pkt_sel + 1, total);
        } else {
          snprintf(tmp, sizeof(tmp), "-- Packets --");
        }
        display.drawTextCentered(display.width() / 2, TOP_BAR_H, tmp);

        if (total == 0) {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H + 22, "No packets yet");
        } else {
          if (_pkt_sel >= total) _pkt_sel = total - 1;
          // Show 4 visible entries, scrolled so selection is visible
          int visible = 4;
          int scroll = 0;
          if (_pkt_sel >= visible) scroll = _pkt_sel - visible + 1;
          int y = TOP_BAR_H + 10;
          for (int vi = 0; vi < visible && (scroll + vi) < total; vi++, y += 10) {
            int item = scroll + vi;
            int idx = (_task->_pkt_log_next - 1 - item + PACKET_LOG_SIZE) % PACKET_LOG_SIZE;
            auto& pkt = _task->_pkt_log[idx];
            const char* type_str;
            switch (pkt.payload_type) {
              case 0x00: type_str = "REQ"; break;
              case 0x01: type_str = "RSP"; break;
              case 0x02: type_str = "TXT"; break;
              case 0x03: type_str = "ACK"; break;
              case 0x04: type_str = "ADV"; break;
              case 0x05: case 0x06: type_str = "GRP"; break;
              case 0x07: type_str = "ANO"; break;
              case 0x08: type_str = "PTH"; break;
              case 0x09: type_str = "TRC"; break;
              case 0x0A: type_str = "MPT"; break;
              case 0x0B: type_str = "CTL"; break;
              case 0x0F: type_str = "RAW"; break;
              default:   type_str = "???"; break;
            }
            float snr_f = (float)pkt.snr_x4 / 4.0f;
            unsigned long age_s = (millis() - pkt.timestamp) / 1000;
            char age_buf[8];
            if (age_s < 60) { snprintf(age_buf, sizeof(age_buf), "%lus", age_s); _needs_fast_refresh = true; }
            else if (age_s < 3600) snprintf(age_buf, sizeof(age_buf), "%lum", age_s / 60);
            else snprintf(age_buf, sizeof(age_buf), "%luh", age_s / 3600);
            char marker = (item == _pkt_sel && !_sb_active) ? '>' : ' ';
            snprintf(tmp, sizeof(tmp), "%c%s %02X %d/%.1f %s", marker, type_str, pkt.first_hop, pkt.rssi, snr_f, age_buf);
            display.setColor(item == _pkt_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
            display.setCursor(0, y);
            display.print(tmp);
          }
        }
      }
    } else if (_page == HomePage::ADVERT) {
      display.setColor(DisplayDriver::GREEN);
      display.drawXbm((display.width() - 32) / 2, TOP_BAR_H + 4, advert_icon, 32, 32);
      display.drawTextCentered(display.width() / 2, TOP_BAR_H + 40, "advert: " PRESS_LABEL);
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = sensors.getLocationProvider();
      char buf[50];
      int y = TOP_BAR_H;
      bool gps_state = _task->getGPSState();
#ifdef PIN_GPS_SWITCH
      bool hw_gps_state = digitalRead(PIN_GPS_SWITCH);
      if (gps_state != hw_gps_state) {
        strcpy(buf, gps_state ? "gps off(hw)" : "gps off(sw)");
      } else {
        strcpy(buf, gps_state ? "gps on" : "gps off");
      }
#else
      strcpy(buf, gps_state ? "gps on" : "gps off");
#endif
      display.drawTextLeftAlign(0, y, buf);
      if (nmea == NULL) {
        y = y + 12;
        display.drawTextLeftAlign(0, y, "Can't access GPS");
      } else {
        strcpy(buf, nmea->isValid()?"fix":"no fix");
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "sat");
        sprintf(buf, "%d", nmea->satellitesCount());
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "pos");
        sprintf(buf, "%.4f %.4f", 
          nmea->getLatitude()/1000000., nmea->getLongitude()/1000000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
        display.drawTextLeftAlign(0, y, "alt");
        sprintf(buf, "%.2f", nmea->getAltitude()/1000.);
        display.drawTextRightAlign(display.width()-1, y, buf);
        y = y + 12;
      }
    } else if (_page == HomePage::NAV) {
      LocationProvider* nmea = sensors.getLocationProvider();
      if (nmea == NULL || !nmea->isValid()) {
        // No fix state
        display.setColor(DisplayDriver::YELLOW);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H + 12, "Waiting for fix...");
        char satbuf[16];
        if (nmea != NULL) {
          snprintf(satbuf, sizeof(satbuf), "Sats: %ld", nmea->satellitesCount());
        } else {
          strcpy(satbuf, "No GPS");
        }
        display.setColor(DisplayDriver::LIGHT);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H + 28, satbuf);
      } else {
        // Speed in mph (getSpeed returns knots * 1000)
        float speed_mph = nmea->getSpeed() / 1000.0f * 1.15078f;
        // Track max speed
        if (speed_mph > _max_speed) _max_speed = speed_mph;
        // Odometer: accumulate distance using GPS position deltas (haversine)
        unsigned long now_ms = millis();
        if (speed_mph > 1.0f && (_odo_last == 0 || (now_ms - _odo_last) >= 2000)) {
          long cur_lat = nmea->getLatitude();   // microdegrees
          long cur_lon = nmea->getLongitude();
          if (_odo_last_lat != 0 || _odo_last_lon != 0) {
            // Haversine distance in miles
            float lat1 = _odo_last_lat / 1000000.0f * 3.14159f / 180.0f;
            float lat2 = cur_lat / 1000000.0f * 3.14159f / 180.0f;
            float dlat = lat2 - lat1;
            float dlon = (cur_lon - _odo_last_lon) / 1000000.0f * 3.14159f / 180.0f;
            float a = sinf(dlat / 2) * sinf(dlat / 2) +
                      cosf(lat1) * cosf(lat2) * sinf(dlon / 2) * sinf(dlon / 2);
            float c = 2.0f * atan2f(sqrtf(a), sqrtf(1.0f - a));
            float dist_mi = 3958.8f * c;  // Earth radius in miles
            if (dist_mi < 0.5f) {  // sanity cap: skip GPS jumps
              _odometer += dist_mi;
            }
          }
          _odo_last_lat = cur_lat;
          _odo_last_lon = cur_lon;
          _odo_last = now_ms;
        }
        // Course in degrees (getCourse returns thousandths of degrees)
        float course_deg = nmea->getCourse() / 1000.0f;
        bool moving = speed_mph > 2.0f;

        // 16-point compass lookup
        static const char* compass_dirs[] = {
          "N","NNE","NE","ENE","E","ESE","SE","SSE",
          "S","SSW","SW","WSW","W","WNW","NW","NNW"
        };

        // === Top row: speed, heading, sats ===
        display.setColor(DisplayDriver::YELLOW);
        display.setTextSize(1);
        char spdbuf[16];
        snprintf(spdbuf, sizeof(spdbuf), "%.0f/%.0f", speed_mph, _max_speed);
        display.setCursor(0, TOP_BAR_H);
        display.print(spdbuf);

        // Heading text (center-ish) or waypoint name
        if (_nav_has_waypoint) {
          char wpbuf[16];
          snprintf(wpbuf, sizeof(wpbuf), "->%s", _nav_wp_name);
          display.setColor(DisplayDriver::GREEN);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H, wpbuf);
        } else if (moving) {
          int dir_idx = ((int)(course_deg + 11.25f) % 360) / 22.5f;
          if (dir_idx < 0) dir_idx = 0;
          if (dir_idx > 15) dir_idx = 15;
          display.setColor(DisplayDriver::GREEN);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H, compass_dirs[dir_idx]);
        } else {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, TOP_BAR_H, "--");
        }

        // Satellite count (right, below name row)
        char satbuf[8];
        snprintf(satbuf, sizeof(satbuf), "%ldsat", nmea->satellitesCount());
        display.setColor(DisplayDriver::LIGHT);
        int satW = display.getTextWidth(satbuf);
        display.setCursor(display.width() - satW - 1, TOP_BAR_H + 10);
        display.print(satbuf);

        // === Center: Compass rose ===
        int cx = display.width() / 2;
        int cy = TOP_BAR_H + 30;
        int r = 12;

        // Draw compass circle using small dots
        display.setColor(DisplayDriver::LIGHT);
        for (int a = 0; a < 360; a += 15) {
          float rad = a * 3.14159f / 180.0f;
          int px = cx + (int)(r * sinf(rad));
          int py = cy - (int)(r * cosf(rad));
          display.fillRect(px, py, 1, 1);
        }

        // Cardinal direction labels
        display.setCursor(cx - 2, cy - r - 8);
        display.print("N");
        display.setCursor(cx - 2, cy + r + 2);
        display.print("S");
        display.setCursor(cx + r + 2, cy - 3);
        display.print("E");
        display.setCursor(cx - r - 8, cy - 3);
        display.print("W");

        // Direction arrow (only when moving)
        if (moving) {
          display.setColor(DisplayDriver::YELLOW);
          float rad = course_deg * 3.14159f / 180.0f;
          float sr = sinf(rad);
          float cr = cosf(rad);
          // Arrow tip
          int tipX = cx + (int)((r - 2) * sr);
          int tipY = cy - (int)((r - 2) * cr);
          // Arrow base (opposite direction, shorter)
          int baseX = cx - (int)(4 * sr);
          int baseY = cy + (int)(4 * cr);
          // Draw arrow line using small rects
          int steps = r;
          for (int s = 0; s <= steps; s++) {
            int px = baseX + (tipX - baseX) * s / steps;
            int py = baseY + (tipY - baseY) * s / steps;
            display.fillRect(px, py, 2, 2);
          }
          // Arrow head - wider near tip
          int lx = tipX - (int)(3 * cr);
          int ly = tipY - (int)(3 * sr);
          int rx = tipX + (int)(3 * cr);
          int ry = tipY + (int)(3 * sr);
          int mx = tipX + (int)(3 * sr);
          int my = tipY - (int)(3 * cr);
          // Draw arrowhead lines
          for (int s = 0; s <= 4; s++) {
            int px1 = lx + (mx - lx) * s / 4;
            int py1 = ly + (my - ly) * s / 4;
            display.fillRect(px1, py1, 1, 1);
            int px2 = rx + (mx - rx) * s / 4;
            int py2 = ry + (my - ry) * s / 4;
            display.fillRect(px2, py2, 1, 1);
          }
        }

        // === Waypoint target arrow (GREEN) ===
        float wp_bearing_rad = 0;
        float wp_dist_mi = 0;
        if (_nav_has_waypoint) {
          float my_lat = nmea->getLatitude() / 1000000.0f;
          float my_lon = nmea->getLongitude() / 1000000.0f;
          float lat1 = my_lat * 3.14159f / 180.0f;
          float lat2 = _nav_wp_lat * 3.14159f / 180.0f;
          float dLat = lat2 - lat1;
          float dLon = (_nav_wp_lon - my_lon) * 3.14159f / 180.0f;
          // Bearing (great circle)
          wp_bearing_rad = atan2f(sinf(dLon) * cosf(lat2),
                                  cosf(lat1) * sinf(lat2) - sinf(lat1) * cosf(lat2) * cosf(dLon));
          // Distance (Haversine, R = 3958.8 mi)
          float a = sinf(dLat / 2) * sinf(dLat / 2) +
                    cosf(lat1) * cosf(lat2) * sinf(dLon / 2) * sinf(dLon / 2);
          wp_dist_mi = 2 * 3958.8f * atan2f(sqrtf(a), sqrtf(1 - a));

          // Draw target arrow on compass (GREEN line from center toward bearing)
          display.setColor(DisplayDriver::GREEN);
          float bsr = sinf(wp_bearing_rad);
          float bcr = cosf(wp_bearing_rad);
          int btipX = cx + (int)((r - 1) * bsr);
          int btipY = cy - (int)((r - 1) * bcr);
          int bbaseX = cx + (int)(2 * bsr);
          int bbaseY = cy - (int)(2 * bcr);
          for (int s = 0; s <= r; s++) {
            int px = bbaseX + (btipX - bbaseX) * s / r;
            int py = bbaseY + (btipY - bbaseY) * s / r;
            display.fillRect(px, py, 2, 2);
          }
          // Small arrowhead
          int blx = btipX - (int)(2 * bcr);
          int bly = btipY - (int)(2 * bsr);
          int brx = btipX + (int)(2 * bcr);
          int bry = btipY + (int)(2 * bsr);
          int bmx = btipX + (int)(2 * bsr);
          int bmy = btipY - (int)(2 * bcr);
          for (int s = 0; s <= 3; s++) {
            int px1 = blx + (bmx - blx) * s / 3;
            int py1 = bly + (bmy - bly) * s / 3;
            display.fillRect(px1, py1, 1, 1);
            int px2 = brx + (bmx - brx) * s / 3;
            int py2 = bry + (bmy - bry) * s / 3;
            display.fillRect(px2, py2, 1, 1);
          }
        }

        // === Bottom rows ===
        if (_nav_has_waypoint) {
          // Distance (left)
          display.setColor(DisplayDriver::GREEN);
          char distbuf[12];
          if (wp_dist_mi < 0.1f) {
            snprintf(distbuf, sizeof(distbuf), "%.0fft", wp_dist_mi * 5280);
          } else if (wp_dist_mi < 10.0f) {
            snprintf(distbuf, sizeof(distbuf), "%.2fmi", wp_dist_mi);
          } else {
            snprintf(distbuf, sizeof(distbuf), "%.1fmi", wp_dist_mi);
          }
          display.setCursor(0, 56);
          display.print(distbuf);

          // ETE from VMG (right)
          char etebuf[10];
          float heading_rad = course_deg * 3.14159f / 180.0f;
          float vmg = speed_mph * cosf(heading_rad - wp_bearing_rad);
          if (vmg > 0.5f) {
            float ete_min = (wp_dist_mi / vmg) * 60.0f;
            if (ete_min < 60) {
              snprintf(etebuf, sizeof(etebuf), "%.0fm", ete_min);
            } else {
              int hrs = (int)(ete_min / 60);
              int mins = (int)ete_min % 60;
              snprintf(etebuf, sizeof(etebuf), "%dh%02dm", hrs, mins);
            }
          } else {
            strcpy(etebuf, "--");
          }
          int eteW = display.getTextWidth(etebuf);
          display.setCursor(display.width() - eteW - 1, 56);
          display.print(etebuf);
        } else {
          // Default: alt + odometer
          display.setColor(DisplayDriver::LIGHT);
          float alt_ft = nmea->getAltitude() / 1000.0f * 3.28084f;
          char altbuf[12];
          snprintf(altbuf, sizeof(altbuf), "%.0fft", alt_ft);
          display.setCursor(0, 56);
          display.print(altbuf);

          // Odometer (right side of same row)
          char odobuf[14];
          if (_odometer < 10.0f) {
            snprintf(odobuf, sizeof(odobuf), "%.2fmi", _odometer);
          } else {
            snprintf(odobuf, sizeof(odobuf), "%.1fmi", _odometer);
          }
          int odoW = display.getTextWidth(odobuf);
          display.setCursor(display.width() - odoW - 1, 56);
          display.print(odobuf);
        }
      }
      // Keep screen on while nav screen lock is active
      if (_nav_screen_lock) {
        _task->extendAutoOff();
      }
      return 1000; // refresh every 1s for live GPS data
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == HomePage::SENSORS) {
      int y = TOP_BAR_H;
      refresh_sensors();
      char buf[30];
      char name[30];
      LPPReader r(sensors_lpp.getBuffer(), sensors_lpp.getSize());

      for (int i = 0; i < sensors_scroll_offset; i++) {
        uint8_t channel, type;
        r.readHeader(channel, type);
        r.skipData(type);
      }

      for (int i = 0; i < (sensors_scroll?UI_RECENT_LIST_SIZE:sensors_nb); i++) {
        uint8_t channel, type;
        if (!r.readHeader(channel, type)) { // reached end, reset
          r.reset();
          r.readHeader(channel, type);
        }

        display.setCursor(0, y);
        float v;
        switch (type) {
          case LPP_GPS: // GPS
            float lat, lon, alt;
            r.readGPS(lat, lon, alt);
            strcpy(name, "gps"); sprintf(buf, "%.4f %.4f", lat, lon);
            break;
          case LPP_VOLTAGE:
            r.readVoltage(v);
            strcpy(name, "voltage"); sprintf(buf, "%6.2f", v);
            break;
          case LPP_CURRENT:
            r.readCurrent(v);
            strcpy(name, "current"); sprintf(buf, "%.3f", v);
            break;
          case LPP_TEMPERATURE:
            r.readTemperature(v);
            strcpy(name, "temperature"); sprintf(buf, "%.2f", v);
            break;
          case LPP_RELATIVE_HUMIDITY:
            r.readRelativeHumidity(v);
            strcpy(name, "humidity"); sprintf(buf, "%.2f", v);
            break;
          case LPP_BAROMETRIC_PRESSURE:
            r.readPressure(v);
            strcpy(name, "pressure"); sprintf(buf, "%.2f", v);
            break;
          case LPP_ALTITUDE:
            r.readAltitude(v);
            strcpy(name, "altitude"); sprintf(buf, "%.0f", v);
            break;
          case LPP_POWER:
            r.readPower(v);
            strcpy(name, "power"); sprintf(buf, "%6.2f", v);
            break;
          default:
            r.skipData(type);
            strcpy(name, "unk"); sprintf(buf, "");
        }
        display.setCursor(0, y);
        display.print(name);
        display.setCursor(
          display.width()-display.getTextWidth(buf)-1, y
        );
        display.print(buf);
        y = y + 12;
      }
      if (sensors_scroll) sensors_scroll_offset = (sensors_scroll_offset+1)%sensors_nb;
      else sensors_scroll_offset = 0;
#endif
    } else if (_page == HomePage::SETTINGS) {
      display.setTextSize(1);
      display.setColor(DisplayDriver::YELLOW);
      display.drawTextCentered(display.width() / 2, TOP_BAR_H, "-- Settings --");

      // Settings entries: index 0 = GMT offset (special), rest are bool toggles
      const int max_settings = 10;
      const char* names[max_settings];
      bool values[max_settings];
      bool is_gmt[max_settings];
      int sc = 0;
      // GMT offset (special non-bool entry)
      int id_gmt_render = sc;
      int id_motion_render = -1;
      names[sc] = "GMT offset"; values[sc] = false; is_gmt[sc] = true; sc++;
      names[sc] = "Battery voltage"; values[sc] = _show_voltage; is_gmt[sc] = false; sc++;
      names[sc] = "Signal bars"; values[sc] = _show_snr; is_gmt[sc] = false; sc++;
#if ENV_INCLUDE_GPS == 1
      names[sc] = "Speed HUD"; values[sc] = _show_speed; is_gmt[sc] = false; sc++;
#endif
      names[sc] = "Beep w/ BLE"; values[sc] = _beep_on_ble; is_gmt[sc] = false; sc++;
      names[sc] = "Auto TX check"; values[sc] = _auto_tx_check; is_gmt[sc] = false; sc++;
#if ENV_INCLUDE_GPS == 1
      id_motion_render = sc;
      names[sc] = "Motion mode"; values[sc] = false; is_gmt[sc] = false; sc++;
#endif
      names[sc] = "Bluetooth"; values[sc] = _task->isSerialEnabled(); is_gmt[sc] = false; sc++;
#if ENV_INCLUDE_GPS == 1
      names[sc] = "GPS"; values[sc] = _task->getGPSState(); is_gmt[sc] = false; sc++;
#endif

      if (_settings_sel >= sc) _settings_sel = sc - 1;

      int visible = 4;
      int scroll = 0;
      if (_settings_sel >= visible) scroll = _settings_sel - visible + 1;

      int y = TOP_BAR_H + 10;
      for (int i = scroll; i < scroll + visible && i < sc; i++, y += 10) {
        bool selected = (i == _settings_sel);
        display.setColor(selected ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
        if (selected && !_sb_active) {
          display.setCursor(0, y);
          display.print(">");
        }
        if (is_gmt[i]) {
          snprintf(tmp, sizeof(tmp), "%s [%s%d]", names[i], _gmt_offset >= 0 ? "+" : "", _gmt_offset);
        } else if (i == id_motion_render) {
          const char* mm[] = {"Off", "Auto", "Bike", "Drive"};
          snprintf(tmp, sizeof(tmp), "%s [%s]", names[i], mm[_motion_mode_setting & 0x03]);
        } else {
          snprintf(tmp, sizeof(tmp), "%s [%s]", names[i], values[i] ? "ON" : "OFF");
        }
        display.drawTextEllipsized(8, y, display.width() - 8, tmp);
      }
    } else if (_page == HomePage::SHUTDOWN) {
      display.setColor(DisplayDriver::GREEN);
      display.setTextSize(1);
      if (_shutdown_init) {
        display.drawTextCentered(display.width() / 2, TOP_BAR_H + 14, "hibernating...");
      } else {
        display.drawXbm((display.width() - 32) / 2, TOP_BAR_H + 4, power_icon, 32, 32);
        display.drawTextCentered(display.width() / 2, TOP_BAR_H + 40, "hibernate:" PRESS_LABEL);
      }
    }
    // Faster refresh when scrolling message text
    if (_page == HomePage::MESSAGES && _msg_scroll_px > 0) {
      _task->extendAutoOff();
      return 400;
    }
    // Fast refresh for flashing ping indicator on Signals page
    if (_page == HomePage::SIGNALS && _task->_auto_ping_pending) return 300;
    if (_needs_fast_refresh) return 1000;  // update live timers every second
    return 5000;   // next render after 5000 ms
  }

  bool handleInput(char c) override {
    if (_sb_active) return handleStatusBarKey(c);

    if (!_page_active) {
      // === LEVEL 1: CAROUSEL NAVIGATION ===
      if (c == KEY_LEFT || c == KEY_PREV) {
        _page = (_page + HomePage::Count - 1) % HomePage::Count;
        return true;
      }
      if (c == KEY_NEXT || c == KEY_RIGHT) {
        _page = (_page + 1) % HomePage::Count;
        return true;
      }
      if (c == KEY_UP) {
        enterStatusBar();
        return true;
      }
      if (c == KEY_ENTER) {
        if (_page == HomePage::FIRST) return true;  // Home dashboard shown directly, no level 2
        _page_active = true;
        // Reset page state on entry from carousel
        if (_page == HomePage::MESSAGES) {
          _msg_sel = 0xFF;  // clamp to newest on render
          _msg_sel_prev = 0xFF;
          _msg_scroll_px = 0;
          _msg_vscroll = 0;
          _msg_compose_menu = false;
          _msg_target_menu = false;
          _msg_detail = false;
        }
        return true;
      }
      return false;
    }

    // === LEVEL 2: INSIDE PAGE ===

    // Return from status-bar-launched page: restore previous location
    if (_sb_return && c == KEY_CANCEL && !isInSubState()) {
      _page = _sb_prev_page;
      _page_active = _sb_prev_active;
      _sb_return = false;
      return true;
    }

    if (_page == HomePage::TRACE) {
      if (_ct_gps_pending || _ct_gps_done || _ct_gps_no_fix) {
        if (c == KEY_CANCEL) {
          _ct_gps_pending = false;
          _ct_gps_done = false;
          _ct_gps_no_fix = false;
          _ct_detail_scroll = 0;
          reselectContact(_ct_target_name);
          _ct_action = true;
          return true;
        }
        if (c == KEY_ENTER && _ct_gps_done) {
          // Navigate to this contact's location
          ContactCache* cc = findCache(_ct_path_key);
          if (cc && cc->has_gps) {
            _nav_wp_lat = cc->gps_lat;
            _nav_wp_lon = cc->gps_lon;
            strncpy(_nav_wp_name, _ct_target_name, sizeof(_nav_wp_name) - 1);
            _nav_wp_name[sizeof(_nav_wp_name) - 1] = '\0';
            _nav_has_waypoint = true;
            _ct_gps_pending = false;
            _ct_gps_done = false;
            _ct_gps_no_fix = false;
            _page = HomePage::NAV;
          }
          return true;
        }
        if (_ct_gps_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_ping_pending || _ct_ping_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_ping_pending = false;
          _ct_ping_done = false;
          _ct_detail_scroll = 0;
          reselectContact(_ct_target_name);
          _ct_action = true;  // return to contact card
          return true;
        }
        if (_ct_ping_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_status_pending || _ct_status_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_status_pending = false;
          _ct_status_done = false;
          _ct_detail_scroll = 0;
          reselectContact(_ct_target_name);
          _ct_action = true;  // return to contact card
          return true;
        }
        if (_ct_status_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_telem_pending || _ct_telem_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_telem_pending = false;
          _ct_telem_done = false;
          _ct_detail_scroll = 0;
          reselectContact(_ct_target_name);
          _ct_action = true;  // return to contact card
          return true;
        }
        if (_ct_telem_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_path_pending) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_path_pending = false;
          _ct_path_found = false;
          _ct_detail_scroll = 0;
          reselectContact(_ct_target_name);
          _ct_action = true;  // return to contact card
          return true;
        }
        if (_ct_path_found) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_action) {
        // Action/detail menu
        if (c == KEY_CANCEL) {
          _ct_action = false;
          _ct_detail_scroll = 0;
          return true;
        }
        if (c == KEY_UP) {
          if (_ct_action_sel > 0) {
            _ct_action_sel--;
            if (_ct_action_sel < _ct_detail_scroll) _ct_detail_scroll = _ct_action_sel;
          } else if (_ct_detail_scroll > 0) {
            _ct_detail_scroll--;
          }
          return true;
        }
        if (c == KEY_DOWN) {
          if (_ct_action_sel < _ct_action_count - 1) {
            _ct_action_sel++;
            if (_ct_action_sel >= _ct_detail_scroll + 4) _ct_detail_scroll = _ct_action_sel - 3;
          } else {
            // Scroll down into info lines
            ContactInfo ci;
            if (getContactByKey(_ct_action_key, ci)) {
              ContactCache* cc = findCache(ci.id.pub_key);
              int total = _ct_action_count + 1; // +1 for "last heard"
              if (cc && cc->has_path_info) total += 2;
              if (cc && cc->has_telem) total += 1;
              if (cc && cc->has_status) total += 2;
              if (_ct_detail_scroll + 4 < total) _ct_detail_scroll++;
            }
          }
          return true;
        }
        if (c == KEY_ENTER) {
          ContactInfo ci;
          if (getContactByKey(_ct_action_key, ci)) {
            // Determine which action was selected
            const char* actions[8];
            uint8_t act_count = 0;
            if (ci.type == ADV_TYPE_CHAT) {
              actions[act_count++] = "Send DM";
              actions[act_count++] = "Find Path";
              actions[act_count++] = "Telemetry";
#if ENV_INCLUDE_GPS == 1
              actions[act_count++] = "Send GPS";
              actions[act_count++] = "Req Location";
#endif
            } else if (ci.type == ADV_TYPE_REPEATER) {
              actions[act_count++] = "Ping";
              actions[act_count++] = "Find Path";
              actions[act_count++] = "Telemetry";
              actions[act_count++] = "Status";
            } else {
              actions[act_count++] = "Find Path";
              actions[act_count++] = "Telemetry";
            }
#if ENV_INCLUDE_GPS == 1
            {
              ContactCache* ncc = findCache(ci.id.pub_key);
              bool has_advert_gps = (ci.gps_lat != 0 || ci.gps_lon != 0);
              bool has_cached_gps = (ncc && ncc->has_gps);
              if (has_advert_gps || has_cached_gps) {
                actions[act_count++] = "Navigate";
              }
            }
#endif
            const char* chosen = actions[_ct_action_sel];
            if (strcmp(chosen, "Ping") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendPing(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _ct_action = false;
                _ct_ping_pending = true;
                _ct_ping_done = false;
                _ct_detail_scroll = 0;
                _ct_ping_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
            } else if (strcmp(chosen, "Send DM") == 0) {
              _ct_action = false;
              _task->startDMCompose(ci);
            } else if (strcmp(chosen, "Find Path") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendPathFind(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _ct_action = false;
                _ct_path_pending = true;
                _ct_path_found = false;
                _ct_detail_scroll = 0;
                _ct_path_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
            } else if (strcmp(chosen, "Telemetry") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendTelemetryReq(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _ct_action = false;
                _ct_telem_pending = true;
                _ct_telem_done = false;
                _ct_detail_scroll = 0;
                _ct_telem_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
            } else if (strcmp(chosen, "Status") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendStatusReq(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _ct_action = false;
                _ct_status_pending = true;
                _ct_status_done = false;
                _ct_detail_scroll = 0;
                _ct_status_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
#if ENV_INCLUDE_GPS == 1
            } else if (strcmp(chosen, "Send GPS") == 0) {
              _ct_action = false;
              _task->sendGPSDM(ci);
            } else if (strcmp(chosen, "Req Location") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendTelemetryReq(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _ct_action = false;
                _ct_gps_pending = true;
                _ct_gps_done = false;
                _ct_gps_no_fix = false;
                _ct_detail_scroll = 0;
                _ct_gps_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
            } else if (strcmp(chosen, "Navigate") == 0) {
              // Copy GPS coords to waypoint state
              ContactCache* ncc = findCache(ci.id.pub_key);
              bool has_cached_gps = (ncc && ncc->has_gps);
              if (has_cached_gps) {
                _nav_wp_lat = ncc->gps_lat;
                _nav_wp_lon = ncc->gps_lon;
              } else {
                // From advert (microdegrees to decimal degrees)
                _nav_wp_lat = ci.gps_lat / 1000000.0f;
                _nav_wp_lon = ci.gps_lon / 1000000.0f;
              }
              strncpy(_nav_wp_name, ci.name, sizeof(_nav_wp_name) - 1);
              _nav_wp_name[sizeof(_nav_wp_name) - 1] = '\0';
              _nav_has_waypoint = true;
              _ct_action = false;
              _page = HomePage::NAV;
#endif
            }
          }
          return true;
        }
        return true;
      }
      // Back out of contact list
      if (c == KEY_CANCEL) {
        _page_active = false;
        return true;
      }
      // Contact filter: LEFT/RIGHT cycles filter
      if (c == KEY_LEFT) {
        _ct_filter = (_ct_filter + 2) % 3;
        _ct_sel = 0;
        rebuildContactsSorted();
        return true;
      }
      if (c == KEY_RIGHT) {
        _ct_filter = (_ct_filter + 1) % 3;
        _ct_sel = 0;
        rebuildContactsSorted();
        return true;
      }
      // Contact list
      if (_ct_count > 0) {
        if (c == KEY_UP) {
          if (_ct_sel > 0) _ct_sel--;
          else enterStatusBar();
          return true;
        }
        if (c == KEY_DOWN) {
          if (_ct_sel < _ct_count - 1) _ct_sel++;
          else enterStatusBar();
          return true;
        }
        if (c == KEY_ENTER) {
          ContactInfo ci;
          if (the_mesh.getContactByIdx(_ct_sorted[_ct_sel], ci)) {
            memcpy(_ct_action_key, ci.id.pub_key, PUB_KEY_SIZE);
            _ct_action = true;
            _ct_action_sel = 0;
            _ct_detail_scroll = 0;
          }
          return true;
        }
      }
      if ((c == KEY_UP || c == KEY_DOWN) && _ct_count == 0) {
        enterStatusBar();
        return true;
      }
      return false;
    }
    if (_page == HomePage::NEARBY) {
      if (_scan_action) {
        if (c == KEY_CANCEL) {
          _scan_action = false;
          _scan_detail_scroll = 0;
          return true;
        }
        if (c == KEY_UP) {
          if (_scan_action_sel > 0) {
            _scan_action_sel--;
            if (_scan_action_sel < _scan_detail_scroll) _scan_detail_scroll = _scan_action_sel;
          } else if (_scan_detail_scroll > 0) {
            _scan_detail_scroll--;
          }
          return true;
        }
        if (c == KEY_DOWN) {
          if (_scan_action_sel < _scan_action_count - 1) {
            _scan_action_sel++;
            if (_scan_action_sel >= _scan_detail_scroll + 4) _scan_detail_scroll = _scan_action_sel - 3;
          } else {
            // Scroll into info lines
            ScanResult& sr = _scan_results[_scan_sel];
            int total = _scan_action_count + 4; // 4 info lines always present
            if (sr.in_contacts) {
              ContactCache* cc = findCache(sr.pub_key);
              if (cc && cc->has_path_info) total += 1;
              if (cc && cc->has_telem) total += 1;
              if (cc && cc->has_status) total += 1;
            }
            if (_scan_detail_scroll + 4 < total) _scan_detail_scroll++;
          }
          return true;
        }
        if (c == KEY_ENTER && _scan_action_count > 0) {
          ScanResult& sr = _scan_results[_scan_sel];
          ContactInfo ci;
          if (sr.in_contacts && getContactByKey(sr.pub_key, ci)) {
            const char* actions[6];
            uint8_t act_count = 0;
            if (sr.node_type == ADV_TYPE_REPEATER) {
              actions[act_count++] = "Ping";
            }
            actions[act_count++] = "Find Path";
            actions[act_count++] = "Telemetry";
            if (sr.node_type == ADV_TYPE_REPEATER) {
              actions[act_count++] = "Status";
            }
#if ENV_INCLUDE_GPS == 1
            {
              ContactCache* ncc = findCache(ci.id.pub_key);
              bool has_gps = (ci.gps_lat != 0 || ci.gps_lon != 0) || (ncc && ncc->has_gps);
              if (has_gps) {
                actions[act_count++] = "Navigate";
              }
            }
#endif
            const char* chosen = actions[_scan_action_sel];
            if (strcmp(chosen, "Ping") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendPing(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _scan_action = false;
                _ct_ping_pending = true;
                _ct_ping_done = false;
                _ct_detail_scroll = 0;
                _ct_ping_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
            } else if (strcmp(chosen, "Find Path") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendPathFind(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _scan_action = false;
                _ct_path_pending = true;
                _ct_path_found = false;
                _ct_detail_scroll = 0;
                _ct_path_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
            } else if (strcmp(chosen, "Telemetry") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendTelemetryReq(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _scan_action = false;
                _ct_telem_pending = true;
                _ct_telem_done = false;
                _ct_detail_scroll = 0;
                _ct_telem_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
            } else if (strcmp(chosen, "Status") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendStatusReq(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _scan_action = false;
                _ct_status_pending = true;
                _ct_status_done = false;
                _ct_detail_scroll = 0;
                _ct_status_timeout = millis() + est_timeout + 2000;
                strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
              } else {
                _task->showAlert("Send failed", 800);
              }
#if ENV_INCLUDE_GPS == 1
            } else if (strcmp(chosen, "Navigate") == 0) {
              ContactCache* ncc = findCache(ci.id.pub_key);
              bool has_cached_gps = (ncc && ncc->has_gps);
              if (has_cached_gps) {
                _nav_wp_lat = ncc->gps_lat;
                _nav_wp_lon = ncc->gps_lon;
              } else {
                _nav_wp_lat = ci.gps_lat / 1000000.0f;
                _nav_wp_lon = ci.gps_lon / 1000000.0f;
              }
              strncpy(_nav_wp_name, ci.name, sizeof(_nav_wp_name) - 1);
              _nav_wp_name[sizeof(_nav_wp_name) - 1] = '\0';
              _nav_has_waypoint = true;
              _scan_action = false;
              _page = HomePage::NAV;
#endif
            }
          }
          return true;
        }
        return true;
      }
      // Handle pending sub-states (path/telem/status results)
      if (_ct_gps_pending || _ct_gps_done || _ct_gps_no_fix) {
        if (c == KEY_CANCEL) {
          _ct_gps_pending = false; _ct_gps_done = false; _ct_gps_no_fix = false;
          _ct_detail_scroll = 0;
          _scan_action = true;
          return true;
        }
#if ENV_INCLUDE_GPS == 1
        if (c == KEY_ENTER && _ct_gps_done) {
          ContactCache* cc = findCache(_ct_path_key);
          if (cc && cc->has_gps) {
            _nav_wp_lat = cc->gps_lat; _nav_wp_lon = cc->gps_lon;
            strncpy(_nav_wp_name, _ct_target_name, sizeof(_nav_wp_name) - 1);
            _nav_wp_name[sizeof(_nav_wp_name) - 1] = '\0';
            _nav_has_waypoint = true;
            _ct_gps_pending = false; _ct_gps_done = false; _ct_gps_no_fix = false;
            _page = HomePage::NAV;
          }
          return true;
        }
#endif
        if (_ct_gps_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_ping_pending || _ct_ping_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_ping_pending = false; _ct_ping_done = false;
          _ct_detail_scroll = 0;
          _scan_action = true;
          return true;
        }
        if (_ct_ping_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_status_pending || _ct_status_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_status_pending = false; _ct_status_done = false;
          _ct_detail_scroll = 0;
          _scan_action = true;
          return true;
        }
        if (_ct_status_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_telem_pending || _ct_telem_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_telem_pending = false; _ct_telem_done = false;
          _ct_detail_scroll = 0;
          _scan_action = true;
          return true;
        }
        if (_ct_telem_done) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      if (_ct_path_pending) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_path_pending = false; _ct_path_found = false;
          _ct_detail_scroll = 0;
          _scan_action = true;
          return true;
        }
        if (_ct_path_found) {
          if (c == KEY_UP && _ct_detail_scroll > 0) { _ct_detail_scroll--; return true; }
          if (c == KEY_DOWN && _ct_detail_scroll < 3) { _ct_detail_scroll++; return true; }
        }
        return true;
      }
      // Result list
      if (c == KEY_CANCEL) {
        _page_active = false;
        return true;
      }
      // RIGHT or ENTER (when empty): start/restart scan
      if (c == KEY_RIGHT || (c == KEY_ENTER && _scan_count == 0)) {
        _scan_tag = random(1, 0x7FFFFFFF);
        the_mesh.startDiscoveryScan(_scan_tag);
        _scan_active = true;
        _scan_timeout = millis() + 8000;
        _scan_count = 0;
        memset(_scan_results, 0, sizeof(_scan_results));
        _scan_sel = 0;
        return true;
      }
      // ENTER on a result: open action menu
      if (c == KEY_ENTER && _scan_count > 0) {
        ScanResult& sr = _scan_results[_scan_sel];
        if (sr.valid && sr.in_contacts) {
          _scan_action = true;
          _scan_action_sel = 0;
          _scan_detail_scroll = 0;
        } else {
          _task->showAlert("Not in contacts", 1000);
        }
        return true;
      }
      if (c == KEY_UP && _scan_count > 0) {
        if (_scan_sel > 0) _scan_sel--;
        else enterStatusBar();
        return true;
      }
      if (c == KEY_DOWN && _scan_count > 0) {
        if (_scan_sel < _scan_count - 1) _scan_sel++;
        else enterStatusBar();
        return true;
      }
      if ((c == KEY_UP || c == KEY_DOWN) && _scan_count == 0) {
        enterStatusBar();
        return true;
      }
      return false;
    }
    if (_page == HomePage::SIGNALS) {
      if (_sig_action) {
        if (c == KEY_CANCEL) {
          _sig_action = false;
          return true;
        }
        if (c == KEY_UP) {
          if (_sig_detail_scroll > 0) _sig_detail_scroll--;
          else if (_sig_action_sel > 0) _sig_action_sel--;
          return true;
        }
        if (c == KEY_DOWN) {
          if (_sig_action_sel < 1) _sig_action_sel++;
          else if (_sig_detail_scroll < 3) _sig_detail_scroll++;
          return true;
        }
        if (c == KEY_ENTER) {
          if (_sig_action_sel == 0) {
            // Ping
            if (_task->_auto_ping_queue_count == 0 && !_task->_auto_ping_pending && !_task->_probe_active) {
              AbstractUITask::SignalEntry& se = _task->_signals[_sig_sel];
              se.tx_failed = false;
              se.fail_count = 0;
              se.has_tx = false;
              _task->_auto_ping_queue[0] = se.id;
              _task->_manual_ping_id = se.id;
              _task->_auto_ping_queue_count = 1;
              _task->_auto_ping_next = 0;
              _task->_auto_ping_next_time = millis() + 500;
              _task->showAlert("Ping queued", 800);
            } else {
              _task->showAlert("Ping busy", 800);
            }
            _sig_action = false;
          } else if (_sig_action_sel == 1) {
            // Delete
            for (uint8_t i = _sig_sel; i + 1 < _task->_signal_count; i++) {
              _task->_signals[i] = _task->_signals[i + 1];
            }
            _task->_signal_count--;
            if (_task->_signal_cycle > 0) _task->_signal_cycle--;
            if (_sig_sel > 0 && _sig_sel >= _task->_signal_count) _sig_sel = _task->_signal_count - 1;
            _sig_action = false;
          }
          return true;
        }
        return true;
      }
      // Signal list
      if (c == KEY_CANCEL) {
        _page_active = false;
        return true;
      }
      if (c == KEY_ENTER && _task->_signal_count > 0) {
        _sig_action = true;
        _sig_action_sel = 0;
        _sig_detail_scroll = 0;
        return true;
      }
      if (c == KEY_UP && _task->_signal_count > 0) {
        if (_sig_sel > 0) _sig_sel--;
        else enterStatusBar();
        return true;
      }
      if (c == KEY_DOWN && _task->_signal_count > 0) {
        if (_sig_sel < _task->_signal_count - 1) _sig_sel++;
        else enterStatusBar();
        return true;
      }
      if ((c == KEY_UP || c == KEY_DOWN) && _task->_signal_count == 0) {
        enterStatusBar();
        return true;
      }
      return false;
    }
    if (_page == HomePage::MESSAGES) {
      int total = countFilteredMsgs();
      if (_msg_detail) {
        // In detail view
        if (c == KEY_CANCEL) {
          if (_msg_reply_menu) {
            _msg_reply_menu = false;
          } else {
            _msg_detail = false;
            _msg_detail_scroll = 0;
            _path_sel = -1;
          }
          return true;
        }
        // LEFT/RIGHT: select repeaters on the path/heard-by line
        if (c == KEY_LEFT || c == KEY_RIGHT) {
          int _filt_idx = getFilteredMsgIndex(_msg_sel);
          if (_filt_idx < 0) return true;
          int buf_idx = (_task->_msg_log_next - 1 - _filt_idx + MSG_LOG_SIZE) % MSG_LOG_SIZE;
          auto& entry = _task->_msg_log[buf_idx];
          int plen = 0;
          if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF) {
            plen = entry.path_len;
          } else if (entry.is_sent && entry.repeat_path_len > 0) {
            // Count only repeaters with signal data (matching display filter)
            for (int r = 0; r < entry.repeat_path_len; r++) {
              if (entry.repeat_path_rssi[r] != 0) plen++;
            }
          }
          if (plen > 0) {
            if (c == KEY_RIGHT) {
              if (_path_sel < 0) _path_sel = 0;
              else if (_path_sel < plen - 1) _path_sel++;
            } else { // KEY_LEFT
              if (_path_sel > 0) _path_sel--;
              else if (_path_sel == 0) _path_sel = -1;
            }
          }
          return true;
        }
        if (_msg_reply_menu) {
          if (c == KEY_UP || c == KEY_DOWN) {
            _msg_reply_sel = 1 - _msg_reply_sel;
            return true;
          }
        } else {
          if (c == KEY_UP) {
            if (_msg_detail_scroll > 0) _msg_detail_scroll--;
            return true;
          }
          if (c == KEY_DOWN) {
            _msg_detail_scroll++;  // clamped during render
            return true;
          }
        }
        if (c == KEY_ENTER && _path_sel >= 0 && total > 0) {
          // Ping selected repeater
          int _filt_idx2 = getFilteredMsgIndex(_msg_sel);
          if (_filt_idx2 < 0) return true;
          int buf_idx = (_task->_msg_log_next - 1 - _filt_idx2 + MSG_LOG_SIZE) % MSG_LOG_SIZE;
          auto& entry = _task->_msg_log[buf_idx];
          uint8_t rpt_hash = 0;
          if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF && _path_sel < entry.path_len) {
            rpt_hash = entry.path[_path_sel];
          } else if (entry.is_sent) {
            // Map display index to actual repeat_path index (filtered by signal)
            int mapped = 0, disp_idx = 0;
            for (int r = 0; r < entry.repeat_path_len; r++) {
              if (entry.repeat_path_rssi[r] != 0) {
                if (disp_idx == _path_sel) { mapped = r; break; }
                disp_idx++;
              }
            }
            if (mapped < entry.repeat_path_len) rpt_hash = entry.repeat_path[mapped];
          }
          if (rpt_hash != 0) {
            // Search contacts for matching last pub_key byte
            ContactInfo ci;
            int n = the_mesh.getNumContacts();
            bool found = false;
            for (int i = 0; i < n; i++) {
              if (the_mesh.getContactByIdx(i, ci) && ci.id.pub_key[0] == rpt_hash) {
                uint32_t est_timeout;
                int result = the_mesh.sendPathFind(ci, est_timeout);
                if (result != MSG_SEND_FAILED) {
                  _msg_detail = false;
                  _path_sel = -1;
                  _page = HomePage::TRACE;
                  _page_active = true;
                  _ct_path_pending = true;
                  _ct_path_found = false;
                  _ct_detail_scroll = 0;
                  _ct_path_timeout = millis() + est_timeout + 2000;
                  strncpy(_ct_target_name, ci.name, sizeof(_ct_target_name));
                  _ct_target_name[sizeof(_ct_target_name) - 1] = '\0';
                  memcpy(_ct_path_key, ci.id.pub_key, PUB_KEY_SIZE);
                  found = true;
                } else {
                  _task->showAlert("Send failed", 800);
                  found = true;
                }
                break;
              }
            }
            if (!found) {
              char alert[24];
              snprintf(alert, sizeof(alert), "Unknown repeater %02X", rpt_hash);
              _task->showAlert(alert, 1000);
            }
          }
          return true;
        }
        if (c == KEY_ENTER && total > 0) {
          int _filt_idx3 = getFilteredMsgIndex(_msg_sel);
          if (_filt_idx3 < 0) return true;
          int buf_idx = (_task->_msg_log_next - 1 - _filt_idx3 + MSG_LOG_SIZE) % MSG_LOG_SIZE;
          auto& entry = _task->_msg_log[buf_idx];

          // Channel message reply: show reply type menu first
          if (entry.channel_idx >= 0 && !entry.is_sent) {
            if (!_msg_reply_menu) {
              // First press: show the reply type menu
              _msg_reply_menu = true;
              _msg_reply_sel = 0;
              _msg_detail_scroll = 255; // will be clamped during render to show bottom
              return true;
            }
            // Second press: execute selected reply
            _msg_detail = false;
            _msg_reply_menu = false;
            if (_msg_reply_sel == 0) {
              // Reply on channel with @sender
              ChannelDetails cd;
              if (the_mesh.getChannel(entry.channel_idx, cd)) {
                // Extract sender name from "sender: text" format for @mention
                const char* colon = strstr(entry.text, ": ");
                char mention[28] = "";
                if (colon && colon - entry.text < 24) {
                  int nlen = colon - entry.text;
                  mention[0] = '@';
                  memcpy(&mention[1], entry.text, nlen);
                  mention[1 + nlen] = ' ';
                  mention[2 + nlen] = '\0';
                }
                _task->startChannelCompose(entry.channel_idx, cd.name, mention[0] ? mention : NULL);
              }
            } else {
              // DM the sender directly
              const char* colon = strstr(entry.text, ": ");
              if (colon && colon - entry.text < 24) {
                char sender_name[24];
                int nlen = colon - entry.text;
                memcpy(sender_name, entry.text, nlen);
                sender_name[nlen] = '\0';
                ContactInfo ci;
                int n = the_mesh.getNumContacts();
                for (int i = 0; i < n; i++) {
                  if (the_mesh.getContactByIdx(i, ci) && strcmp(ci.name, sender_name) == 0) {
                    _task->startDMCompose(ci);
                    return true;
                  }
                }
                _task->showAlert("Contact not found", 800);
              } else {
                _task->showAlert("Unknown sender", 800);
              }
            }
            return true;
          }
          // DM reply: compose DM to sender (no @mention needed)
          if (entry.contact_name[0] != '\0' && !entry.is_sent) {
            _msg_detail = false;
            ContactInfo ci;
            int n = the_mesh.getNumContacts();
            for (int i = 0; i < n; i++) {
              if (the_mesh.getContactByIdx(i, ci) && strcmp(ci.name, entry.contact_name) == 0) {
                _task->startDMCompose(ci);
                return true;
              }
            }
            _task->showAlert("Contact not found", 800);
            return true;
          }
          // Resend: sent message with no heard repeats and not delivered
          if (entry.is_sent && entry.heard_repeats == 0 && !entry.delivered) {
            bool resent = false;
            if (entry.channel_idx < 0 && entry.contact_name[0] != '\0') {
              // DM resend
              ContactInfo ci;
              int n = the_mesh.getNumContacts();
              for (int i = 0; i < n; i++) {
                if (the_mesh.getContactByIdx(i, ci) && strcmp(ci.name, entry.contact_name) == 0) {
                  uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
                  uint32_t expected_ack = 0;
                  uint32_t est_timeout = 0;
                  the_mesh.sendMessage(ci, ts, 0, entry.text, expected_ack, est_timeout);
                  the_mesh.registerExpectedAck(expected_ack, NULL);
                  entry.expected_ack = expected_ack;
                  entry.delivered = false;
                  resent = true;
                  break;
                }
              }
              if (!resent) {
                _task->showAlert("Contact not found", 800);
                return true;
              }
            } else if (entry.channel_idx >= 0) {
              // Channel resend
              ChannelDetails cd;
              if (the_mesh.getChannel(entry.channel_idx, cd)) {
                uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
                the_mesh.sendGroupMessage(ts, cd.channel,
                  the_mesh.getNodePrefs()->node_name, entry.text, strlen(entry.text));
                resent = true;
              } else {
                _task->showAlert("No channel", 800);
                return true;
              }
            }
            if (resent) {
              entry.tx_count++;
              // Strip old TX marker if present, then append new one
              char* marker = strstr(entry.text, " (TX:");
              if (marker) *marker = '\0';
              int tlen = strlen(entry.text);
              snprintf(entry.text + tlen, sizeof(entry.text) - tlen, " (TX: #%d)", entry.tx_count);
              memcpy(entry.packet_hash, the_mesh.getLastSentHash(), MAX_HASH_SIZE);
              entry.heard_repeats = 0;
              _msg_detail = false;
              _task->showAlert("Resent!", 800);
            }
            return true;
          }
          return true;
        }
        return true; // consume all keys in detail mode
      }
      // Quick-send compose menu handling
      if (_msg_compose_menu) {
        // Count non-empty presets
        int preset_count = 0;
        int preset_indices[PRESET_MSG_COUNT];
        for (int i = 0; i < PRESET_MSG_COUNT; i++) {
          if (i == PRESET_GPS_INDEX) continue;
          if (preset_messages[i] && preset_messages[i][0] != '\0') {
            preset_indices[preset_count++] = i;
          }
        }
        int total_items = 1 + preset_count;
        if (c == KEY_UP) {
          if (_msg_compose_sel > 0) _msg_compose_sel--;
          else _msg_compose_menu = false;
          return true;
        }
        if (c == KEY_DOWN) {
          if (_msg_compose_sel < total_items - 1) _msg_compose_sel++;
          return true;
        }
        if (c == KEY_CANCEL) {
          _msg_compose_menu = false;
          return true;
        }
        if (c == KEY_ENTER) {
          if (_msg_compose_sel == 0) {
            // Keyboard: open full compose pre-targeted
            int fval = _msg_filter_channels[_msg_filter];
            if (fval >= 0) {
              ChannelDetails cd;
              if (the_mesh.getChannel(fval, cd)) {
                _msg_compose_menu = false;
                _task->startChannelCompose(fval, cd.name);
              } else {
                _task->showAlert("No channel", 800);
              }
            } else {
              // DM target
              int dm_idx = -fval - 2;
              const char* dm_name = (dm_idx >= 0 && dm_idx < 4) ? _msg_filter_dm_names[dm_idx] : NULL;
              if (dm_name) {
                ContactInfo ci;
                int n = the_mesh.getNumContacts();
                bool found = false;
                for (int i = 0; i < n; i++) {
                  if (the_mesh.getContactByIdx(i, ci) && strcmp(ci.name, dm_name) == 0) {
                    _msg_compose_menu = false;
                    _task->startDMCompose(ci);
                    found = true;
                    break;
                  }
                }
                if (!found) _task->showAlert("Contact not found", 800);
              }
            }
            _msg_compose_menu = false;
            return true;
          } else {
            // Preset: send immediately
            int pi = preset_indices[_msg_compose_sel - 1];
            const char* text = preset_messages[pi];
            int fval = _msg_filter_channels[_msg_filter];
            if (fval >= 0) {
              // Channel send
              ChannelDetails cd;
              if (the_mesh.getChannel(fval, cd)) {
                uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
                bool ok = the_mesh.sendGroupMessage(ts, cd.channel,
                            the_mesh.getNodePrefs()->node_name, text, strlen(text));
                the_mesh.queueSentChannelMessage(fval, ts, text, strlen(text));
                _task->addToMsgLog("You", text, true, 0, fval, NULL, NULL, the_mesh.getLastSentHash());
                _task->notify(UIEventType::ack);
                _task->showAlert(ok ? "Sent!" : "Send failed", 800);
              } else {
                _task->showAlert("No channel", 800);
              }
            } else {
              // DM send
              int dm_idx = -fval - 2;
              const char* dm_name = (dm_idx >= 0 && dm_idx < 4) ? _msg_filter_dm_names[dm_idx] : NULL;
              if (dm_name) {
                ContactInfo ci;
                int n = the_mesh.getNumContacts();
                bool found = false;
                for (int i = 0; i < n; i++) {
                  if (the_mesh.getContactByIdx(i, ci) && strcmp(ci.name, dm_name) == 0) {
                    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
                    uint32_t expected_ack = 0;
                    uint32_t est_timeout = 0;
                    int result = the_mesh.sendMessage(ci, ts, 0, text, expected_ack, est_timeout);
                    the_mesh.registerExpectedAck(expected_ack, NULL);
                    _task->addToMsgLog("You", text, true, 0, -1, ci.name, NULL, the_mesh.getLastSentHash(), expected_ack);
                    _task->notify(UIEventType::ack);
                    _task->showAlert(result > 0 ? "DM Sent!" : "DM failed", 800);
                    found = true;
                    break;
                  }
                }
                if (!found) _task->showAlert("Contact not found", 800);
              }
            }
            _msg_sel = 0xFF;  // scroll to latest after send
            _msg_compose_menu = false;
            return true;
          }
        }
        return true;  // consume all keys in compose menu
      }
      // Channel/DM target chooser (All tab)
      if (_msg_target_menu) {
        if (c == KEY_UP) {
          if (_msg_target_sel > 0) _msg_target_sel--;
          else _msg_target_menu = false;
          return true;
        }
        if (c == KEY_DOWN) {
          if (_msg_target_sel < 1) _msg_target_sel++;
          return true;
        }
        if (c == KEY_CANCEL) {
          _msg_target_menu = false;
          return true;
        }
        if (c == KEY_ENTER) {
          _msg_target_menu = false;
          if (_msg_target_sel == 0) {
            _task->gotoChannelSelect();
          } else {
            _task->gotoContactSelect(false);
          }
          return true;
        }
        return true;
      }
      // CANCEL: back to carousel from message list
      if (c == KEY_CANCEL) {
        _page_active = false;
        _msg_filter = 0;  // reset filter on exit
        _msg_compose_menu = false;
        _msg_target_menu = false;
        return true;
      }
      // LEFT/RIGHT: cycle message filter
      if (c == KEY_LEFT) {
        rebuildMsgFilters();
        _msg_filter = (_msg_filter + _msg_filter_count - 1) % _msg_filter_count;
        _msg_sel = 0xFF;  // clamp to newest (bottom) on render
        _msg_sel_prev = 0xFF;
        _msg_scroll_px = 0;
        _msg_vscroll = 0;
        _msg_compose_menu = false;
        _msg_target_menu = false;
        return true;
      }
      if (c == KEY_RIGHT) {
        rebuildMsgFilters();
        _msg_filter = (_msg_filter + 1) % _msg_filter_count;
        _msg_sel = 0xFF;  // clamp to newest (bottom) on render
        _msg_sel_prev = 0xFF;
        _msg_scroll_px = 0;
        _msg_vscroll = 0;
        _msg_compose_menu = false;
        _msg_target_menu = false;
        return true;
      }
      {
        int filtered_total = countFilteredMsgs();
        if (filtered_total > 0) {
          if (_msg_sel >= filtered_total) _msg_sel = filtered_total - 1;
          if (c == KEY_UP) {
            if (_msg_sel > 0) _msg_sel--;
            else enterStatusBar();
            return true;
          }
          if (c == KEY_DOWN) {
            if (_msg_sel < filtered_total - 1) {
              _msg_sel++;
            } else if (_msg_filter > 0) {
              _msg_compose_menu = true;
              _msg_compose_sel = 0;
            } else {
              // "All" tab: show Channel/DM target chooser
              _msg_target_menu = true;
              _msg_target_sel = 0;
            }
            return true;
          }
          if (c == KEY_ENTER) {
            _msg_detail = true;
            _msg_detail_scroll = 0;
            _path_sel = -1;
            return true;
          }
        } else if (c == KEY_UP) {
          enterStatusBar();
          return true;
        } else if (c == KEY_DOWN) {
          // Allow compose from empty message list
          if (_msg_filter > 0) {
            _msg_compose_menu = true;
            _msg_compose_sel = 0;
          } else {
            _msg_target_menu = true;
            _msg_target_sel = 0;
          }
          return true;
        }
      }
      return false;
    }
    if (_page == HomePage::PRESETS) {
      if (_preset_target_choosing) {
        // Sub-menu: Channel / DM selection
        if (c == KEY_UP || c == KEY_DOWN) {
          _preset_target_sel = 1 - _preset_target_sel;
          return true;
        }
        if (c == KEY_CANCEL) {
          _preset_target_choosing = false;
          return true;
        }
        if (c == KEY_ENTER) {
          _preset_target_choosing = false;
          if (_preset_target_sel == 0) {
            // Channel: go to channel select, send preset after selection
            _task->gotoChannelSelect();
          } else {
            // DM: go to contact select, send preset after selection
            _task->gotoContactSelect(false);
          }
          return true;
        }
        return true;
      }
      if (_preset_edit_mode) {
        // Edit/Delete sub-menu key handling
        int preset_count = 0;
        for (int i = 0; i < PRESET_MSG_COUNT; i++) {
          if (preset_buf[i][0] != '\0') preset_count++;
        }
        if (c == KEY_CANCEL) {
          _preset_edit_mode = false;
          return true;
        }
        if (c == KEY_UP && _preset_edit_sel > 0) {
          _preset_edit_sel--;
          return true;
        }
        if (c == KEY_DOWN && _preset_edit_sel < preset_count - 1) {
          _preset_edit_sel++;
          return true;
        }
        if (c == KEY_ENTER && preset_count > 0) {
          // Delete selected preset: find the nth non-empty slot
          int nth = 0;
          for (int i = 0; i < PRESET_MSG_COUNT; i++) {
            if (preset_buf[i][0] == '\0') continue;
            if (nth == _preset_edit_sel) {
              // Shift remaining presets up
              for (int j = i; j < PRESET_MSG_COUNT - 1; j++) {
                strncpy(preset_buf[j], preset_buf[j + 1], PRESET_MAX_LEN);
              }
              preset_buf[PRESET_MSG_COUNT - 1][0] = '\0';
              preset_messages[PRESET_MSG_COUNT - 1] = preset_buf[PRESET_MSG_COUNT - 1];
              savePresetsToFile();
              _task->showAlert("Deleted!", 800);
              if (_preset_edit_sel >= preset_count - 1 && _preset_edit_sel > 0) _preset_edit_sel--;
              break;
            }
            nth++;
          }
          return true;
        }
        return true;
      }
      int total_items = PRESET_MSG_COUNT + 6;
      if (c == KEY_UP) {
        if (_preset_sel > 0) _preset_sel--;
        else enterStatusBar();
        return true;
      }
      if (c == KEY_DOWN) {
        if (_preset_sel < total_items - 1) _preset_sel++;
        else enterStatusBar();
        return true;
      }
      if (c == KEY_ENTER) {
        if (_preset_sel == PRESET_MSG_COUNT) {
          // "[Compose...]" selected
          _task->gotoComposeScreen();
          return true;
        }
        if (_preset_sel == PRESET_MSG_COUNT + 1) {
          // "[Reply DM...]" selected
          _task->_preset_pending = false;
          _task->gotoContactSelect(false);
          return true;
        }
        if (_preset_sel == PRESET_MSG_COUNT + 2) {
          // "[Send GPS DM...]" selected
          _task->_preset_pending = false;
          _task->gotoContactSelect(true);
          return true;
        }
        if (_preset_sel == PRESET_MSG_COUNT + 3) {
          // "[Channel Msg...]" selected
          _task->_preset_pending = false;
          _task->gotoChannelSelect();
          return true;
        }
        if (_preset_sel == PRESET_MSG_COUNT + 4) {
          // "[+ Add Message]" selected - enter compose, save result as preset
          int slot = -1;
          for (int i = 0; i < PRESET_MSG_COUNT; i++) {
            if (preset_buf[i][0] == '\0') { slot = i; break; }
          }
          if (slot < 0) {
            _task->showAlert("Presets full!", 800);
          } else {
            _task->_preset_add_mode = true;
            _task->gotoComposeScreen();
          }
          return true;
        }
        if (_preset_sel == PRESET_MSG_COUNT + 5) {
          // "[Edit/Delete]" selected
          _preset_edit_mode = true;
          _preset_edit_sel = 0;
          return true;
        }
        // Preset selected: resolve text, then show Channel/DM target sub-menu
        const char* text = NULL;
        char gps_text[48];
#if ENV_INCLUDE_GPS == 1
        if (_preset_sel == PRESET_GPS_INDEX) {
          LocationProvider* nmea = sensors.getLocationProvider();
          if (nmea != NULL && nmea->isValid()) {
            snprintf(gps_text, sizeof(gps_text), "GPS: %.6f, %.6f",
                     nmea->getLatitude() / 1000000.0, nmea->getLongitude() / 1000000.0);
            text = gps_text;
          } else {
            _task->showAlert("No GPS fix", 800);
            return true;
          }
        } else {
          text = preset_messages[_preset_sel];
        }
#else
        if (_preset_sel == PRESET_GPS_INDEX) {
          _task->showAlert("GPS not enabled", 800);
          return true;
        }
        text = preset_messages[_preset_sel];
#endif
        if (text != NULL && text[0] != '\0') {
          strncpy(_task->_pending_preset, text, sizeof(_task->_pending_preset) - 1);
          _task->_pending_preset[sizeof(_task->_pending_preset) - 1] = '\0';
          _task->_preset_pending = true;
          _preset_target_choosing = true;
          _preset_target_sel = 0;
        }
        return true;
      }
    }
    if (c == KEY_ENTER && _page == HomePage::ADVERT) {
      _task->notify(UIEventType::ack);
      if (the_mesh.advert()) {
        _task->showAlert("Advert sent!", 1000);
      } else {
        _task->showAlert("Advert failed..", 1000);
      }
      return true;
    }
#if ENV_INCLUDE_GPS == 1
    if (c == KEY_ENTER && _page == HomePage::GPS) {
      _task->toggleGPS();
      return true;
    }
    if (c == KEY_UP && _page == HomePage::NAV) {
      _nav_screen_lock = !_nav_screen_lock;
      _task->showAlert(_nav_screen_lock ? "Screen lock: ON" : "Screen lock: OFF", 800);
      return true;
    }
    if (c == KEY_DOWN && _page == HomePage::NAV) {
      if (_nav_has_waypoint) {
        _nav_has_waypoint = false;
        _task->showAlert("WP cleared", 800);
      } else {
        _max_speed = 0;
        _odometer = 0;
        _odo_last = 0;
        _odo_last_lat = 0;
        _odo_last_lon = 0;
        _task->showAlert("Trip reset", 800);
      }
      return true;
    }
#endif
    if (_page == HomePage::PACKETS) {
      int total = _task->_pkt_log_count;
      if (_pkt_detail) {
        if (c == KEY_CANCEL) {
          _pkt_detail = false;
          _pkt_detail_scroll = 0;
          return true;
        }
        if (c == KEY_UP) {
          if (_pkt_detail_scroll > 0) _pkt_detail_scroll--;
          return true;
        }
        if (c == KEY_DOWN) {
          _pkt_detail_scroll++;  // clamped during render
          return true;
        }
      } else if (total > 0) {
        if (c == KEY_ENTER) {
          _pkt_detail = true;
          _pkt_detail_scroll = 0;
          return true;
        }
        if (c == KEY_UP) {
          if (_pkt_sel > 0) _pkt_sel--;
          else enterStatusBar();
          return true;
        }
        if (c == KEY_DOWN) {
          if (_pkt_sel < total - 1) _pkt_sel++;
          else enterStatusBar();
          return true;
        }
      }
    }
#if UI_SENSORS_PAGE == 1
    if (c == KEY_ENTER && _page == HomePage::SENSORS) {
      _task->toggleGPS();
      next_sensors_refresh=0;
      return true;
    }
#endif
    if (c == KEY_ENTER && _page == HomePage::SHUTDOWN) {
      _shutdown_init = true;  // need to wait for button to be released
      return true;
    }
    if (_page == HomePage::SETTINGS) {
      int sc = 0;
      int id_gmt = sc++;
      int id_voltage = sc++;
      int id_snr = sc++;
#if ENV_INCLUDE_GPS == 1
      int id_speed = sc++;
#endif
      int id_beep_ble = sc++;
      int id_auto_tx = sc++;
#if ENV_INCLUDE_GPS == 1
      int id_motion = sc++;
#endif
      int id_ble = sc++;
#if ENV_INCLUDE_GPS == 1
      int id_gps = sc++;
#endif
      if (_settings_sel >= sc) _settings_sel = sc - 1;

      if (c == KEY_UP) { if (_settings_sel > 0) _settings_sel--; else enterStatusBar(); return true; }
      if (c == KEY_DOWN) { if (_settings_sel < sc - 1) _settings_sel++; else enterStatusBar(); return true; }
      // GMT offset: LEFT/RIGHT to adjust
      if (_settings_sel == id_gmt && (c == KEY_LEFT || c == KEY_RIGHT)) {
        if (c == KEY_LEFT && _gmt_offset > -12) _gmt_offset--;
        if (c == KEY_RIGHT && _gmt_offset < 14) _gmt_offset++;
        _node_prefs->gmt_offset = _gmt_offset;
        the_mesh.savePrefs();
        char alert[16];
        snprintf(alert, sizeof(alert), "GMT: %s%d", _gmt_offset >= 0 ? "+" : "", _gmt_offset);
        _task->showAlert(alert, 800);
        return true;
      }
      if (c == KEY_ENTER) {
        if (_settings_sel == id_gmt) {
          // ENTER on GMT: treat same as RIGHT for convenience
          if (_gmt_offset < 14) _gmt_offset++;
          _node_prefs->gmt_offset = _gmt_offset;
          the_mesh.savePrefs();
          char alert[16];
          snprintf(alert, sizeof(alert), "GMT: %s%d", _gmt_offset >= 0 ? "+" : "", _gmt_offset);
          _task->showAlert(alert, 800);
        } else if (_settings_sel == id_voltage) {
          _show_voltage = !_show_voltage;
          _node_prefs->ui_flags = buildUiFlags();
          the_mesh.savePrefs();
          _task->showAlert(_show_voltage ? "Voltage: ON" : "Voltage: OFF", 800);
        } else if (_settings_sel == id_snr) {
          _show_snr = !_show_snr;
          _node_prefs->ui_flags = buildUiFlags();
          the_mesh.savePrefs();
          _task->showAlert(_show_snr ? "Signal: ON" : "Signal: OFF", 800);
        }
#if ENV_INCLUDE_GPS == 1
        else if (_settings_sel == id_speed) {
          _show_speed = !_show_speed;
          _node_prefs->ui_flags = buildUiFlags();
          the_mesh.savePrefs();
          _task->showAlert(_show_speed ? "Speed: ON" : "Speed: OFF", 800);
        }
#endif
        else if (_settings_sel == id_beep_ble) {
          _beep_on_ble = !_beep_on_ble;
          _node_prefs->ui_flags = buildUiFlags();
          the_mesh.savePrefs();
          _task->showAlert(_beep_on_ble ? "Beep w/ BLE: ON" : "Beep w/ BLE: OFF", 800);
        }
        else if (_settings_sel == id_auto_tx) {
          _auto_tx_check = !_auto_tx_check;
          _task->_auto_tx_enabled = _auto_tx_check;
          _node_prefs->ui_flags = buildUiFlags();
          the_mesh.savePrefs();
          _task->showAlert(_auto_tx_check ? "Auto TX: ON" : "Auto TX: OFF", 800);
        }
#if ENV_INCLUDE_GPS == 1
        else if (_settings_sel == id_motion) {
          _motion_mode_setting = (_motion_mode_setting + 1) & 0x03;
          _task->_motion_mode = _motion_mode_setting;
          _node_prefs->ui_flags = buildUiFlags();
          the_mesh.savePrefs();
          const char* mm[] = {"Off", "Auto", "Bike", "Drive"};
          if (_motion_mode_setting == 1 && !_task->getGPSState()) {
            _task->showAlert("Auto needs GPS", 1200);
          } else {
            char alert[24];
            snprintf(alert, sizeof(alert), "Motion: %s", mm[_motion_mode_setting & 0x03]);
            _task->showAlert(alert, 800);
          }
        }
#endif
        else if (_settings_sel == id_ble) {
          if (_task->isSerialEnabled()) _task->disableSerial();
          else _task->enableSerial();
        }
#if ENV_INCLUDE_GPS == 1
        else if (_settings_sel == id_gps) {
          _task->toggleGPS();
        }
#endif
        return true;
      }
      if (c == KEY_CANCEL) {
        _page_active = false;
        return true;
      }
      return true;  // consume remaining keys in settings
    }
    // Generic CANCEL: go back to carousel from any page top level
    if (c == KEY_CANCEL) {
      _page_active = false;
      _nav_screen_lock = false;
      return true;
    }
    return false;
  }
};

class MsgPreviewScreen : public UIScreen {
  UITask* _task;
  mesh::RTCClock* _rtc;

  struct MsgEntry {
    uint32_t timestamp;
    char origin[62];
    char msg[78];
  };
  #define MAX_UNREAD_MSGS   32
  int num_unread;
  int head = MAX_UNREAD_MSGS - 1; // index of latest unread message
  MsgEntry unread[MAX_UNREAD_MSGS];

public:
  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { num_unread = 0; }

  void addPreview(uint8_t path_len, const char* from_name, const char* msg) {
    head = (head + 1) % MAX_UNREAD_MSGS;
    if (num_unread < MAX_UNREAD_MSGS) num_unread++;

    auto p = &unread[head];
    p->timestamp = _rtc->getCurrentTime();
    if (path_len == 0xFF) {
      sprintf(p->origin, "(D) %s:", from_name);
    } else {
      sprintf(p->origin, "(%d) %s:", (uint32_t) path_len, from_name);
    }
    StrHelper::strncpy(p->msg, msg, sizeof(p->msg));
  }

  int render(DisplayDriver& display) override {
    char tmp[16];
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);
    sprintf(tmp, "Unread: %d", num_unread);
    display.print(tmp);

    auto p = &unread[head];

    int secs = _rtc->getCurrentTime() - p->timestamp;
    if (secs < 60) {
      sprintf(tmp, "%ds", secs);
    } else if (secs < 60*60) {
      sprintf(tmp, "%dm", secs / 60);
    } else {
      sprintf(tmp, "%dh", secs / (60*60));
    }
    display.setCursor(display.width() - display.getTextWidth(tmp) - 2, 0);
    display.print(tmp);

    display.drawRect(0, 11, display.width(), 1);  // horiz line

    display.setCursor(0, 14);
    display.setColor(DisplayDriver::YELLOW);
    char filtered_origin[sizeof(p->origin)];
    display.translateUTF8ToBlocks(filtered_origin, p->origin, sizeof(filtered_origin));
    display.print(filtered_origin);

    display.setCursor(0, 25);
    display.setColor(DisplayDriver::LIGHT);
    char filtered_msg[sizeof(p->msg)];
    display.translateUTF8ToBlocks(filtered_msg, p->msg, sizeof(filtered_msg));
    display.printWordWrap(filtered_msg, display.width());

#if AUTO_OFF_MILLIS==0 // probably e-ink
    return 10000; // 10 s
#else
    return 1000;  // next render after 1000 ms
#endif
  }

  bool handleInput(char c) override {
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      head = (head + MAX_UNREAD_MSGS - 1) % MAX_UNREAD_MSGS;
      num_unread--;
      if (num_unread == 0) {
        _task->gotoHomeScreen();
      }
      return true;
    }
    if (c == KEY_ENTER) {
      num_unread = 0;  // clear unread queue
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

class ComposeScreen : public UIScreen {
  static const uint8_t KB_ROWS = 5;
  static const uint8_t KB_COLS = 10;
  static const char KB_CHARS[KB_ROWS * KB_COLS];
  // special keys: \x01=SND, \x02=SHIFT, \x04=ESC (back button = backspace)

  UITask* _task;
  char _compose_buf[MAX_TEXT_LEN];
  uint8_t _compose_len;
  uint8_t _kb_row, _kb_col;
  bool _dm_mode;
  bool _caps;
  ContactInfo _dm_contact;
  int _channel_idx;       // -1 = default (ch 0), >=0 = specific channel
  char _channel_name[32]; // display name for selected channel

public:
  ComposeScreen(UITask* task) : _task(task) {
    reset();
  }

  void reset() {
    _compose_len = 0;
    _compose_buf[0] = '\0';
    _kb_row = 0;
    _kb_col = 0;
    _dm_mode = false;
    _caps = false;  // start lowercase
    _channel_idx = -1;
  }

  void setDMTarget(const ContactInfo& contact) {
    _dm_mode = true;
    _dm_contact = contact;
  }

  void setChannel(int idx, const char* name) {
    _channel_idx = idx;
    strncpy(_channel_name, name, sizeof(_channel_name) - 1);
    _channel_name[sizeof(_channel_name) - 1] = '\0';
  }

  // Pre-fill compose buffer with text (e.g., "@username ")
  void prefill(const char* text) {
    int len = strlen(text);
    if (len >= MAX_TEXT_LEN) len = MAX_TEXT_LEN - 1;
    memcpy(_compose_buf, text, len);
    _compose_buf[len] = '\0';
    _compose_len = len;
  }

  int render(DisplayDriver& display) override {
    // Two-line text area: line 1 = prefix, line 2 = message text
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);

    // Line 1: prefix (DM:name, #channel, or ">")
    char prefix[32];
    if (_dm_mode) {
      snprintf(prefix, sizeof(prefix), "DM:%s", _dm_contact.name);
    } else if (_channel_idx >= 0) {
      if (_channel_name[0] == '#') {
        snprintf(prefix, sizeof(prefix), "%s", _channel_name);
      } else {
        snprintf(prefix, sizeof(prefix), "#%s", _channel_name);
      }
    } else {
      strcpy(prefix, "> broadcast");
    }
    display.drawTextEllipsized(0, 0, display.width(), prefix);

    // Line 2: message text with cursor, scrolls to show tail
    display.setColor(DisplayDriver::YELLOW);
    int avail_w = display.width() - display.getTextWidth("_");
    int start = 0;
    if (_compose_len > 0) {
      int char_w = display.getTextWidth("A");
      int max_chars = (char_w > 0) ? avail_w / char_w : 19;
      if (_compose_len > max_chars) start = _compose_len - max_chars;
    }
    char tail[MAX_TEXT_LEN + 2];
    snprintf(tail, sizeof(tail), "%s_", &_compose_buf[start]);
    display.drawTextEllipsized(0, 10, display.width(), tail);

    // Divider
    display.drawRect(0, 20, display.width(), 1);

    // Keyboard grid (5 rows x 10 cols, compact)
    int cell_w = display.width() / KB_COLS;
    int start_y = 22;
    int row_h = 8;

    for (uint8_t r = 0; r < KB_ROWS; r++) {
      for (uint8_t c = 0; c < KB_COLS; c++) {
        int idx = r * KB_COLS + c;
        int x = c * cell_w;
        int y = start_y + r * row_h;
        bool selected = (r == _kb_row && c == _kb_col);

        if (selected) {
          display.setColor(DisplayDriver::YELLOW);
          display.fillRect(x, y, cell_w, row_h);
          display.setColor(DisplayDriver::DARK);
        } else {
          display.setColor(DisplayDriver::LIGHT);
        }

        char ch = KB_CHARS[idx];
        if (ch == '\x01') {
          display.setCursor(x + 1, y + 1);
          display.print("SND");
        } else if (ch == '\x02') {
          display.setCursor(x + 1, y + 1);
          display.print(_caps ? "aa" : "AA");
        } else if (ch == '\x03') {
          display.setCursor(x + 1, y + 1);
          display.print("GP");
        } else if (ch == '\x04') {
          display.setCursor(x + 1, y + 1);
          display.print("ESC");
        } else if (ch == ' ') {
          display.setCursor(x + 2, y + 1);
          display.print("_");
        } else {
          char display_ch = ch;
          if (ch >= 'A' && ch <= 'Z' && !_caps) {
            display_ch = ch + 32;  // lowercase
          }
          char s[2] = { display_ch, '\0' };
          display.setCursor(x + (cell_w - display.getTextWidth(s)) / 2, y + 1);
          display.print(s);
        }
      }
    }
    return 5000;
  }

  bool handleInput(char c) override {
    if (c == KEY_UP) {
      _kb_row = (_kb_row + KB_ROWS - 1) % KB_ROWS;
      return true;
    }
    if (c == KEY_DOWN) {
      _kb_row = (_kb_row + 1) % KB_ROWS;
      return true;
    }
    if (c == KEY_LEFT) {
      _kb_col = (_kb_col + KB_COLS - 1) % KB_COLS;
      return true;
    }
    if (c == KEY_RIGHT) {
      _kb_col = (_kb_col + 1) % KB_COLS;
      return true;
    }
    if (c == KEY_ENTER) {
      char ch = KB_CHARS[_kb_row * KB_COLS + _kb_col];
      if (ch == '\x01') {
        // SND: send the composed message (or save as preset)
        if (_compose_len > 0 && _task->_preset_add_mode) {
          // Save composed text as new preset
          _task->_preset_add_mode = false;
          for (int i = 0; i < PRESET_MSG_COUNT; i++) {
            if (preset_buf[i][0] == '\0') {
              strncpy(preset_buf[i], _compose_buf, PRESET_MAX_LEN - 1);
              preset_buf[i][PRESET_MAX_LEN - 1] = '\0';
              preset_messages[i] = preset_buf[i];
              savePresetsToFile();
              _task->showAlert("Preset saved!", 800);
              break;
            }
          }
          reset();
          _task->gotoHomeScreen();
          return true;
        }
        if (_compose_len > 0) {
          if (_dm_mode) {
            uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
            uint32_t expected_ack = 0;
            uint32_t est_timeout = 0;
            int result = the_mesh.sendMessage(_dm_contact, ts, 0, _compose_buf, expected_ack, est_timeout);
            the_mesh.registerExpectedAck(expected_ack, NULL);
            // Note: DM sync to companion app not supported - protocol has no outgoing flag
            _task->addToMsgLog("You", _compose_buf, true, 0, -1, _dm_contact.name, NULL, the_mesh.getLastSentHash(), expected_ack);
            _task->notify(UIEventType::ack);
            _task->showAlert(result > 0 ? "DM Sent!" : "DM failed", 800);
          } else {
            int ch_idx = (_channel_idx >= 0) ? _channel_idx : 0;
            ChannelDetails ch_det;
            if (the_mesh.getChannel(ch_idx, ch_det)) {
              uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
              bool ok = the_mesh.sendGroupMessage(ts, ch_det.channel,
                            the_mesh.getNodePrefs()->node_name, _compose_buf, _compose_len);
              the_mesh.queueSentChannelMessage(ch_idx, ts, _compose_buf, _compose_len);
              _task->addToMsgLog("You", _compose_buf, true, 0, ch_idx, NULL, NULL, the_mesh.getLastSentHash());
              _task->notify(UIEventType::ack);
              _task->showAlert(ok ? "Sent!" : "Send failed", 800);
            } else {
              _task->showAlert("No channel", 800);
            }
          }
        }
        // Save target info before reset clears it
        int ret_ch = _dm_mode ? -1 : ((_channel_idx >= 0) ? _channel_idx : 0);
        char ret_dm[24] = "";
        if (_dm_mode) strncpy(ret_dm, _dm_contact.name, 23);
        reset();
        _task->gotoMessagesScreenFiltered(ret_ch, ret_dm[0] ? ret_dm : NULL);
        return true;
      }
      if (ch == '\x02') {
        // SHIFT: toggle caps
        _caps = !_caps;
        return true;
      }
      if (ch == '\x03') {
        // GPS paste: append current coordinates to compose buffer
#if ENV_INCLUDE_GPS == 1
        LocationProvider* nmea = sensors.getLocationProvider();
        if (nmea != NULL && nmea->isValid()) {
          char gps_str[32];
          snprintf(gps_str, sizeof(gps_str), "%.6f,%.6f",
                   nmea->getLatitude() / 1000000.0, nmea->getLongitude() / 1000000.0);
          int gps_len = strlen(gps_str);
          for (int gi = 0; gi < gps_len && _compose_len < MAX_TEXT_LEN - 1; gi++) {
            _compose_buf[_compose_len++] = gps_str[gi];
          }
          _compose_buf[_compose_len] = '\0';
        } else {
          _task->showAlert("No GPS fix", 800);
        }
#else
        _task->showAlert("GPS not enabled", 800);
#endif
        return true;
      }
      if (ch == '\x04') {
        // ESC: exit compose immediately
        _task->_preset_add_mode = false;
        reset();
        _task->gotoHomeScreen();
        return true;
      }
      // Append character (apply case for letters)
      if (_compose_len < MAX_TEXT_LEN - 1) {
        char typed = ch;
        if (ch >= 'A' && ch <= 'Z' && !_caps) {
          typed = ch + 32;  // lowercase
        }
        _compose_buf[_compose_len++] = typed;
        _compose_buf[_compose_len] = '\0';
      }
      return true;
    }
    if (c == KEY_CANCEL) {
      // Backspace, or cancel if empty
      if (_compose_len > 0) {
        _compose_len--;
        _compose_buf[_compose_len] = '\0';
      } else {
        _task->_preset_add_mode = false;
        reset();
        _task->gotoHomeScreen();
      }
      return true;
    }
    return false;
  }
};

class ContactSelectScreen : public UIScreen {
  UITask* _task;
  uint8_t _contact_sel;
  int _num_filtered;
  bool _gps_mode;  // true = send GPS DM, false = compose DM

  // Filtered index map: maps display position -> actual contact index
  // Only includes ADV_TYPE_CHAT contacts (not repeaters, rooms, sensors)
  #define MAX_FILTERED_CONTACTS 64
  uint16_t _filtered[MAX_FILTERED_CONTACTS];

  void rebuildFiltered() {
    _num_filtered = 0;
    int total = the_mesh.getNumContacts();
    ContactInfo ci;
    for (int i = 0; i < total && _num_filtered < MAX_FILTERED_CONTACTS; i++) {
      if (the_mesh.getContactByIdx(i, ci) && ci.type == ADV_TYPE_CHAT) {
        _filtered[_num_filtered++] = i;
      }
    }
    if (_contact_sel >= _num_filtered) _contact_sel = 0;
  }

public:
  ContactSelectScreen(UITask* task) : _task(task), _contact_sel(0), _num_filtered(0), _gps_mode(false) {}

  void setGPSMode(bool gps) { _gps_mode = gps; }

  int render(DisplayDriver& display) override {
    rebuildFiltered();

    display.setTextSize(1);
    display.setColor(DisplayDriver::YELLOW);
    char hdr[32];
    if (_gps_mode) {
      snprintf(hdr, sizeof(hdr), "-- GPS DM (%d) --", _num_filtered);
    } else {
      snprintf(hdr, sizeof(hdr), "-- Contacts (%d) --", _num_filtered);
    }
    display.drawTextCentered(display.width() / 2, 18, hdr);

    if (_num_filtered == 0) {
      display.setColor(DisplayDriver::LIGHT);
      display.drawTextCentered(display.width() / 2, 38, "No contacts");
    } else {
      int visible = 3;
      int scroll_top = 0;
      if (_contact_sel >= visible) scroll_top = _contact_sel - visible + 1;
      if (scroll_top > _num_filtered - visible) scroll_top = _num_filtered - visible;
      if (scroll_top < 0) scroll_top = 0;

      int y = 30;
      ContactInfo ci;
      for (int i = scroll_top; i < scroll_top + visible && i < _num_filtered; i++, y += 10) {
        if (i == _contact_sel) {
          display.setColor(DisplayDriver::YELLOW);
          display.setCursor(0, y);
          display.print(">");
        }
        display.setColor(i == _contact_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
        display.setCursor(8, y);
        if (the_mesh.getContactByIdx(_filtered[i], ci)) {
          display.print(ci.name);
        }
      }
    }
    return 5000;
  }

  bool handleInput(char c) override {
    if (_num_filtered == 0) {
      if (c == KEY_CANCEL || c == KEY_ENTER) {
        _task->gotoHomeScreen();
        return true;
      }
      return false;
    }
    if (c == KEY_UP) {
      _contact_sel = (_contact_sel + _num_filtered - 1) % _num_filtered;
      return true;
    }
    if (c == KEY_DOWN) {
      _contact_sel = (_contact_sel + 1) % _num_filtered;
      return true;
    }
    if (c == KEY_ENTER) {
      ContactInfo ci;
      if (the_mesh.getContactByIdx(_filtered[_contact_sel], ci)) {
        if (_task->_preset_pending) {
          // Send pending preset as DM to selected contact
          _task->sendPresetDM(ci);
        } else if (_gps_mode) {
          _task->sendGPSDM(ci);
        } else {
          _task->startDMCompose(ci);
        }
      }
      return true;
    }
    if (c == KEY_CANCEL) {
      _task->_preset_pending = false;
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

class ChannelSelectScreen : public UIScreen {
  UITask* _task;
  uint8_t _channel_sel;
  int _num_channels;

public:
  ChannelSelectScreen(UITask* task) : _task(task), _channel_sel(0), _num_channels(0) {}

  int render(DisplayDriver& display) override {
    // Count available channels
    _num_channels = 0;
    ChannelDetails ch;
    while (_num_channels < 40 && the_mesh.getChannel(_num_channels, ch)) {
      _num_channels++;
    }

    display.setTextSize(1);
    display.setColor(DisplayDriver::YELLOW);
    char hdr[32];
    snprintf(hdr, sizeof(hdr), "-- Channels (%d) --", _num_channels);
    display.drawTextCentered(display.width() / 2, 18, hdr);

    if (_num_channels == 0) {
      display.setColor(DisplayDriver::LIGHT);
      display.drawTextCentered(display.width() / 2, 38, "No channels");
    } else {
      if (_channel_sel >= _num_channels) _channel_sel = 0;
      int visible = 3;
      int scroll_top = 0;
      if (_channel_sel >= visible) scroll_top = _channel_sel - visible + 1;
      if (scroll_top > _num_channels - visible) scroll_top = _num_channels - visible;
      if (scroll_top < 0) scroll_top = 0;

      int y = 30;
      for (int i = scroll_top; i < scroll_top + visible && i < _num_channels; i++, y += 10) {
        if (i == _channel_sel) {
          display.setColor(DisplayDriver::YELLOW);
          display.setCursor(0, y);
          display.print(">");
        }
        display.setColor(i == _channel_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
        display.setCursor(8, y);
        ChannelDetails cd;
        if (the_mesh.getChannel(i, cd)) {
          if (cd.name[0] == '#') {
            display.print(cd.name);
          } else {
            char label[36];
            snprintf(label, sizeof(label), "#%s", cd.name);
            display.print(label);
          }
        }
      }
    }
    return 5000;
  }

  bool handleInput(char c) override {
    if (_num_channels == 0) {
      if (c == KEY_CANCEL || c == KEY_ENTER) {
        _task->gotoHomeScreen();
        return true;
      }
      return false;
    }
    if (c == KEY_UP) {
      _channel_sel = (_channel_sel + _num_channels - 1) % _num_channels;
      return true;
    }
    if (c == KEY_DOWN) {
      _channel_sel = (_channel_sel + 1) % _num_channels;
      return true;
    }
    if (c == KEY_ENTER) {
      if (_task->_preset_pending) {
        // Send pending preset to selected channel
        _task->sendPresetToChannel(_channel_sel);
        return true;
      }
      ChannelDetails cd;
      if (the_mesh.getChannel(_channel_sel, cd)) {
        _task->startChannelCompose(_channel_sel, cd.name);
      }
      return true;
    }
    if (c == KEY_CANCEL) {
      _task->_preset_pending = false;
      _task->gotoHomeScreen();
      return true;
    }
    return false;
  }
};

// QWERTY keyboard layout
// \x01=SND  \x02=SHIFT  \x04=ESC  (back button = backspace)
const char ComposeScreen::KB_CHARS[KB_ROWS * KB_COLS] = {
  'Q','W','E','R','T','Y','U','I','O','P',
  'A','S','D','F','G','H','J','K','L',' ',
  'Z','X','C','V','B','N','M',',','.','-',
  '0','1','2','3','4','5','6','7','8','9',
  '\x02','@','!','?','#','\'',':','\x03','\x04','\x01'
};

void UITask::begin(DisplayDriver* display, SensorManager* sensors, NodePrefs* node_prefs) {
  _display = display;
  _sensors = sensors;
  _auto_off = millis() + AUTO_OFF_MILLIS;

#if defined(PIN_USER_BTN)
  user_btn.begin();
#endif
#if defined(PIN_USER_BTN_ANA)
  analog_btn.begin();
#endif

  _node_prefs = node_prefs;
  _auto_tx_enabled = !(node_prefs->ui_flags & 0x10);
  _motion_mode = (node_prefs->ui_flags >> 5) & 0x03;
  _discovery_sweep_time = millis() + 30000;  // first sweep 30s after boot

#if ENV_INCLUDE_GPS == 1
  // Apply GPS preferences from stored prefs
  if (_sensors != NULL && _node_prefs != NULL) {
    _sensors->setSettingValue("gps", _node_prefs->gps_enabled ? "1" : "0");
    if (_node_prefs->gps_interval > 0) {
      char interval_str[12];  // Max: 24 hours = 86400 seconds (5 digits + null)
      sprintf(interval_str, "%u", _node_prefs->gps_interval);
      _sensors->setSettingValue("gps_interval", interval_str);
    }
  }
#endif

  if (_display != NULL) {
    _display->turnOn();
  }

#ifdef PIN_BUZZER
  buzzer.begin();
  buzzer.quiet(_node_prefs->buzzer_quiet);
#endif

#ifdef PIN_VIBRATION
  vibration.begin();
#endif

  ui_started_at = millis();
  _alert_expiry = 0;

  loadPresetsFromFile();

  splash = new SplashScreen(this);
  home = new HomeScreen(this, &rtc_clock, sensors, node_prefs);
  msg_preview = new MsgPreviewScreen(this, &rtc_clock);
  compose = new ComposeScreen(this);
  contact_select = new ContactSelectScreen(this);
  channel_select = new ChannelSelectScreen(this);
  setCurrScreen(splash);
}

void UITask::showAlert(const char* text, int duration_millis) {
  strcpy(_alert, text);
  _alert_expiry = millis() + duration_millis;
}

void UITask::notify(UIEventType t) {
#if defined(PIN_BUZZER)
switch(t){
  case UIEventType::contactMessage:
    // gemini's pick
    buzzer.play("MsgRcv3:d=4,o=6,b=200:32e,32g,32b,16c7");
    break;
  case UIEventType::channelMessage:
    buzzer.play("kerplop:d=16,o=6,b=120:32g#,32c#");
    break;
  case UIEventType::ack:
    buzzer.play("ack:d=32,o=8,b=120:c");
    break;
  case UIEventType::roomMessage:
  case UIEventType::newContactMessage:
  case UIEventType::none:
  default:
    break;
}
#endif

#ifdef PIN_VIBRATION
  // Trigger vibration for all UI events except none
  if (t != UIEventType::none) {
    vibration.trigger();
  }
#endif
}


void UITask::msgRead(int msgcount) {
  _msgcount = msgcount;
  // Don't switch screens — just update count silently
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount, int channel_idx, const uint8_t* path) {
  _msgcount = msgcount;
  _unread_ui++;

  // For DMs (path_len=0xFF), from_name is the contact name
  // For channel msgs (channel_idx>=0), from_name is the channel name
  const char* dm_contact = (channel_idx < 0) ? from_name : NULL;
  addToMsgLog(from_name, text, false, path_len, channel_idx, dm_contact, path);
  // Don't switch screens — just update unread count (shown in envelope icon)
  // User navigates to Messages themselves via status bar shortcut or carousel

  if (_display != NULL) {
    if (!_display->isOn() && !hasConnection()) {
      _display->turnOn();
    }
    if (_display->isOn()) {
    _auto_off = millis() + AUTO_OFF_MILLIS;  // extend the auto-off timer
    _next_refresh = 100;  // trigger refresh
    }
  }
}

void UITask::userLedHandler() {
#ifdef PIN_STATUS_LED
  int cur_time = millis();
  if (cur_time > next_led_change) {
    if (led_state == 0) {
      led_state = 1;
      if (_unread_ui > 0) {
        last_led_increment = LED_ON_MSG_MILLIS;
      } else {
        last_led_increment = LED_ON_MILLIS;
      }
      next_led_change = cur_time + last_led_increment;
    } else {
      led_state = 0;
      next_led_change = cur_time + LED_CYCLE_MILLIS - last_led_increment;
    }
    digitalWrite(PIN_STATUS_LED, led_state == LED_STATE_ON);
  }
#endif
}

void UITask::setCurrScreen(UIScreen* c) {
  curr = c;
  _next_refresh = 100;
}

void UITask::gotoMessagesScreen() {
  HomeScreen* hs = (HomeScreen*)home;
  hs->_page = HomeScreen::MESSAGES;
  hs->_page_active = true;
  hs->_msg_sel = 0xFF;  // clamp to newest (bottom) on render
  hs->_msg_sel_prev = 0xFF;
  hs->_msg_scroll_px = 0;
  hs->_msg_vscroll = 0;
  hs->_msg_filter = 0;
  hs->_msg_compose_menu = false;
  hs->_msg_target_menu = false;
  setCurrScreen(home);
}

void UITask::gotoMessagesScreenFiltered(int channel_idx, const char* dm_name) {
  HomeScreen* hs = (HomeScreen*)home;
  hs->_page = HomeScreen::MESSAGES;
  hs->_page_active = true;
  hs->_msg_sel = 0xFF;
  hs->_msg_sel_prev = 0xFF;
  hs->_msg_scroll_px = 0;
  hs->_msg_vscroll = 0;
  hs->_msg_compose_menu = false;
  hs->_msg_target_menu = false;
  // Rebuild filters and find the matching tab
  hs->rebuildMsgFilters();
  hs->_msg_filter = 0;  // default to "All"
  for (int i = 1; i < hs->_msg_filter_count; i++) {
    int fval = hs->_msg_filter_channels[i];
    if (channel_idx >= 0 && fval == channel_idx) {
      hs->_msg_filter = i;
      break;
    }
    if (dm_name && fval <= -2) {
      int dm_idx = -fval - 2;
      if (dm_idx >= 0 && dm_idx < 4 && strcmp(hs->_msg_filter_dm_names[dm_idx], dm_name) == 0) {
        hs->_msg_filter = i;
        break;
      }
    }
  }
  setCurrScreen(home);
}

void UITask::gotoComposeScreen() {
  ((ComposeScreen*)compose)->reset();
  setCurrScreen(compose);
}

void UITask::gotoContactSelect(bool gps_mode) {
  ((ContactSelectScreen*)contact_select)->setGPSMode(gps_mode);
  setCurrScreen(contact_select);
}

void UITask::gotoChannelSelect() {
  setCurrScreen(channel_select);
}

void UITask::startDMCompose(const ContactInfo& contact, const char* prefill_text) {
  ((ComposeScreen*)compose)->reset();
  ((ComposeScreen*)compose)->setDMTarget(contact);
  if (prefill_text) ((ComposeScreen*)compose)->prefill(prefill_text);
  setCurrScreen(compose);
}

void UITask::startChannelCompose(int channel_idx, const char* channel_name, const char* prefill_text) {
  ((ComposeScreen*)compose)->reset();
  ((ComposeScreen*)compose)->setChannel(channel_idx, channel_name);
  if (prefill_text) ((ComposeScreen*)compose)->prefill(prefill_text);
  setCurrScreen(compose);
}

void UITask::sendGPSDM(const ContactInfo& contact) {
#if ENV_INCLUDE_GPS == 1
  LocationProvider* nmea = _sensors->getLocationProvider();
  if (nmea != NULL && nmea->isValid()) {
    char gps_text[48];
    snprintf(gps_text, sizeof(gps_text), "GPS: %.6f, %.6f",
             nmea->getLatitude() / 1000000.0, nmea->getLongitude() / 1000000.0);
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    uint32_t expected_ack = 0;
    uint32_t est_timeout = 0;
    int result = the_mesh.sendMessage(contact, ts, 0, gps_text, expected_ack, est_timeout);
    the_mesh.registerExpectedAck(expected_ack, NULL);
    // Note: DM sync to companion app not supported - protocol has no outgoing flag
    addToMsgLog("You", gps_text, true, 0, -1, contact.name, NULL, the_mesh.getLastSentHash(), expected_ack);
    notify(UIEventType::ack);
    showAlert(result > 0 ? "GPS DM Sent!" : "GPS DM failed", 800);
    gotoMessagesScreenFiltered(-1, contact.name);
    return;
  } else {
    showAlert("No GPS fix", 800);
  }
#else
  showAlert("GPS not enabled", 800);
#endif
  gotoHomeScreen();
}

void UITask::sendPresetToChannel(int channel_idx) {
  if (!_preset_pending || _pending_preset[0] == '\0') {
    _preset_pending = false;
    gotoHomeScreen();
    return;
  }
  ChannelDetails ch;
  if (the_mesh.getChannel(channel_idx, ch)) {
    uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
    bool ok = the_mesh.sendGroupMessage(ts, ch.channel,
                  the_mesh.getNodePrefs()->node_name, _pending_preset, strlen(_pending_preset));
    the_mesh.queueSentChannelMessage(channel_idx, ts, _pending_preset, strlen(_pending_preset));
    addToMsgLog("You", _pending_preset, true, 0, channel_idx, NULL, NULL, the_mesh.getLastSentHash());
    notify(UIEventType::ack);
    showAlert(ok ? "Sent!" : "Send failed", 800);
  } else {
    showAlert("No channel", 800);
  }
  _preset_pending = false;
  gotoMessagesScreenFiltered(channel_idx, NULL);
}

void UITask::sendPresetDM(const ContactInfo& contact) {
  if (!_preset_pending || _pending_preset[0] == '\0') {
    _preset_pending = false;
    gotoHomeScreen();
    return;
  }
  uint32_t ts = the_mesh.getRTCClock()->getCurrentTimeUnique();
  uint32_t expected_ack = 0;
  uint32_t est_timeout = 0;
  int result = the_mesh.sendMessage(contact, ts, 0, _pending_preset, expected_ack, est_timeout);
  the_mesh.registerExpectedAck(expected_ack, NULL);
  addToMsgLog("You", _pending_preset, true, 0, -1, contact.name, NULL, the_mesh.getLastSentHash(), expected_ack);
  notify(UIEventType::ack);
  showAlert(result > 0 ? "DM Sent!" : "DM failed", 800);
  _preset_pending = false;
  gotoMessagesScreenFiltered(-1, contact.name);
}

void UITask::logPacket(uint8_t payload_type, uint8_t path_len, const uint8_t* path, int16_t rssi, int8_t snr_x4, uint8_t route_type, uint8_t payload_len) {
  auto& entry = _pkt_log[_pkt_log_next];
  entry.payload_type = payload_type;
  entry.first_hop = (path_len > 0 && path != NULL) ? path[path_len - 1] : 0;
  entry.rssi = rssi;
  entry.snr_x4 = snr_x4;
  entry.timestamp = millis();
  entry.route_type = route_type;
  entry.payload_len = payload_len;
  entry.path_len = (path_len > 8) ? 8 : path_len;
  if (path_len > 0 && path != NULL) {
    memcpy(entry.path, path, entry.path_len);
  }
  _pkt_log_next = (_pkt_log_next + 1) % PACKET_LOG_SIZE;
  if (_pkt_log_count < PACKET_LOG_SIZE) _pkt_log_count++;
}

void UITask::addToMsgLog(const char* origin, const char* text, bool is_sent, uint8_t path_len, int channel_idx, const char* contact_name, const uint8_t* path, const uint8_t* packet_hash, uint32_t expected_ack) {
  auto& entry = _msg_log[_msg_log_next];
  entry.timestamp = the_mesh.getRTCClock()->getCurrentTime();
  strncpy(entry.origin, origin, sizeof(entry.origin) - 1);
  entry.origin[sizeof(entry.origin) - 1] = '\0';
  strncpy(entry.text, text, sizeof(entry.text) - 1);
  entry.text[sizeof(entry.text) - 1] = '\0';
  entry.is_sent = is_sent;
  entry.path_len = path_len;
  entry.channel_idx = channel_idx;
  if (contact_name) {
    strncpy(entry.contact_name, contact_name, sizeof(entry.contact_name) - 1);
    entry.contact_name[sizeof(entry.contact_name) - 1] = '\0';
  } else {
    entry.contact_name[0] = '\0';
  }
  if (path && path_len > 0 && path_len != 0xFF && path_len <= MAX_PATH_SIZE) {
    memcpy(entry.path, path, path_len);
  } else {
    memset(entry.path, 0, sizeof(entry.path));
  }
  if (!is_sent) {
    entry.rssi = (int16_t)radio_driver.getLastRSSI();
    entry.snr_x4 = (int8_t)(radio_driver.getLastSNR() * 4.0f);
  } else {
    entry.rssi = 0;
    entry.snr_x4 = 0;
  }
  // Repeat tracking for sent messages
  if (is_sent && packet_hash) {
    memcpy(entry.packet_hash, packet_hash, MAX_HASH_SIZE);
  } else {
    memset(entry.packet_hash, 0, MAX_HASH_SIZE);
  }
  entry.heard_repeats = 0;
  entry.repeat_rssi = 0;
  entry.repeat_snr_x4 = 0;
  entry.repeat_path_len = 0;
  memset(entry.repeat_path, 0, sizeof(entry.repeat_path));
  memset(entry.repeat_path_rssi, 0, sizeof(entry.repeat_path_rssi));
  memset(entry.repeat_path_snr_x4, 0, sizeof(entry.repeat_path_snr_x4));
  entry.tx_count = 1;
  // Cancel any in-flight auto-ping when sending (don't wipe signal entries)
  if (is_sent) {
    _auto_ping_queue_count = 0;
    _auto_ping_next = 0;
    _auto_ping_pending = false;
  }
  entry.expected_ack = expected_ack;
  entry.delivered = false;

  _msg_log_next = (_msg_log_next + 1) % MSG_LOG_SIZE;
  if (_msg_log_count < MSG_LOG_SIZE) _msg_log_count++;
}

void UITask::updateMsgLogRetry(const char* text, const char* contact_name, const uint8_t* packet_hash, uint32_t expected_ack) {
  for (int i = 0; i < _msg_log_count; i++) {
    int idx = (_msg_log_next - 1 - i + MSG_LOG_SIZE) % MSG_LOG_SIZE;
    auto& entry = _msg_log[idx];
    if (!entry.is_sent) continue;
    if (entry.channel_idx >= 0) continue; // skip channel messages
    if (contact_name && strcmp(entry.contact_name, contact_name) != 0) continue;
    if (strcmp(entry.text, text) != 0) continue;
    // Found matching entry — update for retry
    if (packet_hash) memcpy(entry.packet_hash, packet_hash, MAX_HASH_SIZE);
    entry.expected_ack = expected_ack;
    entry.tx_count++;
    entry.heard_repeats = 0;
    entry.repeat_path_len = 0;
    entry.delivered = false;
    return;
  }
  // No match found — add as new entry
  addToMsgLog("You", text, true, 0, -1, contact_name, NULL, packet_hash, expected_ack);
}

void UITask::onAckReceived(uint32_t ack_hash, int16_t rssi, int8_t snr_x4) {
  if (ack_hash == 0) return;
  for (int i = 0; i < _msg_log_count; i++) {
    int idx = (_msg_log_next - 1 - i + MSG_LOG_SIZE) % MSG_LOG_SIZE;
    auto& entry = _msg_log[idx];
    if (!entry.is_sent) continue;
    if (entry.channel_idx >= 0) continue;
    if (entry.expected_ack == ack_hash) {
      entry.delivered = true;
      if (rssi != 0) {
        entry.rssi = rssi;
        entry.snr_x4 = snr_x4;
      }
      return;
    }
  }
}

void UITask::matchRxPacket(const uint8_t* packet_hash, uint8_t path_len, const uint8_t* path, int16_t rssi, int8_t snr_x4) {
  for (int i = 0; i < _msg_log_count; i++) {
    int idx = (_msg_log_next - 1 - i + MSG_LOG_SIZE) % MSG_LOG_SIZE;
    auto& entry = _msg_log[idx];
    if (!entry.is_sent) continue;
    // Check if packet_hash is non-zero (was stored)
    bool has_hash = false;
    for (int j = 0; j < MAX_HASH_SIZE; j++) {
      if (entry.packet_hash[j] != 0) { has_hash = true; break; }
    }
    if (!has_hash) continue;
    if (memcmp(entry.packet_hash, packet_hash, MAX_HASH_SIZE) == 0) {
      if (entry.heard_repeats < 255) entry.heard_repeats++;
      entry.repeat_rssi = rssi;
      entry.repeat_snr_x4 = snr_x4;
      // Accumulate unique repeater hashes from this path, with per-repeater signal
      // Path order: path[0]=first hop from sender (furthest), path[path_len-1]=last hop (direct to us)
      int last_hop = path_len - 1;
      for (int p = 0; p < path_len; p++) {
        // Check if this repeater hash is already stored
        bool found = false;
        for (int r = 0; r < entry.repeat_path_len; r++) {
          if (entry.repeat_path[r] == path[p]) {
            // Update signal only for last hop (direct transmitter to us)
            if (p == last_hop) {
              entry.repeat_path_rssi[r] = rssi;
              entry.repeat_path_snr_x4[r] = snr_x4;
            }
            found = true;
            break;
          }
        }
        if (!found && entry.repeat_path_len < MAX_PATH_SIZE) {
          int idx = entry.repeat_path_len++;
          entry.repeat_path[idx] = path[p];
          // Only last hop has measurable signal (direct transmitter to us)
          if (p == last_hop) {
            entry.repeat_path_rssi[idx] = rssi;
            entry.repeat_path_snr_x4[idx] = snr_x4;
          } else {
            entry.repeat_path_rssi[idx] = 0;
            entry.repeat_path_snr_x4[idx] = 0;
          }
        }
      }
      // Merge retransmission RX data into existing signal entries
      for (int r = 0; r < entry.repeat_path_len; r++) {
        if (entry.repeat_path_snr_x4[r] == 0) continue;
        uint8_t rid = entry.repeat_path[r];
        int idx = -1;
        for (int s = 0; s < _signal_count; s++) {
          if (_signals[s].id == rid) { idx = s; break; }
        }
        if (idx < 0 && _signal_count < SIGNAL_MAX) {
          idx = _signal_count++;
          _signals[idx].id = rid;
          _signals[idx].has_tx = false;
          _signals[idx].tx_failed = false;
          _signals[idx].tx_snr_x4 = 0;
          _signals[idx].rx_count = 0;
          _signals[idx].tx_count = 0;
          _signals[idx].last_rtt_ms = 0;
          _signals[idx].fail_count = 0;
          _signals[idx].last_fail_time = 0;
          _signals[idx].last_ping_time = 0;
        }
        if (idx >= 0) {
          _signals[idx].rx_snr_x4 = entry.repeat_path_snr_x4[r];
          _signals[idx].has_rx = true;
          _signals[idx].last_heard = millis();
          _signals[idx].rx_count++;
        }
      }
      if (_signal_count > 0) {
        _signal_time = millis();
      }
      // Add heard repeaters to auto-ping queue (accumulate, don't reset in-flight pings)
      bool was_empty = (_auto_ping_queue_count == 0);
      for (int r = 0; r < entry.repeat_path_len && _auto_ping_queue_count < AUTO_PING_QUEUE_MAX; r++) {
        if (entry.repeat_path_snr_x4[r] != 0) {
          bool dup = false;
          for (int q = 0; q < _auto_ping_queue_count; q++) {
            if (_auto_ping_queue[q] == entry.repeat_path[r]) { dup = true; break; }
          }
          if (!dup) {
            _auto_ping_queue[_auto_ping_queue_count++] = entry.repeat_path[r];
          }
        }
      }
      if (was_empty && _auto_ping_queue_count > 0) {
        _auto_ping_next_time = millis() + 500;  // start pinging after brief delay
      }
      break;
    }
  }
}

void UITask::onPathUpdated(const ContactInfo& contact, int16_t rssi, int8_t snr_x4) {
  if (!home) return;  // not yet initialized
  HomeScreen* hs = (HomeScreen*)home;

  // Cache path info for this contact
  HomeScreen::ContactCache* cc = hs->findOrCreateCache(contact.id.pub_key);
  cc->has_path_info = true;
  cc->path_hops = contact.out_path_len > 0 ? contact.out_path_len : 0;
  if (contact.out_path_len > 0) {
    memcpy(cc->path, contact.out_path, contact.out_path_len);
  }
  cc->rssi = rssi;
  cc->snr_x4 = snr_x4;

  if (!hs->_ct_path_pending) return;
  if (memcmp(contact.id.pub_key, hs->_ct_path_key, PUB_KEY_SIZE) != 0) return;
  hs->_ct_path_found = true;
}

void UITask::onTelemetryResponse(const ContactInfo& contact, float voltage, float temperature, float gps_lat, float gps_lon) {
  if (!home) return;
  HomeScreen* hs = (HomeScreen*)home;

  // Cache telemetry for this contact
  HomeScreen::ContactCache* cc = hs->findOrCreateCache(contact.id.pub_key);
  cc->has_telem = true;
  cc->voltage = voltage;
  cc->temperature = temperature;
  if (gps_lat != 0 || gps_lon != 0) {
    cc->has_gps = true;
    cc->gps_lat = gps_lat;
    cc->gps_lon = gps_lon;
  }

  if (!hs->_ct_telem_pending && !hs->_ct_gps_pending) return;
  if (hs->_ct_gps_pending) {
    hs->_ct_gps_pending = false;
    if (gps_lat != 0 || gps_lon != 0) {
      hs->_ct_gps_done = true;
    } else {
      hs->_ct_gps_no_fix = true;
    }
  }
  if (hs->_ct_telem_pending) {
    hs->_ct_telem_done = true;
    hs->_ct_telem_pending = false;
  }
}

void UITask::onStatusResponse(const ContactInfo& contact, uint32_t uptime_secs, uint16_t batt_mv) {
  if (!home) return;
  HomeScreen* hs = (HomeScreen*)home;

  // Cache status for this contact
  HomeScreen::ContactCache* cc = hs->findOrCreateCache(contact.id.pub_key);
  cc->has_status = true;
  cc->uptime_secs = uptime_secs;
  cc->batt_mv = batt_mv;

  if (!hs->_ct_status_pending) return;
  hs->_ct_status_done = true;
  hs->_ct_status_pending = false;
}

void UITask::onPingResponse(uint32_t latency_ms, float snr_there, float snr_back) {
  if (!home) return;
  HomeScreen* hs = (HomeScreen*)home;

  // Handle auto-ping response
  if (_auto_ping_pending) {
    _auto_ping_pending = false;
    // Find matching entry and fill TX data
    for (uint8_t i = 0; i < _signal_count; i++) {
      if (_signals[i].id == _auto_ping_current_id) {
        _signals[i].tx_snr_x4 = (int8_t)(snr_there * 4);
        _signals[i].has_tx = true;
        _signals[i].fail_count = 0;
        _signals[i].tx_count++;
        _signals[i].last_rtt_ms = latency_ms;
        // Update RX from ping reply (snr_back = how well we heard them)
        if (_signals[i].has_rx) {
          _signals[i].rx_snr_x4 = (int8_t)((_signals[i].rx_snr_x4 * 3 + (int8_t)(snr_back * 4)) / 4);
        } else {
          _signals[i].rx_snr_x4 = (int8_t)(snr_back * 4);
          _signals[i].has_rx = true;
        }
        _signals[i].rx_count++;
        _signals[i].last_heard = millis();
        break;
      }
    }
    _signal_time = millis();
    // Also update live RX with snr_back
    _last_rx_id = _auto_ping_current_id;
    _last_rx_snr_x4 = (int8_t)(snr_back * 4);
    _last_rx_time = millis();
    if (_manual_ping_id == _auto_ping_current_id) {
      char alert[32];
      snprintf(alert, sizeof(alert), "Ping %02X: %lums", _auto_ping_current_id, (unsigned long)latency_ms);
      showAlert(alert, 1200);
      _manual_ping_id = 0;
    }
    _auto_ping_next++;
    _auto_ping_next_time = millis() + 1000;
    return;
  }

  // Handle user-initiated ping
  if (!hs->_ct_ping_pending) return;
  hs->_ct_ping_done = true;
  hs->_ct_ping_pending = false;
  hs->_ct_ping_latency = latency_ms;
  hs->_ct_ping_snr_there = snr_there;
  hs->_ct_ping_snr_back = snr_back;

  // Create single entry with both TX+RX
  uint8_t repeater_id = hs->_ct_path_key[0];
  _signals[0].id = repeater_id;
  _signals[0].tx_snr_x4 = (int8_t)(snr_there * 4);
  _signals[0].has_tx = true;
  _signals[0].tx_count = 1;
  _signals[0].last_rtt_ms = latency_ms;
  _signals[0].rx_snr_x4 = (int8_t)(snr_back * 4);
  _signals[0].has_rx = true;
  _signals[0].rx_count = 1;
  _signal_count = 1;
  _signal_time = millis();
  _signal_cycle = 0;
  _signal_cycle_time = millis();
  // Also update live RX
  _last_rx_id = repeater_id;
  _last_rx_snr_x4 = (int8_t)(snr_back * 4);
  _last_rx_time = millis();
}

void UITask::onDiscoverResponse(uint8_t node_type, int8_t snr_x4, int16_t rssi, uint8_t path_len, const uint8_t* pub_key, uint8_t pub_key_len) {
  if (!home) return;
  HomeScreen* hs = (HomeScreen*)home;

  // Signal probe mode: collect repeaters for auto-ping
  if (_probe_active && node_type == ADV_TYPE_REPEATER && pub_key_len >= 1) {
    uint8_t id = pub_key[0];
    // Dedup against existing _signals[] entries
    bool dup = false;
    for (int i = 0; i < _signal_count; i++) {
      if (_signals[i].id == id) { dup = true; break; }
    }
    if (!dup && _signal_count < SIGNAL_MAX && _auto_ping_queue_count < AUTO_PING_QUEUE_MAX) {
      _signals[_signal_count].id = id;
      _signals[_signal_count].rx_snr_x4 = snr_x4;
      _signals[_signal_count].has_rx = true;
      _signals[_signal_count].tx_snr_x4 = 0;
      _signals[_signal_count].has_tx = false;
      _signals[_signal_count].tx_failed = false;
      _signals[_signal_count].last_heard = millis();
      _signals[_signal_count].rx_count = 1;
      _signals[_signal_count].tx_count = 0;
      _signals[_signal_count].last_rtt_ms = 0;
      _signals[_signal_count].fail_count = 0;
      _signals[_signal_count].last_fail_time = 0;
      _signals[_signal_count].last_ping_time = 0;
      _signal_count++;
      _signal_time = millis();
      _auto_ping_queue[_auto_ping_queue_count++] = id;
    }
    // Fall through — NearbyScreen handler below won't trigger since _scan_active is false
  }

  if (!hs->_scan_active) return;

  // Check for duplicate by pub_key prefix (4 bytes)
  for (int i = 0; i < hs->_scan_count; i++) {
    if (hs->_scan_results[i].valid && pub_key_len >= 4 &&
        memcmp(hs->_scan_results[i].pub_key, pub_key, 4) == 0) {
      return;  // duplicate
    }
  }

  // Find empty slot
  if (hs->_scan_count >= SCAN_RESULT_SIZE) return;

  HomeScreen::ScanResult& sr = hs->_scan_results[hs->_scan_count];
  memset(&sr, 0, sizeof(sr));
  sr.valid = true;
  sr.node_type = node_type;
  sr.snr_x4 = snr_x4;
  sr.rssi = rssi;
  sr.path_len = path_len;
  int copy_len = pub_key_len < PUB_KEY_SIZE ? pub_key_len : PUB_KEY_SIZE;
  memcpy(sr.pub_key, pub_key, copy_len);

  // Try to look up contact
  ContactInfo* ci = the_mesh.lookupContactByPubKey(pub_key, copy_len);
  if (ci) {
    strncpy(sr.name, ci->name, sizeof(sr.name) - 1);
    sr.name[sizeof(sr.name) - 1] = '\0';
    sr.in_contacts = true;
  } else {
    snprintf(sr.name, sizeof(sr.name), "<%02X> ???", pub_key[0]);
    sr.in_contacts = false;
  }

  hs->_scan_count++;
}

/*
  hardware-agnostic pre-shutdown activity should be done here
*/
void UITask::shutdown(bool restart){

  #ifdef PIN_BUZZER
  /* note: we have a choice here -
     we can do a blocking buzzer.loop() with non-deterministic consequences
     or we can set a flag and delay the shutdown for a couple of seconds
     while a non-blocking buzzer.loop() plays out in UITask::loop()
  */
  buzzer.shutdown();
  uint32_t buzzer_timer = millis(); // fail-safe shutdown
  while (buzzer.isPlaying() && (millis() - 2500) < buzzer_timer)
    buzzer.loop();

  #endif // PIN_BUZZER

  if (restart) {
    _board->reboot();
  } else {
    _display->turnOff();
    radio_driver.powerOff();
    _board->powerOff();
  }
}

bool UITask::isButtonPressed() const {
#ifdef PIN_USER_BTN
  return user_btn.isPressed();
#else
  return false;
#endif
}

void UITask::loop() {
  char c = 0;
#if UI_HAS_JOYSTICK
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);  // REVISIT: could be mapped to different key code
  }
  ev = joystick_left.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_LEFT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_LEFT);
  }
  ev = joystick_right.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_RIGHT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_RIGHT);
  }
  ev = joystick_up.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_UP);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_UP);
  }
  ev = joystick_down.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_DOWN);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_DOWN);
  }
  ev = back_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_CANCEL);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    checkDisplayOn(KEY_CANCEL);
    startSignalProbe(true);
    c = 0;
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#elif defined(PIN_USER_BTN)
  int ev = user_btn.check();
  if (ev == BUTTON_EVENT_CLICK) {
    c = checkDisplayOn(KEY_NEXT);
  } else if (ev == BUTTON_EVENT_LONG_PRESS) {
    c = handleLongPress(KEY_ENTER);
  } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
    c = handleDoubleClick(KEY_PREV);
  } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
    c = handleTripleClick(KEY_SELECT);
  }
#endif
#if defined(PIN_USER_BTN_ANA)
  if (abs(millis() - _analogue_pin_read_millis) > 10) {
    ev = analog_btn.check();
    if (ev == BUTTON_EVENT_CLICK) {
      c = checkDisplayOn(KEY_NEXT);
    } else if (ev == BUTTON_EVENT_LONG_PRESS) {
      c = handleLongPress(KEY_ENTER);
    } else if (ev == BUTTON_EVENT_DOUBLE_CLICK) {
      c = handleDoubleClick(KEY_PREV);
    } else if (ev == BUTTON_EVENT_TRIPLE_CLICK) {
      c = handleTripleClick(KEY_SELECT);
    }
    _analogue_pin_read_millis = millis();
  }
#endif
#if defined(BACKLIGHT_BTN)
  if (millis() > next_backlight_btn_check) {
    bool touch_state = digitalRead(PIN_BUTTON2);
#if defined(DISP_BACKLIGHT)
    digitalWrite(DISP_BACKLIGHT, !touch_state);
#elif defined(EXP_PIN_BACKLIGHT)
    expander.digitalWrite(EXP_PIN_BACKLIGHT, !touch_state);
#endif
    next_backlight_btn_check = millis() + 300;
  }
#endif

  if (c != 0 && curr) {
    curr->handleInput(c);
    _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
    _next_refresh = 100;  // trigger refresh
  }

  userLedHandler();

  // Process auto-ping queue
  if (_auto_ping_queue_count > 0 && !_auto_ping_pending &&
      _auto_ping_next < _auto_ping_queue_count &&
      millis() >= _auto_ping_next_time) {
    HomeScreen* hs = home ? (HomeScreen*)home : nullptr;
    bool user_ping_active = hs && hs->_ct_ping_pending;

    if (!user_ping_active) {
      uint8_t hash = _auto_ping_queue[_auto_ping_next];
      ContactInfo* ci = the_mesh.lookupContactByPubKey(&hash, 1);
      if (ci) {
        uint32_t est_timeout;
        int result = the_mesh.sendPing(*ci, est_timeout);
        if (result != MSG_SEND_FAILED) {
          _auto_ping_pending = true;
          _auto_ping_current_id = hash;
          _auto_ping_timeout = millis() + est_timeout + 2000;
          // Record ping time on the signal entry
          for (uint8_t i = 0; i < _signal_count; i++) {
            if (_signals[i].id == hash) { _signals[i].last_ping_time = millis(); break; }
          }
        } else {
          // Send failed — mark tx_failed on signal entry
          for (uint8_t i = 0; i < _signal_count; i++) {
            if (_signals[i].id == hash) {
              _signals[i].tx_failed = true;
              _signals[i].fail_count++;
              _signals[i].last_fail_time = millis();
              break;
            }
          }
          if (_manual_ping_id == hash) {
            showAlert("Ping: FAILED", 1200);
            _manual_ping_id = 0;
          }
          _auto_ping_next++;
          _auto_ping_next_time = millis() + 1000;
        }
      } else {
        // Repeater not in contacts — mark tx_failed on signal entry
        for (uint8_t i = 0; i < _signal_count; i++) {
          if (_signals[i].id == hash) {
            _signals[i].tx_failed = true;
            _signals[i].fail_count++;
            _signals[i].last_fail_time = millis();
            break;
          }
        }
        if (_manual_ping_id == hash) {
          showAlert("Ping: no contact", 1200);
          _manual_ping_id = 0;
        }
        _auto_ping_next++;
        _auto_ping_next_time = millis() + 500;
      }
    }
  }

  // Auto-ping timeout — mark tx_failed on signal entry
  if (_auto_ping_pending && millis() > _auto_ping_timeout) {
    for (uint8_t i = 0; i < _signal_count; i++) {
      if (_signals[i].id == _auto_ping_current_id) {
        _signals[i].tx_failed = true;
        _signals[i].fail_count++;
        _signals[i].last_fail_time = millis();
        break;
      }
    }
    if (_manual_ping_id == _auto_ping_current_id) {
      char alert[24];
      snprintf(alert, sizeof(alert), "Ping %02X: FAILED", _auto_ping_current_id);
      showAlert(alert, 1200);
      _manual_ping_id = 0;
    }
    _auto_ping_pending = false;
    _auto_ping_next++;
    _auto_ping_next_time = millis() + 1000;
  }

  // Auto-ping queue fully drained — reset so probe can re-trigger
  if (_auto_ping_queue_count > 0 && _auto_ping_next >= _auto_ping_queue_count && !_auto_ping_pending) {
    _auto_ping_queue_count = 0;
  }

  // Speed-adaptive timing divisor
  unsigned int td = 1;
#if ENV_INCLUDE_GPS == 1
  if (_motion_mode == 2) { td = 2; }        // Bike: always active
  else if (_motion_mode == 3) { td = 4; }   // Drive: always active
  else if (_motion_mode == 1) {              // Auto: speed-based
    LocationProvider* nmea = _sensors->getLocationProvider();
    if (nmea != NULL && nmea->isValid()) {
      float speed_mph = nmea->getSpeed() / 1000.0f * 1.15078f;
      if (speed_mph >= 25.0f) td = 4;
      else if (speed_mph >= 5.0f) td = 2;
    }
  }
#endif

  // Adaptive signal refresh (sweep every 30s when idle, auto_tx_enabled)
  // Best repeater: adaptive backoff (30s/60s/120s based on check count)
  // Others: fixed 120s interval or retry on tx_failed
  if (_auto_tx_enabled && _auto_ping_queue_count == 0 && !_auto_ping_pending && !_probe_active &&
      _signal_count > 0 && millis() >= _retry_ping_time) {
    _retry_ping_time = millis() + 60000UL / td;

    // Find best repeater (same logic as status bar)
    int best = 0;
    for (int i = 1; i < _signal_count; i++) {
      bool cur_bidi = _signals[i].has_rx && _signals[i].has_tx;
      bool bst_bidi = _signals[best].has_rx && _signals[best].has_tx;
      if (cur_bidi && !bst_bidi) {
        best = i;
      } else if (cur_bidi == bst_bidi) {
        int8_t cur_snr = cur_bidi ? min(_signals[i].rx_snr_x4, _signals[i].tx_snr_x4) : _signals[i].rx_snr_x4;
        int8_t bst_snr = bst_bidi ? min(_signals[best].rx_snr_x4, _signals[best].tx_snr_x4) : _signals[best].rx_snr_x4;
        if (cur_snr > bst_snr) best = i;
      }
    }

    // Track best repeater changes — reset backoff when best changes
    if (_signals[best].id != _best_ping_id) {
      _best_ping_id = _signals[best].id;
      _best_ping_count = 0;
    }

    // Adaptive interval for best: 60s (checks 0-3), 120s (4-7), 300s (8+)
    unsigned long best_interval = _best_ping_count < 4 ? 60000UL / td : _best_ping_count < 8 ? 120000UL / td : 300000UL / td;
    // Backoff interval for failed pings: 60s, 120s, then stop retrying
    unsigned long fail_interval = _signals[best].fail_count < 2 ? 60000UL / td :
                                  _signals[best].fail_count < 4 ? 120000UL / td : 0UL;
    unsigned long best_age = millis() - _signals[best].last_heard;
    unsigned long tx_age = _signals[best].last_ping_time > 0 ? millis() - _signals[best].last_ping_time : 999999UL;
    if (best_age < 300000UL / td) {
      bool retry_failed = _signals[best].tx_failed && fail_interval > 0 &&
                          (millis() - _signals[best].last_fail_time > fail_interval);
      bool refresh_stale = tx_age > best_interval && _signals[best].has_tx;
      bool never_checked = !_signals[best].has_tx && !_signals[best].tx_failed;
      if (retry_failed || refresh_stale || never_checked) {
        _signals[best].tx_failed = false;
        _signals[best].has_tx = false;
        _auto_ping_queue[_auto_ping_queue_count++] = _signals[best].id;
        _best_ping_count++;
      }
    }

    // Fill remaining slots with other stale (>120s) or failed entries
    for (uint8_t i = 0; i < _signal_count && _auto_ping_queue_count < AUTO_PING_QUEUE_MAX; i++) {
      if (i == best) continue;
      unsigned long age = millis() - _signals[i].last_heard;
      if (age >= 300000UL / td) continue;  // too old, will be pruned
      unsigned long fi = _signals[i].fail_count < 2 ? 60000UL / td :
                         _signals[i].fail_count < 4 ? 120000UL / td : 0UL;
      bool retry_failed = _signals[i].tx_failed && fi > 0 &&
                          (millis() - _signals[i].last_fail_time > fi);
      unsigned long txa = _signals[i].last_ping_time > 0 ? millis() - _signals[i].last_ping_time : 999999UL;
      bool refresh_stale = txa > 120000UL / td && _signals[i].has_tx;
      bool never_checked = !_signals[i].has_tx && !_signals[i].tx_failed;
      if (retry_failed || refresh_stale || never_checked) {
        _signals[i].tx_failed = false;
        _signals[i].has_tx = false;
        _auto_ping_queue[_auto_ping_queue_count++] = _signals[i].id;
      }
    }

    if (_auto_ping_queue_count > 0) {
      _auto_ping_next = 0;
      _auto_ping_next_time = millis() + 500;
    }
  }

  // Signal probe completion: discovery scan finished
  if (_probe_active && millis() > _probe_timeout) {
    _probe_active = false;
    if (_auto_tx_enabled && _signal_count > 0) {
      _auto_ping_next = 0;
      _auto_ping_next_time = millis() + 500;
    } else if (_signal_count == 0) {
      showAlert("No repeaters found", 1500);
    }
  }

  // Auto-trigger signal probe when signal data goes stale (5 min)
  if (_auto_tx_enabled && !_probe_active && !_probe_done && _auto_ping_queue_count == 0 &&
      _last_rx_time > 0 && !_auto_ping_pending) {
    unsigned long now = millis();
    if ((now - _signal_time > 300000UL / td) && (now - _last_rx_time > 300000UL / td)) {
      startSignalProbe(false);
    }
  }

  // Periodic discovery while in motion mode (find new repeaters entering range)
#if ENV_INCLUDE_GPS == 1
  if (_motion_mode > 0 && _auto_tx_enabled && !_probe_active && !_auto_ping_pending &&
      _auto_ping_queue_count == 0 && millis() >= _discovery_sweep_time) {
    startSignalProbe(false);
    _discovery_sweep_time = millis() + 120000UL / td;
  }
#endif

#ifdef PIN_BUZZER
  if (buzzer.isPlaying())  buzzer.loop();
#endif

  if (curr) curr->poll();

  if (_display != NULL && _display->isOn()) {
    if (millis() >= _next_refresh && curr) {
      _display->startFrame();
      int delay_millis = curr->render(*_display);
      if (millis() < _alert_expiry) {  // render alert popup
        _display->setTextSize(1);
        int y = _display->height() / 3;
        int p = _display->height() / 32;
        _display->setColor(DisplayDriver::DARK);
        _display->fillRect(p, y, _display->width() - p*2, y);
        _display->setColor(DisplayDriver::LIGHT);  // draw box border
        _display->drawRect(p, y, _display->width() - p*2, y);
        _display->drawTextCentered(_display->width() / 2, y + p*3, _alert);
        _next_refresh = _alert_expiry;   // will need refresh when alert is dismissed
      } else {
        _next_refresh = millis() + delay_millis;
      }
      _display->endFrame();
    }
#if AUTO_OFF_MILLIS > 0
    if (millis() > _auto_off) {
      _display->turnOff();
    }
#endif
  }

#ifdef PIN_VIBRATION
  vibration.loop();
#endif

#ifdef AUTO_SHUTDOWN_MILLIVOLTS
  if (millis() > next_batt_chck) {
    uint16_t milliVolts = getBattMilliVolts();
    if (milliVolts > 0 && milliVolts < AUTO_SHUTDOWN_MILLIVOLTS) {

      // show low battery shutdown alert
      // we should only do this for eink displays, which will persist after power loss
      #if defined(THINKNODE_M1) || defined(LILYGO_TECHO)
      if (_display != NULL) {
        _display->startFrame();
        _display->setTextSize(2);
        _display->setColor(DisplayDriver::RED);
        _display->drawTextCentered(_display->width() / 2, 20, "Low Battery.");
        _display->drawTextCentered(_display->width() / 2, 40, "Shutting Down!");
        _display->endFrame();
      }
      #endif

      shutdown();

    }
    next_batt_chck = millis() + 8000;
  }
#endif
}

char UITask::checkDisplayOn(char c) {
  if (_display != NULL) {
    if (!_display->isOn()) {
      _display->turnOn();   // turn display on and consume event
      c = 0;
    }
    _auto_off = millis() + AUTO_OFF_MILLIS;   // extend auto-off timer
    _next_refresh = 0;  // trigger refresh
  }
  return c;
}

char UITask::handleLongPress(char c) {
  if (millis() - ui_started_at < 8000) {   // long press in first 8 seconds since startup -> CLI/rescue
    the_mesh.enterCLIRescue();
    c = 0;   // consume event
  }
  return c;
}

char UITask::handleDoubleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: double click triggered");
  checkDisplayOn(c);
  return c;
}

void UITask::startSignalProbe(bool manual) {
  if (_probe_active) return;  // already probing

  HomeScreen* hs = home ? (HomeScreen*)home : nullptr;
  if (!hs) return;

  // Don't interfere with NearbyScreen scan, active pings, or user composing
  if (hs->_scan_active) return;
  if (_auto_ping_queue_count > 0) return;
  if (_auto_ping_pending) return;
  if (hs->_ct_ping_pending) return;
  if (curr == compose || curr == contact_select || curr == channel_select) return;

  // Start discovery scan
  _probe_scan_tag = random(1, 0x7FFFFFFF);
  the_mesh.startDiscoveryScan(_probe_scan_tag);
  _probe_active = true;
  _probe_timeout = millis() + 8000;
  _probe_done = true;

  // Cancel in-flight pings (don't wipe signal entries — new discoveries merge in)
  _auto_ping_queue_count = 0;
  _auto_ping_next = 0;
  _auto_ping_pending = false;

  if (manual) {
    showAlert("Signal probe...", 2000);
  }
}

char UITask::handleTripleClick(char c) {
  MESH_DEBUG_PRINTLN("UITask: triple click triggered");
  checkDisplayOn(c);
  toggleBuzzer();
  c = 0;
  return c;
}

bool UITask::getGPSState() {
  if (_sensors != NULL) {
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        return !strcmp(_sensors->getSettingValue(i), "1");
      }
    }
  } 
  return false;
}

void UITask::extendAutoOff() {
  _auto_off = millis() + AUTO_OFF_MILLIS;
}

void UITask::toggleGPS() {
    if (_sensors != NULL) {
    // toggle GPS on/off
    int num = _sensors->getNumSettings();
    for (int i = 0; i < num; i++) {
      if (strcmp(_sensors->getSettingName(i), "gps") == 0) {
        if (strcmp(_sensors->getSettingValue(i), "1") == 0) {
          _sensors->setSettingValue("gps", "0");
          _node_prefs->gps_enabled = 0;
          notify(UIEventType::ack);
        } else {
          _sensors->setSettingValue("gps", "1");
          _node_prefs->gps_enabled = 1;
          notify(UIEventType::ack);
        }
        the_mesh.savePrefs();
        showAlert(_node_prefs->gps_enabled ? "GPS: Enabled" : "GPS: Disabled", 800);
        _next_refresh = 0;
        break;
      }
    }
  }
}

void UITask::toggleBuzzer() {
    // Toggle buzzer quiet mode
  #ifdef PIN_BUZZER
    if (buzzer.isQuiet()) {
      buzzer.quiet(false);
      notify(UIEventType::ack);
    } else {
      buzzer.quiet(true);
    }
    _node_prefs->buzzer_quiet = buzzer.isQuiet();
    the_mesh.savePrefs();
    showAlert(buzzer.isQuiet() ? "Buzzer: OFF" : "Buzzer: ON", 800);
    _next_refresh = 0;  // trigger refresh
  #endif
}
