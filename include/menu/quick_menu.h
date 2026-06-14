#pragma once

#include <stdint.h>
#include "menu/quick_menu_types.h"

bool quick_menu_is_active();

void quick_menu_enter();
void quick_menu_exit();

// 供菜单 Action 在执行后跳转到另一个菜单页，例如 NFC 列表进入详情页。
void quick_menu_open_page(QuickMenuPage page);

void quick_menu_tick();
void quick_menu_handle_key(QuickMenuKey key);

QuickMenuPage quick_menu_get_page();
const char* quick_menu_get_page_title();

uint8_t quick_menu_get_item_count();
int quick_menu_get_selected_index();

uint32_t quick_menu_get_revision();

bool quick_menu_get_item_view(uint8_t index, QuickMenuItemView& out);