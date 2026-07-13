#pragma once

#include <Arduino.h>
#include <stdint.h>

struct Bq27441Sample {
    bool valid = false;
    uint16_t voltage_mv = 0;
    uint16_t soc_percent = 0;
    int16_t average_current_ma = 0;
    uint16_t remaining_capacity_mah = 0;
    uint16_t full_charge_capacity_mah = 0;
    uint16_t design_capacity_mah = 0;
    uint16_t flags = 0;
    uint16_t state_of_health_percent = 0;
};

// 续航估算使用的轻量采样，只读取剩余容量和平均电流。
struct Bq27441RuntimeSample {
    bool valid = false;
    int16_t average_current_ma = 0;
    uint16_t remaining_capacity_mah = 0;
};

static constexpr uint16_t BQ27441_FLAG_FC = (1u << 9);
static constexpr uint16_t BQ27441_FLAG_CHG = (1u << 8);
static constexpr uint16_t BQ27441_FLAG_OCVTAKEN = (1u << 7);
static constexpr uint16_t BQ27441_FLAG_ITPOR = (1u << 5);
static constexpr uint16_t BQ27441_FLAG_CFGUPMODE = (1u << 4);
static constexpr uint16_t BQ27441_FLAG_BAT_DET = (1u << 3);
static constexpr uint16_t BQ27441_FLAG_SOC1 = (1u << 2);
static constexpr uint16_t BQ27441_FLAG_SOCF = (1u << 1);
static constexpr uint16_t BQ27441_FLAG_DSG = (1u << 0);

bool bq27441_begin();
bool bq27441_is_ready();
bool bq27441_read(Bq27441Sample* out);
bool bq27441_read_runtime(Bq27441RuntimeSample* out);
uint8_t bq27441_last_i2c_error();

uint16_t bq27441_design_capacity_mah();
uint16_t bq27441_target_design_capacity_mah();
bool bq27441_set_design_capacity_mah(uint16_t capacity_mah);
bool bq27441_configure_if_needed(uint16_t flags_hint = 0);
const char* bq27441_flags_to_text(uint16_t flags);