/**
 * LovyanGFX 設定 (Seeed XIAO ESP32C3 + ILI9341 2.2インチ 240x320)
 *
 * RSSリーダー / ライブカメラの両方から使う共通定義。
 *
 * 注意: GPIO9 は ESP32-C3 のストラッピングピン兼 BOOT ボタンなので、
 *       SPI の MISO には割り当てない (LCD からの読み出しは使わない)。
 */
#pragma once

#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_ILI9341 _panel_instance;
  lgfx::Bus_SPI _bus_instance;

public:
  LGFX(void) {
    {
      auto cfg = _bus_instance.config();
      cfg.spi_host = SPI2_HOST;
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read  = 16000000;
      cfg.spi_3wire  = false;
      cfg.use_lock   = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;
      cfg.pin_sclk = 8;
      cfg.pin_mosi = 10;
      cfg.pin_miso = -1;  // 未使用 (GPIO9 は BOOT ボタンに譲る)
      cfg.pin_dc   = 5;
      _bus_instance.config(cfg);
      _panel_instance.setBus(&_bus_instance);
    }
    {
      auto cfg = _panel_instance.config();
      cfg.pin_cs   = 7;
      cfg.pin_rst  = 6;
      cfg.pin_busy = -1;
      cfg.memory_width  = 240;
      cfg.memory_height = 320;
      cfg.panel_width   = 240;
      cfg.panel_height  = 320;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;
      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits  = 1;
      cfg.readable = false;  // MISO 未接続のため読み出し不可
      cfg.invert   = false;
      cfg.rgb_order = false;
      cfg.dlen_16bit = false;
      cfg.bus_shared = true;
      _panel_instance.config(cfg);
    }
    setPanel(&_panel_instance);
  }
};
