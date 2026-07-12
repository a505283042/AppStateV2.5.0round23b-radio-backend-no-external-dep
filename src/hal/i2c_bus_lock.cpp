#include "hal/i2c_bus_lock.h"

#include <Arduino.h>
#include <Wire.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "board/board_pins.h"
#include "utils/log.h"

namespace {

StaticSemaphore_t s_i2c_bus_mu_buf;
SemaphoreHandle_t s_i2c_bus_mu = nullptr;
portMUX_TYPE s_state_mux = portMUX_INITIALIZER_UNLOCKED;
bool s_i2c_bus_ready = false;
bool s_recovery_pending = false;
uint32_t s_generation = 1;
uint8_t s_consecutive_critical_failures = 0;
uint8_t s_last_error_code = 0;
uint32_t s_next_recovery_ms = 0;
uint32_t s_last_recovery_log_ms = 0;

constexpr uint8_t I2C_FAILURES_BEFORE_RECOVERY = 3;
constexpr uint32_t I2C_RECOVERY_RETRY_MS = 2000;
constexpr uint32_t I2C_RECOVERY_LOG_INTERVAL_MS = 10000;
constexpr uint32_t I2C_CLOCK_HZ = 100000;
constexpr uint16_t I2C_TRANSACTION_TIMEOUT_MS = 40;

SemaphoreHandle_t i2c_bus_mutex()
{
    if (!s_i2c_bus_mu) {
        s_i2c_bus_mu = xSemaphoreCreateRecursiveMutexStatic(&s_i2c_bus_mu_buf);
    }
    return s_i2c_bus_mu;
}

bool time_reached(uint32_t now, uint32_t target)
{
    return static_cast<int32_t>(now - target) >= 0;
}

void release_i2c_lines()
{
    // 先释放 SDA，再给 SCL 最多 9 个脉冲，帮助卡在发送状态的从设备退出。
    pinMode(board::PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(board::PIN_I2C_SCL, OUTPUT_OPEN_DRAIN);
    digitalWrite(board::PIN_I2C_SCL, HIGH);
    delayMicroseconds(8);

    for (uint8_t i = 0; i < 9 && digitalRead(board::PIN_I2C_SDA) == LOW; ++i) {
        digitalWrite(board::PIN_I2C_SCL, LOW);
        delayMicroseconds(8);
        digitalWrite(board::PIN_I2C_SCL, HIGH);
        delayMicroseconds(8);
    }

    // 生成一个 STOP：SCL 高电平期间 SDA 从低变高。
    pinMode(board::PIN_I2C_SDA, OUTPUT_OPEN_DRAIN);
    digitalWrite(board::PIN_I2C_SDA, LOW);
    delayMicroseconds(8);
    digitalWrite(board::PIN_I2C_SCL, HIGH);
    delayMicroseconds(8);
    digitalWrite(board::PIN_I2C_SDA, HIGH);
    delayMicroseconds(8);

    pinMode(board::PIN_I2C_SDA, INPUT_PULLUP);
    pinMode(board::PIN_I2C_SCL, INPUT_PULLUP);
}

bool recover_bus_locked()
{
    portENTER_CRITICAL(&s_state_mux);
    s_i2c_bus_ready = false;
    portEXIT_CRITICAL(&s_state_mux);

    Wire.end();
    release_i2c_lines();

    const bool started = Wire.begin(board::PIN_I2C_SDA, board::PIN_I2C_SCL);
    if (!started) {
        return false;
    }

    Wire.setClock(I2C_CLOCK_HZ);
    Wire.setTimeOut(I2C_TRANSACTION_TIMEOUT_MS);

    portENTER_CRITICAL(&s_state_mux);
    s_i2c_bus_ready = true;
    portEXIT_CRITICAL(&s_state_mux);
    return true;
}

}  // namespace

void i2c_bus_lock()
{
    SemaphoreHandle_t mu = i2c_bus_mutex();
    if (mu) {
        xSemaphoreTakeRecursive(mu, portMAX_DELAY);
    }
}

void i2c_bus_unlock()
{
    if (s_i2c_bus_mu) {
        xSemaphoreGiveRecursive(s_i2c_bus_mu);
    }
}

void i2c_bus_set_ready(bool ready)
{
    portENTER_CRITICAL(&s_state_mux);
    s_i2c_bus_ready = ready;
    if (ready) {
        s_recovery_pending = false;
        s_consecutive_critical_failures = 0;
        s_last_error_code = 0;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

bool i2c_bus_is_ready()
{
    portENTER_CRITICAL(&s_state_mux);
    const bool ready = s_i2c_bus_ready;
    portEXIT_CRITICAL(&s_state_mux);
    return ready;
}

bool i2c_bus_io_allowed()
{
    portENTER_CRITICAL(&s_state_mux);
    const bool allowed = s_i2c_bus_ready && !s_recovery_pending;
    portEXIT_CRITICAL(&s_state_mux);
    return allowed;
}

void i2c_bus_note_critical_result(bool success, uint8_t error_code)
{
    const uint32_t now = millis();

    portENTER_CRITICAL(&s_state_mux);
    if (success) {
        s_consecutive_critical_failures = 0;
        s_last_error_code = 0;
        portEXIT_CRITICAL(&s_state_mux);
        return;
    }

    s_last_error_code = error_code;
    if (s_consecutive_critical_failures < 255) {
        ++s_consecutive_critical_failures;
    }

    if (s_consecutive_critical_failures >= I2C_FAILURES_BEFORE_RECOVERY && !s_recovery_pending) {
        s_recovery_pending = true;
        s_next_recovery_ms = now;
    }
    portEXIT_CRITICAL(&s_state_mux);
}

bool i2c_bus_service()
{
    const uint32_t now = millis();

    portENTER_CRITICAL(&s_state_mux);
    const bool pending = s_recovery_pending;
    const uint32_t next_recovery_ms = s_next_recovery_ms;
    portEXIT_CRITICAL(&s_state_mux);

    if (!pending || !time_reached(now, next_recovery_ms)) return false;

    i2c_bus_lock();
    const bool ok = recover_bus_locked();
    i2c_bus_unlock();

    if (ok) {
        portENTER_CRITICAL(&s_state_mux);
        s_recovery_pending = false;
        s_consecutive_critical_failures = 0;
        s_last_error_code = 0;
        const uint32_t generation = ++s_generation;
        portEXIT_CRITICAL(&s_state_mux);

        LOGW("[I2C] 总线恢复完成：代次=%lu SDA=%d SCL=%d 超时=%ums",
             static_cast<unsigned long>(generation),
             board::PIN_I2C_SDA,
             board::PIN_I2C_SCL,
             static_cast<unsigned>(I2C_TRANSACTION_TIMEOUT_MS));
        return true;
    }

    portENTER_CRITICAL(&s_state_mux);
    s_next_recovery_ms = now + I2C_RECOVERY_RETRY_MS;
    const uint8_t error_code = s_last_error_code;
    portEXIT_CRITICAL(&s_state_mux);

    if (s_last_recovery_log_ms == 0 || now - s_last_recovery_log_ms >= I2C_RECOVERY_LOG_INTERVAL_MS) {
        s_last_recovery_log_ms = now;
        LOGW("[I2C] 总线恢复失败：err=%u，%lums 后重试",
             static_cast<unsigned>(error_code),
             static_cast<unsigned long>(I2C_RECOVERY_RETRY_MS));
    }
    return false;
}

uint32_t i2c_bus_generation()
{
    portENTER_CRITICAL(&s_state_mux);
    const uint32_t generation = s_generation;
    portEXIT_CRITICAL(&s_state_mux);
    return generation;
}
