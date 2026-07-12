#pragma once

#include <Arduino.h>
#include <stdint.h>

/** PCB1 上的 U3 MCP23017 驱动。 */
bool mcp23017_u3_begin();
bool mcp23017_u3_is_ready();

// 主循环维护入口：总线恢复或设备掉线后重新下发寄存器配置。
void mcp23017_u3_service();

bool mcp23017_u3_write_a(uint8_t value);
bool mcp23017_u3_write_b(uint8_t value);

// 推荐接口：返回值明确表示本次读取是否有效。
bool mcp23017_u3_read_port_a(uint8_t* out);
bool mcp23017_u3_read_port_b(uint8_t* out);

// 兼容旧调用；失败时返回 0xFF。
uint8_t mcp23017_u3_read_a();
uint8_t mcp23017_u3_read_b();

bool mcp23017_u3_set_a(uint8_t bit, bool level);
bool mcp23017_u3_set_b(uint8_t bit, bool level);

bool mcp23017_u3_read_a_bit(uint8_t bit, bool* level);
bool mcp23017_u3_read_b_bit(uint8_t bit, bool* level);

/** 调试用：打印当前 GPIOA/GPIOB 状态。 */
void mcp23017_u3_debug_dump();
