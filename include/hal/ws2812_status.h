#pragma once

/** 初始化 GPIO3 上的一颗 WS2812 状态灯。 */
bool ws2812_status_begin();

/** 非阻塞更新播放灯效；应在主循环高频调用。 */
void ws2812_status_tick();

/** 立即发送全黑并清空当前动画状态。 */
void ws2812_status_off();

/** 设置变化后强制重新计算灯效。 */
void ws2812_status_force_refresh();
