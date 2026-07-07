#pragma once

#include <Arduino.h>
#include <stdint.h>

namespace board {

// ============================================================
// ESP32-S3 GPIO pins, PCB1 MCP23017 version
// Source: PCB1_2026-06-04 风险确认 MCP23017版
// ============================================================

// I2C / MCP23017
static constexpr int PIN_I2C_SDA = 18;
static constexpr int PIN_I2C_SCL = 8;
static constexpr int PIN_EXP_INTA = 2;

// LED
static constexpr int PIN_WS2812 = 3;

// Power
static constexpr int PIN_POWER_CTRL = 47;
static constexpr int PIN_POWER_PLAY = 48;
static constexpr int PIN_BQ27441_GPOUT = 1;
static constexpr int PIN_RTC_INT = -1;
static constexpr int PIN_BAT_ADC = PIN_BQ27441_GPOUT;

// NFC
static constexpr int PIN_NFC_IRQ = 4;

// Hall sensor / magnetic switch
static constexpr int PIN_HALL_OUT = 9;

// Shared SPI for TFT + NFC
static constexpr int PIN_SPI_MISO = 5;
static constexpr int PIN_SPI_MOSI = 6;
static constexpr int PIN_SPI_CLK = 7;

static constexpr int PIN_NFC_CS = 15;
static constexpr int PIN_TFT_DC = 16;
static constexpr int PIN_TFT_CS = 17;

// TF card SPI
static constexpr int PIN_SD_MISO = 10;
static constexpr int PIN_SD_SCK = 11;
static constexpr int PIN_SD_MOSI = 12;
static constexpr int PIN_SD_CS = 13;

// UART
static constexpr int PIN_UART1_RX = 14;
static constexpr int PIN_UART1_TX = 21;

static constexpr int PIN_UART0_RX = 44;
static constexpr int PIN_UART0_TX = 43;

// EC06 encoder A/B remain on ESP32 GPIO
static constexpr int PIN_EC06_B = 38;
static constexpr int PIN_EC06_A = 39;

// Bluetooth module EWM104-BT62SP
// IO45 是 ESP32-S3 启动绑带脚，外部电路不能强拉异常电平。
static constexpr int PIN_BT_MODE_CTRL = 45;

// I2S DAC
static constexpr int PIN_I2S_LRCK = 40;
static constexpr int PIN_I2S_DOUT = 41;
static constexpr int PIN_I2S_BCLK = 42;

// ============================================================
// MCP23017 U3
// A0/A1/A2 = GND, address = 0x20
// ============================================================

static constexpr uint8_t MCP23017_U3_ADDR = 0x20;

// PORTB bits
static constexpr uint8_t MCP_B_SOL_CTRL_A = 0;
static constexpr uint8_t MCP_B_SOL_CTRL_B = 1;
static constexpr uint8_t MCP_B_RST_NFC = 2;
static constexpr uint8_t MCP_B_RST_TFT = 3;
static constexpr uint8_t MCP_B_BLK = 4;
static constexpr uint8_t MCP_B_PG = 5;
static constexpr uint8_t MCP_B_CHG_STAT = 6;
static constexpr uint8_t MCP_B_BT_PWR_EN = 7;

// PORTA bits
static constexpr uint8_t MCP_A_MUTE_EN = 0;
static constexpr uint8_t MCP_A_SHDN_EN = 1;
static constexpr uint8_t MCP_A_KEY_BACK_MODE = 2;
static constexpr uint8_t MCP_A_EC06_E = 3;
static constexpr uint8_t MCP_A_BT_LINK = 4;
static constexpr uint8_t MCP_A_BT_SW_CTRL = 5;
static constexpr uint8_t MCP_A_KEY_PREV_NFC = 6;
static constexpr uint8_t MCP_A_KEY_NEXT_LIST = 7;

}  // namespace board