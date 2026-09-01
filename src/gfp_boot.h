#pragma once

#include <Arduino.h>
#include <LovyanGFX.hpp>

// Minimal boot status lines: "SD mount ....." -> "SD mount ..... ok"

class GfpBoot final {
public:
  void attach(lgfx::LGFX_Device* tft);

  void begin(const char* label);
  void end(const char* suffix, uint32_t minVisibleMs = 0);
  void println(const char* line);

  void tick(uint32_t now);
  void pumpDots(uint32_t ms);
  void hold(uint32_t ms);

private:
  void redrawDots();

  lgfx::LGFX_Device* _tft = nullptr;
  int _dotX = 0;
  int _y = 0;
  int _dotCount = 0;
  uint32_t _lastDotMs = 0;
  bool _active = false;
  uint16_t _bg = TFT_BLACK;

  static constexpr int kMaxDots = 5;
  static constexpr int kDotAreaW = 40;
  static constexpr int kDotAreaH = 10;
  static constexpr uint32_t kDotIntervalMs = 160;
  static constexpr uint32_t kMinVisibleMs = 500;
};

GfpBoot& gfpBoot();
