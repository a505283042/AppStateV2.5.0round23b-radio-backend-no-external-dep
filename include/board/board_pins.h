#pragma once

#include "board/board_pins_pcb1_mcp23017.h"

// ============================================================
// 兼容旧代码的宏名：实际引脚来自 PCB1 MCP23017 版新表
// ============================================================

// UI SPI: TFT + RC522 共用
#define PIN_SPI_UI_SCK   board::PIN_SPI_CLK
#define PIN_SPI_UI_MISO  board::PIN_SPI_MISO
#define PIN_SPI_UI_MOSI  board::PIN_SPI_MOSI

#define PIN_TFT_CS       board::PIN_TFT_CS
#define PIN_TFT_DC       board::PIN_TFT_DC
#define PIN_TFT_RST      (-1)   // TFT_RST 已迁移到 MCP23017 B3

#define PIN_RC522_CS     board::PIN_NFC_CS
#define PIN_RC522_RST    (-1)   // RC522_RST 已迁移到 MCP23017 B2
#define PIN_RC522_IRQ    board::PIN_NFC_IRQ

// SD SPI
#define PIN_SPI_SD_SCK   board::PIN_SD_SCK
#define PIN_SPI_SD_MISO  board::PIN_SD_MISO
#define PIN_SPI_SD_MOSI  board::PIN_SD_MOSI
#define PIN_SD_CS        board::PIN_SD_CS

// Encoder / keys on ESP32
#define PIN_EC06_A       board::PIN_EC06_A
#define PIN_EC06_B       board::PIN_EC06_B
#define PIN_POWER_PLAY   board::PIN_POWER_PLAY

// I2S
#define PIN_I2S_BCLK     board::PIN_I2S_BCLK
#define PIN_I2S_DOUT     board::PIN_I2S_DOUT
#define PIN_I2S_LRCK     board::PIN_I2S_LRCK

// Other
#define PIN_WS2812       board::PIN_WS2812
#define PIN_POWER_CTRL   board::PIN_POWER_CTRL
#define PIN_BAT_ADC      board::PIN_BAT_ADC