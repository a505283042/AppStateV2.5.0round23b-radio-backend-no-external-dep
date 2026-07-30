#include "storage/storage_config_writer.h"

#include <stdio.h>
#include <string.h>

#include "storage/storage.h"
#include "storage/storage_io.h"
#include "utils/log.h"

extern SdFat sd;

namespace {

static constexpr size_t kConfigPathBufferBytes = 192;
static constexpr const char* kConfigRootPrefix = "/System/config/";

static bool config_path_allowed(const char* path)
{
  if (!path || !*path) return false;
  return strncmp(path,
                 kConfigRootPrefix,
                 strlen(kConfigRootPrefix)) == 0 &&
         strstr(path, "..") == nullptr;
}

static bool build_sidecar_path(const char* final_path,
                               const char* suffix,
                               char out[kConfigPathBufferBytes])
{
  if (!final_path || !suffix || !out) return false;
  const int written = snprintf(out,
                               kConfigPathBufferBytes,
                               "%s%s",
                               final_path,
                               suffix);
  return written > 0 &&
         static_cast<size_t>(written) < kConfigPathBufferBytes;
}

static void remove_if_exists_locked(const char* path)
{
  if (path && sd.exists(path)) {
    (void)sd.remove(path);
  }
}

static bool validate_file_locked(const char* path,
                                 StorageConfigValidateCallback validator,
                                 void* validator_context)
{
  if (!path || !sd.exists(path)) return false;

  File32 file = sd.open(path, O_RDONLY);
  if (!file) return false;

  const bool ok = validator ? validator(file, validator_context) : true;
  file.close();
  return ok;
}

static bool promote_recovery_file_locked(const char* candidate_path,
                                         const char* final_path,
                                         StorageConfigValidateCallback validator,
                                         void* validator_context)
{
  if (!validate_file_locked(candidate_path, validator, validator_context)) {
    return false;
  }

  if (sd.exists(final_path)) {
    remove_if_exists_locked(final_path);
  }

  if (!sd.rename(candidate_path, final_path)) {
    LOGE("[配置文件] 恢复文件提升失败：%s -> %s",
         candidate_path,
         final_path);
    return false;
  }

  if (!validate_file_locked(final_path, validator, validator_context)) {
    LOGE("[配置文件] 恢复后的正式文件校验失败：%s", final_path);
    return false;
  }

  LOGW("[配置文件] 已恢复：%s", final_path);
  return true;
}

}  // namespace

bool storage_config_atomic_write(const char* final_path,
                                 StorageConfigWriteCallback writer,
                                 void* writer_context,
                                 StorageConfigValidateCallback validator,
                                 void* validator_context,
                                 uint32_t lock_timeout_ms)
{
  if (!storage_is_ready()) {
    LOGW("[配置文件] 保存失败：TF卡未就绪");
    return false;
  }
  if (!config_path_allowed(final_path) || !writer) {
    LOGE("[配置文件] 保存参数无效：%s", final_path ? final_path : "-");
    return false;
  }

  char tmp_path[kConfigPathBufferBytes] = {};
  char backup_path[kConfigPathBufferBytes] = {};
  if (!build_sidecar_path(final_path, ".tmp", tmp_path) ||
      !build_sidecar_path(final_path, ".bak", backup_path)) {
    LOGE("[配置文件] 路径过长：%s", final_path);
    return false;
  }

  StorageSdLockGuard guard(lock_timeout_ms);
  if (!guard) {
    LOGW("[配置文件] 保存失败：等待SD锁超时：%s", final_path);
    return false;
  }

  remove_if_exists_locked(tmp_path);

  File32 file = sd.open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOGE("[配置文件] 无法创建临时文件：%s", tmp_path);
    return false;
  }

  bool ok = writer(file, writer_context);
  if (ok) ok = file.sync();
  file.close();

  if (!ok || !validate_file_locked(tmp_path,
                                   validator,
                                   validator_context)) {
    remove_if_exists_locked(tmp_path);
    LOGE("[配置文件] 临时文件写入或校验失败：%s", tmp_path);
    return false;
  }

  const bool had_final = sd.exists(final_path);
  remove_if_exists_locked(backup_path);

  if (had_final && !sd.rename(final_path, backup_path)) {
    remove_if_exists_locked(tmp_path);
    LOGE("[配置文件] 无法创建旧配置备份：%s", final_path);
    return false;
  }

  if (!sd.rename(tmp_path, final_path)) {
    LOGE("[配置文件] 临时文件提升失败，尝试恢复旧配置：%s", final_path);
    if (had_final) {
      (void)sd.rename(backup_path, final_path);
    }
    return false;
  }

  if (!validate_file_locked(final_path, validator, validator_context)) {
    LOGE("[配置文件] 正式文件替换后校验失败：%s", final_path);
    remove_if_exists_locked(final_path);
    if (had_final) {
      (void)sd.rename(backup_path, final_path);
    }
    return false;
  }

  // 第一次创建配置时不能遗留与当前内容无关的旧备份。
  if (!had_final) {
    remove_if_exists_locked(backup_path);
  }

  LOGI("[配置文件] 原子保存成功：%s 备份=%s",
       final_path,
       had_final ? "保留" : "无");
  return true;
}

bool storage_config_recover(const char* final_path,
                            StorageConfigValidateCallback validator,
                            void* validator_context,
                            uint32_t lock_timeout_ms)
{
  if (!storage_is_ready() || !config_path_allowed(final_path)) return false;

  char tmp_path[kConfigPathBufferBytes] = {};
  char backup_path[kConfigPathBufferBytes] = {};
  if (!build_sidecar_path(final_path, ".tmp", tmp_path) ||
      !build_sidecar_path(final_path, ".bak", backup_path)) {
    return false;
  }

  StorageSdLockGuard guard(lock_timeout_ms);
  if (!guard) {
    LOGW("[配置文件] 恢复检查跳过：等待SD锁超时：%s", final_path);
    return false;
  }

  if (validate_file_locked(final_path, validator, validator_context)) {
    // 正式文件有效时，遗留 .tmp 一定不是当前事务的正式结果。
    remove_if_exists_locked(tmp_path);
    return true;
  }

  if (promote_recovery_file_locked(tmp_path,
                                   final_path,
                                   validator,
                                   validator_context)) {
    return true;
  }
  remove_if_exists_locked(tmp_path);

  if (promote_recovery_file_locked(backup_path,
                                   final_path,
                                   validator,
                                   validator_context)) {
    return true;
  }

  return false;
}
