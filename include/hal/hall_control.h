#pragma once

#include <stdint.h>

/** 初始化 GPIO9 霍尔状态和电磁铁联动控制。 */
void hall_control_begin();

/**
 * 高频维护霍尔防抖、电磁铁到位确认和播放状态联动。
 *
 * - 电磁铁开启：播放键只驱动摆臂，霍尔到位后决定播放/暂停。
 * - 电磁铁关闭、霍尔开启：播放位允许播放；停止位必须手动拨到播放位后才执行播放请求。
 * - 两者都关闭：霍尔不参与播放控制。
 */
void hall_control_tick();

/**
 * 处理用户播放/暂停请求的电磁铁联动语义。
 *
 * 实体播放键和 Web 播放/暂停必须统一经过此接口：
 * - 正在播放时，请求摆臂进入靠近霍尔的停止位；
 * - 暂停或停止时，请求摆臂进入离开霍尔的播放位；
 * - 摆臂已经处于目标位时，不重复驱动电磁铁，直接切换播放状态；
 * - 仅霍尔开启且请求播放时：摆臂已在播放位则返回 false 直接播放；
 *   摆臂在停止位则提示手动拨动并返回 true，阻止本次直接播放。
 * - 电磁铁和霍尔均关闭时返回 false，由播放器执行普通播放/暂停切换。
 *
 * @return true 表示霍尔/电磁铁位置控制已接管本次请求，调用方不得再直接切换播放状态。
 */
bool hall_control_handle_user_toggle();

/** 延迟起播请求等待摆臂到播放位后的完成回调。 */
using HallPlayPositionCallback = void (*)(bool success);

/** 请求结果：已经在播放位、已开始移动，或请求失败。 */
enum class HallPlayPositionRequestResult : uint8_t {
    Ready = 0,
    Started,
    Failed,
};

/**
 * 请求把摆臂移动到“离开霍尔”的播放位。
 *
 * - 霍尔关闭，或摆臂已经离开：返回 Ready，调用方可立即执行起播动作；
 * - 电磁铁开启且摆臂靠近：启动电磁铁并返回 Started，到位/失败后调用 callback；
 * - 仅霍尔开启且摆臂靠近：不驱动电磁铁，提示用户手动拨到播放位并返回 Started；
 *   霍尔稳定确认离开后调用 callback(true)，关闭霍尔或模式变化时调用 callback(false)；
 * - 当前状态不允许动作、已有动作进行中或驱动失败：返回 Failed。
 *
 * callback 只在返回 Started 后调用一次。失败时 success=false，调用方不得起播。
 */
HallPlayPositionRequestResult hall_control_request_play_position(
    HallPlayPositionCallback callback);

/** 当前稳定状态是否为摆臂磁铁靠近霍尔。 */
bool hall_control_is_near();

/** 当前是否正在等待电磁铁驱动摆臂到位。 */
bool hall_control_motion_active();

/** 电磁铁联动模式下，摆臂仍在靠近位置时是否应阻止外部恢复请求。 */
bool hall_control_blocks_resume();

/** 最近一次霍尔控制是否把播放器置为暂停。 */
bool hall_control_pause_latched();
