#pragma once

/**
 * @brief 显示关机流程提示。
 *
 * 会 hold UI 自动刷新，避免播放器页面覆盖提示。
 */
void ui_power_show_shutdown_stage(const char* line1, const char* line2);