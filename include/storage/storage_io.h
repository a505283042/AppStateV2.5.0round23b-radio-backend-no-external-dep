#pragma once

#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

extern SemaphoreHandle_t g_sd_mutex;

bool storage_sd_init_mutex(void);
bool storage_sd_lock(uint32_t timeout_ms = 1000);
void storage_sd_unlock(void);

class StorageSdLockGuard {
public:
  explicit StorageSdLockGuard(uint32_t timeout_ms = 1000)
      : m_locked(storage_sd_lock(timeout_ms)) {}
  ~StorageSdLockGuard() {
    if (m_locked) storage_sd_unlock();
  }

  bool locked() const { return m_locked; }
  explicit operator bool() const { return m_locked; }

private:
  bool m_locked;
};
