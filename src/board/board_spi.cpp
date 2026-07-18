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
#include "hal/i2c_bus_lock.h"
#include "hal/pcf85063.h"

SPIClass SPI_SD;              /* SD专用SPI类实例 */
static StaticSemaphore_t s_ui_spi_mtx_storage{};
static SemaphoreHandle_t s_ui_spi_mtx = nullptr;

/* 初始化板级SPI总线 - 初始化默认SPI和SD专用SPI */
bool board_spi_init(void)
{
    static bool inited = false;  /* 静态标志，确保初始化只执行一次 */
    if (inited) return s_ui_spi_mtx != nullptr;

    // UI 与 NFC 共用默认 SPI。共享锁必须在任何设备访问总线前建立，
    // 使用静态互斥量避免启动阶段堆不足导致锁创建失败。
    if (!s_ui_spi_mtx) {
        s_ui_spi_mtx =
            xSemaphoreCreateRecursiveMutexStatic(&s_ui_spi_mtx_storage);
    }
    if (!s_ui_spi_mtx) {
        Serial.println("[总线] 创建 UI SPI 递归互斥量失败");
        return false;
    }

    Serial.println("[启动] 初始化SPI总线...");

    // ---------- I2C / MCP23017 / BQ27441 ----------
    // ready=false 期间所有 I2C 设备驱动必须跳过 Wire 访问，避免启动早期误调用。
    i2c_bus_set_ready(false);

    pinMode(board::PIN_EXP_INTA, INPUT_PULLUP);

    Wire.begin(board::PIN_I2C_SDA, board::PIN_I2C_SCL);
    // BQ27441 + MCP23017 + PCF85063 共用一条 I2C，总线优先稳定。
    // 400k 下 BQ27441 偶发 requestFrom 失败，降到 100k。
    Wire.setClock(100000);
    // 明确限制单次事务超时，避免总线异常时每个按键/电池读取长时间阻塞。
    Wire.setTimeOut(40);
    i2c_bus_set_ready(true);

    const bool mcp_ok = mcp23017_u3_begin();
    mcp23017_u3_debug_dump();

    // PCF85063 RTC 与 MCP23017/BQ27441 共用 I2C。
    // 若 RTC 闹钟拉起整机，ESP32 已在更早阶段拉住 POWER_CTRL，
    // 这里可以安全清除 AF，避免关机后 RTC_INT 一直保持触发导致循环开机。
    (void)pcf85063_begin(true);

    if (mcp_ok) {
        board_hw_control_begin();

        // 功放保持静音 + 关断，等真正播放前再打开。

        board_hw_debug_dump();

        mcp23017_u3_set_b(board::MCP_B_RST_TFT, false);
        mcp23017_u3_set_b(board::MCP_B_RST_NFC, false);

        (void)board_hw_set_backlight(false);

        delay(20);

        mcp23017_u3_set_b(board::MCP_B_RST_TFT, true);
        mcp23017_u3_set_b(board::MCP_B_RST_NFC, true);

        delay(120);

        // TFT 控制器此时尚未 init，背光继续保持关闭。
        // ui_init() 绘制完成黑色启动首帧后再开启，避免白屏、花屏或残影。
        Serial.println("[总线] 背光保持关闭，等待 UI 首帧完成");

    } else {
        Serial.println("[总线] 初始化IIC扩展失败");
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

    Serial.printf("[总线] 初始化UI和SDSPI总线参数: SCK=%d MOSI=%d MISO=%d\n",
                  PIN_SPI_UI_SCK, PIN_SPI_UI_MOSI, PIN_SPI_UI_MISO);
    Serial.printf("[总线] 屏幕：CS=%d DC=%d RST=MCPB%d BLK=MCPB%d\n",
                  PIN_TFT_CS,
                  PIN_TFT_DC,
                  board::MCP_B_RST_TFT,
                  board::MCP_B_BLK);
    Serial.printf("[总线] RC522 芯片选择引脚=%d 复位引脚=MCPB%d 中断请求引脚=%d\n",
                  PIN_RC522_CS,
                  board::MCP_B_RST_NFC,
                  PIN_RC522_IRQ);
    Serial.printf("[总线] SD卡 时钟引脚=%d 主出从入引脚=%d 主入从出引脚=%d 芯片选择引脚=%d\n",
                  PIN_SPI_SD_SCK,
                  PIN_SPI_SD_MOSI,
                  PIN_SPI_SD_MISO,
                  PIN_SD_CS);

    inited = true;
    return true;
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
