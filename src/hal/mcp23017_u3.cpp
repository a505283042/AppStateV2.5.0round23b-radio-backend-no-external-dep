#include "hal/mcp23017_u3.h"

#include <Wire.h>

#include "hal/i2c_bus_lock.h"
#include "board/board_pins_pcb1_mcp23017.h"
#include "utils/log.h"

namespace {

constexpr uint8_t REG_IODIRA = 0x00;
constexpr uint8_t REG_IODIRB = 0x01;
constexpr uint8_t REG_IPOLA = 0x02;
constexpr uint8_t REG_IPOLB = 0x03;
constexpr uint8_t REG_GPINTENA = 0x04;
constexpr uint8_t REG_GPINTENB = 0x05;
constexpr uint8_t REG_DEFVALA = 0x06;
constexpr uint8_t REG_DEFVALB = 0x07;
constexpr uint8_t REG_INTCONA = 0x08;
constexpr uint8_t REG_INTCONB = 0x09;
constexpr uint8_t REG_IOCON = 0x0A;
constexpr uint8_t REG_GPPUA = 0x0C;
constexpr uint8_t REG_GPPUB = 0x0D;
constexpr uint8_t REG_GPIOA = 0x12;
constexpr uint8_t REG_GPIOB = 0x13;
constexpr uint8_t REG_OLATA = 0x14;
constexpr uint8_t REG_OLATB = 0x15;

constexpr uint8_t IOCON_MIRROR_ODR = 0x44;

constexpr uint8_t IODIRA_VALUE =
    (1 << board::MCP_A_KEY_BACK_MODE) |
    (1 << board::MCP_A_EC06_E) |
    (1 << board::MCP_A_BT_LINK) |
    (1 << board::MCP_A_KEY_PREV_NFC) |
    (1 << board::MCP_A_KEY_NEXT_LIST);

constexpr uint8_t IODIRB_VALUE =
    (1 << board::MCP_B_PG) |
    (1 << board::MCP_B_CHG_STAT);

constexpr uint8_t GPPUA_VALUE = IODIRA_VALUE;
constexpr uint8_t GPPUB_VALUE = IODIRB_VALUE;

constexpr uint8_t MCP_FAILURES_BEFORE_OFFLINE = 3;
constexpr uint32_t MCP_REINIT_INTERVAL_MS = 2000;
constexpr uint32_t MCP_WARN_INTERVAL_MS = 10000;

bool s_ready = false;
uint8_t s_olat_a = 0x00;
uint8_t s_olat_b = 0x00;
uint8_t s_consecutive_failures = 0;
uint32_t s_next_reinit_ms = 0;
uint32_t s_last_warn_ms = 0;
uint32_t s_seen_bus_generation = 0;

bool time_reached(uint32_t now, uint32_t target)
{
  return static_cast<int32_t>(now - target) >= 0;
}

void note_success()
{
  s_consecutive_failures = 0;
  i2c_bus_note_critical_result(true, 0);
}

void note_failure(const char* op, uint8_t reg, uint8_t error)
{
  if (s_consecutive_failures < 255) {
    ++s_consecutive_failures;
  }

  i2c_bus_note_critical_result(false, error);

  const uint32_t now = millis();
  if (s_last_warn_ms == 0 || now - s_last_warn_ms >= MCP_WARN_INTERVAL_MS) {
    s_last_warn_ms = now;
    LOGW("[MCP23017] %s失败：寄存器=0x%02X err=%u 连续失败=%u",
         op,
         reg,
         static_cast<unsigned>(error),
         static_cast<unsigned>(s_consecutive_failures));
  }

  if (s_consecutive_failures >= MCP_FAILURES_BEFORE_OFFLINE) {
    s_ready = false;
    s_next_reinit_ms = now + MCP_REINIT_INTERVAL_MS;
  }
}

bool raw_write_reg(uint8_t reg, uint8_t value)
{
  if (!i2c_bus_io_allowed()) return false;

  i2c_bus_lock();
  Wire.beginTransmission(board::MCP23017_U3_ADDR);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t err = Wire.endTransmission(true);
  i2c_bus_unlock();

  if (err != 0) {
    note_failure("写寄存器", reg, err);
    return false;
  }

  note_success();
  return true;
}

bool raw_read_reg(uint8_t reg, uint8_t* out)
{
  if (!out || !i2c_bus_io_allowed()) return false;

  i2c_bus_lock();
  Wire.beginTransmission(board::MCP23017_U3_ADDR);
  Wire.write(reg);
  const uint8_t select_err = Wire.endTransmission(false);
  if (select_err != 0) {
    i2c_bus_unlock();
    note_failure("选择读取寄存器", reg, select_err);
    return false;
  }

  const size_t received = Wire.requestFrom(
      static_cast<uint8_t>(board::MCP23017_U3_ADDR),
      static_cast<size_t>(1),
      true);

  if (received != 1 || Wire.available() < 1) {
    i2c_bus_unlock();
    // requestFrom 只返回数量，这里使用自定义错误码表示读取阶段失败。
    note_failure("读取寄存器", reg, static_cast<uint8_t>(0xF0u | (received & 0x0Fu)));
    return false;
  }

  *out = static_cast<uint8_t>(Wire.read());
  i2c_bus_unlock();
  note_success();
  return true;
}

bool configure_device(bool preserve_outputs)
{
  if (!i2c_bus_io_allowed()) return false;

  if (!preserve_outputs) {
    s_olat_a = 0x00;
    s_olat_b = 0x00;
  }

  bool ok = true;

  // 先恢复输出 latch，再配置方向，避免设备复位后的输出瞬间跳变。
  ok &= raw_write_reg(REG_OLATA, s_olat_a);
  ok &= raw_write_reg(REG_OLATB, s_olat_b);

  ok &= raw_write_reg(REG_IPOLA, 0x00);
  ok &= raw_write_reg(REG_IPOLB, 0x00);

  ok &= raw_write_reg(REG_GPINTENA, 0x00);
  ok &= raw_write_reg(REG_GPINTENB, 0x00);
  ok &= raw_write_reg(REG_DEFVALA, 0x00);
  ok &= raw_write_reg(REG_DEFVALB, 0x00);
  ok &= raw_write_reg(REG_INTCONA, 0x00);
  ok &= raw_write_reg(REG_INTCONB, 0x00);

  ok &= raw_write_reg(REG_IOCON, IOCON_MIRROR_ODR);
  ok &= raw_write_reg(REG_GPPUA, GPPUA_VALUE);
  ok &= raw_write_reg(REG_GPPUB, GPPUB_VALUE);
  ok &= raw_write_reg(REG_IODIRA, IODIRA_VALUE);
  ok &= raw_write_reg(REG_IODIRB, IODIRB_VALUE);

  uint8_t dummy = 0;
  ok &= raw_read_reg(REG_GPIOA, &dummy);
  ok &= raw_read_reg(REG_GPIOB, &dummy);

  if (ok) {
    s_ready = true;
    s_consecutive_failures = 0;
    s_next_reinit_ms = 0;
    s_seen_bus_generation = i2c_bus_generation();
  }

  return ok;
}

bool write_reg(uint8_t reg, uint8_t value)
{
  if (!s_ready) return false;
  return raw_write_reg(reg, value);
}

bool read_reg(uint8_t reg, uint8_t* out)
{
  if (!s_ready) return false;
  return raw_read_reg(reg, out);
}

bool update_bit(uint8_t reg, uint8_t* shadow, uint8_t bit, bool level)
{
  if (!shadow || bit >= 8) return false;

  uint8_t next = *shadow;
  if (level) {
    next |= static_cast<uint8_t>(1u << bit);
  } else {
    next &= static_cast<uint8_t>(~(1u << bit));
  }

  if (!write_reg(reg, next)) return false;
  *shadow = next;
  return true;
}

}  // namespace

bool mcp23017_u3_begin()
{
  s_ready = false;
  s_consecutive_failures = 0;

  const bool ok = configure_device(false);
  if (ok) {
    LOGI("[MCP23017] U3 初始化成功：地址=0x%02X IODIRA=0x%02X IODIRB=0x%02X",
         board::MCP23017_U3_ADDR,
         IODIRA_VALUE,
         IODIRB_VALUE);
  } else {
    LOGW("[MCP23017] U3 初始化失败：地址=0x%02X，等待后台恢复",
         board::MCP23017_U3_ADDR);
    s_next_reinit_ms = millis() + MCP_REINIT_INTERVAL_MS;
  }
  return ok;
}

bool mcp23017_u3_is_ready()
{
  return s_ready;
}

void mcp23017_u3_service()
{
  if (!i2c_bus_io_allowed()) return;

  const uint32_t generation = i2c_bus_generation();
  const bool bus_was_recovered = generation != s_seen_bus_generation;
  if (s_ready && !bus_was_recovered) return;

  const uint32_t now = millis();
  if (!bus_was_recovered && !time_reached(now, s_next_reinit_ms)) return;

  if (configure_device(true)) {
    LOGW("[MCP23017] U3 已恢复：总线代次=%lu OLATA=0x%02X OLATB=0x%02X",
         static_cast<unsigned long>(generation),
         s_olat_a,
         s_olat_b);
  } else {
    s_ready = false;
    s_next_reinit_ms = now + MCP_REINIT_INTERVAL_MS;
  }
}

bool mcp23017_u3_write_a(uint8_t value)
{
  if (!write_reg(REG_OLATA, value)) return false;
  s_olat_a = value;
  return true;
}

bool mcp23017_u3_write_b(uint8_t value)
{
  if (!write_reg(REG_OLATB, value)) return false;
  s_olat_b = value;
  return true;
}

bool mcp23017_u3_read_port_a(uint8_t* out)
{
  return read_reg(REG_GPIOA, out);
}

bool mcp23017_u3_read_port_b(uint8_t* out)
{
  return read_reg(REG_GPIOB, out);
}

uint8_t mcp23017_u3_read_a()
{
  uint8_t value = 0xFF;
  (void)mcp23017_u3_read_port_a(&value);
  return value;
}

uint8_t mcp23017_u3_read_b()
{
  uint8_t value = 0xFF;
  (void)mcp23017_u3_read_port_b(&value);
  return value;
}

bool mcp23017_u3_set_a(uint8_t bit, bool level)
{
  return update_bit(REG_OLATA, &s_olat_a, bit, level);
}

bool mcp23017_u3_set_b(uint8_t bit, bool level)
{
  return update_bit(REG_OLATB, &s_olat_b, bit, level);
}

bool mcp23017_u3_read_a_bit(uint8_t bit, bool* level)
{
  if (!level || bit >= 8) return false;
  uint8_t value = 0;
  if (!mcp23017_u3_read_port_a(&value)) return false;
  *level = (value & static_cast<uint8_t>(1u << bit)) != 0;
  return true;
}

bool mcp23017_u3_read_b_bit(uint8_t bit, bool* level)
{
  if (!level || bit >= 8) return false;
  uint8_t value = 0;
  if (!mcp23017_u3_read_port_b(&value)) return false;
  *level = (value & static_cast<uint8_t>(1u << bit)) != 0;
  return true;
}

void mcp23017_u3_debug_dump()
{
  uint8_t gpio_a = 0xFF;
  uint8_t gpio_b = 0xFF;
  const bool a_ok = mcp23017_u3_read_port_a(&gpio_a);
  const bool b_ok = mcp23017_u3_read_port_b(&gpio_b);

  LOGD("[MCP23017] 状态：就绪=%d A有效=%d B有效=%d GPIOA=0x%02X GPIOB=0x%02X OLATA=0x%02X OLATB=0x%02X",
       s_ready ? 1 : 0,
       a_ok ? 1 : 0,
       b_ok ? 1 : 0,
       gpio_a,
       gpio_b,
       s_olat_a,
       s_olat_b);
}
