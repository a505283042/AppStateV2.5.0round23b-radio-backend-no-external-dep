/* 板级SPI总线模块头文件 */
#pragma once
#include <SPI.h>  /* 包含SPI库 */

extern SPIClass SPI_SD;          /* SD专用SPI类实例 */
void board_spi_init(void);       /* 初始化板级SPI总线 */

/* UI SPI 共享锁（TFT + RC522） */
void board_spi_ui_lock(void);
void board_spi_ui_unlock(void);
