#include <Arduino.h> 
#include <SPI.h>              /* 包含SPI库 */
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board/board_pins.h"  /* 包含板级引脚定义 */
#include "board/board_spi.h"   /* 包含板级SPI总线模块 */

SPIClass SPI_SD;              /* SD专用SPI类实例 */
static SemaphoreHandle_t s_ui_spi_mtx = nullptr;

/* 初始化板级SPI总线 - 初始化默认SPI和SD专用SPI */
void board_spi_init(void)
{
    static bool inited = false;  /* 静态标志，确保初始化只执行一次 */
    if (inited) return;          /* 如果已经初始化则直接返回 */
    inited = true;

    Serial.println("[SPI] init buses...");

    if (!s_ui_spi_mtx) {
        s_ui_spi_mtx = xSemaphoreCreateRecursiveMutex();
    }

    // ---------- UI SPI: 全局 SPI（给 TFT_eSPI + RC522 用） ----------
    // SS 参数务必用 -1（别传 TFT_CS/RC522_CS）
    ::SPI.end();
    ::SPI.begin(PIN_SPI_UI_SCK, PIN_SPI_UI_MISO, PIN_SPI_UI_MOSI, -1);

    // 把两颗片选都拉高，确保空闲态不选中
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);

    pinMode(PIN_RC522_CS, OUTPUT);
    digitalWrite(PIN_RC522_CS, HIGH);

    // ---------- SD SPI: 独立 SPI（给 SdFat 用） ----------
    SPI_SD.end();
    SPI_SD.begin(PIN_SPI_SD_SCK, PIN_SPI_SD_MISO, PIN_SPI_SD_MOSI, -1);

    Serial.printf("[SPI] UI  SCK=%d MOSI=%d MISO=%d\n",
                  PIN_SPI_UI_SCK, PIN_SPI_UI_MOSI, PIN_SPI_UI_MISO);
    Serial.printf("[SPI] TFT CS=%d DC=%d RST=%d\n",
                  PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
    Serial.printf("[SPI] RC522 CS=%d RST=%d\n",
                  PIN_RC522_CS, PIN_RC522_RST);
    Serial.printf("[SPI] SD  SCK=%d MOSI=%d MISO=%d CS=%d\n",
                  PIN_SPI_SD_SCK, PIN_SPI_SD_MOSI, PIN_SPI_SD_MISO, PIN_SD_CS);
}

void board_spi_ui_lock(void)
{
    if (s_ui_spi_mtx) {
        xSemaphoreTakeRecursive(s_ui_spi_mtx, portMAX_DELAY);
    }
}

void board_spi_ui_unlock(void)
{
    if (s_ui_spi_mtx) {
        xSemaphoreGiveRecursive(s_ui_spi_mtx);
    }
}
