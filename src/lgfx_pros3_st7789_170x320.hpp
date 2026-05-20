#pragma once
// Display config: ProS3 + ST7789 170x320 (SPI).
//
// Pin defaults match your current wiring:
//   MOSI=35, SCK=36, RST=2, CS=4, DC=5, BL=14
//
// Offsets can be overridden via build flags:
//   -D TFT_OFFSET_X=35 -D TFT_OFFSET_Y=0

#include <LovyanGFX.hpp>

#ifndef TFT_CS
  #define TFT_CS 4
#endif
#ifndef TFT_DC
  #define TFT_DC 5
#endif
#ifndef TFT_RST
  #define TFT_RST 2
#endif

#ifndef TFT_SCLK
  #define TFT_SCLK 36
#endif
#ifndef TFT_MOSI
  #define TFT_MOSI 35
#endif
#ifndef TFT_MISO
  // Most ST7789 modules are write-only; leave MISO disconnected.
  #define TFT_MISO -1
#endif

#ifndef TFT_BL
  #define TFT_BL 14
#endif

#ifndef TFT_OFFSET_X
  #define TFT_OFFSET_X 35
#endif
#ifndef TFT_OFFSET_Y
  #define TFT_OFFSET_Y 0
#endif

// SPI host:
// On some ESP32-S3 variants the Arduino default SPI maps cleanly to SPI2.
// If the display stays black, try SPI2_HOST vs SPI3_HOST.
#ifndef TFT_SPI_HOST
  #define TFT_SPI_HOST SPI2_HOST
#endif

// Some ST7789 modules want invert on, others off.
// If colors look like a photographic negative, set this to 0.
#ifndef TFT_INVERT
  // Start with ON (common for ST7789); flip to 0 if colors look negative.
  #define TFT_INVERT 1
#endif

class LGFX_ST7789_170x320 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX_ST7789_170x320() {
    {
      auto cfg = _bus.config();
      cfg.spi_host = TFT_SPI_HOST;
      cfg.spi_mode = 0;
      // Run the TFT fast. If you see corruption, drop back to 40/26.7/20MHz.
      cfg.freq_write = 60000000;
      cfg.freq_read = 60000000;
      // We use a dedicated DC pin (4-wire SPI). Do NOT enable 3-wire mode.
      cfg.spi_3wire = false;

      cfg.pin_sclk = TFT_SCLK;
      cfg.pin_mosi = TFT_MOSI;
      cfg.pin_miso = TFT_MISO;
      cfg.pin_dc = TFT_DC;

      _bus.config(cfg);
      _panel.setBus(&_bus);
    }

    {
      auto cfg = _panel.config();
      cfg.pin_cs = TFT_CS;
      cfg.pin_rst = TFT_RST;
      cfg.pin_busy = -1;

      cfg.panel_width = 170;
      cfg.panel_height = 320;

      cfg.offset_x = TFT_OFFSET_X;
      cfg.offset_y = TFT_OFFSET_Y;

      cfg.invert = (TFT_INVERT != 0);
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;

      _panel.config(cfg);
    }

    setPanel(&_panel);
  }
};

