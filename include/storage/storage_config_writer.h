#pragma once

#include <Arduino.h>
#include <SdFat.h>

// 配置文件写入回调。调用期间已经持有 SD 递归互斥锁。
using StorageConfigWriteCallback = bool (*)(File32& file, void* context);

// 配置文件校验回调。文件以只读方式打开，调用期间已经持有 SD 递归互斥锁。
using StorageConfigValidateCallback = bool (*)(File32& file, void* context);

// 延迟配置写入完成回调。回调在安全提交窗口执行，只应更新轻量状态，不能再次写TF卡。
using StorageConfigCommitCallback = void (*)(const char* final_path,
                                             bool success,
                                             void* context);

/**
 * @brief 使用 .tmp + .bak 事务方式替换受管配置或系统资源文件。
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

/**
 * @brief 将完整配置或电台资源复制到 PSRAM，等待安全窗口落盘。
 *
 * 同一路径重复暂存时只保留最新内容。validator_context 与 callback_context
 * 必须在提交完成前保持有效；建议只传静态对象或 nullptr。
 */
bool storage_config_stage_psram(const char* final_path,
                                const uint8_t* data,
                                size_t data_size,
                                StorageConfigValidateCallback validator = nullptr,
                                void* validator_context = nullptr,
                                StorageConfigCommitCallback callback = nullptr,
                                void* callback_context = nullptr);

/**
 * @brief 在旧音频文件关闭、下一音频文件尚未打开的安全窗口提交全部待写配置。
 *
 * 写入失败的项目会继续保留在 PSRAM，等待下一个安全窗口重试。
 */
bool storage_config_commit_pending(uint32_t lock_timeout_ms = 4000);

// 查询待写配置。
bool storage_config_has_pending(void);
bool storage_config_has_pending_path(const char* final_path);
size_t storage_config_pending_size(const char* final_path);

/**
 * @brief 将某个待写配置复制到调用方缓冲，供网页预览尚未落盘的新值。
 */
bool storage_config_read_pending(const char* final_path,
                                 uint8_t* out,
                                 size_t out_capacity,
                                 size_t* out_size);

/**
 * @brief 丢弃指定路径的待写配置。
 *
 * 用于多文件配置暂存失败时回滚已经入队的另一部分，避免只落盘半套配置。
 */
bool storage_config_discard_pending_path(const char* final_path,
                                         const char* reason = nullptr);

/**
 * @brief 丢弃全部待写配置。换卡时必须调用，避免旧卡配置写入新卡。
 */
void storage_config_discard_pending(const char* reason = nullptr);
