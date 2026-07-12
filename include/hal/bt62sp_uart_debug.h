#pragma once

#include <stdint.h>

enum class Bt62spVolumeQueryResult : uint8_t {
    None = 0,
    Success,
    Timeout,
};

struct Bt62spVolumeQueryEvent {
    Bt62spVolumeQueryResult result = Bt62spVolumeQueryResult::None;
    uint32_t request_id = 0;
    uint8_t volume = 0;
};

// 初始化 EWM104-BT62SP 串口调试桥。
// USB 串口仍然使用 Serial，BT62SP 使用 UART1。
void bt62sp_uart_debug_begin(uint32_t baud = 1000000);

// 非阻塞转发：
// - 电脑 USB 串口输入 AT... 会自动补 \r\n 后转发到 BT62SP。
// - BT62SP 返回内容会打印到电脑 USB 串口。
// - 同时推进音量查询/设置命令状态机。
void bt62sp_uart_debug_update();

// 非阻塞请求查询 BT62SP 当前保存音量。
// settle_ms 用于等待模块上电稳定，不会调用 delay() 阻塞当前任务。
// 返回 true 表示查询已排队，最终结果由 bt62sp_uart_debug_take_volume_query_event() 获取。
bool bt62sp_uart_debug_request_volume_query(uint32_t settle_ms,
                                            uint32_t* out_request_id = nullptr);

// 取消尚未发送或正在等待响应的音量查询。
void bt62sp_uart_debug_cancel_volume_query();

// 获取并消费一次音量查询结果。没有新结果时返回 false。
bool bt62sp_uart_debug_take_volume_query_event(Bt62spVolumeQueryEvent* out_event);

// 非阻塞排队发送 BT62SP 音量设置命令 AT+VOL=<0..100>。
// 新的设置会覆盖尚未发送的旧设置，并取消仍在进行的音量查询。
bool bt62sp_uart_debug_set_volume(uint8_t volume);
