#include "storage/storage_manifest_v1.h"

#include <SdFat.h>
#include <algorithm>
#include <cstring>
#include <utility>

#include "storage/storage_io.h"
#include "storage/system_paths.h"
#include "utils/log.h"

extern SdFat sd;

namespace {

static constexpr uint32_t kManifestMagic = 0x31464E4Du;  // "MNF1"
static constexpr uint16_t kManifestLegacyVersion = 1;
static constexpr uint16_t kManifestAttributesVersion = 2;
static constexpr uint16_t kManifestVersion = 3;
static constexpr uint16_t kManifestFlagsCrc32 = 1u << 0;
static constexpr uint16_t kManifestFlagsCatalogCrc32 = 1u << 1;
static constexpr uint16_t kManifestFlagsDirectorySnapshots = 1u << 2;
static constexpr uint32_t kManifestBaseHeaderBytes = 24;
static constexpr uint32_t kManifestCatalogHeaderBytes = 28;
static constexpr uint32_t kManifestHeaderBytes = 32;
static constexpr uint32_t kManifestMaxEntries = UINT16_MAX;
static constexpr uint32_t kManifestMaxDirectories = 8192;
static constexpr uint32_t kManifestMaxPayloadBytes = 8u * 1024u * 1024u;
static constexpr uint16_t kManifestMaxStringBytes = 4096;

struct ManifestHeaderV1 {
  uint32_t magic = kManifestMagic;
  uint16_t version = kManifestVersion;
  uint16_t flags = kManifestFlagsCrc32 |
                   kManifestFlagsCatalogCrc32 |
                   kManifestFlagsDirectorySnapshots;
  uint32_t header_size = kManifestHeaderBytes;
  uint32_t entry_count = 0;
  uint32_t payload_size = 0;
  uint32_t crc32 = 0;
  uint32_t catalog_crc32 = 0;
  uint32_t directory_count = 0;
};

static_assert(sizeof(ManifestHeaderV1) == kManifestHeaderBytes,
              "ManifestHeaderV1 布局变化会破坏磁盘格式");

static uint32_t crc32_update(uint32_t crc,
                             const uint8_t* data,
                             size_t size)
{
  if (!data || size == 0) return crc;

  for (size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
      crc = (crc >> 1) ^ (0xEDB88320u & mask);
    }
  }
  return crc;
}

static uint32_t crc32_begin()
{
  return 0xFFFFFFFFu;
}

static uint32_t crc32_finish(uint32_t crc)
{
  return ~crc;
}

static bool write_bytes(File32& file, const void* data, size_t size)
{
  if (size == 0) return true;
  if (!data) return false;
  return file.write(reinterpret_cast<const uint8_t*>(data), size) == size;
}

static bool read_bytes(File32& file, void* data, size_t size)
{
  if (size == 0) return true;
  if (!data) return false;
  return file.read(reinterpret_cast<uint8_t*>(data), size) == (int)size;
}

static bool write_u8(File32& file, uint8_t value)
{
  return write_bytes(file, &value, sizeof(value));
}

static bool write_u16(File32& file, uint16_t value)
{
  return write_bytes(file, &value, sizeof(value));
}

static bool write_u32(File32& file, uint32_t value)
{
  return write_bytes(file, &value, sizeof(value));
}

static bool read_u8(File32& file, uint8_t& value)
{
  return read_bytes(file, &value, sizeof(value));
}

static bool read_u16(File32& file, uint16_t& value)
{
  return read_bytes(file, &value, sizeof(value));
}

static bool read_u32(File32& file, uint32_t& value)
{
  return read_bytes(file, &value, sizeof(value));
}

static bool write_header(File32& file, const ManifestHeaderV1& header)
{
  return write_u32(file, header.magic) &&
         write_u16(file, header.version) &&
         write_u16(file, header.flags) &&
         write_u32(file, header.header_size) &&
         write_u32(file, header.entry_count) &&
         write_u32(file, header.payload_size) &&
         write_u32(file, header.crc32) &&
         write_u32(file, header.catalog_crc32) &&
         write_u32(file, header.directory_count);
}

static bool read_header(File32& file, ManifestHeaderV1& header)
{
  if (!read_u32(file, header.magic) ||
      !read_u16(file, header.version) ||
      !read_u16(file, header.flags) ||
      !read_u32(file, header.header_size) ||
      !read_u32(file, header.entry_count) ||
      !read_u32(file, header.payload_size) ||
      !read_u32(file, header.crc32)) {
    return false;
  }

  header.catalog_crc32 = 0;
  header.directory_count = 0;
  if (header.header_size >= kManifestCatalogHeaderBytes &&
      !read_u32(file, header.catalog_crc32)) {
    return false;
  }
  if (header.header_size >= kManifestHeaderBytes &&
      !read_u32(file, header.directory_count)) {
    return false;
  }
  return header.header_size == kManifestBaseHeaderBytes ||
         header.header_size == kManifestCatalogHeaderBytes ||
         header.header_size == kManifestHeaderBytes;
}

static bool string_size_valid(const PsramString& value)
{
  return value.length() <= kManifestMaxStringBytes;
}

static bool entry_strings_valid(const StorageManifestEntryV1& entry)
{
  return string_size_valid(entry.audio_rel);
}

static uint32_t fingerprint_serialized_size(uint16_t version)
{
  if (version <= kManifestLegacyVersion) {
    return sizeof(uint8_t) + 4u * sizeof(uint32_t);
  }

  return 2u * sizeof(uint8_t) +
         sizeof(uint32_t) +
         2u * sizeof(uint16_t) +
         3u * sizeof(uint32_t);
}

static bool calculate_payload_layout(const StorageMusicManifestV1& manifest,
                                     uint32_t& out_payload_size,
                                     uint32_t& out_crc)
{
  if (manifest.entries.size() > kManifestMaxEntries) {
    LOGE("[曲库清单] 条目过多：%u", (unsigned)manifest.entries.size());
    return false;
  }
  if (manifest.directories.size() > kManifestMaxDirectories) {
    LOGE("[曲库清单] 目录快照过多：%u",
         (unsigned)manifest.directories.size());
    return false;
  }

  uint64_t payload_size = 0;
  uint32_t crc = crc32_begin();

  auto add_crc = [&](const void* data, size_t size) {
    crc = crc32_update(crc,
                       reinterpret_cast<const uint8_t*>(data),
                       size);
    payload_size += size;
  };

  auto add_string = [&](const PsramString& value) {
    const uint16_t length = (uint16_t)value.length();
    add_crc(&length, sizeof(length));
    if (length > 0) {
      add_crc(value.c_str(), length);
    }
  };

  auto add_fingerprint = [&](const StorageFileFingerprintV1& fp) {
    const uint8_t present = fp.present ? 1u : 0u;
    const uint8_t attributes_valid = fp.attributes_valid ? 1u : 0u;
    add_crc(&present, sizeof(present));
    add_crc(&attributes_valid, sizeof(attributes_valid));
    add_crc(&fp.size, sizeof(fp.size));
    add_crc(&fp.modify_date, sizeof(fp.modify_date));
    add_crc(&fp.modify_time, sizeof(fp.modify_time));
    add_crc(&fp.head_crc, sizeof(fp.head_crc));
    add_crc(&fp.middle_crc, sizeof(fp.middle_crc));
    add_crc(&fp.tail_crc, sizeof(fp.tail_crc));
  };

  for (const auto& entry : manifest.entries) {
    if (entry.audio_rel.isEmpty() || !entry_strings_valid(entry)) {
      LOGE("[曲库清单] 条目字符串无效：路径=%s",
           entry.audio_rel.c_str());
      return false;
    }

    add_string(entry.audio_rel);

    add_fingerprint(entry.audio_fingerprint);
    add_fingerprint(entry.lrc_fingerprint);
    add_fingerprint(entry.cover_fingerprint);

    if (payload_size > kManifestMaxPayloadBytes) {
      LOGE("[曲库清单] 清单数据过大：%llu",
           (unsigned long long)payload_size);
      return false;
    }
  }

  for (const auto& directory : manifest.directories) {
    if (!string_size_valid(directory.dir_rel) ||
        !string_size_valid(directory.effective_cover_rel)) {
      LOGE("[曲库清单] 目录快照字符串无效：%s",
           directory.dir_rel.c_str());
      return false;
    }

    add_string(directory.dir_rel);
    add_fingerprint(directory.directory_attributes);
    add_string(directory.effective_cover_rel);
    add_fingerprint(directory.effective_cover_attributes);

    const uint8_t flags =
        (directory.has_local_cover ? 1u : 0u) |
        (directory.has_subdirectories ? 2u : 0u);
    add_crc(&flags, sizeof(flags));
    add_crc(&directory.subtree_track_count,
            sizeof(directory.subtree_track_count));

    if (payload_size > kManifestMaxPayloadBytes) {
      LOGE("[曲库清单] 清单数据过大：%llu",
           (unsigned long long)payload_size);
      return false;
    }
  }

  out_payload_size = (uint32_t)payload_size;
  out_crc = crc32_finish(crc);
  return true;
}

static bool write_string(File32& file, const PsramString& value)
{
  const uint16_t length = (uint16_t)value.length();
  return write_u16(file, length) &&
         write_bytes(file, value.c_str(), length);
}

static bool read_string_crc(File32& file,
                            PsramString& out,
                            uint32_t& crc,
                            uint32_t& remaining)
{
  if (remaining < sizeof(uint16_t)) return false;

  uint16_t length = 0;
  if (!read_u16(file, length)) return false;
  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(&length),
                     sizeof(length));
  remaining -= sizeof(uint16_t);

  if (length > kManifestMaxStringBytes || length > remaining) {
    return false;
  }

  out.clear();
  if (length == 0) return true;
  if (!out.resize_for_write(length)) return false;

  uint16_t copied = 0;
  while (copied < length) {
    const uint16_t left = length - copied;
    const uint16_t chunk = left > 128u ? 128u : left;
    char* destination = out.data() + copied;
    if (file.read(reinterpret_cast<uint8_t*>(destination), chunk) != chunk) {
      out.clear();
      return false;
    }
    crc = crc32_update(crc,
                       reinterpret_cast<const uint8_t*>(destination),
                       chunk);
    copied += chunk;
    remaining -= chunk;
  }
  return true;
}

static bool write_fingerprint(File32& file,
                              const StorageFileFingerprintV1& fp)
{
  return write_u8(file, fp.present ? 1u : 0u) &&
         write_u8(file, fp.attributes_valid ? 1u : 0u) &&
         write_u32(file, fp.size) &&
         write_u16(file, fp.modify_date) &&
         write_u16(file, fp.modify_time) &&
         write_u32(file, fp.head_crc) &&
         write_u32(file, fp.middle_crc) &&
         write_u32(file, fp.tail_crc);
}

static bool read_fingerprint_crc(File32& file,
                                 StorageFileFingerprintV1& fp,
                                 uint32_t& crc,
                                 uint32_t& remaining,
                                 uint16_t manifest_version)
{
  const uint32_t need = fingerprint_serialized_size(manifest_version);
  if (remaining < need) return false;

  uint8_t present = 0;
  uint8_t attributes_valid = 0;
  if (!read_u8(file, present)) return false;
  crc = crc32_update(crc, &present, sizeof(present));

  if (manifest_version >= kManifestAttributesVersion) {
    if (!read_u8(file, attributes_valid)) return false;
    crc = crc32_update(crc, &attributes_valid, sizeof(attributes_valid));
  }

  if (!read_u32(file, fp.size)) return false;
  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(&fp.size),
                     sizeof(fp.size));

  if (manifest_version >= kManifestAttributesVersion) {
    if (!read_u16(file, fp.modify_date) ||
        !read_u16(file, fp.modify_time)) {
      return false;
    }
    crc = crc32_update(crc,
                       reinterpret_cast<const uint8_t*>(&fp.modify_date),
                       sizeof(fp.modify_date));
    crc = crc32_update(crc,
                       reinterpret_cast<const uint8_t*>(&fp.modify_time),
                       sizeof(fp.modify_time));
  }

  if (!read_u32(file, fp.head_crc) ||
      !read_u32(file, fp.middle_crc) ||
      !read_u32(file, fp.tail_crc)) {
    return false;
  }

  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(&fp.head_crc),
                     sizeof(fp.head_crc));
  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(&fp.middle_crc),
                     sizeof(fp.middle_crc));
  crc = crc32_update(crc,
                     reinterpret_cast<const uint8_t*>(&fp.tail_crc),
                     sizeof(fp.tail_crc));

  remaining -= need;
  fp.present = present != 0;
  fp.attributes_valid = manifest_version >= kManifestAttributesVersion &&
                        attributes_valid != 0;
  return true;
}

static bool write_entry(File32& file, const StorageManifestEntryV1& entry)
{
  return write_string(file, entry.audio_rel) &&
         write_fingerprint(file, entry.audio_fingerprint) &&
         write_fingerprint(file, entry.lrc_fingerprint) &&
         write_fingerprint(file, entry.cover_fingerprint);
}

static bool read_entry_crc(File32& file,
                           StorageManifestEntryV1& entry,
                           uint32_t& crc,
                           uint32_t& remaining,
                           uint16_t manifest_version)
{
  entry = StorageManifestEntryV1{};

  if (!read_string_crc(file, entry.audio_rel, crc, remaining) ||
      !read_fingerprint_crc(file, entry.audio_fingerprint,
                            crc, remaining, manifest_version) ||
      !read_fingerprint_crc(file, entry.lrc_fingerprint,
                            crc, remaining, manifest_version) ||
      !read_fingerprint_crc(file, entry.cover_fingerprint,
                            crc, remaining, manifest_version)) {
    return false;
  }

  return !entry.audio_rel.isEmpty();
}

static bool write_directory_snapshot(
    File32& file,
    const StorageDirectorySnapshotV1& directory)
{
  const uint8_t flags =
      (directory.has_local_cover ? 1u : 0u) |
      (directory.has_subdirectories ? 2u : 0u);

  return write_string(file, directory.dir_rel) &&
         write_fingerprint(file, directory.directory_attributes) &&
         write_string(file, directory.effective_cover_rel) &&
         write_fingerprint(file, directory.effective_cover_attributes) &&
         write_u8(file, flags) &&
         write_u32(file, directory.subtree_track_count);
}

static bool read_directory_snapshot_crc(
    File32& file,
    StorageDirectorySnapshotV1& directory,
    uint32_t& crc,
    uint32_t& remaining)
{
  directory = StorageDirectorySnapshotV1{};
  if (!read_string_crc(file, directory.dir_rel, crc, remaining) ||
      !read_fingerprint_crc(file,
                            directory.directory_attributes,
                            crc,
                            remaining,
                            kManifestVersion) ||
      !read_string_crc(file,
                       directory.effective_cover_rel,
                       crc,
                       remaining) ||
      !read_fingerprint_crc(file,
                            directory.effective_cover_attributes,
                            crc,
                            remaining,
                            kManifestVersion)) {
    return false;
  }

  if (remaining < sizeof(uint8_t) + sizeof(uint32_t)) return false;

  uint8_t flags = 0;
  if (!read_u8(file, flags) ||
      !read_u32(file, directory.subtree_track_count)) {
    return false;
  }
  crc = crc32_update(crc, &flags, sizeof(flags));
  crc = crc32_update(
      crc,
      reinterpret_cast<const uint8_t*>(&directory.subtree_track_count),
      sizeof(directory.subtree_track_count));
  remaining -= sizeof(uint8_t) + sizeof(uint32_t);

  directory.has_local_cover = (flags & 1u) != 0;
  directory.has_subdirectories = (flags & 2u) != 0;
  return true;
}

static bool validate_header(const ManifestHeaderV1& header,
                            uint32_t file_size)
{
  const bool version_valid =
      header.version == kManifestLegacyVersion ||
      header.version == kManifestAttributesVersion ||
      header.version == kManifestVersion;
  const uint32_t expected_header_size =
      header.version == kManifestLegacyVersion
          ? kManifestBaseHeaderBytes
          : (header.version == kManifestAttributesVersion
                 ? kManifestCatalogHeaderBytes
                 : kManifestHeaderBytes);

  if (header.magic != kManifestMagic ||
      !version_valid ||
      header.header_size != expected_header_size ||
      (header.flags & kManifestFlagsCrc32) == 0 ||
      header.entry_count > kManifestMaxEntries ||
      header.directory_count > kManifestMaxDirectories ||
      header.payload_size > kManifestMaxPayloadBytes) {
    return false;
  }

  if (header.version >= kManifestAttributesVersion &&
      (header.flags & kManifestFlagsCatalogCrc32) == 0) {
    return false;
  }
  if (header.version >= kManifestVersion &&
      (header.flags & kManifestFlagsDirectorySnapshots) == 0) {
    return false;
  }
  if (header.version < kManifestVersion &&
      header.directory_count != 0) {
    return false;
  }

  return file_size == header.header_size + header.payload_size;
}

static bool load_manifest_locked(StorageMusicManifestV1& out_manifest,
                                 const char* path,
                                 bool quiet)
{
  out_manifest.clear();

  File32 file = sd.open(path, O_RDONLY);
  if (!file) {
    if (!quiet) {
      LOGW("[曲库清单] 文件不存在或无法打开：%s", path);
    }
    return false;
  }

  const uint64_t size64 = file.fileSize();
  if (size64 > UINT32_MAX) {
    file.close();
    return false;
  }

  ManifestHeaderV1 header{};
  if (!read_header(file, header) ||
      !validate_header(header, (uint32_t)size64)) {
    file.close();
    if (!quiet) {
      LOGW("[曲库清单] 文件头或尺寸无效：%s", path);
    }
    return false;
  }

  out_manifest.entries.reserve(header.entry_count);
  uint32_t remaining = header.payload_size;
  uint32_t crc = crc32_begin();

  for (uint32_t i = 0; i < header.entry_count; ++i) {
    StorageManifestEntryV1 entry{};
    if (!read_entry_crc(file, entry, crc, remaining, header.version)) {
      file.close();
      out_manifest.clear();
      if (!quiet) {
        LOGW("[曲库清单] 条目读取失败：%s index=%lu",
             path,
             (unsigned long)i);
      }
      return false;
    }
    out_manifest.entries.push_back(std::move(entry));
  }

  out_manifest.directories.reserve(header.directory_count);
  for (uint32_t i = 0; i < header.directory_count; ++i) {
    StorageDirectorySnapshotV1 directory{};
    if (!read_directory_snapshot_crc(
            file, directory, crc, remaining)) {
      file.close();
      out_manifest.clear();
      if (!quiet) {
        LOGW("[曲库清单] 目录快照读取失败：%s index=%lu",
             path,
             (unsigned long)i);
      }
      return false;
    }
    out_manifest.directories.push_back(std::move(directory));
  }

  file.close();
  crc = crc32_finish(crc);

  if (remaining != 0 || crc != header.crc32) {
    out_manifest.clear();
    if (!quiet) {
      LOGW("[曲库清单] CRC 或剩余尺寸无效：%s expected=0x%08lx actual=0x%08lx remaining=%lu",
           path,
           (unsigned long)header.crc32,
           (unsigned long)crc,
           (unsigned long)remaining);
    }
    return false;
  }

  out_manifest.catalog_crc_valid =
      header.header_size >= kManifestCatalogHeaderBytes &&
      (header.flags & kManifestFlagsCatalogCrc32) != 0;
  out_manifest.catalog_crc32 = header.catalog_crc32;
  out_manifest.format_version = header.version;

  std::sort(out_manifest.entries.begin(),
            out_manifest.entries.end(),
            [](const StorageManifestEntryV1& a,
               const StorageManifestEntryV1& b) {
              return a.audio_rel.compareTo(b.audio_rel) < 0;
            });

  for (size_t i = 1; i < out_manifest.entries.size(); ++i) {
    if (out_manifest.entries[i - 1].audio_rel ==
        out_manifest.entries[i].audio_rel) {
      out_manifest.clear();
      if (!quiet) {
        LOGW("[曲库清单] 存在重复音频路径：%s",
             path);
      }
      return false;
    }
  }

  std::sort(out_manifest.directories.begin(),
            out_manifest.directories.end(),
            [](const StorageDirectorySnapshotV1& a,
               const StorageDirectorySnapshotV1& b) {
              return a.dir_rel.compareTo(b.dir_rel) < 0;
            });
  for (size_t i = 1; i < out_manifest.directories.size(); ++i) {
    if (out_manifest.directories[i - 1].dir_rel ==
        out_manifest.directories[i].dir_rel) {
      out_manifest.clear();
      if (!quiet) {
        LOGW("[曲库清单] 存在重复目录快照：%s", path);
      }
      return false;
    }
  }

  return true;
}

static bool remove_if_exists_locked(const char* path)
{
  if (!sd.exists(path)) return true;
  return sd.remove(path);
}

}  // namespace

bool storage_manifest_load_v1(StorageMusicManifestV1& out_manifest,
                              const char* manifest_path)
{
  StorageSdLockGuard sd_lock(2000);
  if (!sd_lock) {
    LOGE("[曲库清单] 加载锁超时");
    out_manifest.clear();
    return false;
  }

  if (load_manifest_locked(out_manifest, manifest_path, true)) {
    LOGI("[曲库清单] 加载成功：来源=%s 条目=%u 目录=%u 版本=%u",
         manifest_path,
         (unsigned)out_manifest.entries.size(),
         (unsigned)out_manifest.directories.size(),
         (unsigned)out_manifest.format_version);
    return true;
  }

  const String backup_path = String(manifest_path) + ".bak";
  if (load_manifest_locked(out_manifest, backup_path.c_str(), true)) {
    LOGW("[曲库清单] 正式文件不可用，已使用备份：%s 条目=%u 目录=%u 版本=%u",
         backup_path.c_str(),
         (unsigned)out_manifest.entries.size(),
         (unsigned)out_manifest.directories.size(),
         (unsigned)out_manifest.format_version);
    return true;
  }

  out_manifest.clear();
  LOGW("[曲库清单] 没有可用清单，将执行全量解析");
  return false;
}

bool storage_manifest_save_v1(const StorageMusicManifestV1& manifest,
                              const char* manifest_path)
{
  if (!manifest.catalog_crc_valid) {
    LOGE("[曲库清单] 拒绝保存：缺少对应 Catalog 的一致性 CRC");
    return false;
  }

  uint32_t payload_size = 0;
  uint32_t payload_crc = 0;
  if (!calculate_payload_layout(manifest,
                                payload_size,
                                payload_crc)) {
    return false;
  }

  StorageSdLockGuard sd_lock(2000);
  if (!sd_lock) {
    LOGE("[曲库清单] 保存锁超时");
    return false;
  }

  sd.mkdir(SystemPaths::kRoot);
  sd.mkdir(SystemPaths::kLibraryDir);

  const String final_path(manifest_path);
  const String tmp_path = final_path + ".tmp";
  const String backup_path = final_path + ".bak";

  remove_if_exists_locked(tmp_path.c_str());

  ManifestHeaderV1 header{};
  header.entry_count = (uint32_t)manifest.entries.size();
  header.directory_count = (uint32_t)manifest.directories.size();
  header.payload_size = payload_size;
  header.crc32 = payload_crc;
  header.catalog_crc32 = manifest.catalog_crc32;

  File32 file = sd.open(tmp_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOGE("[曲库清单] 无法创建临时文件：%s", tmp_path.c_str());
    return false;
  }

  bool ok = write_header(file, header);
  for (const auto& entry : manifest.entries) {
    if (!ok) break;
    ok = write_entry(file, entry);
  }
  for (const auto& directory : manifest.directories) {
    if (!ok) break;
    ok = write_directory_snapshot(file, directory);
  }
  if (ok) ok = file.sync();
  file.close();

  if (!ok) {
    remove_if_exists_locked(tmp_path.c_str());
    LOGE("[曲库清单] 临时文件写入失败：%s", tmp_path.c_str());
    return false;
  }

  StorageMusicManifestV1 verify_manifest;
  if (!load_manifest_locked(verify_manifest, tmp_path.c_str(), true) ||
      verify_manifest.entries.size() != manifest.entries.size() ||
      verify_manifest.directories.size() != manifest.directories.size() ||
      !verify_manifest.catalog_crc_valid ||
      verify_manifest.catalog_crc32 != manifest.catalog_crc32) {
    remove_if_exists_locked(tmp_path.c_str());
    LOGE("[曲库清单] 临时文件落盘校验失败：%s", tmp_path.c_str());
    return false;
  }

  const bool had_final = sd.exists(final_path.c_str());
  if (had_final) {
    remove_if_exists_locked(backup_path.c_str());
  }
  if (had_final && !sd.rename(final_path.c_str(), backup_path.c_str())) {
    LOGE("[曲库清单] 无法创建旧清单备份");
    return false;
  }

  if (!sd.rename(tmp_path.c_str(), final_path.c_str())) {
    LOGE("[曲库清单] 临时文件提升失败，尝试恢复备份");
    if (had_final) {
      (void)sd.rename(backup_path.c_str(), final_path.c_str());
    }
    return false;
  }

  StorageMusicManifestV1 final_verify;
  if (!load_manifest_locked(final_verify, final_path.c_str(), true) ||
      final_verify.entries.size() != manifest.entries.size() ||
      final_verify.directories.size() != manifest.directories.size() ||
      !final_verify.catalog_crc_valid ||
      final_verify.catalog_crc32 != manifest.catalog_crc32) {
    LOGE("[曲库清单] 正式文件替换后校验失败");
    remove_if_exists_locked(final_path.c_str());
    if (had_final) {
      (void)sd.rename(backup_path.c_str(), final_path.c_str());
    }
    return false;
  }

  LOGI("[曲库清单] 原子保存成功：%s 版本=%u 条目=%u 目录=%u payload=%lu CRC=0x%08lx catalog=0x%08lx 备份=%s",
       manifest_path,
       (unsigned)kManifestVersion,
       (unsigned)manifest.entries.size(),
       (unsigned)manifest.directories.size(),
       (unsigned long)payload_size,
       (unsigned long)payload_crc,
       (unsigned long)manifest.catalog_crc32,
       had_final ? "已保留" : "首次保存无旧版");
  return true;
}

uint32_t storage_manifest_catalog_crc_v1(const MusicCatalogV3& catalog)
{
  uint32_t crc = crc32_begin();

  const uint32_t counts[] = {
      catalog.track_count,
      catalog.album_count,
      catalog.artist_count,
      catalog.pool.size,
  };
  crc = crc32_update(
      crc,
      reinterpret_cast<const uint8_t*>(counts),
      sizeof(counts));

  crc = crc32_update(crc, catalog.pool.data, catalog.pool.size);
  crc = crc32_update(
      crc,
      reinterpret_cast<const uint8_t*>(catalog.tracks),
      (size_t)catalog.track_count * sizeof(TrackRowV3));
  crc = crc32_update(
      crc,
      reinterpret_cast<const uint8_t*>(catalog.albums),
      (size_t)catalog.album_count * sizeof(AlbumRowV3));
  crc = crc32_update(
      crc,
      reinterpret_cast<const uint8_t*>(catalog.artists),
      (size_t)catalog.artist_count * sizeof(ArtistRowV3));

  return crc32_finish(crc);
}
