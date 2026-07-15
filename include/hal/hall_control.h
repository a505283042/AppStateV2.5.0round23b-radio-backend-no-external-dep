#pragma once

/** 初始化 GPIO9 霍尔状态控制。 */
void hall_control_begin();

/**
 * 高频维护霍尔防抖和播放状态约束。
 *
 * 稳定靠近：暂停；稳定离开：只恢复由霍尔造成的暂停。
 */
void hall_control_tick();

/** 当前稳定状态是否为磁铁靠近。 */
bool hall_control_is_near();

/** 当前是否应阻止任何恢复/重新起播请求。 */
bool hall_control_blocks_resume();

/** 当前暂停是否由霍尔控制产生，离开时需要恢复。 */
bool hall_control_pause_latched();
