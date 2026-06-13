#pragma once
#include <Arduino.h>
#include <string.h>

// 0=错误, 1=警告, 2=信息, 3=调试
#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#ifndef LOG_TAG
#define LOG_TAG "APP"
#endif

static inline const char* log_level_cn(const char* level)
{
  if (strcmp(level, "D") == 0) return "调试";
  if (strcmp(level, "I") == 0) return "信息";
  if (strcmp(level, "W") == 0) return "警告";
  if (strcmp(level, "E") == 0) return "错误";
  return level;
}

static inline const char* log_tag_cn(const char* tag)
{
  if (strcmp(tag, "APP") == 0) return "应用";
  if (strcmp(tag, "UI") == 0) return "界面";
  if (strcmp(tag, "WEBCOVER") == 0) return "网页封面";
  return tag;
}

#if LOG_LEVEL >= 3
  #define LOGD(fmt, ...) Serial.printf("[%s][%s] " fmt "\n", log_level_cn("D"), log_tag_cn(LOG_TAG), ##__VA_ARGS__)
#else
  #define LOGD(...) do {} while (0)
#endif

#if LOG_LEVEL >= 2
  #define LOGI(fmt, ...) Serial.printf("[%s][%s] " fmt "\n", log_level_cn("I"), log_tag_cn(LOG_TAG), ##__VA_ARGS__)
#else
  #define LOGI(...) do {} while (0)
#endif

#if LOG_LEVEL >= 1
  #define LOGW(fmt, ...) Serial.printf("[%s][%s] " fmt "\n", log_level_cn("W"), log_tag_cn(LOG_TAG), ##__VA_ARGS__)
#else
  #define LOGW(...) do {} while (0)
#endif

#if LOG_LEVEL >= 0
  #define LOGE(fmt, ...) Serial.printf("[%s][%s] " fmt "\n", log_level_cn("E"), log_tag_cn(LOG_TAG), ##__VA_ARGS__)
#endif
