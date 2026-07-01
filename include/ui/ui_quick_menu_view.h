#pragma once

/**
 * @brief 重置快捷菜单绘制缓存。
 *
 * 进入菜单前调用一次，确保下一帧完整绘制。
 */
void ui_quick_menu_view_reset();
bool ui_quick_menu_view_needs_draw();

/**
 * @brief 绘制快捷菜单页面。
 *
 * 调用前需要外层已经完成 ui_draw_lock()。
 * 内部会判断是否需要重画，没变化时直接返回。
 */
void ui_draw_quick_menu();