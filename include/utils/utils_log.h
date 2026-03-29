/* 日志工具模块头文件 */
#ifndef UTILS_LOG_H
#define UTILS_LOG_H

#include <Arduino.h>  /* 包含Arduino核心库 */

/* =========================================================
 * 日志工具
 * 用于调试的简单日志宏
 * ========================================================= */

/* 信息级别日志宏 - 输出信息级别的日志消息 */
#define LOGI(tag, fmt, ...) \
    Serial.printf("[INFO][%s] " fmt "\r\n", tag, ##__VA_ARGS__)

/* 警告级别日志宏 - 输出警告级别的日志消息 */
#define LOGW(tag, fmt, ...) \
    Serial.printf("[WARN][%s] " fmt "\r\n", tag, ##__VA_ARGS__)

/* 错误级别日志宏 - 输出错误级别的日志消息 */
#define LOGE(tag, fmt, ...) \
    Serial.printf("[ERROR][%s] " fmt "\r\n", tag, ##__VA_ARGS__)

#endif // UTILS_LOG_H