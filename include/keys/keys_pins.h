#pragma once

#include "board/board_pins.h"

// 负数代表不是 ESP32 直连 GPIO，由 keys.cpp 特殊处理。
#define PIN_KEY_DISABLED        (-1)
#define PIN_KEY_MCP_BACK_MODE   (-100)
#define PIN_KEY_MCP_EC06_E      (-101)
#define PIN_KEY_MCP_PREV_NFC    (-102)
#define PIN_KEY_MCP_NEXT_LIST   (-103)

// 新 PCB1：
// BACK/MODE -> MCP A2
// EC06_E    -> MCP A3
// PREV/NFC  -> MCP A6
// NEXT/LIST -> MCP A7
// POWER_PLAY -> ESP32 GPIO48
#define PIN_KEY_MODE   PIN_KEY_MCP_BACK_MODE
#define PIN_KEY_PLAY   PIN_POWER_PLAY
#define PIN_KEY_PREV   PIN_KEY_MCP_PREV_NFC
#define PIN_KEY_NEXT   PIN_KEY_MCP_NEXT_LIST

// HALL_OUT -> ESP32 GPIO9，用于磁吸 / 霍尔触发播放暂停。
#define PIN_KEY_HALL_OUT PIN_HALL_OUT

// 新板没有独立 VOL+/VOL- GPIO；先禁用旧音量按键，避免误触发。
#define PIN_KEY_VOLDN  PIN_KEY_DISABLED
#define PIN_KEY_VOLUP  PIN_KEY_DISABLED