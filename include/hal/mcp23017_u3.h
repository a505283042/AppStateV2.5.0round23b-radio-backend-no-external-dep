#pragma once

#include <Arduino.h>
#include <stdint.h>

/**
 * @brief PCB1 上的 U3 MCP23017 驱动。
 *
 * 第一版目标：
 * - 初始化 A/B 口方向和安全默认输出
 * - 提供 GPIOA/GPIOB 读写
 * - 提供单 bit set/read
 *
 * 先用轮询，不急着启用 INTA 中断。
 */
bool mcp23017_u3_begin();

bool mcp23017_u3_is_ready();

bool mcp23017_u3_write_a(uint8_t value);
bool mcp23017_u3_write_b(uint8_t value);

uint8_t mcp23017_u3_read_a();
uint8_t mcp23017_u3_read_b();

bool mcp23017_u3_set_a(uint8_t bit, bool level);
bool mcp23017_u3_set_b(uint8_t bit, bool level);

bool mcp23017_u3_read_a_bit(uint8_t bit, bool* level);
bool mcp23017_u3_read_b_bit(uint8_t bit, bool* level);

/** 调试用：打印当前 GPIOA/GPIOB 状态。 */
void mcp23017_u3_debug_dump();