#pragma once

#include "menu/quick_menu_types.h"

const QuickMenuPageDef& quick_menu_get_nfc_page();

// 供播放器页的 NFC 绑定类型小弹窗直接调用。
bool quick_menu_nfc_bind_current_track();
bool quick_menu_nfc_bind_current_artist();
bool quick_menu_nfc_bind_current_album();