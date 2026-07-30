#include "storage/storage_config_writer.h"

#include <stdio.h>
#include <string.h>

#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include "storage/storage.h"
#include "storage/storage_io.h"
#include "utils/log.h"

extern SdFat sd;

namespace {

static constexpr size_t kConfigPathBufferBytes = 192;
static constexpr const char* kConfigRootPrefix = "/System/config/";
static constexpr const char* kRadioAssetRootPrefix = "/System/assets/radio/";
static constexpr const char* kDefaultCoverPath = "/System/assets/default_cover.jpg";
static constexpr const char* kNetCoverLoadingPath = "/System/assets/net_cover_loading.jpg";

struct PendingConfigWrite {
  PendingConfigWrite* next = nullptr;
  size_t data_size = 0;
  StorageConfigValidateCallback validator = nullptr;
  void* validator_context = nullptr;
  StorageConfigCommitCallback callback = nullptr;
  void* callback_context = nullptr;
  char final_path[kConfigPathBufferBytes] = {};
  uint8_t data[1];
};

static PendingConfigWrite* s_pending_config_head = nullptr;
static StaticSemaphore_t s_pending_config_mutex_storage;
static SemaphoreHandle_t s_pending_config_mutex = nullptr;

static SemaphoreHandle_t pending_mutex_get()
{
  if (!s_pending_config_mutex) {
    // 首次暂存发生在调度器启动后的普通任务上下文；使用静态互斥量，不占用堆。
    s_pending_config_mutex =
        xSemaphoreCreateMutexStatic(&s_pending_config_mutex_storage);
  }
  return s_pending_config_mutex;
}

class PendingConfigLockGuard {
 public:
  PendingConfigLockGuard()
  {
    SemaphoreHandle_t mutex = pending_mutex_get();
    locked_ = mutex && xSemaphoreTake(mutex, portMAX_DELAY) == pdTRUE;
  }

  ~PendingConfigLockGuard()
  {
    if (locked_) xSemaphoreGive(s_pending_config_mutex);
  }

  explicit operator bool() const { return locked_; }

 private:
  bool locked_ = false;
};

static size_t pending_alloc_bytes(size_t data_size)
{
  return sizeof(PendingConfigWrite) - sizeof(uint8_t) + data_size;
}

static void pending_free(PendingConfigWrite* item)
{
  if (item) heap_caps_free(item);
}

static PendingConfigWrite* pending_find_locked(const char* final_path)
{
  for (PendingConfigWrite* item = s_pending_config_head; item; item = item->next) {
    if (strcmp(item->final_path, final_path) == 0) return item;
  }
  return nullptr;
}

static void pending_notify_and_free(PendingConfigWrite* item, bool success)
{
  if (!item) return;
  if (item->callback) {
    item->callback(item->final_path, success, item->callback_context);
  }
  pending_free(item);
}

struct PendingMemoryWriterContext {
  const uint8_t* data = nullptr;
  size_t size = 0;
};

static bool write_pending_memory(File32& file, void* context)
{
  PendingMemoryWriterContext* memory =
      static_cast<PendingMemoryWriterContext*>(context);
  if (!memory || (!memory->data && memory->size > 0)) return false;

  size_t offset = 0;
  while (offset < memory->size) {
    const size_t written = file.write(memory->data + offset,
                                      memory->size - offset);
    if (written == 0) return false;
    offset += written;
  }
  return true;
}

static bool config_path_allowed(const char* path)
{
  if (!path || !*path || strstr(path, "..") != nullptr) return false;

  const bool config_file =
      strncmp(path, kConfigRootPrefix, strlen(kConfigRootPrefix)) == 0;
  const bool radio_asset =
      strncmp(path,
              kRadioAssetRootPrefix,
              strlen(kRadioAssetRootPrefix)) == 0;
  const bool managed_cover =
      strcmp(path, kDefaultCoverPath) == 0 ||
      strcmp(path, kNetCoverLoadingPath) == 0;
  return config_file || radio_asset || managed_cover;
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


bool storage_config_stage_psram(const char* final_path,
                                const uint8_t* data,
                                size_t data_size,
                                StorageConfigValidateCallback validator,
                                void* validator_context,
                                StorageConfigCommitCallback callback,
                                void* callback_context)
{
  if (!storage_is_ready()) {
    LOGW("[配置文件] 暂存失败：TF卡未就绪");
    return false;
  }
  if (!config_path_allowed(final_path) || !data || data_size == 0) {
    LOGE("[配置文件] 暂存参数无效：%s 大小=%u",
         final_path ? final_path : "-",
         (unsigned)data_size);
    return false;
  }

  const size_t path_len = strlen(final_path);
  if (path_len >= kConfigPathBufferBytes) {
    LOGE("[配置文件] 暂存路径过长：%s", final_path);
    return false;
  }

  PendingConfigWrite* fresh = static_cast<PendingConfigWrite*>(
      heap_caps_malloc(pending_alloc_bytes(data_size),
                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!fresh) {
    LOGE("[配置文件] PSRAM暂存分配失败：%s 大小=%u",
         final_path,
         (unsigned)data_size);
    return false;
  }

  memset(fresh, 0, sizeof(PendingConfigWrite));
  fresh->data_size = data_size;
  fresh->validator = validator;
  fresh->validator_context = validator_context;
  fresh->callback = callback;
  fresh->callback_context = callback_context;
  memcpy(fresh->final_path, final_path, path_len + 1);
  memcpy(fresh->data, data, data_size);

  PendingConfigWrite* replaced = nullptr;
  {
    PendingConfigLockGuard guard;
    if (!guard) {
      pending_free(fresh);
      LOGE("[配置文件] 暂存失败：无法创建待写互斥量");
      return false;
    }

    PendingConfigWrite** link = &s_pending_config_head;
    while (*link) {
      if (strcmp((*link)->final_path, final_path) == 0) {
        replaced = *link;
        fresh->next = replaced->next;
        *link = fresh;
        break;
      }
      link = &((*link)->next);
    }
    if (!replaced) {
      fresh->next = s_pending_config_head;
      s_pending_config_head = fresh;
    }
  }

  // 被新内容覆盖不属于落盘失败，不触发旧回调，直接释放旧PSRAM节点。
  pending_free(replaced);

  LOGI("[配置文件] 已暂存到PSRAM：%s 大小=%u%s",
       final_path,
       (unsigned)data_size,
       replaced ? "（覆盖旧待写内容）" : "");
  return true;
}

bool storage_config_commit_pending(uint32_t lock_timeout_ms)
{
  PendingConfigWrite* batch = nullptr;
  {
    PendingConfigLockGuard guard;
    if (!guard) return false;
    batch = s_pending_config_head;
    s_pending_config_head = nullptr;
  }

  if (!batch) return true;

  bool all_ok = true;
  size_t committed = 0;
  size_t failed = 0;

  while (batch) {
    PendingConfigWrite* item = batch;
    batch = batch->next;
    item->next = nullptr;

    PendingMemoryWriterContext memory{};
    memory.data = item->data;
    memory.size = item->data_size;

    const bool ok = storage_config_atomic_write(item->final_path,
                                                write_pending_memory,
                                                &memory,
                                                item->validator,
                                                item->validator_context,
                                                lock_timeout_ms);
    if (ok) {
      ++committed;
      pending_notify_and_free(item, true);
      continue;
    }

    all_ok = false;
    ++failed;

    // 提交期间可能已经收到同一路径的新版本；新版本优先，旧失败节点不再回队。
    bool newer_exists = false;
    bool requeued = false;
    {
      PendingConfigLockGuard guard;
      if (guard) {
        newer_exists = pending_find_locked(item->final_path) != nullptr;
        if (!newer_exists) {
          item->next = s_pending_config_head;
          s_pending_config_head = item;
          requeued = true;
        }
      }
    }

    if (newer_exists || !requeued) {
      pending_notify_and_free(item, false);
    } else if (item->callback) {
      // 写入失败但节点会保留重试，只通知失败，不释放。
      item->callback(item->final_path, false, item->callback_context);
    }
  }

  LOGI("[配置文件] 安全窗口提交完成：成功=%u 失败=%u",
       (unsigned)committed,
       (unsigned)failed);
  return all_ok;
}

bool storage_config_has_pending(void)
{
  PendingConfigLockGuard guard;
  return guard && s_pending_config_head != nullptr;
}

bool storage_config_has_pending_path(const char* final_path)
{
  if (!final_path) return false;
  PendingConfigLockGuard guard;
  return guard && pending_find_locked(final_path) != nullptr;
}

size_t storage_config_pending_size(const char* final_path)
{
  if (!final_path) return 0;
  PendingConfigLockGuard guard;
  if (!guard) return 0;
  PendingConfigWrite* item = pending_find_locked(final_path);
  return item ? item->data_size : 0;
}

bool storage_config_read_pending(const char* final_path,
                                 uint8_t* out,
                                 size_t out_capacity,
                                 size_t* out_size)
{
  if (out_size) *out_size = 0;
  if (!final_path || !out) return false;

  PendingConfigLockGuard guard;
  if (!guard) return false;
  PendingConfigWrite* item = pending_find_locked(final_path);
  if (!item || item->data_size > out_capacity) return false;

  memcpy(out, item->data, item->data_size);
  if (out_size) *out_size = item->data_size;
  return true;
}

bool storage_config_discard_pending_path(const char* final_path,
                                         const char* reason)
{
  if (!final_path || !*final_path) return false;

  PendingConfigWrite* removed = nullptr;
  {
    PendingConfigLockGuard guard;
    if (!guard) return false;

    PendingConfigWrite** link = &s_pending_config_head;
    while (*link) {
      if (strcmp((*link)->final_path, final_path) == 0) {
        removed = *link;
        *link = removed->next;
        removed->next = nullptr;
        break;
      }
      link = &((*link)->next);
    }
  }

  if (!removed) return false;

  pending_notify_and_free(removed, false);
  LOGW("[配置文件] 已丢弃待写配置：路径=%s 原因=%s",
       final_path,
       reason ? reason : "未指定");
  return true;
}

void storage_config_discard_pending(const char* reason)
{
  PendingConfigWrite* batch = nullptr;
  {
    PendingConfigLockGuard guard;
    if (!guard) return;
    batch = s_pending_config_head;
    s_pending_config_head = nullptr;
  }

  size_t count = 0;
  while (batch) {
    PendingConfigWrite* item = batch;
    batch = batch->next;
    item->next = nullptr;
    pending_notify_and_free(item, false);
    ++count;
  }

  if (count > 0) {
    LOGW("[配置文件] 已丢弃待写配置：数量=%u 原因=%s",
         (unsigned)count,
         reason ? reason : "未指定");
  }
}
