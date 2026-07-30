#include <Arduino.h>           /* 包含Arduino核心库 */
#include <SPI.h>               /* 包含SPI库 */
#include <SdFat.h>              /* 包含SD卡库 */

#include "board/board_spi.h"   /* 包含板级SPI总线模块 */
#include "board/board_pins.h"  /* 包含板级引脚定义 */

#include "app_state.h"         /* 包含应用状态模块 */
#include "hal/bt62sp_uart_debug.h"  /* BT62SP串口调试桥 */
#include "utils/runtime_monitor.h" /* 运行时内存与任务栈监控 */

/* Arduino主设置函数 - 系统初始化入口点 */
void setup() {  
  // ---------- Power hold / main power enable ----------
  // PCB1: POWER_CTRL = GPIO47
  // 尽量在外设初始化前拉高，避免启动阶段电源保持不住导致 brownout。
  pinMode(PIN_POWER_CTRL, OUTPUT);
  digitalWrite(PIN_POWER_CTRL, HIGH);
  // WS2812 先保持低电平，避免上电乱闪和额外负载。
  pinMode(PIN_WS2812, OUTPUT);
  digitalWrite(PIN_WS2812, LOW);

  Serial.begin(115200);
  delay(100);

  Serial.printf("[电源] 提前拉高 POWER_CTRL GPIO%d，保持整机供电\n", PIN_POWER_CTRL);
  bt62sp_uart_debug_begin();
  
  app_state_init();      /* 初始化应用状态 */
}

/* Arduino主循环函数 - 系统主循环 */
void loop() {
  app_state_update();    /* 更新应用状态 */
  bt62sp_uart_debug_update(); /* 处理电脑串口与BT62SP UART调试桥 */
  runtime_monitor_update(); /* 非阻塞定时采样，已并入 loopTask */
  vTaskDelay(1);         /* 延时1ms */
}
