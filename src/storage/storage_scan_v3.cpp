#include "storage/storage_scan_v3.h"
#include <FS.h>
#include <SdFat.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <algorithm>
#include <cstring>
#include <esp_heap_caps.h>

#include "utils/log.h"
#include "storage/storage_io.h"
#include "meta/meta_id3.h"
#include "meta/meta_flac.h"
#include "meta/meta_id3_cover.h"
#include "meta/meta_flac_cover.h"
#include "ui/ui.h"
#include "app_flags.h"

/*
 * V3 音乐扫描模块。
 *
 * 这一版额外做了两件对稳定性很重要的事：
 * - 扫描时周期性 vTaskDelay(1)，避免 rescan_v3 长时间占用 CPU 触发 WDT
 * - 封面搜索先查固定候选名，再做轻量回退枚举，减少目录扫描压力
 */

extern SdFat sd;

static uint32_t s_scan_v3_last_yield_ms = 0;
static uint32_t s_scan_v3_last_progress_log_ms = 0;

static inline void scan_v3_reset_coop_state()
{
    s_scan_v3_last_yield_ms = millis();
    s_scan_v3_last_progress_log_ms = s_scan_v3_last_yield_ms;
}

/* 扫描期间主动让出 CPU，避免长目录扫描饿死 IDLE0。 */
static inline void scan_v3_cooperate_wdt()
{
    uint32_t now = millis();
    if ((uint32_t)(now - s_scan_v3_last_yield_ms) >= 8) {
        s_scan_v3_last_yield_ms = now;
        vTaskDelay(1);
    }
}

static inline void scan_v3_maybe_log_progress(int scanned, const String& where)
{
    uint32_t now = millis();
    if ((uint32_t)(now - s_scan_v3_last_progress_log_ms) >= 1500) {
        s_scan_v3_last_progress_log_ms = now;
        LOGD("[曲库扫描] 进度：歌曲=%d 目录=%s", scanned, where.c_str());
    }
}

/* =========================
 * 小工具
 * ========================= */

static String basename_no_ext_v3(const String& filename)
{
    int dot = filename.lastIndexOf('.');
    if (dot <= 0) return filename;
    return filename.substring(0, dot);
}

static String parent_dir_of_v3(const String& full_path)
{
    int slash = full_path.lastIndexOf('/');
    if (slash <= 0) return "/";
    return full_path.substring(0, slash);
}

static bool file_exists_v3(const String& path)
{
    StorageSdLockGuard sd_lock(500);
    if (!sd_lock) return false;

    File32 f = sd.open(path.c_str(), O_RDONLY);
    bool ok = (bool)f;
    if (f) f.close();
    return ok;
}

static String to_music_relative_path_v3(const String& abs_path)
{
    if (abs_path.isEmpty()) return String();
    if (abs_path.startsWith("/Music/")) return abs_path.substring(7);
    return abs_path;
}

static uint8_t ext_to_code_v3(const String& ext)
{
    String e = ext;
    e.toLowerCase();
    if (e == ".mp3")  return EXT_MP3;
    if (e == ".flac") return EXT_FLAC;
    return EXT_UNKNOWN;
}

static uint16_t make_flags_v3(const TrackBuildTempV3& t)
{
    uint16_t flags = TF_NONE;

    if (!t.lrc_rel.isEmpty()) flags |= TF_HAS_LRC;

    if (t.cover_source == COVER_MP3_APIC || t.cover_source == COVER_FLAC_PICTURE) {
        flags |= TF_HAS_EMBED_COVER;
    }
    if (t.cover_source == COVER_FILE_FALLBACK && !t.cover_path_rel.isEmpty()) {
        flags |= TF_HAS_FILE_COVER;
    }

    if (t.ext_code == EXT_MP3)  flags |= TF_IS_MP3;
    if (t.ext_code == EXT_FLAC) flags |= TF_IS_FLAC;

    return flags;
}

/*
 * 目录封面选择策略：
 * 1) 先查固定候选名（cover/folder/front.*）
 * 2) 未命中时再做轻量枚举，优先名字里带 cover/folder/front/album/art 的图片
 */
static String pick_cover_in_folder_v3(const String& folder)
{
    static const char* fixed[] = {
        "cover.jpg", "cover.jpeg", "cover.png",
        "folder.jpg", "folder.jpeg", "folder.png",
        "front.jpg", "front.jpeg", "front.png"
    };

    for (auto name : fixed) {
        scan_v3_cooperate_wdt();
        String p = folder + "/" + name;
        if (file_exists_v3(p)) return p;
    }

    StorageSdLockGuard sd_lock(500);
    if (!sd_lock) {
        return String();
    }

    SdFile dir;
    if (!dir.open(folder.c_str(), O_RDONLY) || !dir.isDir()) {
        dir.close();
        return String();
    }

    String fallback_image;
    int image_seen = 0;
    int entries_seen = 0;

    SdFile f;
    while (f.openNext(&dir, O_RDONLY)) {
        scan_v3_cooperate_wdt();

        if (app_rescan_should_abort()) {
            LOGD("[曲库扫描] 封面 scan aborted: %s", folder.c_str());
            f.close();
            break;
        }

        entries_seen++;

        if (!f.isDir()) {
            char name[256];
            f.getName(name, sizeof(name));
            String n(name);
            String lower = n;
            lower.toLowerCase();

            bool is_image = lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png");
            if (is_image) {
                image_seen++;
                String full = folder + "/" + n;

                if (fallback_image.isEmpty()) {
                    fallback_image = full;
                }

                bool preferred = (lower.indexOf("cover") >= 0) ||
                                 (lower.indexOf("folder") >= 0) ||
                                 (lower.indexOf("front") >= 0) ||
                                 (lower.indexOf("album") >= 0) ||
                                 (lower.indexOf("art") >= 0);
                if (preferred) {
                    f.close();
                    dir.close();
                    return full;
                }

                if (image_seen >= 12 || entries_seen >= 64) {
                    f.close();
                    dir.close();
                    return fallback_image;
                }
            }
        }
        f.close();
    }

    dir.close();
    return fallback_image;
}

static bool should_skip_dir_v3(const String& dir_name)
{
    if (dir_name.isEmpty()) return true;
    if (dir_name == "." || dir_name == "..") return true;
    if (dir_name.startsWith(".")) return true;
    return false;
}

static void derive_dir_hints_v3(const String& dir_path,
                                const char* music_root,
                                String& out_artist,
                                String& out_album)
{
    out_artist = "";
    out_album = "";

    String root = music_root ? String(music_root) : String("/Music");
    String rel = dir_path;

    if (rel.startsWith(root)) {
        rel = rel.substring(root.length());
    }

    while (rel.startsWith("/")) {
        rel.remove(0, 1);
    }

    if (rel.isEmpty()) {
        return;
    }

    std::vector<String> parts;
    int start = 0;
    while (start < rel.length()) {
        int slash = rel.indexOf('/', start);
        String seg = (slash < 0) ? rel.substring(start) : rel.substring(start, slash);
        seg.trim();
        if (!seg.isEmpty()) {
            parts.push_back(seg);
        }
        if (slash < 0) break;
        start = slash + 1;
    }

    if (parts.empty()) {
        return;
    }

    if (parts.size() >= 2) {
        out_artist = parts.front();
        out_album = parts.back();
    } else {
        // 单层子目录时优先把目录名当作专辑名；artist 留空，交给元数据覆盖
        out_album = parts.front();
    }
}

/* =========================
 * 单曲扫描核心
 * ========================= */

static bool scan_one_audio_file_core_v3(const String& full_path,
                                        const String& fn,
                                        const String& fallback_artist,
                                        const String& fallback_album,
                                        const String& fallback_cover_path,
                                        const String* known_lrc_abs,
                                        TrackBuildTempV3& out_track)
{
    String lower = fn;
    lower.toLowerCase();

    if (!(lower.endsWith(".mp3") || lower.endsWith(".flac"))) {
        return false;
    }

    out_track = TrackBuildTempV3{};

    int dot = fn.lastIndexOf('.');
    String ext = (dot >= 0) ? fn.substring(dot) : "";
    ext.toLowerCase();
    out_track.ext_code = ext_to_code_v3(ext);

    out_track.title  = basename_no_ext_v3(fn);
    out_track.artist = fallback_artist;
    out_track.album  = fallback_album;
    out_track.audio_rel = to_music_relative_path_v3(full_path);

    if (known_lrc_abs) {
        out_track.lrc_rel = to_music_relative_path_v3(*known_lrc_abs);
    } else {
        const String base_no_ext = basename_no_ext_v3(fn);
        const String parent_dir = parent_dir_of_v3(full_path);
        const char* const lrc_exts[] = {".lrc", ".LRC", ".Lrc"};
        for (const char* lrc_ext : lrc_exts) {
            const String lrc_abs =
                parent_dir + "/" + base_no_ext + lrc_ext;
            if (file_exists_v3(lrc_abs)) {
                out_track.lrc_rel = to_music_relative_path_v3(lrc_abs);
                break;
            }
        }
    }

    out_track.cover_source = COVER_NONE;
    out_track.cover_offset = 0;
    out_track.cover_size = 0;
    out_track.cover_mime = "";
    out_track.cover_path_rel = "";

    if (!fallback_cover_path.isEmpty()) {
        out_track.cover_source = COVER_FILE_FALLBACK;
        out_track.cover_path_rel = to_music_relative_path_v3(fallback_cover_path);
    }

    if (lower.endsWith(".mp3")) {
        Mp3CoverLoc loc;
        if (id3_find_apic(sd, full_path.c_str(), loc) && loc.found) {
            out_track.cover_source = COVER_MP3_APIC;
            out_track.cover_offset = loc.offset;
            out_track.cover_size = loc.size;
            out_track.cover_mime = loc.mime;
            out_track.cover_path_rel = "";
        }

        Id3BasicInfo meta;
        if (id3_read_basic(sd, full_path.c_str(), meta)) {
            if (meta.title.length())  out_track.title  = meta.title;
            if (meta.artist.length()) out_track.artist = meta.artist;
            if (meta.album.length())  out_track.album  = meta.album;
        }
    } else {
        FlacCoverLoc loc;
        if (flac_find_picture(sd, full_path.c_str(), loc) && loc.found) {
            out_track.cover_source = COVER_FLAC_PICTURE;
            out_track.cover_offset = loc.offset;
            out_track.cover_size = loc.size;
            out_track.cover_mime = loc.mime;
            out_track.cover_path_rel = "";
        }

        FlacBasicInfo meta;
        if (flac_read_vorbis_basic(sd, full_path.c_str(), meta)) {
            if (meta.title.length())  out_track.title  = meta.title;
            if (meta.artist.length()) out_track.artist = meta.artist;
            if (meta.album.length())  out_track.album  = meta.album;
        }
    }

    out_track.flags = make_flags_v3(out_track);
    return true;
}

bool storage_scan_one_audio_file_v3(const String& full_path,
                                    const String& fallback_artist,
                                    const String& fallback_album,
                                    const String& fallback_cover_path,
                                    TrackBuildTempV3& out_track)
{
    String fn = full_path.substring(full_path.lastIndexOf('/') + 1);
    return scan_one_audio_file_core_v3(full_path,
                                       fn,
                                       fallback_artist,
                                       fallback_album,
                                       fallback_cover_path,
                                       nullptr,
                                       out_track);
}

/* =========================
 * 递归扫描
 * ========================= */

static bool scan_dir_recursive_v3(const String& dir_path,
                                  const char* music_root,
                                  const String& inherited_cover_path,
                                  StorageTrackBuildListV3& out_tracks,
                                  int& scanned)
{
    scan_v3_cooperate_wdt();

    SdFile dir;
    if (!dir.open(dir_path.c_str(), O_RDONLY) || !dir.isDir()) {
        LOGE("[曲库扫描] 打开 目录 失败: %s", dir_path.c_str());
        dir.close();
        return false;
    }

    String local_cover = pick_cover_in_folder_v3(dir_path);
    String effective_cover = local_cover.isEmpty() ? inherited_cover_path : local_cover;

    String fallback_artist;
    String fallback_album;
    derive_dir_hints_v3(dir_path, music_root, fallback_artist, fallback_album);

    SdFile f;
    while (f.openNext(&dir, O_RDONLY)) {
        scan_v3_cooperate_wdt();

        if (app_rescan_should_abort()) {
            LOGI("[曲库扫描] scan aborted by user");
            f.close();
            break;
        }

        scan_v3_maybe_log_progress(scanned, dir_path);

        char name[256];
        f.getName(name, sizeof(name));
        String entry_name(name);

        if (f.isDir()) {
            String child_dir = dir_path + "/" + entry_name;
            f.close();

            if (should_skip_dir_v3(entry_name)) {
                continue;
            }

            if (!scan_dir_recursive_v3(child_dir, music_root, effective_cover, out_tracks, scanned)) {
                dir.close();
                return false;
            }
            continue;
        }

        String full_path = dir_path + "/" + entry_name;

        TrackBuildTempV3 t;
        if (storage_scan_one_audio_file_v3(full_path,
                                           fallback_artist,
                                           fallback_album,
                                           effective_cover,
                                           t)) {
            out_tracks.push_back(std::move(t));
            scanned++;
            ui_scan_tick(scanned);
        }

        delay(0);
        f.close();
    }

    dir.close();
    return !app_rescan_should_abort();
}


/* =========================
 * 增量扫描与 Manifest
 * ========================= */

namespace {

static constexpr size_t kFingerprintSampleBytes = 512;
static constexpr uint32_t kFingerprintFullCrcMaxBytes = 16u * 1024u;

struct CachedFingerprintV1 {
    PsramString path;
    StorageFileFingerprintV1 fingerprint;
    bool ok = false;
    bool content_complete = false;
};

struct DirectoryEntrySnapshotV3 {
    PsramString name;
    bool is_dir = false;
    bool is_audio = false;
    StorageFileFingerprintV1 audio_attributes;
};

struct PsramStringLessV3 {
    bool operator()(const PsramString& left,
                    const PsramString& right) const noexcept
    {
        return left.compareTo(right) < 0;
    }
};

using DirectoryEntryListV3 =
    PsramVector<DirectoryEntrySnapshotV3>;
using DirectoryLrcMapV3 =
    PsramMap<PsramString, PsramString, PsramStringLessV3>;

struct IncrementalScanContextV3 {
    const char* music_root = "/Music";
    const StorageMusicManifestV1* old_manifest = nullptr;
    const MusicCatalogV3* reuse_catalog = nullptr;
    const StorageTrackIndexListV3* catalog_path_order = nullptr;
    StorageTrackSeenListV3* old_seen = nullptr;
    StorageTrackBuildListV3* out_tracks = nullptr;
    StorageMusicManifestV1* out_manifest = nullptr;
    StorageIncrementalScanStatsV3* stats = nullptr;
    bool strict_verify = false;
    PsramVector<CachedFingerprintV1> cover_fingerprint_cache;
};

struct ScanHeapSnapshotV3 {
    uint32_t internal_free = 0;
    uint32_t psram_free = 0;
};

static ScanHeapSnapshotV3 scan_heap_snapshot_v3()
{
    ScanHeapSnapshotV3 snapshot{};
    snapshot.internal_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    snapshot.psram_free = (uint32_t)heap_caps_get_free_size(
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return snapshot;
}

static long signed_heap_delta_v3(uint32_t before, uint32_t after)
{
    return (long)after - (long)before;
}

static void count_psram_text_v3(const PsramString& text,
                                uint32_t& total,
                                uint32_t& external,
                                uint32_t& internal_fallback)
{
    if (text.isEmpty()) return;
    ++total;
    if (text.isExternal()) {
        ++external;
    } else {
        ++internal_fallback;
    }
}

static void log_incremental_scan_memory_v3(
    const char* phase,
    const ScanHeapSnapshotV3& start,
    const StorageTrackBuildListV3& tracks,
    const StorageMusicManifestV1& old_manifest,
    const StorageMusicManifestV1& next_manifest,
    const StorageTrackIndexListV3& path_order,
    const StorageTrackSeenListV3& old_seen,
    const PsramVector<CachedFingerprintV1>& cover_cache)
{
    const ScanHeapSnapshotV3 current = scan_heap_snapshot_v3();

    LOGI("[增量扫描][内存] %s 内部空闲=%luB 变化=%ldB PSRAM空闲=%luB 变化=%ldB",
         phase ? phase : "?",
         (unsigned long)current.internal_free,
         signed_heap_delta_v3(start.internal_free, current.internal_free),
         (unsigned long)current.psram_free,
         signed_heap_delta_v3(start.psram_free, current.psram_free));

    LOGI("[增量扫描][PSRAM] tracks=%luB ext=%d old_manifest=%luB ext=%d next_manifest=%luB ext=%d order=%luB ext=%d seen=%luB ext=%d cover_cache=%luB ext=%d",
         (unsigned long)(tracks.capacity() * sizeof(TrackBuildTempV3)),
         (!tracks.empty() && esp_ptr_external_ram(tracks.data())) ? 1 : 0,
         (unsigned long)(old_manifest.entries.capacity() * sizeof(StorageManifestEntryV1)),
         (!old_manifest.entries.empty() && esp_ptr_external_ram(old_manifest.entries.data())) ? 1 : 0,
         (unsigned long)(next_manifest.entries.capacity() * sizeof(StorageManifestEntryV1)),
         (!next_manifest.entries.empty() && esp_ptr_external_ram(next_manifest.entries.data())) ? 1 : 0,
         (unsigned long)(path_order.capacity() * sizeof(uint32_t)),
         (!path_order.empty() && esp_ptr_external_ram(path_order.data())) ? 1 : 0,
         (unsigned long)(old_seen.capacity() * sizeof(uint8_t)),
         (!old_seen.empty() && esp_ptr_external_ram(old_seen.data())) ? 1 : 0,
         (unsigned long)(cover_cache.capacity() * sizeof(CachedFingerprintV1)),
         (!cover_cache.empty() && esp_ptr_external_ram(cover_cache.data())) ? 1 : 0);

    uint32_t text_total = 0;
    uint32_t text_external = 0;
    uint32_t text_internal_fallback = 0;

    for (const auto& track : tracks) {
        count_psram_text_v3(track.title, text_total, text_external, text_internal_fallback);
        count_psram_text_v3(track.artist, text_total, text_external, text_internal_fallback);
        count_psram_text_v3(track.album, text_total, text_external, text_internal_fallback);
        count_psram_text_v3(track.audio_rel, text_total, text_external, text_internal_fallback);
        count_psram_text_v3(track.lrc_rel, text_total, text_external, text_internal_fallback);
        count_psram_text_v3(track.cover_path_rel, text_total, text_external, text_internal_fallback);
        count_psram_text_v3(track.cover_mime, text_total, text_external, text_internal_fallback);
    }

    for (const auto& entry : old_manifest.entries) {
        count_psram_text_v3(entry.audio_rel, text_total, text_external, text_internal_fallback);
    }
    for (const auto& entry : next_manifest.entries) {
        count_psram_text_v3(entry.audio_rel, text_total, text_external, text_internal_fallback);
    }
    for (const auto& item : cover_cache) {
        count_psram_text_v3(item.path, text_total, text_external, text_internal_fallback);
    }

    LOGI("[增量扫描][文本] 非空=%lu PSRAM=%lu 内部回落=%lu",
         (unsigned long)text_total,
         (unsigned long)text_external,
         (unsigned long)text_internal_fallback);
}

static bool is_audio_filename_v3(const String& filename)
{
    String lower = filename;
    lower.toLowerCase();
    return lower.endsWith(".mp3") || lower.endsWith(".flac");
}

static uint32_t fingerprint_crc32_update_v1(uint32_t crc,
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

static bool fingerprint_sample_crc_v1(File32& file,
                                      uint32_t position,
                                      uint32_t size,
                                      uint32_t& out_crc)
{
    out_crc = 0;
    if (size == 0) return true;
    if (!file.seekSet(position)) return false;

    static uint8_t sample[kFingerprintSampleBytes];
    uint32_t crc = 0xFFFFFFFFu;
    uint32_t remaining = size;

    while (remaining > 0) {
        scan_v3_cooperate_wdt();
        const uint32_t chunk = remaining > sizeof(sample)
            ? (uint32_t)sizeof(sample)
            : remaining;
        const int read_count = file.read(sample, chunk);
        if (read_count != (int)chunk) {
            return false;
        }
        crc = fingerprint_crc32_update_v1(crc, sample, chunk);
        remaining -= chunk;
    }

    out_crc = ~crc;
    return true;
}

template <typename FileType>
static bool fingerprint_attributes_from_open_file_v1(
    FileType& file,
    StorageFileFingerprintV1& out)
{
    out = StorageFileFingerprintV1{};
    if (!file || file.isDir()) return false;

    const uint64_t size64 = file.fileSize();
    if (size64 > UINT32_MAX) return false;

    out.present = true;
    out.size = (uint32_t)size64;
    out.attributes_valid = file.getModifyDateTime(
        &out.modify_date, &out.modify_time);
    return true;
}

static bool fingerprint_file_v1(const String& path,
                                bool include_content,
                                StorageFileFingerprintV1& out)
{
    out = StorageFileFingerprintV1{};
    if (path.isEmpty()) return true;

    File32 file = sd.open(path.c_str(), O_RDONLY);
    if (!file || file.isDir()) {
        if (file) file.close();
        return false;
    }

    if (!fingerprint_attributes_from_open_file_v1(file, out)) {
        file.close();
        return false;
    }

    if (!include_content) {
        file.close();
        return true;
    }

    const uint32_t size = out.size;
    bool ok = true;
    if (size <= kFingerprintFullCrcMaxBytes) {
        // 歌词和小封面完整计算 CRC，避免等长修改刚好落在采样区之外。
        ok = fingerprint_sample_crc_v1(file, 0, size, out.head_crc);
        out.middle_crc = out.head_crc;
        out.tail_crc = out.head_crc;
    } else {
        const uint32_t sample_size = (uint32_t)kFingerprintSampleBytes;
        const uint32_t middle_position =
            (size / 2u) - (sample_size / 2u);
        const uint32_t tail_position = size - sample_size;

        ok = fingerprint_sample_crc_v1(
            file, 0, sample_size, out.head_crc);
        ok = ok && fingerprint_sample_crc_v1(
            file, middle_position, sample_size, out.middle_crc);
        ok = ok && fingerprint_sample_crc_v1(
            file, tail_position, sample_size, out.tail_crc);
    }
    file.close();

    if (!ok) {
        out = StorageFileFingerprintV1{};
        return false;
    }
    return true;
}

static int cover_priority_from_name_v3(const String& filename)
{
    String lower = filename;
    lower.toLowerCase();

    static const char* fixed[] = {
        "cover.jpg", "cover.jpeg", "cover.png",
        "folder.jpg", "folder.jpeg", "folder.png",
        "front.jpg", "front.jpeg", "front.png"
    };

    for (size_t i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (lower == fixed[i]) return (int)i;
    }

    const bool is_image = lower.endsWith(".jpg") ||
                          lower.endsWith(".jpeg") ||
                          lower.endsWith(".png");
    if (!is_image) return -1;

    const bool preferred = lower.indexOf("cover") >= 0 ||
                           lower.indexOf("folder") >= 0 ||
                           lower.indexOf("front") >= 0 ||
                           lower.indexOf("album") >= 0 ||
                           lower.indexOf("art") >= 0;
    return preferred ? 100 : 200;
}

static bool inventory_directory_v3(
    const String& dir_path,
    DirectoryEntryListV3& out_entries,
    DirectoryLrcMapV3& out_lrc_by_base,
    String& out_local_cover)
{
    out_entries.clear();
    out_lrc_by_base.clear();
    out_local_cover = String();

    SdFile dir;
    if (!dir.open(dir_path.c_str(), O_RDONLY) || !dir.isDir()) {
        LOGE("[增量扫描] 打开目录失败：%s", dir_path.c_str());
        dir.close();
        return false;
    }

    int best_cover_priority = 1000;
    SdFile file;
    while (file.openNext(&dir, O_RDONLY)) {
        scan_v3_cooperate_wdt();

        if (app_rescan_should_abort()) {
            file.close();
            dir.close();
            return false;
        }

        char name[256];
        file.getName(name, sizeof(name));
        const String entry_name(name);

        DirectoryEntrySnapshotV3 entry{};
        entry.name = entry_name;
        entry.is_dir = file.isDir();

        if (!entry.is_dir) {
            entry.is_audio = is_audio_filename_v3(entry_name);
            if (entry.is_audio &&
                !fingerprint_attributes_from_open_file_v1(
                    file, entry.audio_attributes)) {
                LOGE("[增量扫描] 音频目录属性读取失败：%s/%s",
                     dir_path.c_str(),
                     entry_name.c_str());
                file.close();
                dir.close();
                return false;
            }

            String lower = entry_name;
            lower.toLowerCase();
            if (lower.endsWith(".lrc")) {
                const String base = basename_no_ext_v3(lower);
                const String full_path = dir_path + "/" + entry_name;
                out_lrc_by_base[PsramString(base)] = full_path;
            }

            const int cover_priority =
                cover_priority_from_name_v3(entry_name);
            if (cover_priority >= 0 &&
                cover_priority < best_cover_priority) {
                best_cover_priority = cover_priority;
                out_local_cover = dir_path + "/" + entry_name;
            }
        }

        out_entries.push_back(std::move(entry));
        file.close();
    }

    dir.close();
    return true;
}

static String find_lrc_path_from_inventory_v3(
    const String& filename,
    const DirectoryLrcMapV3& lrc_by_base)
{
    String base = basename_no_ext_v3(filename);
    base.toLowerCase();

    const auto it = lrc_by_base.find(PsramString(base));
    if (it == lrc_by_base.end()) return String();
    return String(it->second.c_str());
}

static bool fingerprint_attributes_equal_v1(
    const StorageFileFingerprintV1& a,
    const StorageFileFingerprintV1& b)
{
    if (a.present != b.present) return false;
    if (!a.present) return true;

    return a.attributes_valid &&
           b.attributes_valid &&
           a.size == b.size &&
           a.modify_date == b.modify_date &&
           a.modify_time == b.modify_time;
}

static bool fingerprint_content_equal_v1(
    const StorageFileFingerprintV1& a,
    const StorageFileFingerprintV1& b)
{
    return a.present == b.present &&
           a.size == b.size &&
           a.head_crc == b.head_crc &&
           a.middle_crc == b.middle_crc &&
           a.tail_crc == b.tail_crc;
}

static String music_absolute_path_v3(const char* music_root,
                                     const String& relative_path)
{
    if (relative_path.isEmpty()) return String();
    if (relative_path.startsWith("/")) return relative_path;

    String root = music_root ? String(music_root) : String("/Music");
    if (!root.endsWith("/")) root += "/";
    return root + relative_path;
}

static String music_absolute_path_v3(const char* music_root,
                                     const PsramString& relative_path)
{
    return music_absolute_path_v3(
        music_root,
        String(relative_path.c_str()));
}



static bool fingerprint_cached_v1(
    const String& path,
    bool include_content,
    PsramVector<CachedFingerprintV1>& cache,
    StorageFileFingerprintV1& out)
{
    if (path.isEmpty()) {
        out = StorageFileFingerprintV1{};
        return true;
    }

    for (auto& item : cache) {
        if (item.path != path) continue;

        if (include_content && !item.content_complete) {
            item.ok = fingerprint_file_v1(path, true, item.fingerprint);
            item.content_complete = item.ok;
        }
        out = item.fingerprint;
        return item.ok;
    }

    CachedFingerprintV1 item{};
    item.path = path;
    item.ok = fingerprint_file_v1(
        path, include_content, item.fingerprint);
    item.content_complete = item.ok && include_content;
    out = item.fingerprint;
    cache.push_back(std::move(item));
    return cache.back().ok;
}

static int find_manifest_entry_v1(const StorageMusicManifestV1& manifest,
                                  const String& audio_rel)
{
    size_t low = 0;
    size_t high = manifest.entries.size();

    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const int compare =
            manifest.entries[middle].audio_rel.compareTo(audio_rel);
        if (compare < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low < manifest.entries.size() &&
        manifest.entries[low].audio_rel == audio_rel) {
        return (int)low;
    }
    return -1;
}

static StorageManifestEntryV1 manifest_entry_from_temp_v1(
    const TrackBuildTempV3& track,
    const StorageFileFingerprintV1& audio_fingerprint,
    const StorageFileFingerprintV1& lrc_fingerprint,
    const StorageFileFingerprintV1& cover_fingerprint)
{
    StorageManifestEntryV1 entry{};
    entry.audio_rel = track.audio_rel;
    entry.audio_fingerprint = audio_fingerprint;
    entry.lrc_fingerprint = lrc_fingerprint;

    if (track.cover_source == COVER_FILE_FALLBACK &&
        !track.cover_path_rel.isEmpty()) {
        entry.cover_fingerprint = cover_fingerprint;
    }
    return entry;
}

static const char* catalog_track_audio_rel_v3(
    const MusicCatalogV3& catalog,
    uint32_t track_index)
{
    if (!catalog.tracks || track_index >= catalog.track_count) return "";
    return pool_str_v3(
        catalog.pool,
        catalog.tracks[track_index].audio_rel_off);
}

static bool build_catalog_path_order_v3(
    const MusicCatalogV3& catalog,
    StorageTrackIndexListV3& out_order)
{
    out_order.clear();
    if (!catalog.tracks || catalog.track_count == 0) return false;

    out_order.reserve(catalog.track_count);
    for (uint32_t i = 0; i < catalog.track_count; ++i) {
        const char* path = catalog_track_audio_rel_v3(catalog, i);
        if (!path || path[0] == '\0') {
            out_order.clear();
            return false;
        }
        out_order.push_back(i);
    }

    std::sort(out_order.begin(),
              out_order.end(),
              [&](uint32_t left, uint32_t right) {
                return strcmp(
                    catalog_track_audio_rel_v3(catalog, left),
                    catalog_track_audio_rel_v3(catalog, right)) < 0;
              });

    for (size_t i = 1; i < out_order.size(); ++i) {
        if (strcmp(catalog_track_audio_rel_v3(catalog, out_order[i - 1]),
                   catalog_track_audio_rel_v3(catalog, out_order[i])) == 0) {
            out_order.clear();
            return false;
        }
    }
    return true;
}

template <typename TextType>
static int find_catalog_track_v3(
    const MusicCatalogV3& catalog,
    const StorageTrackIndexListV3& order,
    const TextType& audio_rel)
{
    size_t low = 0;
    size_t high = order.size();

    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const char* path = catalog_track_audio_rel_v3(
            catalog, order[middle]);
        const int compare = strcmp(path ? path : "", audio_rel.c_str());
        if (compare < 0) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    if (low < order.size()) {
        const char* path = catalog_track_audio_rel_v3(catalog, order[low]);
        if (path && strcmp(path, audio_rel.c_str()) == 0) {
            return (int)order[low];
        }
    }
    return -1;
}

static bool temp_from_catalog_track_v3(
    const MusicCatalogV3& catalog,
    uint32_t track_index,
    TrackBuildTempV3& out)
{
    out = TrackBuildTempV3{};
    if (!catalog.tracks || track_index >= catalog.track_count) return false;

    const TrackRowV3& row = catalog.tracks[track_index];
    out.title = pool_str_v3(catalog.pool, row.title_off);
    out.artist = pool_str_v3(catalog.pool, row.artist_off);
    out.audio_rel = pool_str_v3(catalog.pool, row.audio_rel_off);
    out.lrc_rel = pool_str_v3(catalog.pool, row.lrc_rel_off);
    out.cover_path_rel = pool_str_v3(catalog.pool, row.cover_path_off);
    out.cover_mime = pool_str_v3(catalog.pool, row.mime_off);
    out.cover_offset = row.cover_offset;
    out.cover_size = row.cover_size;
    out.cover_source = row.cover_source;
    out.ext_code = row.ext_code;
    out.flags = row.flags;

    if (row.album_id != INVALID_ID32 &&
        catalog.albums &&
        row.album_id < catalog.album_count) {
        out.album = pool_str_v3(
            catalog.pool,
            catalog.albums[row.album_id].name_off);
    }
    return !out.audio_rel.isEmpty();
}

static bool manifest_matches_catalog_v3(
    const StorageMusicManifestV1& manifest,
    const MusicCatalogV3& catalog,
    const StorageTrackIndexListV3& order)
{
    if (!manifest.catalog_crc_valid ||
        manifest.catalog_crc32 !=
            storage_manifest_catalog_crc_v1(catalog) ||
        manifest.entries.size() != catalog.track_count ||
        order.size() != catalog.track_count) {
        return false;
    }

    for (const auto& entry : manifest.entries) {
        if (find_catalog_track_v3(catalog, order, entry.audio_rel) < 0) {
            return false;
        }
    }
    return true;
}

static bool manifest_entry_matches_v1(
    const StorageManifestEntryV1& old_entry,
    const MusicCatalogV3& catalog,
    uint32_t track_index,
    const StorageFileFingerprintV1& audio_fingerprint,
    const String& current_lrc_rel,
    const StorageFileFingerprintV1& lrc_fingerprint,
    const String& current_cover_rel,
    const StorageFileFingerprintV1& cover_fingerprint,
    bool attributes_only)
{
    if (!catalog.tracks || track_index >= catalog.track_count) return false;
    const TrackRowV3& row = catalog.tracks[track_index];

    const bool audio_matches = attributes_only
        ? fingerprint_attributes_equal_v1(
              old_entry.audio_fingerprint, audio_fingerprint)
        : fingerprint_content_equal_v1(
              old_entry.audio_fingerprint, audio_fingerprint);
    if (!audio_matches) return false;

    const char* catalog_lrc_rel = pool_str_v3(
        catalog.pool, row.lrc_rel_off);
    // 歌词通常很小，快速模式也完整校验 CRC，避免 FAT 两秒时间精度漏掉等长修改。
    const bool lrc_matches = fingerprint_content_equal_v1(
        old_entry.lrc_fingerprint, lrc_fingerprint);
    if (current_lrc_rel != (catalog_lrc_rel ? catalog_lrc_rel : "") ||
        !lrc_matches) {
        return false;
    }

    if (row.cover_source == COVER_MP3_APIC ||
        row.cover_source == COVER_FLAC_PICTURE) {
        // 内嵌封面已经包含在音频文件指纹中，目录封面变化与本曲无关。
        return true;
    }

    if (row.cover_source == COVER_FILE_FALLBACK) {
        const char* catalog_cover_rel = pool_str_v3(
            catalog.pool, row.cover_path_off);
        // 目录封面按目录缓存，快速模式也只需完整读取每张封面一次。
        const bool cover_matches = fingerprint_content_equal_v1(
            old_entry.cover_fingerprint, cover_fingerprint);
        return current_cover_rel ==
                   (catalog_cover_rel ? catalog_cover_rel : "") &&
               cover_matches;
    }

    // 旧索引没有封面时，如果目录中新出现了封面，需要重新解析并更新索引。
    return current_cover_rel.isEmpty();
}

static bool load_scan_fingerprints_v1(
    const String& full_path,
    const String& lrc_abs,
    const String& effective_cover_path,
    const StorageFileFingerprintV1* audio_attributes,
    bool include_content,
    IncrementalScanContextV3& context,
    StorageFileFingerprintV1& audio_fingerprint,
    StorageFileFingerprintV1& lrc_fingerprint,
    StorageFileFingerprintV1& cover_fingerprint)
{
    if (!include_content && audio_attributes) {
        audio_fingerprint = *audio_attributes;
    } else if (!fingerprint_file_v1(
                   full_path, include_content, audio_fingerprint)) {
        LOGE("[增量扫描] 音频指纹读取失败：%s", full_path.c_str());
        return false;
    }
    if (!fingerprint_file_v1(
            lrc_abs, true, lrc_fingerprint)) {
        LOGE("[增量扫描] 歌词指纹读取失败：%s", lrc_abs.c_str());
        return false;
    }
    if (!fingerprint_cached_v1(
            effective_cover_path,
            true,
            context.cover_fingerprint_cache,
            cover_fingerprint)) {
        LOGE("[增量扫描] 封面指纹读取失败：%s",
             effective_cover_path.c_str());
        return false;
    }
    return true;
}

static void update_reused_manifest_entry_v1(
    const TrackBuildTempV3& track,
    const StorageFileFingerprintV1& audio_fingerprint,
    const StorageFileFingerprintV1& lrc_fingerprint,
    const StorageFileFingerprintV1& cover_fingerprint,
    StorageManifestEntryV1& entry)
{
    entry.audio_fingerprint = audio_fingerprint;
    entry.lrc_fingerprint = lrc_fingerprint;
    entry.cover_fingerprint = StorageFileFingerprintV1{};

    if (track.cover_source == COVER_FILE_FALLBACK &&
        !track.cover_path_rel.isEmpty()) {
        entry.cover_fingerprint = cover_fingerprint;
    }
}

static bool scan_incremental_audio_file_v3(
    const String& full_path,
    const String& filename,
    const String& fallback_artist,
    const String& fallback_album,
    const String& effective_cover_path,
    const String& lrc_abs,
    const StorageFileFingerprintV1* audio_attributes,
    IncrementalScanContextV3& context)
{
    if (!is_audio_filename_v3(filename)) return true;

    const String audio_rel = to_music_relative_path_v3(full_path);
    const String lrc_rel = to_music_relative_path_v3(lrc_abs);
    const String cover_rel = to_music_relative_path_v3(effective_cover_path);

    int old_index = -1;
    int catalog_track_index = -1;
    if (context.old_manifest) {
        old_index = find_manifest_entry_v1(
            *context.old_manifest, audio_rel);
        if (old_index >= 0) {
            catalog_track_index = find_catalog_track_v3(
                *context.reuse_catalog,
                *context.catalog_path_order,
                audio_rel);
        }
    }

    StorageFileFingerprintV1 audio_fingerprint{};
    StorageFileFingerprintV1 lrc_fingerprint{};
    StorageFileFingerprintV1 cover_fingerprint{};

    bool content_loaded = context.strict_verify || old_index < 0;
    if (!load_scan_fingerprints_v1(
            full_path,
            lrc_abs,
            effective_cover_path,
            audio_attributes,
            content_loaded,
            context,
            audio_fingerprint,
            lrc_fingerprint,
            cover_fingerprint)) {
        return false;
    }

    TrackBuildTempV3 track{};
    StorageManifestEntryV1 next_entry{};
    bool reused = false;

    if (old_index >= 0 && catalog_track_index >= 0) {
        const StorageManifestEntryV1& old_entry =
            context.old_manifest->entries[(size_t)old_index];

        bool unchanged = false;
        if (context.strict_verify) {
            ++context.stats->content_verified;
            unchanged = manifest_entry_matches_v1(
                old_entry,
                *context.reuse_catalog,
                (uint32_t)catalog_track_index,
                audio_fingerprint,
                lrc_rel,
                lrc_fingerprint,
                cover_rel,
                cover_fingerprint,
                false);
        } else {
            unchanged = manifest_entry_matches_v1(
                old_entry,
                *context.reuse_catalog,
                (uint32_t)catalog_track_index,
                audio_fingerprint,
                lrc_rel,
                lrc_fingerprint,
                cover_rel,
                cover_fingerprint,
                true);

            if (unchanged) {
                ++context.stats->attribute_reused;
            } else {
                // FAT 属性变化或旧版清单没有属性时，再读取内容 CRC 确认。
                if (!load_scan_fingerprints_v1(
                        full_path,
                        lrc_abs,
                        effective_cover_path,
                        audio_attributes,
                        true,
                        context,
                        audio_fingerprint,
                        lrc_fingerprint,
                        cover_fingerprint)) {
                    return false;
                }
                content_loaded = true;
                ++context.stats->content_verified;
                unchanged = manifest_entry_matches_v1(
                    old_entry,
                    *context.reuse_catalog,
                    (uint32_t)catalog_track_index,
                    audio_fingerprint,
                    lrc_rel,
                    lrc_fingerprint,
                    cover_rel,
                    cover_fingerprint,
                    false);
            }
        }

        if (unchanged &&
            temp_from_catalog_track_v3(
                *context.reuse_catalog,
                (uint32_t)catalog_track_index,
                track)) {
            next_entry = old_entry;
            if (content_loaded) {
                // 严格校验或 v1 清单升级后，将最新 FAT 属性写入下一份清单。
                update_reused_manifest_entry_v1(
                    track,
                    audio_fingerprint,
                    lrc_fingerprint,
                    cover_fingerprint,
                    next_entry);
            }
            reused = true;
            (*context.old_seen)[(size_t)old_index] = 1;
            ++context.stats->reused;
        }
    }

    if (!reused) {
        if (!content_loaded &&
            !load_scan_fingerprints_v1(
                full_path,
                lrc_abs,
                effective_cover_path,
                audio_attributes,
                true,
                context,
                audio_fingerprint,
                lrc_fingerprint,
                cover_fingerprint)) {
            return false;
        }

        if (!scan_one_audio_file_core_v3(full_path,
                                         filename,
                                         fallback_artist,
                                         fallback_album,
                                         effective_cover_path,
                                         &lrc_abs,
                                         track)) {
            LOGE("[增量扫描] 音频完整解析失败：%s", full_path.c_str());
            return false;
        }

        StorageFileFingerprintV1 parsed_lrc_fingerprint = lrc_fingerprint;
        if (track.lrc_rel != lrc_rel) {
            const String parsed_lrc_abs = music_absolute_path_v3(
                context.music_root, track.lrc_rel);
            if (!fingerprint_file_v1(
                    parsed_lrc_abs, true, parsed_lrc_fingerprint)) {
                LOGE("[增量扫描] 解析后歌词指纹读取失败：%s",
                     parsed_lrc_abs.c_str());
                return false;
            }
        }

        StorageFileFingerprintV1 parsed_cover_fingerprint{};
        if (track.cover_source == COVER_FILE_FALLBACK &&
            !track.cover_path_rel.isEmpty()) {
            const String parsed_cover_abs = music_absolute_path_v3(
                context.music_root, track.cover_path_rel);
            if (!fingerprint_cached_v1(
                    parsed_cover_abs,
                    true,
                    context.cover_fingerprint_cache,
                    parsed_cover_fingerprint)) {
                LOGE("[增量扫描] 解析后封面指纹读取失败：%s",
                     parsed_cover_abs.c_str());
                return false;
            }
        }

        next_entry = manifest_entry_from_temp_v1(
            track,
            audio_fingerprint,
            parsed_lrc_fingerprint,
            parsed_cover_fingerprint);

        if (old_index >= 0) {
            (*context.old_seen)[(size_t)old_index] = 1;
            ++context.stats->modified;
        } else {
            ++context.stats->added;
        }
    }

    context.out_tracks->push_back(std::move(track));
    context.out_manifest->entries.push_back(std::move(next_entry));
    ++context.stats->discovered;

    UiScanProgress progress{};
    progress.full_scan = context.stats->full_scan;
    progress.forced_full_scan = context.stats->forced_full_scan;
    progress.strict_incremental = context.stats->strict_incremental;
    progress.discovered = context.stats->discovered;
    progress.reused = context.stats->reused;
    progress.added = context.stats->added;
    progress.modified = context.stats->modified;
    progress.deleted = context.stats->deleted;
    progress.current_path = audio_rel.c_str();
    ui_scan_tick(progress);
    return true;
}

static bool scan_dir_incremental_v3(
    const String& dir_path,
    const String& inherited_cover_path,
    IncrementalScanContextV3& context)
{
    scan_v3_cooperate_wdt();

    DirectoryEntryListV3 entries;
    DirectoryLrcMapV3 lrc_by_base;
    String local_cover;
    if (!inventory_directory_v3(
            dir_path,
            entries,
            lrc_by_base,
            local_cover)) {
        return false;
    }

    const String effective_cover = local_cover.isEmpty()
        ? inherited_cover_path
        : local_cover;

    String fallback_artist;
    String fallback_album;
    derive_dir_hints_v3(dir_path,
                        context.music_root,
                        fallback_artist,
                        fallback_album);

    for (const auto& entry : entries) {
        scan_v3_cooperate_wdt();

        if (app_rescan_should_abort()) {
            return false;
        }

        scan_v3_maybe_log_progress(
            (int)context.stats->discovered,
            dir_path);

        const String entry_name(entry.name.c_str());
        if (entry.is_dir) {
            if (should_skip_dir_v3(entry_name)) {
                continue;
            }

            const String child_dir = dir_path + "/" + entry_name;
            if (!scan_dir_incremental_v3(
                    child_dir,
                    effective_cover,
                    context)) {
                return false;
            }
            continue;
        }

        if (!entry.is_audio) {
            continue;
        }

        const String full_path = dir_path + "/" + entry_name;
        const String lrc_abs = find_lrc_path_from_inventory_v3(
            entry_name,
            lrc_by_base);

        if (!scan_incremental_audio_file_v3(
                full_path,
                entry_name,
                fallback_artist,
                fallback_album,
                effective_cover,
                lrc_abs,
                &entry.audio_attributes,
                context)) {
            return false;
        }

        delay(0);
    }

    return !app_rescan_should_abort();
}

}  // namespace

bool storage_scan_music_incremental_v3(
    StorageTrackBuildListV3& out_tracks,
    StorageMusicManifestV1& out_manifest,
    StorageIncrementalScanStatsV3& out_stats,
    const MusicCatalogV3* reuse_catalog,
    const char* music_root,
    const char* manifest_path,
    bool force_full_scan,
    bool strict_verify)
{
    StorageSdLockGuard sd_lock(2000);
    if (!sd_lock) {
        LOGE("[增量扫描] SD 锁超时");
        return false;
    }

    scan_v3_reset_coop_state();
    const uint32_t scan_started_ms = millis();

    const ScanHeapSnapshotV3 heap_start = scan_heap_snapshot_v3();

    out_tracks.clear();
    out_manifest.clear();
    out_stats = StorageIncrementalScanStatsV3{};

    StorageMusicManifestV1 old_manifest;
    StorageTrackIndexListV3 catalog_path_order;

    const bool manifest_file_loaded = !force_full_scan &&
        storage_manifest_load_v1(old_manifest, manifest_path);
    const bool catalog_ready = !force_full_scan && reuse_catalog &&
        reuse_catalog->tracks &&
        reuse_catalog->track_count > 0 &&
        build_catalog_path_order_v3(
            *reuse_catalog, catalog_path_order);

    out_stats.manifest_loaded = manifest_file_loaded &&
        catalog_ready &&
        manifest_matches_catalog_v3(
            old_manifest,
            *reuse_catalog,
            catalog_path_order);
    out_stats.full_scan = force_full_scan || !out_stats.manifest_loaded;
    out_stats.forced_full_scan = force_full_scan;
    out_stats.strict_incremental = strict_verify && !out_stats.full_scan;
    ui_scan_begin(out_stats.full_scan,
                  out_stats.forced_full_scan,
                  out_stats.strict_incremental);

    if (force_full_scan) {
        LOGI("[增量扫描] 用户请求强制全量解析，本轮忽略现有清单");
    } else if (manifest_file_loaded && !out_stats.manifest_loaded) {
        LOGW("[增量扫描] 清单与当前 V3 Catalog 不匹配，本轮回退全量解析");
        old_manifest.clear();
        catalog_path_order.clear();
    }

    StorageTrackSeenListV3 old_seen;
    if (out_stats.manifest_loaded) {
        old_seen.assign(old_manifest.entries.size(), 0);
        out_tracks.reserve(old_manifest.entries.size() + 8);
        out_manifest.entries.reserve(old_manifest.entries.size() + 8);
        LOGI("[增量扫描] 使用旧清单和当前 Catalog：条目=%u 清单版本=%u 校验=%s",
             (unsigned)old_manifest.entries.size(),
             (unsigned)old_manifest.format_version,
             strict_verify ? "严格内容" : "快速属性");
        LOGI("[增量扫描] 目录优化：每个目录单次枚举，歌词与封面从目录快照匹配");
    } else {
        LOGI("[增量扫描] %s，本轮执行全量解析",
             force_full_scan
                 ? "已选择强制全量"
                 : "首次、清单无效或 Catalog 不匹配");
    }

    IncrementalScanContextV3 context{};
    context.music_root = music_root;
    context.old_manifest = out_stats.manifest_loaded
        ? &old_manifest
        : nullptr;
    context.reuse_catalog = out_stats.manifest_loaded
        ? reuse_catalog
        : nullptr;
    context.catalog_path_order = out_stats.manifest_loaded
        ? &catalog_path_order
        : nullptr;
    context.old_seen = &old_seen;
    context.out_tracks = &out_tracks;
    context.out_manifest = &out_manifest;
    context.stats = &out_stats;
    context.strict_verify = strict_verify;

    const bool scan_ok = scan_dir_incremental_v3(
        String(music_root),
        String(),
        context);

    if (!scan_ok || app_rescan_should_abort()) {
        out_tracks.clear();
        out_manifest.clear();
        out_stats.elapsed_ms = millis() - scan_started_ms;
        if (app_rescan_should_abort()) {
            ui_scan_abort();
        } else {
            ui_scan_end();
        }
        return false;
    }

    if (out_stats.manifest_loaded) {
        for (uint8_t seen : old_seen) {
            if (!seen) ++out_stats.deleted;
        }
    }

    std::sort(out_manifest.entries.begin(),
              out_manifest.entries.end(),
              [](const StorageManifestEntryV1& a,
                 const StorageManifestEntryV1& b) {
                return a.audio_rel.compareTo(b.audio_rel) < 0;
              });

    log_incremental_scan_memory_v3(
        "扫描完成",
        heap_start,
        out_tracks,
        old_manifest,
        out_manifest,
        catalog_path_order,
        old_seen,
        context.cover_fingerprint_cache);

    out_stats.elapsed_ms = millis() - scan_started_ms;
    ui_scan_end();

    LOGI("[增量扫描] 完成：模式=%s 校验=%s 强制=%d 发现=%lu 复用=%lu 属性直复用=%lu CRC确认=%lu 新增=%lu 修改=%lu 删除=%lu 用时=%lums",
         out_stats.full_scan ? "全量" : "增量",
         out_stats.full_scan
             ? "完整解析"
             : (out_stats.strict_incremental ? "严格内容" : "快速属性"),
         out_stats.forced_full_scan ? 1 : 0,
         (unsigned long)out_stats.discovered,
         (unsigned long)out_stats.reused,
         (unsigned long)out_stats.attribute_reused,
         (unsigned long)out_stats.content_verified,
         (unsigned long)out_stats.added,
         (unsigned long)out_stats.modified,
         (unsigned long)out_stats.deleted,
         (unsigned long)out_stats.elapsed_ms);

    return !out_tracks.empty();
}

bool storage_scan_music_v3(StorageTrackBuildListV3& out_tracks,
                           const char* music_root)
{
    StorageSdLockGuard sd_lock(2000);
    if (!sd_lock) {
        LOGE("[曲库扫描] 锁 超时");
        return false;
    }

    // 取消标志只在 app_request_start_rescan() 中初始化。
    // 这里不能再次清零，否则扫描任务刚启动时可能吞掉用户的快速取消请求。
    scan_v3_reset_coop_state();
    ui_scan_begin(true, true, false);

    out_tracks.clear();
    int scanned = 0;

    if (!scan_dir_recursive_v3(String(music_root), music_root, "", out_tracks, scanned)) {
        if (app_rescan_should_abort()) {
            ui_scan_abort();
        } else {
            ui_scan_end();
        }
        return false;
    }

    ui_scan_end();

    LOGI("[曲库扫描] 递归扫描完成：歌曲=%d", (int)out_tracks.size());
    return !out_tracks.empty();
}
