#pragma once
#include <stdbool.h>
#include "ui/ui.h"

extern volatile bool g_rescan_done;
extern volatile bool g_rescanning;
extern volatile bool g_rescan_success;
extern volatile bool g_abort_scan; // 扫描中断标志
extern volatile play_mode_t g_play_mode;  // 播放模式
