/* 板级引脚定义模块头文件 */
#ifndef BOARD_PINS_H
#define BOARD_PINS_H

/* =========================================================
 * 自定义板级引脚定义 (ESP32-S3)
 * ========================================================= 
 * 注意：
 * - 继续使用 PSRAM
 * - 引脚 35/36/37 不能用于SPI
 * ========================================================= */

/* ---------------- SPI总线: SD卡 (TF卡) ---------------- */
#define PIN_SPI_SD_MOSI      11   /* SD卡MOSI引脚 */
#define PIN_SPI_SD_MISO      13   /* SD卡MISO引脚 */
#define PIN_SPI_SD_SCK       12   /* SD卡SCK引脚 */
#define PIN_SD_CS            10   /* SD卡片选引脚 */

/* ---------------- SPI总线: UI (RC522 + TFT) ---------------- */
#define PIN_SPI_UI_MOSI      14   /* UI设备MOSI引脚 */
#define PIN_SPI_UI_MISO      47   /* UI设备MISO引脚 */
#define PIN_SPI_UI_SCK       21   /* UI设备SCK引脚 */

/* ---- RC522 (SPI) ----
 * RC522的SDA引脚在SPI模式下就是CS
 */
#define PIN_RC522_CS         38   /* RC522片选引脚 */
#define PIN_RC522_RST        39   /* RC522复位引脚 */
#define PIN_RC522_IRQ        45   /* RC522中断引脚 */

/* ---- TFT (SPI) ---- */
#define PIN_TFT_CS           42   /* TFT显示屏片选引脚 */
#define PIN_TFT_DC           41   /* TFT显示屏数据/命令选择引脚 */
#define PIN_TFT_RST          40   /* TFT显示屏复位引脚 */
#define PIN_TFT_BL           -1   /* TFT显示屏背光控制引脚，若没有背光控制脚，设为-1 */

/* ---------------- I2S (PCM5102A) ---------------- */
#define PIN_I2S_BCLK         4    /* I2S位时钟引脚 */
#define PIN_I2S_LRCK         5    /* I2S左右声道时钟引脚 */
#define PIN_I2S_DOUT         6    /* I2S数据输出引脚，DIN->DAC的数据输入，就是ESP32的DOUT */

#endif
