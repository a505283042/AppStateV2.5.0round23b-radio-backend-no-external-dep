#pragma once

/** 初始化 GPIO9 霍尔状态和电磁铁联动控制。 */
void hall_control_begin();

/**
 * 高频维护霍尔防抖、电磁铁到位确认和播放状态联动。
 *
 * - 电磁铁开启：播放键只驱动摆臂，霍尔到位后决定播放/暂停。
 * - 电磁铁关闭、霍尔开启：播放键直接切换，霍尔只在稳定边沿控制一次。
 * - 两者都关闭：霍尔不参与播放控制。
 */
void hall_control_tick();

/**
 * 处理用户播放/暂停请求的电磁铁联动语义。
 *
 * 实体播放键和 Web 播放/暂停必须统一经过此接口：
 * - 电磁铁开启时只驱动摆臂，霍尔到位后再改变播放状态；
 * - 电磁铁关闭时返回 false，由播放器执行普通播放/暂停切换。
 *
 * @return true 表示电磁铁模式已接管本次请求，调用方不得再直接切换播放状态。
 */
bool hall_control_handle_user_toggle();

/** 当前稳定状态是否为摆臂磁铁靠近霍尔。 */
bool hall_control_is_near();

/** 当前是否正在等待电磁铁驱动摆臂到位。 */
bool hall_control_motion_active();

/** 电磁铁联动模式下，摆臂仍在靠近位置时是否应阻止外部恢复请求。 */
bool hall_control_blocks_resume();

/** 最近一次霍尔控制是否把播放器置为暂停。 */
bool hall_control_pause_latched();
