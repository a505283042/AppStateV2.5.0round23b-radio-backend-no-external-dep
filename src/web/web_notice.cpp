#include "web/web_notice.h"

#include <Arduino.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/portmacro.h>

namespace {

portMUX_TYPE s_web_notice_mux = portMUX_INITIALIZER_UNLOCKED;
WebNoticeSnapshot s_web_notice;

void copy_notice_text(char* dst, size_t dst_size, const char* src)
{
    if (!dst || dst_size == 0) return;
    if (!src) src = "";

    size_t len = std::strlen(src);
    if (len >= dst_size) len = dst_size - 1;
    if (len > 0) std::memcpy(dst, src, len);
    dst[len] = '\0';
}

}  // namespace

void web_notice_publish(const char* title,
                        const char* detail,
                        WebNoticeLevel level)
{
    WebNoticeSnapshot next{};
    next.created_ms = millis();
    next.level = level;
    copy_notice_text(next.title, sizeof(next.title), title);
    copy_notice_text(next.detail, sizeof(next.detail), detail);

    portENTER_CRITICAL(&s_web_notice_mux);
    next.sequence = s_web_notice.sequence + 1;
    if (next.sequence == 0) next.sequence = 1;
    s_web_notice = next;
    portEXIT_CRITICAL(&s_web_notice_mux);
}

bool web_notice_snapshot(WebNoticeSnapshot* out)
{
    if (!out) return false;

    portENTER_CRITICAL(&s_web_notice_mux);
    *out = s_web_notice;
    portEXIT_CRITICAL(&s_web_notice_mux);
    return out->sequence != 0;
}
