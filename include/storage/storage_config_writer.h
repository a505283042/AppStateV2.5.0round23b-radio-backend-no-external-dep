#pragma once

#include <Arduino.h>
#include <SdFat.h>

// 配置文件写入回调。调用期间已经持有 SD 递归互斥锁。
using StorageConfigWriteCallback = bool (*)(File32& file, void* context);

// 配置文件校验回调。文件以只读方式打开，调用期间已经持有 SD 递归互斥锁。
using StorageConfigValidateCallback = bool (*)(File32& file, void* context);

/**
 * @brief 使用 .tmp + .bak 事务方式替换 /System/config 下的配置文件。
 *
 * 保存成功后保留上一版 .bak；写入、同步、校验或重命名失败时恢复旧文件。
 */
bool storage_config_atomic_write(const char* final_path,
                                 StorageConfigWriteCallback writer,
                                 void* writer_context,
                                 StorageConfigValidateCallback validator = nullptr,
                                 void* validator_context = nullptr,
                                 uint32_t lock_timeout_ms = 3000);

/**
 * @brief 从有效的 .tmp 或 .bak 恢复配置文件。
 *
 * 正式文件有效时只清理遗留 .tmp；正式文件无效时优先采用已完整落盘的 .tmp，
 * 其次采用 .bak。调用方应在读取配置前调用。
 */
bool storage_config_recover(const char* final_path,
                            StorageConfigValidateCallback validator = nullptr,
                            void* validator_context = nullptr,
                            uint32_t lock_timeout_ms = 3000);
