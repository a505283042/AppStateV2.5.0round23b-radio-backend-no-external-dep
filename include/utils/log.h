#pragma once
#include <Arduino.h>

// 0=ERROR, 1=WARN, 2=INFO, 3=DEBUG
#ifndef LOG_LEVEL
#define LOG_LEVEL 2
#endif

#ifndef LOG_TAG
#define LOG_TAG "APP"
#endif

#if LOG_LEVEL >= 3
  #define LOGD(fmt, ...) Serial.printf("[D][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#else
  #define LOGD(...) do {} while (0)
#endif

#if LOG_LEVEL >= 2
  #define LOGI(fmt, ...) Serial.printf("[I][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#else
  #define LOGI(...) do {} while (0)
#endif

#if LOG_LEVEL >= 1
  #define LOGW(fmt, ...) Serial.printf("[W][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#else
  #define LOGW(...) do {} while (0)
#endif

#if LOG_LEVEL >= 0
  #define LOGE(fmt, ...) Serial.printf("[E][%s] " fmt "\n", LOG_TAG, ##__VA_ARGS__)
#endif
