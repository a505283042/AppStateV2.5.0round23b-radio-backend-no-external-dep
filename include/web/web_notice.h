#pragma once

#include <stdint.h>

enum class WebNoticeLevel : uint8_t {
    Info = 0,
    Warning,
    Error,
};

struct WebNoticeSnapshot {
    uint32_t sequence = 0;
    uint32_t created_ms = 0;
    WebNoticeLevel level = WebNoticeLevel::Info;
    char title[48] = {};
    char detail[112] = {};
};

// 发布一条面向所有已打开网页的短时提示。使用固定缓冲，不进行动态分配。
void web_notice_publish(const char* title,
                        const char* detail,
                        WebNoticeLevel level = WebNoticeLevel::Info);

// 读取最近一条网页提示。尚未发布过提示时返回 false。
bool web_notice_snapshot(WebNoticeSnapshot* out);
