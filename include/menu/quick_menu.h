#pragma once

#include <stdint.h>
#include "menu/quick_menu_types.h"

bool quick_menu_is_active();

void quick_menu_enter();
void quick_menu_exit();

void quick_menu_tick();
void quick_menu_handle_key(QuickMenuKey key);

QuickMenuPage quick_menu_get_page();
const char* quick_menu_get_page_title();

uint8_t quick_menu_get_item_count();
int quick_menu_get_selected_index();

uint32_t quick_menu_get_revision();

bool quick_menu_get_item_view(uint8_t index, QuickMenuItemView& out);