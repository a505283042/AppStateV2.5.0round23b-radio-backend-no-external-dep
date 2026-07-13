#pragma once

#include "menu/quick_menu_types.h"

const QuickMenuPageDef& quick_menu_get_time_alarm_page();
const QuickMenuPageDef& quick_menu_get_alarm_settings_page();
const QuickMenuPageDef& quick_menu_get_alarm_time_page();
const QuickMenuPageDef& quick_menu_get_alarm_weekday_page();

// 退出或重新进入菜单时丢弃未保存的闹钟草稿。
void quick_menu_reset_alarm_draft();
