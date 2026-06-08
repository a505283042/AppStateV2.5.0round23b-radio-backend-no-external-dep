#include <Arduino.h> 
#include <SPI.h>              /* 包含SPI库 */
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board/board_pins.h"  /* 包含板级引脚定义 */
#include "board/board_spi.h"   /* 包含板级SPI总线模块 */
#include <Wire.h>

#include "board/board_pins_pcb1_mcp23017.h"
#include "hal/mcp23017_u3.h"
#include "hal/board_hw_control.h"

SPIClass SPI_SD;              /* SD专用SPI类实例 */
static SemaphoreHandle_t s_ui_spi_mtx = nullptr;

/* 初始化板级SPI总线 - 初始化默认SPI和SD专用SPI */
void board_spi_init(void)
{
    static bool inited = false;  /* 静态标志，确保初始化只执行一次 */
    if (inited) return;          /* 如果已经初始化则直接返回 */
    inited = true;

    Serial.println("[SPI] init buses...");

    // ---------- I2C / MCP23017 ----------
    pinMode(board::PIN_EXP_INTA, INPUT_PULLUP);

    Wire.begin(board::PIN_I2C_SDA, board::PIN_I2C_SCL);
    Wire.setClock(400000);

    const bool mcp_ok = mcp23017_u3_begin();
    mcp23017_u3_debug_dump();

    if (mcp_ok) {
        board_hw_control_begin();

        // ---------- 临时硬件验证：打开功放 ----------

        // 功放打开建议顺序：先静音，再退出关断，最后取消静音，减少爆音。
        board_hw_set_amp_mute(true);
        board_hw_set_amp_shutdown(false);
        delay(100);
        board_hw_set_amp_mute(false);

        board_hw_debug_dump();

        mcp23017_u3_set_b(board::MCP_B_RST_TFT, false);
        mcp23017_u3_set_b(board::MCP_B_RST_NFC, false);

        mcp23017_u3_set_b(board::MCP_B_BLK, false);

        delay(20);

        mcp23017_u3_set_b(board::MCP_B_RST_TFT, true);
        mcp23017_u3_set_b(board::MCP_B_RST_NFC, true);

        delay(120);

        mcp23017_u3_set_b(board::MCP_B_BLK, true);
        Serial.println("[TFT] BLK MCPB4 -> HIGH");
    
    } else {
        Serial.println("[MCP23017] init failed, MCP controlled pins unavailable");
    }

    if (!s_ui_spi_mtx) {
        s_ui_spi_mtx = xSemaphoreCreateRecursiveMutex();
    }

    // ---------- Chip Select safe state ----------
    pinMode(PIN_TFT_CS, OUTPUT);
    digitalWrite(PIN_TFT_CS, HIGH);

    pinMode(PIN_RC522_CS, OUTPUT);
    digitalWrite(PIN_RC522_CS, HIGH);

    pinMode(PIN_SD_CS, OUTPUT);
    digitalWrite(PIN_SD_CS, HIGH);

    // ---------- UI SPI: TFT + RC522 ----------
    // SS 参数务必用 -1（别传 TFT_CS/RC522_CS）
    ::SPI.end();
    ::SPI.begin(PIN_SPI_UI_SCK, PIN_SPI_UI_MISO, PIN_SPI_UI_MOSI, -1);

    // ---------- SD SPI ----------
    SPI_SD.end();
    SPI_SD.begin(PIN_SPI_SD_SCK, PIN_SPI_SD_MISO, PIN_SPI_SD_MOSI, -1);

    Serial.printf("[SPI] UI  SCK=%d MOSI=%d MISO=%d\n",
                  PIN_SPI_UI_SCK, PIN_SPI_UI_MOSI, PIN_SPI_UI_MISO);
    Serial.printf("[SPI] TFT CS=%d DC=%d RST=MCPB%d BLK=MCPB%d\n",
                  PIN_TFT_CS,
                  PIN_TFT_DC,
                  board::MCP_B_RST_TFT,
                  board::MCP_B_BLK);
    Serial.printf("[SPI] RC522 CS=%d RST=MCPB%d IRQ=%d\n",
                  PIN_RC522_CS,
                  board::MCP_B_RST_NFC,
                  PIN_RC522_IRQ);
    Serial.printf("[SPI] SD  SCK=%d MOSI=%d MISO=%d CS=%d\n",
                  PIN_SPI_SD_SCK,
                  PIN_SPI_SD_MOSI,
                  PIN_SPI_SD_MISO,
                  PIN_SD_CS);
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
