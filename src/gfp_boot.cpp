#include "gfp_boot.h"

#include <cstring>

static GfpBoot s_boot;

GfpBoot& gfpBoot() { return s_boot; }

void GfpBoot::attach(lgfx::LGFX_Device* tft) { _tft = tft; }

void GfpBoot::begin(const char* label) {
  if (!_tft || !label) return;

  _y = _tft->getCursorY();
  _tft->print(label);
  _tft->print(' ');
  _dotX = _tft->getCursorX();
  _dotCount = 0;
  _lastDotMs = millis();
  _active = true;
  redrawDots();
}

void GfpBoot::redrawDots() {
  if (!_tft) return;

  _tft->fillRect(_dotX, _y, kDotAreaW, kDotAreaH, _bg);
  _tft->setCursor(_dotX, _y);
  for (int i = 0; i < _dotCount; i++) _tft->print('.');
}

void GfpBoot::tick(uint32_t now) {
  if (!_active || !_tft) return;
  if (now - _lastDotMs < kDotIntervalMs) return;
  _lastDotMs = now;
  _dotCount = (_dotCount % kMaxDots) + 1;
  redrawDots();
}

void GfpBoot::pumpDots(uint32_t ms) {
  if (!_active || !_tft || ms == 0) return;
  const uint32_t t0 = millis();
  uint32_t now = t0;
  while ((uint32_t)(now - t0) < ms) {
    tick(now);
    delay(10);
    now = millis();
  }
}

void GfpBoot::hold(uint32_t ms) {
  if (ms == 0) return;
  delay(ms);
}

void GfpBoot::end(const char* suffix, uint32_t minVisibleMs) {
  if (!_tft) return;

  if (minVisibleMs == 0) minVisibleMs = kMinVisibleMs;
  if (_active) pumpDots(minVisibleMs);

  _active = false;
  _dotCount = kMaxDots;

  const int suffixW = suffix ? ((int)strlen(suffix) * 6) : 0;
  _tft->fillRect(_dotX, _y, kDotAreaW + suffixW + 4, kDotAreaH, _bg);
  _tft->setCursor(_dotX, _y);
  for (int i = 0; i < kMaxDots; i++) _tft->print('.');
  if (suffix) _tft->print(suffix);
  _tft->println();
}

void GfpBoot::println(const char* line) {
  if (!_tft || !line) return;
  _active = false;
  _tft->println(line);
}
