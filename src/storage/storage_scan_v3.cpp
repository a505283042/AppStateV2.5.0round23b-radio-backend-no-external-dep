#include "storage/storage_scan_v3.h"
#include <FS.h>
#include <SdFat.h>
#include <Arduino.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

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
        LOGI("[SCAN_V3] progress: tracks=%d dir=%s", scanned, where.c_str());
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

        if (g_abort_scan) {
            LOGI("[SCAN_V3] cover scan aborted: %s", folder.c_str());
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

    String base_no_ext = basename_no_ext_v3(fn);
    String parent_dir = parent_dir_of_v3(full_path);

    String lrc_abs = parent_dir + "/" + base_no_ext + ".lrc";
    if (file_exists_v3(lrc_abs)) {
        out_track.lrc_rel = to_music_relative_path_v3(lrc_abs);
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
                                       out_track);
}

/* =========================
 * 递归扫描
 * ========================= */

static bool scan_dir_recursive_v3(const String& dir_path,
                                  const char* music_root,
                                  const String& inherited_cover_path,
                                  std::vector<TrackBuildTempV3>& out_tracks,
                                  int& scanned)
{
    scan_v3_cooperate_wdt();

    SdFile dir;
    if (!dir.open(dir_path.c_str(), O_RDONLY) || !dir.isDir()) {
        LOGE("[SCAN_V3] open dir failed: %s", dir_path.c_str());
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

        if (g_abort_scan) {
            LOGI("[SCAN_V3] scan aborted by user");
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
    return !g_abort_scan;
}

bool storage_scan_music_v3(std::vector<TrackBuildTempV3>& out_tracks,
                           const char* music_root)
{
    StorageSdLockGuard sd_lock(2000);
    if (!sd_lock) {
        LOGE("[SCAN_V3] lock timeout");
        return false;
    }

    g_abort_scan = false;
    scan_v3_reset_coop_state();
    ui_scan_begin();

    out_tracks.clear();
    int scanned = 0;

    if (!scan_dir_recursive_v3(String(music_root), music_root, "", out_tracks, scanned)) {
        if (g_abort_scan) {
            ui_scan_abort();
        } else {
            ui_scan_end();
        }
        return false;
    }

    ui_scan_end();

    LOGI("[SCAN_V3] recursive done: tracks=%d", (int)out_tracks.size());
    return !out_tracks.empty();
}
