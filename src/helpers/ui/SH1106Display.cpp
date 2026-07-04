#include "SH1106Display.h"
#include <Adafruit_GrayOLED.h>
#include "Adafruit_SH110X.h"

bool SH1106Display::i2c_probe(TwoWire &wire, uint8_t addr)
{
  wire.beginTransmission(addr);
  uint8_t error = wire.endTransmission();
  return (error == 0);
}

bool SH1106Display::begin()
{
  bool ok = display.begin(DISPLAY_ADDRESS, true) && i2c_probe(Wire, DISPLAY_ADDRESS);
  // GFX auto-wrap breaks lines at character granularity and restarts at x=0, which
  // overdraws whatever is on the row below. Long text must clip (or go through
  // printWordWrap/drawTextEllipsized), never silently wrap.
  display.setTextWrap(false);
  return ok;
}

void SH1106Display::turnOn()
{
  display.oled_command(SH110X_DISPLAYON);
  _isOn = true;
}

void SH1106Display::turnOff()
{
  display.oled_command(SH110X_DISPLAYOFF);
  _isOn = false;
}

void SH1106Display::clear()
{
  display.clearDisplay();
  display.display();
}

void SH1106Display::startFrame(Color bkg)
{
  display.clearDisplay(); // TODO: apply 'bkg'
  _color = SH110X_WHITE;
  display.setTextColor(_color);
  display.setTextSize(1);
  _textsize = 1;
  display.setTextWrap(false); // char-level auto-wrap overdraws the row below
  display.cp437(true); // Use full 256 char 'Code Page 437' font
}

void SH1106Display::setTextSize(int sz)
{
  display.setTextSize(sz);
  _textsize = (sz > 0) ? (uint8_t)sz : 1;
}

void SH1106Display::setColor(Color c)
{
  _color = (c != 0) ? SH110X_WHITE : SH110X_BLACK;
  display.setTextColor(_color);
}

void SH1106Display::setCursor(int x, int y)
{
  display.setCursor(x, y);
}

void SH1106Display::print(const char *str)
{
  display.print(str);
}

// Word-boundary wrap (breaks at ' ', '-', '/'), matching the UI's line counting
// (getTextLines in UITask.cpp) so rendered lines always equal counted lines.
// Continuation lines restart at the original cursor x; lines that would start
// below the screen are dropped. Words longer than max_width clip at the right
// edge (auto-wrap is off) — the counter treats them as one line too.
void SH1106Display::printWordWrap(const char *str, int max_width)
{
  int16_t x0 = display.getCursorX();
  int16_t y = display.getCursorY();
  int line_h = 8 * _textsize;
  int line_w = 0;
  const char *p = str;
  while (*p) {
    // Next word segment: run of non-break chars plus the break char itself
    const char *wEnd = p;
    while (*wEnd && *wEnd != ' ' && *wEnd != '-' && *wEnd != '/') wEnd++;
    if (*wEnd) wEnd++;
    char word[80];
    int wLen = wEnd - p;
    if (wLen >= (int)sizeof(word)) wLen = sizeof(word) - 1;
    memcpy(word, p, wLen);
    word[wLen] = 0;
    int wWidth = getTextWidth(word);
    if (line_w + wWidth >= max_width && line_w > 0) {
      y += line_h;
      if (y >= height()) return;
      display.setCursor(x0, y);
      line_w = 0;
    }
    display.print(word);
    line_w += wWidth;
    p = wEnd;
  }
}

void SH1106Display::fillRect(int x, int y, int w, int h)
{
  display.fillRect(x, y, w, h, _color);
}

void SH1106Display::drawRect(int x, int y, int w, int h)
{
  display.drawRect(x, y, w, h, _color);
}

void SH1106Display::drawXbm(int x, int y, const uint8_t *bits, int w, int h)
{
  display.drawBitmap(x, y, bits, w, h, SH110X_WHITE);
}

uint16_t SH1106Display::getTextWidth(const char *str)
{
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds(str, 0, 0, &x1, &y1, &w, &h);
  return w;
}

void SH1106Display::endFrame()
{
  display.display();
}
