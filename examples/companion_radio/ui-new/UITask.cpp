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
    RECENT,
    TRACE,
    RADIO,
    BLUETOOTH,
    ADVERT,
#if ENV_INCLUDE_GPS == 1
    GPS,
    NAV,
#endif
#if UI_SENSORS_PAGE == 1
    SENSORS,
#endif
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
  float _max_speed;
  float _odometer;          // miles traveled
  unsigned long _odo_last;  // last odometer update time
  bool _nav_screen_lock;
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

  void rebuildContactsSorted() {
    _ct_count = 0;
    int n = the_mesh.getNumContacts();
    int fav_start = 0, fav_count = 0;
    // First pass: favorites
    for (int i = 0; i < n && _ct_count < 64; i++) {
      ContactInfo ci;
      if (the_mesh.getContactByIdx(i, ci) && (ci.flags & 0x01)) {
        _ct_sorted[_ct_count++] = i;
        fav_count++;
      }
    }
    // Second pass: non-favorites
    int nonfav_start = _ct_count;
    for (int i = 0; i < n && _ct_count < 64; i++) {
      ContactInfo ci;
      if (the_mesh.getContactByIdx(i, ci) && !(ci.flags & 0x01)) {
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
    ContactInfo ci;
    for (int i = 0; i < _ct_count; i++) {
      if (the_mesh.getContactByIdx(_ct_sorted[i], ci) && strcmp(ci.name, name) == 0) {
        _ct_sel = i;
        return;
      }
    }
  }

  void renderBatteryIndicator(DisplayDriver& display, uint16_t batteryMilliVolts) {
    if (_show_voltage) {
      // Show voltage as text in the top-right area
      char vbuf[8];
      float volts = (float)batteryMilliVolts / 1000.0f;
      snprintf(vbuf, sizeof(vbuf), "%.2fV", volts);
      display.setColor(DisplayDriver::GREEN);
      display.setTextSize(1);
      int textX = display.width() - (int)strlen(vbuf) * 6 - 2;
      display.setCursor(textX, 1);
      display.print(vbuf);
      return;
    }

    // Convert millivolts to percentage
    const int minMilliVolts = 3000; // Minimum voltage (e.g., 3.0V)
    const int maxMilliVolts = 4200; // Maximum voltage (e.g., 4.2V)
    int batteryPercentage = ((batteryMilliVolts - minMilliVolts) * 100) / (maxMilliVolts - minMilliVolts);
    if (batteryPercentage < 0) batteryPercentage = 0; // Clamp to 0%
    if (batteryPercentage > 100) batteryPercentage = 100; // Clamp to 100%

    // battery icon
    int iconWidth = 24;
    int iconHeight = 10;
    int iconX = display.width() - iconWidth - 5; // Position the icon near the top-right corner
    int iconY = 0;
    display.setColor(DisplayDriver::GREEN);

    // battery outline
    display.drawRect(iconX, iconY, iconWidth, iconHeight);

    // battery "cap"
    display.fillRect(iconX + iconWidth, iconY + (iconHeight / 4), 3, iconHeight / 2);

    // fill the battery based on the percentage
    int fillWidth = (batteryPercentage * (iconWidth - 4)) / 100;
    display.fillRect(iconX + 2, iconY + 2, fillWidth, iconHeight - 4);
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

public:
  HomeScreen(UITask* task, mesh::RTCClock* rtc, SensorManager* sensors, NodePrefs* node_prefs)
     : _task(task), _rtc(rtc), _sensors(sensors), _node_prefs(node_prefs), _page(0),
       _preset_sel(0), _msg_sel(0), _msg_sel_prev(0xFF), _msg_scroll_px(0),
       _msg_detail(false), _msg_detail_scroll(0), _msg_reply_menu(false), _msg_reply_sel(0), _shutdown_init(false), _show_voltage(false), _show_speed(false),
       _show_snr(false), _max_speed(0), _odometer(0), _odo_last(0), _nav_screen_lock(false),
       _preset_target_choosing(false), _preset_target_sel(0),
       _ct_sel(0), _ct_count(0), _ct_action(false), _ct_action_sel(0), _ct_action_count(0), _ct_detail_scroll(0),
       _ct_path_pending(false), _ct_path_found(false),
       _ct_telem_pending(false), _ct_telem_done(false),
       _ct_status_pending(false), _ct_status_done(false),
       sensors_lpp(200) { memset(_ct_cache, 0, sizeof(_ct_cache)); }

  bool isUserBusy() const {
    return _msg_detail || _ct_action || _ct_path_pending || _ct_telem_pending ||
           _ct_telem_done || _ct_status_pending || _ct_status_done ||
           _preset_target_choosing;
  }

  void poll() override {
    if (_shutdown_init && !_task->isButtonPressed()) {  // must wait for USR button to be released
      _task->shutdown();
    }
  }

  int render(DisplayDriver& display) override {
    char tmp[80];
    // node name
    display.setTextSize(1);
    display.setColor(DisplayDriver::GREEN);
    char filtered_name[sizeof(_node_prefs->node_name)];
    display.translateUTF8ToBlocks(filtered_name, _node_prefs->node_name, sizeof(filtered_name));
    display.setCursor(0, 0);
    display.print(filtered_name);

    // battery voltage (hidden when SNR display is active)
    if (!_show_snr) {
      renderBatteryIndicator(display, _task->getBattMilliVolts());
    }

    // SNR/RSSI display OR speed HUD in the top bar (SNR takes priority over speed)
    if (_show_snr) {
      char snr_buf[20];
      snprintf(snr_buf, sizeof(snr_buf), "%02X %.0f/%.1f", _task->_last_rx_id, radio_driver.getLastRSSI(), radio_driver.getLastSNR());
      display.setColor(DisplayDriver::GREEN);
      display.setTextSize(1);
      int snrX = display.width() - (int)strlen(snr_buf) * 6 - 2;
      int nameEnd = (int)strlen(filtered_name) * 6 + 2;
      if (snrX < nameEnd) snrX = nameEnd;
      display.setCursor(snrX, 1);
      display.print(snr_buf);
    }
#if ENV_INCLUDE_GPS == 1
    else if (_show_speed && _page != HomePage::NAV) {
      // speed + direction HUD (between name and battery) - hidden on NAV page
      LocationProvider* nmea = sensors.getLocationProvider();
      float speed_mph = 0;
      bool moving = false;
      if (nmea != NULL && nmea->isValid()) {
        speed_mph = nmea->getSpeed() / 1000.0f * 1.15078f;
        moving = speed_mph > 2.0f;
      }
      // 8-point compass (max 2 chars)
      static const char* hud_dirs[] = {"N","NE","E","SE","S","SW","W","NW"};
      char spd[14];
      if (moving) {
        float course_deg = (nmea != NULL) ? nmea->getCourse() / 1000.0f : 0;
        int di = ((int)(course_deg + 22.5f) % 360) / 45;
        if (di < 0) di = 0;
        if (di > 7) di = 7;
        snprintf(spd, sizeof(spd), "%.0f%s", speed_mph, hud_dirs[di]);
      } else {
        snprintf(spd, sizeof(spd), "%.0fmph", speed_mph);
      }
      display.setColor(DisplayDriver::GREEN);
      display.setTextSize(1);
      int nameEnd = (int)strlen(filtered_name) * 6 + 2;
      int battStart = _show_voltage
        ? display.width() - 5 * 6 - 2
        : display.width() - 24 - 5;
      int spdX = nameEnd + (battStart - nameEnd - (int)strlen(spd) * 6) / 2;
      if (spdX < nameEnd) spdX = nameEnd;
      display.setCursor(spdX, 1);
      display.print(spd);
    }
#endif

    // curr page indicator
    int y = 14;
    int x = display.width() / 2 - 5 * (HomePage::Count-1);
    for (uint8_t i = 0; i < HomePage::Count; i++, x += 10) {
      if (i == _page) {
        display.fillRect(x-1, y-1, 3, 3);
      } else {
        display.fillRect(x, y, 1, 1);
      }
    }

    if (_page == HomePage::FIRST) {
      display.setColor(DisplayDriver::YELLOW);
      display.setTextSize(2);
      sprintf(tmp, "MSG: %d", _task->getMsgCount());
      display.drawTextCentered(display.width() / 2, 20, tmp);

      // Show date/time from RTC
      {
        uint32_t now = _rtc->getCurrentTime();
        if (now > 1577836800) { // after 2020-01-01
          // Basic epoch math for DD-MMM HH:MM (no time.h dependency)
          uint32_t t = now;
          int secs = t % 60; (void)secs;
          t /= 60;
          int mins = t % 60;
          t /= 60;
          int hours = t % 24;
          t /= 24;
          // days since epoch (1970-01-01), compute year/month/day
          int days = (int)t;
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
          int day = days + 1;
          snprintf(tmp, sizeof(tmp), "%02d-%s %02d:%02d", day, mnames[month], hours, mins);
          display.setTextSize(1);
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 36, tmp);
        } else {
          display.setTextSize(1);
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 36, "No time set");
        }
      }

      #ifdef WIFI_SSID
        IPAddress ip = WiFi.localIP();
        snprintf(tmp, sizeof(tmp), "IP: %d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 54, tmp);
      #endif
      if (_task->hasConnection()) {
        display.setColor(DisplayDriver::GREEN);
        display.setTextSize(1);
        display.drawTextCentered(display.width() / 2, 43, "< Connected >");

      } else if (the_mesh.getBLEPin() != 0) { // BT pin
        display.setColor(DisplayDriver::RED);
        display.setTextSize(2);
        sprintf(tmp, "Pin:%d", the_mesh.getBLEPin());
        display.drawTextCentered(display.width() / 2, 43, tmp);
      }
    } else if (_page == HomePage::MESSAGES) {
      display.setTextSize(1);

      if (_msg_detail && _task->_msg_log_count > 0) {
        // Scrollable detail view for selected message
        int buf_idx = (_task->_msg_log_next - 1 - _msg_sel + MSG_LOG_SIZE) % MSG_LOG_SIZE;
        auto& entry = _task->_msg_log[buf_idx];

        // Build detail items: Message text lines, separator, then metadata
        char detail_items[20][48];
        uint8_t detail_count = 0;

        // Full message text, word-wrapped
        {
          int chars_per_line = display.width() / 6;  // ~21 for 128px display
          if (chars_per_line > 46) chars_per_line = 46;
          if (chars_per_line < 10) chars_per_line = 10;
          // Filter the text for display (replace multi-byte UTF-8 with blocks)
          char filtered_text[82];
          display.translateUTF8ToBlocks(filtered_text, entry.text, sizeof(filtered_text));
          const char* src = filtered_text;
          int src_len = strlen(src);
          int pos = 0;
          while (pos < src_len && detail_count < 8) {  // max 8 lines for message
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
          uint32_t t = entry.timestamp;
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

        // Sent messages: Repeats, repeat Signal, repeat Path
        if (entry.is_sent && entry.heard_repeats > 0) {
          snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                   "Repeats: %d", entry.heard_repeats);
          detail_count++;

          if (entry.repeat_rssi != 0) {
            float rsnr = (float)entry.repeat_snr_x4 / 4.0f;
            snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                     "Signal: %d/%.1f", entry.repeat_rssi, rsnr);
            detail_count++;
          }

          if (entry.repeat_path_len > 0) {
            char* p = detail_items[detail_count];
            int pos = snprintf(p, sizeof(detail_items[0]), "Heard by:");
            for (int i = 0; i < entry.repeat_path_len && pos < 46; i++) {
              if (i > 0) {
                pos += snprintf(p + pos, sizeof(detail_items[0]) - pos, ",");
              }
              pos += snprintf(p + pos, sizeof(detail_items[0]) - pos, "%02X", entry.repeat_path[i]);
            }
            detail_count++;
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

        // Received messages: Signal (RSSI/SNR)
        if (!entry.is_sent && entry.rssi != 0) {
          float snr = (float)entry.snr_x4 / 4.0f;
          snprintf(detail_items[detail_count], sizeof(detail_items[0]),
                   "Signal: %d/%.1f", entry.rssi, snr);
          detail_count++;
        }

        // Received messages: Path (only if there are repeater hops)
        if (!entry.is_sent && entry.path_len > 0 && entry.path_len != 0xFF) {
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

        // Clamp scroll (ensure last 3 items visible at most)
        int max_scroll = detail_count > 3 ? detail_count - 3 : 0;
        if (_msg_detail_scroll > max_scroll) _msg_detail_scroll = max_scroll;

        // Header
        display.setColor(DisplayDriver::YELLOW);
        display.drawTextCentered(display.width() / 2, 18, "-- Msg Detail --");

        // Render 3 visible items from scroll offset
        int visible = 3;
        int y = 30;
        for (int i = _msg_detail_scroll; i < _msg_detail_scroll + visible && i < detail_count; i++, y += 12) {
          if (i < separator_idx) {
            // Message text: sent=yellow, received=green
            display.setColor(entry.is_sent ? DisplayDriver::YELLOW : DisplayDriver::GREEN);
          } else if (i >= reply_start_idx) {
            // Reply hints/options: green
            display.setColor(DisplayDriver::GREEN);
          } else {
            // Metadata lines + separator
            display.setColor(DisplayDriver::LIGHT);
          }
          display.drawTextEllipsized(0, y, display.width(), detail_items[i]);
        }
      } else {
        display.setColor(DisplayDriver::YELLOW);
        char hdr[32];
        snprintf(hdr, sizeof(hdr), "-- Messages (%d/%d) --", _msg_sel + 1, _task->_msg_log_count);
        display.drawTextCentered(display.width() / 2, 18, hdr);

        if (_task->_msg_log_count == 0) {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 38, "No messages yet");
        } else {
          // Reset scroll when selection changes
          if (_msg_sel != _msg_sel_prev) {
            _msg_scroll_px = 0;
            _msg_sel_prev = _msg_sel;
          }

          int visible = 3;
          int total = _task->_msg_log_count;
          int scroll_top = 0;
          if (_msg_sel >= visible) scroll_top = _msg_sel - visible + 1;
          if (scroll_top > total - visible) scroll_top = total - visible;
          if (scroll_top < 0) scroll_top = 0;

          int avail_w = display.width() - 8; // pixels available for text after ">" marker
          int y = 30;
          for (int v = scroll_top; v < scroll_top + visible && v < total; v++, y += 12) {
            int buf_idx = (_task->_msg_log_next - 1 - v + MSG_LOG_SIZE) % MSG_LOG_SIZE;
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
        }
      }
    } else if (_page == HomePage::PRESETS) {
      display.setColor(DisplayDriver::YELLOW);
      display.setTextSize(1);

      if (_preset_target_choosing) {
        // Sub-menu: choose Channel or DM target
        display.drawTextCentered(display.width() / 2, 18, "-- Send To --");

        int y = 34;
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
      } else {
        display.drawTextCentered(display.width() / 2, 18, "-- Quick Msg --");

        // items: presets + [Compose] + [Reply DM] + [Send GPS DM] + [Channel Msg]
        int total_items = PRESET_MSG_COUNT + 4;
        int visible = 3;
        int scroll_top = 0;
        if (_preset_sel >= visible) scroll_top = _preset_sel - visible + 1;
        if (scroll_top > total_items - visible) scroll_top = total_items - visible;
        if (scroll_top < 0) scroll_top = 0;

        int y = 30;
        for (int i = scroll_top; i < scroll_top + visible && i < total_items; i++, y += 12) {
          if (i == _preset_sel) {
            display.setColor(DisplayDriver::YELLOW);
            display.setCursor(0, y);
            display.print(">");
          }
          display.setColor(i == _preset_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
          display.setCursor(8, y);
          if (i < PRESET_MSG_COUNT) {
            if (i == PRESET_GPS_INDEX) {
              display.print("Send Location");
            } else {
              display.print(preset_messages[i]);
            }
          } else if (i == PRESET_MSG_COUNT) {
            display.print("[Compose...]");
          } else if (i == PRESET_MSG_COUNT + 1) {
            display.print("[Reply DM...]");
          } else if (i == PRESET_MSG_COUNT + 2) {
            display.print("[Send GPS DM...]");
          } else {
            display.print("[Channel Msg...]");
          }
        }
      }
    } else if (_page == HomePage::RECENT) {
      the_mesh.getRecentlyHeard(recent, UI_RECENT_LIST_SIZE);
      display.setColor(DisplayDriver::GREEN);
      int y = 20;
      for (int i = 0; i < UI_RECENT_LIST_SIZE; i++, y += 11) {
        auto a = &recent[i];
        if (a->name[0] == 0) continue;  // empty slot
        int secs = _rtc->getCurrentTime() - a->recv_timestamp;
        if (secs < 60) {
          sprintf(tmp, "%ds", secs);
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
      if (_ct_status_pending || _ct_status_done) {
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Status: %s", _ct_target_name);
        display.drawTextEllipsized(0, 24, display.width(), tmp);
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
            display.setCursor(0, 36);
            display.print(tmp);
            snprintf(tmp, sizeof(tmp), "Power: %umV", cc->batt_mv);
            display.setCursor(0, 48);
            display.print(tmp);
          } else {
            display.setCursor(0, 40);
            display.print("No data");
          }
        } else {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 40, "Waiting...");
          if (millis() > _ct_status_timeout) {
            _ct_status_pending = false;
            _task->showAlert("Status timeout", 1200);
          }
        }
      } else if (_ct_telem_pending || _ct_telem_done) {
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Telemetry: %s", _ct_target_name);
        display.drawTextEllipsized(0, 24, display.width(), tmp);
        if (_ct_telem_done) {
          ContactCache* cc = findCache(_ct_path_key);
          display.setColor(DisplayDriver::LIGHT);
          if (cc && cc->has_telem) {
            snprintf(tmp, sizeof(tmp), "Batt: %.2fV", cc->voltage);
            display.setCursor(0, 36);
            display.print(tmp);
            if (cc->temperature > -274) {
              snprintf(tmp, sizeof(tmp), "Temp: %.1fC", cc->temperature);
              display.setCursor(0, 48);
              display.print(tmp);
            }
          } else {
            snprintf(tmp, sizeof(tmp), "No data");
            display.setCursor(0, 40);
            display.print(tmp);
          }
        } else {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 40, "Waiting...");
          if (millis() > _ct_telem_timeout) {
            _ct_telem_pending = false;
            _task->showAlert("Telem timeout", 1200);
          }
        }
      } else if (_ct_path_pending) {
        // Path discovery in progress
        display.setColor(DisplayDriver::YELLOW);
        snprintf(tmp, sizeof(tmp), "Finding: %s", _ct_target_name);
        display.drawTextEllipsized(0, 24, display.width(), tmp);
        if (_ct_path_found) {
          ContactCache* cc = findCache(_ct_path_key);
          if (cc && cc->has_path_info) {
            if (cc->path_hops > 0) {
              snprintf(tmp, sizeof(tmp), "Found! %d hops", cc->path_hops);
              display.setColor(DisplayDriver::YELLOW);
              display.drawTextEllipsized(0, 36, display.width(), tmp);
              // Hop hex chain
              char hops[48] = "";
              int pos = 0;
              for (int h = 0; h < cc->path_hops && pos < 44; h++) {
                pos += snprintf(hops + pos, sizeof(hops) - pos, "%s%02X", h > 0 ? " " : "", cc->path[h]);
              }
              display.setColor(DisplayDriver::LIGHT);
              display.drawTextEllipsized(0, 48, display.width(), hops);
            } else {
              display.setColor(DisplayDriver::YELLOW);
              display.drawTextEllipsized(0, 36, display.width(), "Found! Direct");
            }
            float snr_f = (float)cc->snr_x4 / 4.0f;
            snprintf(tmp, sizeof(tmp), "RSSI:%d SNR:%.1f", cc->rssi, snr_f);
            display.setColor(DisplayDriver::LIGHT);
            display.drawTextEllipsized(0, 58, display.width(), tmp);
          } else {
            display.setColor(DisplayDriver::YELLOW);
            display.drawTextCentered(display.width() / 2, 40, "Path updated!");
          }
        } else {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 40, "Searching...");
          if (millis() > _ct_path_timeout) {
            _ct_path_pending = false;
            _task->showAlert("No path found", 1200);
          }
        }
      } else if (_ct_action) {
        // Contact detail + action menu
        ContactInfo ci;
        if (the_mesh.getContactByIdx(_ct_sorted[_ct_sel], ci)) {
          display.setColor(DisplayDriver::YELLOW);
          display.drawTextEllipsized(0, 18, display.width(), ci.name);

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
#endif
          } else {
            items[item_count] = "Find Path"; item_is_action[item_count++] = true;
            items[item_count] = "Telemetry"; item_is_action[item_count++] = true;
            items[item_count] = "Status"; item_is_action[item_count++] = true;
          }
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

          int visible = 3;
          int y = 30;
          for (int i = _ct_detail_scroll; i < _ct_detail_scroll + visible && i < item_count; i++, y += 12) {
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
        snprintf(tmp, sizeof(tmp), "-- Contacts (%d) --", _ct_count);
        display.drawTextCentered(display.width() / 2, 18, tmp);

        if (_ct_count == 0) {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 38, "No contacts");
        } else {
          int visible = 3;
          int scroll_top = 0;
          if (_ct_sel >= visible) scroll_top = _ct_sel - visible + 1;
          if (scroll_top > _ct_count - visible) scroll_top = _ct_count - visible;
          if (scroll_top < 0) scroll_top = 0;

          int y = 30;
          for (int v = scroll_top; v < scroll_top + visible && v < _ct_count; v++, y += 12) {
            ContactInfo ci;
            if (the_mesh.getContactByIdx(_ct_sorted[v], ci)) {
              if (v == _ct_sel) {
                display.setColor(DisplayDriver::YELLOW);
                display.setCursor(0, y);
                display.print(">");
              }
              display.setColor(v == _ct_sel ? DisplayDriver::YELLOW : DisplayDriver::LIGHT);
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
              display.drawTextEllipsized(8, y, display.width() - 8, line);
            }
          }
        }
      }
    } else if (_page == HomePage::RADIO) {
      display.setColor(DisplayDriver::YELLOW);
      display.setTextSize(1);
      // freq / sf
      display.setCursor(0, 20);
      sprintf(tmp, "FQ: %06.3f   SF: %d", _node_prefs->freq, _node_prefs->sf);
      display.print(tmp);

      display.setCursor(0, 31);
      sprintf(tmp, "BW: %03.2f     CR: %d", _node_prefs->bw, _node_prefs->cr);
      display.print(tmp);

      // tx power,  noise floor
      display.setCursor(0, 42);
      sprintf(tmp, "TX: %ddBm", _node_prefs->tx_power_dbm);
      display.print(tmp);
      display.setCursor(0, 53);
      sprintf(tmp, "Noise floor: %d", radio_driver.getNoiseFloor());
      display.print(tmp);
    } else if (_page == HomePage::BLUETOOTH) {
      display.setColor(DisplayDriver::GREEN);
      display.drawXbm((display.width() - 32) / 2, 18,
          _task->isSerialEnabled() ? bluetooth_on : bluetooth_off,
          32, 32);
      display.setTextSize(1);
      display.drawTextCentered(display.width() / 2, 64 - 11, "toggle: " PRESS_LABEL);
    } else if (_page == HomePage::ADVERT) {
      display.setColor(DisplayDriver::GREEN);
      display.drawXbm((display.width() - 32) / 2, 18, advert_icon, 32, 32);
      display.drawTextCentered(display.width() / 2, 64 - 11, "advert: " PRESS_LABEL);
#if ENV_INCLUDE_GPS == 1
    } else if (_page == HomePage::GPS) {
      LocationProvider* nmea = sensors.getLocationProvider();
      char buf[50];
      int y = 18;
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
        display.drawTextCentered(display.width() / 2, 28, "Waiting for fix...");
        char satbuf[16];
        if (nmea != NULL) {
          snprintf(satbuf, sizeof(satbuf), "Sats: %ld", nmea->satellitesCount());
        } else {
          strcpy(satbuf, "No GPS");
        }
        display.setColor(DisplayDriver::LIGHT);
        display.drawTextCentered(display.width() / 2, 42, satbuf);
      } else {
        // Speed in mph (getSpeed returns knots * 1000)
        float speed_mph = nmea->getSpeed() / 1000.0f * 1.15078f;
        // Track max speed
        if (speed_mph > _max_speed) _max_speed = speed_mph;
        // Odometer: accumulate distance (speed * elapsed time)
        unsigned long now_ms = millis();
        if (_odo_last > 0 && speed_mph > 1.0f) {
          float elapsed_hrs = (now_ms - _odo_last) / 3600000.0f;
          _odometer += speed_mph * elapsed_hrs;
        }
        _odo_last = now_ms;
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
        display.setCursor(0, 18);
        display.print(spdbuf);

        // Heading text (center-ish)
        if (moving) {
          int dir_idx = ((int)(course_deg + 11.25f) % 360) / 22.5f;
          if (dir_idx < 0) dir_idx = 0;
          if (dir_idx > 15) dir_idx = 15;
          display.setColor(DisplayDriver::GREEN);
          display.drawTextCentered(display.width() / 2, 18, compass_dirs[dir_idx]);
        } else {
          display.setColor(DisplayDriver::LIGHT);
          display.drawTextCentered(display.width() / 2, 18, "--");
        }

        // Satellite count (right)
        char satbuf[8];
        snprintf(satbuf, sizeof(satbuf), "%ldsat", nmea->satellitesCount());
        display.setColor(DisplayDriver::LIGHT);
        int satW = display.getTextWidth(satbuf);
        display.setCursor(display.width() - satW - 1, 18);
        display.print(satbuf);

        // === Center: Compass rose ===
        int cx = display.width() / 2;
        int cy = 39;
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

        // === Bottom rows: alt + odometer, then coordinates ===
        display.setColor(DisplayDriver::LIGHT);
        float alt_ft = nmea->getAltitude() / 1000.0f * 3.28084f;
        char altbuf[12];
        snprintf(altbuf, sizeof(altbuf), "%.0fft", alt_ft);
        display.setCursor(0, 54);
        display.print(altbuf);

        // Odometer (right side of same row)
        char odobuf[14];
        if (_odometer < 10.0f) {
          snprintf(odobuf, sizeof(odobuf), "%.2fmi", _odometer);
        } else {
          snprintf(odobuf, sizeof(odobuf), "%.1fmi", _odometer);
        }
        int odoW = display.getTextWidth(odobuf);
        display.setCursor(display.width() - odoW - 1, 54);
        display.print(odobuf);
      }
      // Keep screen on while nav screen lock is active
      if (_nav_screen_lock) {
        _task->extendAutoOff();
      }
      return 1000; // refresh every 1s for live GPS data
#endif
#if UI_SENSORS_PAGE == 1
    } else if (_page == HomePage::SENSORS) {
      int y = 18;
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
    } else if (_page == HomePage::SHUTDOWN) {
      display.setColor(DisplayDriver::GREEN);
      display.setTextSize(1);
      if (_shutdown_init) {
        display.drawTextCentered(display.width() / 2, 34, "hibernating...");
      } else {
        display.drawXbm((display.width() - 32) / 2, 18, power_icon, 32, 32);
        display.drawTextCentered(display.width() / 2, 64 - 11, "hibernate:" PRESS_LABEL);
      }
    }
    // Faster refresh when scrolling message text
    if (_page == HomePage::MESSAGES && _msg_scroll_px > 0) {
      _task->extendAutoOff();
      return 400;
    }
    return 5000;   // next render after 5000 ms
  }

  bool handleInput(char c) override {
    if (c == KEY_LEFT || c == KEY_PREV) {
      _nav_screen_lock = false;
      _page = (_page + HomePage::Count - 1) % HomePage::Count;
      return true;
    }
    if (c == KEY_NEXT || c == KEY_RIGHT) {
      _nav_screen_lock = false;
      _page = (_page + 1) % HomePage::Count;
      if (_page == HomePage::MESSAGES) {
        _task->showAlert("Message history", 800);
      } else if (_page == HomePage::PRESETS) {
        _task->showAlert("Quick messages", 800);
      } else if (_page == HomePage::RECENT) {
        _task->showAlert("Recent adverts", 800);
      } else if (_page == HomePage::TRACE) {
        _task->showAlert("Contacts", 800);
      }
      return true;
    }
    if (_page == HomePage::TRACE) {
      if (_ct_status_pending || _ct_status_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_status_pending = false;
          _ct_status_done = false;
          reselectContact(_ct_target_name);
          return true;
        }
        return true;
      }
      if (_ct_telem_pending || _ct_telem_done) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_telem_pending = false;
          _ct_telem_done = false;
          reselectContact(_ct_target_name);
          return true;
        }
        return true;
      }
      if (_ct_path_pending) {
        if (c == KEY_CANCEL || c == KEY_ENTER) {
          _ct_path_pending = false;
          _ct_path_found = false;
          reselectContact(_ct_target_name);
          return true;
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
            if (_ct_action_sel >= _ct_detail_scroll + 3) _ct_detail_scroll = _ct_action_sel - 2;
          } else {
            // Scroll down into info lines
            ContactInfo ci;
            if (the_mesh.getContactByIdx(_ct_sorted[_ct_sel], ci)) {
              ContactCache* cc = findCache(ci.id.pub_key);
              int total = _ct_action_count + 1; // +1 for "last heard"
              if (cc && cc->has_path_info) total += 2;
              if (cc && cc->has_telem) total += 1;
              if (cc && cc->has_status) total += 2;
              if (_ct_detail_scroll + 3 < total) _ct_detail_scroll++;
            }
          }
          return true;
        }
        if (c == KEY_ENTER) {
          ContactInfo ci;
          if (the_mesh.getContactByIdx(_ct_sorted[_ct_sel], ci)) {
            // Determine which action was selected
            const char* actions[5];
            uint8_t act_count = 0;
            if (ci.type == ADV_TYPE_CHAT) {
              actions[act_count++] = "Send DM";
              actions[act_count++] = "Find Path";
              actions[act_count++] = "Telemetry";
#if ENV_INCLUDE_GPS == 1
              actions[act_count++] = "Send GPS";
#endif
            } else if (ci.type == ADV_TYPE_REPEATER) {
              actions[act_count++] = "Find Path";
              actions[act_count++] = "Telemetry";
              actions[act_count++] = "Status";
            } else {
              actions[act_count++] = "Find Path";
              actions[act_count++] = "Telemetry";
            }
            const char* chosen = actions[_ct_action_sel];
            if (strcmp(chosen, "Send DM") == 0) {
              _ct_action = false;
              _task->startDMCompose(ci);
            } else if (strcmp(chosen, "Find Path") == 0) {
              uint32_t est_timeout;
              int result = the_mesh.sendPathFind(ci, est_timeout);
              if (result != MSG_SEND_FAILED) {
                _ct_action = false;
                _ct_path_pending = true;
                _ct_path_found = false;
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
#endif
            }
          }
          return true;
        }
        return true;
      }
      // Contact list
      if (_ct_count > 0) {
        if (c == KEY_UP) {
          if (_ct_sel > 0) _ct_sel--;
          return true;
        }
        if (c == KEY_DOWN) {
          if (_ct_sel < _ct_count - 1) _ct_sel++;
          return true;
        }
        if (c == KEY_ENTER) {
          _ct_action = true;
          _ct_action_sel = 0;
          _ct_detail_scroll = 0;
          return true;
        }
      }
      return false;
    }
    if (_page == HomePage::MESSAGES) {
      int total = _task->_msg_log_count;
      if (_msg_detail) {
        // In detail view
        if (c == KEY_CANCEL || c == KEY_LEFT) {
          if (_msg_reply_menu) {
            _msg_reply_menu = false;
          } else {
            _msg_detail = false;
            _msg_detail_scroll = 0;
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
        if (c == KEY_ENTER && total > 0) {
          int buf_idx = (_task->_msg_log_next - 1 - _msg_sel + MSG_LOG_SIZE) % MSG_LOG_SIZE;
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
      if (total > 0) {
        if (c == KEY_UP) {
          if (_msg_sel > 0) _msg_sel--;
          return true;
        }
        if (c == KEY_DOWN) {
          if (_msg_sel < total - 1) _msg_sel++;
          return true;
        }
        if (c == KEY_ENTER) {
          _msg_detail = true;
          _msg_detail_scroll = 0;
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
      int total_items = PRESET_MSG_COUNT + 4;
      if (c == KEY_UP) {
        _preset_sel = (_preset_sel + total_items - 1) % total_items;
        return true;
      }
      if (c == KEY_DOWN) {
        _preset_sel = (_preset_sel + 1) % total_items;
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
    if (c == KEY_ENTER && _page == HomePage::BLUETOOTH) {
      if (_task->isSerialEnabled()) {  // toggle Bluetooth on/off
        _task->disableSerial();
      } else {
        _task->enableSerial();
      }
      return true;
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
    if (c == KEY_UP && _page == HomePage::GPS) {
      _show_speed = !_show_speed;
      _task->showAlert(_show_speed ? "Speed: ON" : "Speed: OFF", 800);
      return true;
    }
    if (c == KEY_UP && _page == HomePage::NAV) {
      _nav_screen_lock = !_nav_screen_lock;
      _task->showAlert(_nav_screen_lock ? "Screen lock: ON" : "Screen lock: OFF", 800);
      return true;
    }
    if (c == KEY_DOWN && _page == HomePage::NAV) {
      _max_speed = 0;
      _odometer = 0;
      _odo_last = 0;
      _task->showAlert("Trip reset", 800);
      return true;
    }
#endif
    if (c == KEY_UP && _page == HomePage::RADIO) {
      _show_snr = !_show_snr;
      if (_show_snr) _show_voltage = false;
      _task->showAlert(_show_snr ? "SNR: ON" : "SNR: OFF", 800);
      return true;
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
    if ((c == KEY_UP || c == KEY_DOWN) && _page == HomePage::FIRST) {
      _show_voltage = !_show_voltage;
      if (_show_voltage) _show_snr = false;
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
  MsgEntry unread[MAX_UNREAD_MSGS];

public:
  MsgPreviewScreen(UITask* task, mesh::RTCClock* rtc) : _task(task), _rtc(rtc) { num_unread = 0; }

  void addPreview(uint8_t path_len, const char* from_name, const char* msg) {
    if (num_unread >= MAX_UNREAD_MSGS) return;  // full

    auto p = &unread[num_unread++];
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

    auto p = &unread[0];

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
      num_unread--;
      if (num_unread == 0) {
        _task->gotoHomeScreen();
      } else {
        // delete first/curr item from unread queue
        for (int i = 0; i < num_unread; i++) {
          unread[i] = unread[i + 1];
        }
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
        // SND: send the composed message
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
        reset();
        _task->gotoHomeScreen();
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
      for (int i = scroll_top; i < scroll_top + visible && i < _num_filtered; i++, y += 12) {
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
      for (int i = scroll_top; i < scroll_top + visible && i < _num_channels; i++, y += 12) {
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
  if (msgcount == 0) {
    gotoHomeScreen();
  }
}

void UITask::newMsg(uint8_t path_len, const char* from_name, const char* text, int msgcount, int channel_idx, const uint8_t* path) {
  _msgcount = msgcount;

  // For DMs (path_len=0xFF), from_name is the contact name
  // For channel msgs (channel_idx>=0), from_name is the channel name
  const char* dm_contact = (channel_idx < 0) ? from_name : NULL;
  addToMsgLog(from_name, text, false, path_len, channel_idx, dm_contact, path);
  ((MsgPreviewScreen *) msg_preview)->addPreview(path_len, from_name, text);

  // Don't interrupt user if they're composing/selecting contacts/channels
  // or in an interactive sub-state on the home screen
  bool user_busy = (curr == compose || curr == contact_select || curr == channel_select);
  if (!user_busy && curr == home) {
    user_busy = ((HomeScreen*)home)->isUserBusy();
  }
  if (!user_busy) {
    setCurrScreen(msg_preview);
  }

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
      if (_msgcount > 0) {
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
  gotoHomeScreen();
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
  gotoHomeScreen();
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
  entry.tx_count = 1;
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

void UITask::onAckReceived(uint32_t ack_hash) {
  if (ack_hash == 0) return;
  for (int i = 0; i < _msg_log_count; i++) {
    int idx = (_msg_log_next - 1 - i + MSG_LOG_SIZE) % MSG_LOG_SIZE;
    auto& entry = _msg_log[idx];
    if (!entry.is_sent) continue;
    if (entry.channel_idx >= 0) continue;
    if (entry.expected_ack == ack_hash) {
      entry.delivered = true;
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
      // Accumulate unique repeater hashes from this path
      for (int p = 0; p < path_len; p++) {
        // Check if this repeater hash is already stored
        bool found = false;
        for (int r = 0; r < entry.repeat_path_len; r++) {
          if (entry.repeat_path[r] == path[p]) { found = true; break; }
        }
        if (!found && entry.repeat_path_len < MAX_PATH_SIZE) {
          entry.repeat_path[entry.repeat_path_len++] = path[p];
        }
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

void UITask::onTelemetryResponse(const ContactInfo& contact, float voltage, float temperature) {
  if (!home) return;
  HomeScreen* hs = (HomeScreen*)home;

  // Cache telemetry for this contact
  HomeScreen::ContactCache* cc = hs->findOrCreateCache(contact.id.pub_key);
  cc->has_telem = true;
  cc->voltage = voltage;
  cc->temperature = temperature;

  if (!hs->_ct_telem_pending) return;
  hs->_ct_telem_done = true;
  hs->_ct_telem_pending = false;
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
