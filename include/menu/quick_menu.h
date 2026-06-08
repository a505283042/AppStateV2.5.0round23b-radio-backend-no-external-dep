#pragma once

#include <stdint.h>

/**
 * @brief 快捷菜单按键事件。
 *
 * 菜单层只认识抽象按键，不直接关心具体硬件。
 * 后面 keys.cpp 负责把 EC06_E / MODE / PLAY / 旋钮 转成这些事件。
 */
enum class QuickMenuKey : uint8_t {
    Up,
    Down,
    Confirm,
    Back,
    Exit,
};

/**
 * @brief 快捷菜单页面。
 */
enum class QuickMenuPage : uint8_t {
    Root,
    Playback,
    Source,
    Display,
    Network,
    AudioOutput,
    Bluetooth,
    Nfc,
    SystemInfo,
};

/**
 * @brief 给 UI 层使用的菜单项显示信息。
 */
struct QuickMenuItemView {
    const char* label = "";
    const char* value = "";
    bool selected = false;
    bool enabled = true;
    bool placeholder = false;
};

bool quick_menu_is_active();

void quick_menu_enter();
void quick_menu_exit();

void quick_menu_tick();
void quick_menu_handle_key(QuickMenuKey key);

QuickMenuPage quick_menu_get_page();
const char* quick_menu_get_page_title();

uint8_t quick_menu_get_item_count();
int quick_menu_get_selected_index();

bool quick_menu_get_item_view(uint8_t index, QuickMenuItemView& out);