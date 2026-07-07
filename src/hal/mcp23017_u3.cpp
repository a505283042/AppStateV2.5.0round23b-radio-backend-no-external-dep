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
constexpr uint8_t REG_INTFA = 0x0E;
constexpr uint8_t REG_INTFB = 0x0F;
constexpr uint8_t REG_INTCAPA = 0x10;
constexpr uint8_t REG_INTCAPB = 0x11;
constexpr uint8_t REG_GPIOA = 0x12;
constexpr uint8_t REG_GPIOB = 0x13;
constexpr uint8_t REG_OLATA = 0x14;
constexpr uint8_t REG_OLATB = 0x15;

// IOCON:
// bit6 MIRROR = 1
// bit2 ODR    = 1
// INTA/INTB mirror + open-drain, reserved for later interrupt usage.
constexpr uint8_t IOCON_MIRROR_ODR = 0x44;

// A2/A3/A4/A6/A7 input: BACK/MODE, EC06_E, BT_LINK, PREV/NFC, NEXT/LIST
constexpr uint8_t IODIRA_VALUE =
    (1 << board::MCP_A_KEY_BACK_MODE) |
    (1 << board::MCP_A_EC06_E) |
    (1 << board::MCP_A_BT_LINK) |
    (1 << board::MCP_A_KEY_PREV_NFC) |
    (1 << board::MCP_A_KEY_NEXT_LIST);

// B5/B6 input: PG, CHG_STAT
constexpr uint8_t IODIRB_VALUE =
    (1 << board::MCP_B_PG) |
    (1 << board::MCP_B_CHG_STAT);

// 按键和 BT_LINK 内部弱上拉；外部已有 10k 上拉时，这里再开一层保险。
constexpr uint8_t GPPUA_VALUE = IODIRA_VALUE;

// PG/CHG_STAT 外部已有 10k 上拉，可以不开；打开也通常没问题。
// 第一版开弱上拉，避免悬空风险。
constexpr uint8_t GPPUB_VALUE = IODIRB_VALUE;

bool s_ready = false;
uint8_t s_olat_a = 0x00;
uint8_t s_olat_b = 0x00;

bool write_reg(uint8_t reg, uint8_t value) {
  if (!i2c_bus_is_ready()) return false;
  i2c_bus_lock();
  Wire.beginTransmission(board::MCP23017_U3_ADDR);
  Wire.write(reg);
  Wire.write(value);
  const uint8_t err = Wire.endTransmission();
  i2c_bus_unlock();

  if (err != 0) {
    LOGW("[MCP23017] 写寄存器失败 寄存器=0x%02X 值=0x%02X 错误=%u",
         reg,
         value,
         err);
    return false;
  }

  return true;
}

bool read_reg(uint8_t reg, uint8_t* out) {
  if (!out) return false;
  if (!i2c_bus_is_ready()) return false;

  i2c_bus_lock();
  Wire.beginTransmission(board::MCP23017_U3_ADDR);
  Wire.write(reg);
  uint8_t err = Wire.endTransmission(false);
  if (err != 0) {
    i2c_bus_unlock();
    LOGW("[MCP23017] 选择读取寄存器失败 寄存器=0x%02X 错误=%u", reg, err);
    return false;
  }

  const uint8_t n = Wire.requestFrom((int)board::MCP23017_U3_ADDR, 1);
  if (n != 1 || !Wire.available()) {
    i2c_bus_unlock();
    LOGW("[MCP23017] 读取寄存器失败 寄存器=0x%02X 数量=%u", reg, n);
    return false;
  }

  *out = Wire.read();
  i2c_bus_unlock();
  return true;
}

bool update_bit(uint8_t reg, uint8_t* shadow, uint8_t bit, bool level) {
  if (!shadow || bit >= 8) return false;

  uint8_t next = *shadow;
  if (level) {
    next |= (1 << bit);
  } else {
    next &= ~(1 << bit);
  }

  if (!write_reg(reg, next)) {
    return false;
  }

  *shadow = next;
  return true;
}

}  // namespace

bool mcp23017_u3_begin() {
  s_ready = false;

  // 先写输出 latch 到安全态，再设置方向，避免输出瞬间跳变。
  s_olat_a = 0x00;
  s_olat_b = 0x00;

  bool ok = true;

  ok &= write_reg(REG_OLATA, s_olat_a);
  ok &= write_reg(REG_OLATB, s_olat_b);

  // 关闭极性反转。
  ok &= write_reg(REG_IPOLA, 0x00);
  ok &= write_reg(REG_IPOLB, 0x00);

  // 暂时不启用中断，先轮询稳定。
  ok &= write_reg(REG_GPINTENA, 0x00);
  ok &= write_reg(REG_GPINTENB, 0x00);
  ok &= write_reg(REG_DEFVALA, 0x00);
  ok &= write_reg(REG_DEFVALB, 0x00);
  ok &= write_reg(REG_INTCONA, 0x00);
  ok &= write_reg(REG_INTCONB, 0x00);

  // 配置 IOCON。0x0A/0x0B 是同一个 IOCON 镜像寄存器，这里写 0x0A 即可。
  ok &= write_reg(REG_IOCON, IOCON_MIRROR_ODR);

  ok &= write_reg(REG_GPPUA, GPPUA_VALUE);
  ok &= write_reg(REG_GPPUB, GPPUB_VALUE);

  ok &= write_reg(REG_IODIRA, IODIRA_VALUE);
  ok &= write_reg(REG_IODIRB, IODIRB_VALUE);

  // 读一次 GPIO/INTCAP 清潜在中断状态。
  uint8_t dummy = 0;
  (void)read_reg(REG_GPIOA, &dummy);
  (void)read_reg(REG_GPIOB, &dummy);
  (void)read_reg(REG_INTCAPA, &dummy);
  (void)read_reg(REG_INTCAPB, &dummy);

  s_ready = ok;

  if (s_ready) {
    LOGI("[MCP23017] U3 初始化成功：地址=0x%02X IODIRA=0x%02X IODIRB=0x%02X",
         board::MCP23017_U3_ADDR,
         IODIRA_VALUE,
         IODIRB_VALUE);
  } else {
    LOGW("[MCP23017] U3 初始化失败：地址=0x%02X", board::MCP23017_U3_ADDR);
  }

  return s_ready;
}

bool mcp23017_u3_is_ready() {
  return s_ready;
}

bool mcp23017_u3_write_a(uint8_t value) {
  if (!write_reg(REG_OLATA, value)) return false;
  s_olat_a = value;
  return true;
}

bool mcp23017_u3_write_b(uint8_t value) {
  if (!write_reg(REG_OLATB, value)) return false;
  s_olat_b = value;
  return true;
}

uint8_t mcp23017_u3_read_a() {
  uint8_t value = 0xFF;
  (void)read_reg(REG_GPIOA, &value);
  return value;
}

uint8_t mcp23017_u3_read_b() {
  uint8_t value = 0xFF;
  (void)read_reg(REG_GPIOB, &value);
  return value;
}

bool mcp23017_u3_set_a(uint8_t bit, bool level) {
  return update_bit(REG_OLATA, &s_olat_a, bit, level);
}

bool mcp23017_u3_set_b(uint8_t bit, bool level) {
  return update_bit(REG_OLATB, &s_olat_b, bit, level);
}

bool mcp23017_u3_read_a_bit(uint8_t bit, bool* level) {
  if (!level || bit >= 8) return false;
  const uint8_t value = mcp23017_u3_read_a();
  *level = (value & (1 << bit)) != 0;
  return true;
}

bool mcp23017_u3_read_b_bit(uint8_t bit, bool* level) {
  if (!level || bit >= 8) return false;
  const uint8_t value = mcp23017_u3_read_b();
  *level = (value & (1 << bit)) != 0;
  return true;
}

void mcp23017_u3_debug_dump() {
  const uint8_t gpio_a = mcp23017_u3_read_a();
  const uint8_t gpio_b = mcp23017_u3_read_b();

  LOGD("[MCP23017] 状态：就绪=%d GPIOA=0x%02X GPIOB=0x%02X OLATA=0x%02X OLATB=0x%02X",
       s_ready ? 1 : 0,
       gpio_a,
       gpio_b,
       s_olat_a,
       s_olat_b);
}