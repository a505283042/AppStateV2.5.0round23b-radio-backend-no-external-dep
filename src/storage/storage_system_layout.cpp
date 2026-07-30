#include "storage/system_paths.h"

#include <Arduino.h>
#include <SdFat.h>
#include <stdio.h>
#include <string.h>

#include "storage/storage.h"
#include "storage/storage_io.h"
#include "utils/log.h"

extern SdFat sd;

namespace {

struct FixedMigration {
  const char* old_path;
  const char* new_path;
};

static bool ensure_dir_locked(const char* path)
{
  if (sd.exists(path)) return true;
  if (sd.mkdir(path)) {
    LOGI("[系统目录] 已创建：%s", path);
    return true;
  }
  LOGE("[系统目录] 创建失败：%s", path);
  return false;
}

static bool migrate_file_locked(const char* old_path, const char* new_path)
{
  if (!sd.exists(old_path)) return true;

  if (sd.exists(new_path)) {
    // 新文件已经存在时不能覆盖；把旧文件保留为 .legacy，仍清理 /System 根目录。
    for (uint8_t suffix = 0; suffix < 10; ++suffix) {
      String legacy_path = String(new_path) + ".legacy";
      if (suffix > 0) legacy_path += String(suffix);
      if (sd.exists(legacy_path.c_str())) continue;

      if (sd.rename(old_path, legacy_path.c_str())) {
        LOGW("[系统目录] 新旧文件同时存在，旧文件已保留为：%s",
             legacy_path.c_str());
        return true;
      }
      break;
    }

    LOGW("[系统目录] 新旧文件同时存在且旧文件无法归档，暂时保留：旧=%s 新=%s",
         old_path,
         new_path);
    return true;
  }

  if (!sd.rename(old_path, new_path)) {
    LOGE("[系统目录] 文件迁移失败：%s -> %s", old_path, new_path);
    return false;
  }

  LOGI("[系统目录] 文件已迁移：%s -> %s", old_path, new_path);
  return true;
}

static bool starts_with(const char* text, const char* prefix)
{
  return text && prefix && strncmp(text, prefix, strlen(prefix)) == 0;
}

static bool ends_with(const char* text, const char* suffix)
{
  if (!text || !suffix) return false;
  const size_t text_len = strlen(text);
  const size_t suffix_len = strlen(suffix);
  return text_len >= suffix_len &&
         strcmp(text + text_len - suffix_len, suffix) == 0;
}

static bool find_legacy_crash_file_locked(char* out_name, size_t out_size)
{
  if (!out_name || out_size == 0) return false;
  out_name[0] = '\0';

  SdFile root;
  if (!root.open(SystemPaths::kRoot, O_RDONLY) || !root.isDir()) {
    root.close();
    return false;
  }

  bool found = false;
  SdFile file;
  while (file.openNext(&root, O_RDONLY)) {
    if (!file.isDir()) {
      file.getName(out_name, out_size);
      const bool coredump = starts_with(out_name, "coredump_") &&
                            ends_with(out_name, ".bin");
      const bool panic = starts_with(out_name, "panic_") &&
                         ends_with(out_name, ".txt");
      found = coredump || panic;
      if (!found) out_name[0] = '\0';
    }
    file.close();
    if (found) break;
  }
  root.close();
  return found;
}

static void migrate_crash_files_locked()
{
  // 每次只找一个旧崩溃文件，关闭目录句柄后再 rename。
  // 这样既不会在遍历期间修改目录，也不需要常驻 32×96B 名称表。
  static constexpr size_t kMaxCrashFilesPerBoot = 32;
  size_t migrated = 0;

  for (; migrated < kMaxCrashFilesPerBoot; ++migrated) {
    char name[96] = {};
    if (!find_legacy_crash_file_locked(name, sizeof(name))) break;

    char old_path[128] = {};
    char new_path[128] = {};
    snprintf(old_path, sizeof(old_path), "%s/%s", SystemPaths::kRoot, name);
    snprintf(new_path, sizeof(new_path), "%s/%s", SystemPaths::kCrashDir, name);

    if (!migrate_file_locked(old_path, new_path)) break;

    // migrate_file_locked() 在冲突文件无法归档时会保留旧文件并返回 true。
    // 此时必须停止，避免下一轮反复处理同一个名称。
    if (sd.exists(old_path)) {
      LOGW("[系统目录] 崩溃文件仍位于旧目录，本次停止继续迁移：%s", old_path);
      break;
    }
  }

  if (migrated == kMaxCrashFilesPerBoot) {
    LOGW("[系统目录] 单次最多迁移 %u 个崩溃文件，剩余文件下次挂载继续处理",
         (unsigned)kMaxCrashFilesPerBoot);
  }
}

}  // namespace

bool storage_system_layout_prepare()
{
  if (!storage_is_ready()) return false;

  StorageSdLockGuard sd_lock(3000);
  if (!sd_lock) {
    LOGW("[系统目录] 等待 SD 锁超时");
    return false;
  }

  bool ok = true;
  ok = ensure_dir_locked(SystemPaths::kRoot) && ok;
  ok = ensure_dir_locked(SystemPaths::kConfigDir) && ok;
  ok = ensure_dir_locked(SystemPaths::kAssetsDir) && ok;
  ok = ensure_dir_locked(SystemPaths::kRadioAssetsDir) && ok;
  ok = ensure_dir_locked(SystemPaths::kLibraryDir) && ok;
  ok = ensure_dir_locked(SystemPaths::kReportsDir) && ok;
  ok = ensure_dir_locked(SystemPaths::kCrashDir) && ok;

  if (!ok) return false;

  static const FixedMigration kMigrations[] = {
      {"/System/net_music_base.txt", SystemPaths::kNetMusicBase},
      {"/System/net_music_sources.txt", SystemPaths::kNetMusicSources},
      {"/System/nfc_map.txt", SystemPaths::kNfcMap},
      {"/System/radio_list.txt", SystemPaths::kRadioList},
      {"/System/wifi.conf", SystemPaths::kWifiConfig},
      {"/System/default_cover.jpg", SystemPaths::kDefaultCover},
      {"/System/net_cover_loading.jpg", SystemPaths::kNetCoverLoading},
      {"/System/music_index_v3.bin", SystemPaths::kMusicIndexV3},
      {"/System/music_index_v3.bin.bak", "/System/library/music_index_v3.bin.bak"},
      {"/System/music_index_v3.bin.tmp", "/System/library/music_index_v3.bin.tmp"},
      {"/System/music_index_v3.bin.bad", "/System/library/music_index_v3.bin.bad"},
      {"/System/music_manifest_v1.bin", SystemPaths::kMusicManifestV1},
      {"/System/music_manifest_v1.bin.bak", "/System/library/music_manifest_v1.bin.bak"},
      {"/System/music_manifest_v1.bin.tmp", "/System/library/music_manifest_v1.bin.tmp"},
      {"/System/music_scan_report.json", SystemPaths::kMusicScanReport},
      {"/System/music_scan_tracks.csv", SystemPaths::kMusicScanTracks},
      {"/System/panic_summary.txt", SystemPaths::kPanicSummary},
  };

  for (const auto& item : kMigrations) {
    ok = migrate_file_locked(item.old_path, item.new_path) && ok;
  }

  migrate_crash_files_locked();
  return ok;
}
