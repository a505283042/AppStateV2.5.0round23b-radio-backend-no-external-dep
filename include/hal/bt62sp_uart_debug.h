#pragma once

#include <stdint.h>

// 初始化 EWM104-BT62SP 串口调试桥。
// USB 串口仍然使用 Serial，BT62SP 使用 UART1。
void bt62sp_uart_debug_begin(uint32_t baud = 1000000);

// 非阻塞转发：
// - 电脑 USB 串口输入 AT... 会自动补 \r\n 后转发到 BT62SP。
// - BT62SP 返回内容会打印到电脑 USB 串口。
void bt62sp_uart_debug_update();
