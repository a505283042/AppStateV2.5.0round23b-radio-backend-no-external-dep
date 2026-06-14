#pragma once

#include "menu/quick_menu_types.h"

const QuickMenuPageDef& quick_menu_get_nfc_page();

// 供播放器页的 NFC 绑定类型小弹窗直接调用。
const QuickMenuPageDef& quick_menu_get_nfc_page();
const QuickMenuPageDef& quick_menu_get_nfc_list_page();
const QuickMenuPageDef& quick_menu_get_nfc_detail_page();

// NFC列表页不再显示“上一页/下一页”菜单项；由按键/旋钮越界时触发翻页。
void quick_menu_nfc_list_reset_page();
bool quick_menu_nfc_list_prev_page();
bool quick_menu_nfc_list_next_page();