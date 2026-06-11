#pragma once

#include <stdint.h>

/**
 * @brief 保存关键状态并关机。
 *
 * 用于 PLAY 长按，也用于睡眠定时到点后的自动关机。
 */
void app_power_save_and_shutdown();

/**
 * @brief 设置睡眠关机定时。
 *
 * @param minutes 分钟数；传 0 表示取消睡眠关机。
 */
void app_power_sleep_timer_set_minutes(uint16_t minutes);

/** @brief 取消睡眠关机定时。 */
void app_power_sleep_timer_cancel();

/** @brief 睡眠关机定时是否启用。 */
bool app_power_sleep_timer_is_active();

/** @brief 返回睡眠关机剩余秒数；未启用时返回 0。 */
uint32_t app_power_sleep_timer_remaining_seconds();

/** @brief 返回当前睡眠关机档位分钟数；未启用时返回 0。 */
uint16_t app_power_sleep_timer_preset_minutes();

/**
 * @brief 切换到下一个睡眠关机档位。
 *
 * 档位：关闭 -> 15 -> 30 -> 60 -> 90 -> 120 -> 关闭。
 * @return 切换后的档位分钟数，0 表示关闭。
 */
uint16_t app_power_sleep_timer_cycle_next();

/**
 * @brief 睡眠关机后台检查。
 *
 * 需要在 app_state_update() 高频调用；到点后会走 app_power_save_and_shutdown()。
 */
void app_power_sleep_timer_tick();
