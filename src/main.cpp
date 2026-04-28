#include <Arduino.h>           /* 包含Arduino核心库 */
#include <SPI.h>               /* 包含SPI库 */
#include <SdFat.h>              /* 包含SD卡库 */

#include "board/board_spi.h"   /* 包含板级SPI总线模块 */
#include "board/board_pins.h"  /* 包含板级引脚定义 */

#include "app_state.h"         /* 包含应用状态模块 */

/* Arduino主设置函数 - 系统初始化入口点 */
void setup() {  
  app_state_init();      /* 初始化应用状态 */
}

/* Arduino主循环函数 - 系统主循环 */
void loop() {
  app_state_update();    /* 更新应用状态 */
}
