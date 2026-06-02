#pragma once
#include <LovyanGFX.hpp>
#include "board/board_pins.h"

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI _bus;

public:
  LGFX() {
    { // SPI bus with DMA support
      auto cfg = _bus.config();
      cfg.spi_host   = SPI2_HOST;
      cfg.spi_mode   = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 8000000;              // 先保守一点
      cfg.pin_sclk   = PIN_SPI_UI_SCK;
      cfg.pin_mosi   = PIN_SPI_UI_MOSI;
      cfg.pin_miso   = PIN_SPI_UI_MISO;      // 关键：不要是 -1
      cfg.pin_dc     = PIN_TFT_DC;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;     // 比固定 1 更稳妥
      _bus.config(cfg);
      _panel.setBus(&_bus);
    }
    { // panel
      auto cfg = _panel.config();
      cfg.pin_cs  = PIN_TFT_CS;
      cfg.pin_rst = PIN_TFT_RST;
      cfg.pin_busy = -1;
      cfg.panel_width  = 240;
      cfg.panel_height = 240;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.invert   = true;
      cfg.bus_shared = true;   // 正确位置
      _panel.config(cfg);
    }
    setPanel(&_panel);
  }
};
