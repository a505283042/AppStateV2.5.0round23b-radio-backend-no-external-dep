#pragma once
void keys_init();
void keys_update();
void keys_sync_to_hw_state();  // 同步按键状态到硬件，用于状态切换时避免误判
